# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for package third-party notice generation, licensing gates, and relink materials."""

from __future__ import annotations

import importlib.util
import json
from io import StringIO
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import package_notices
from title_codegen_plan import PackageRouteError
from makefile_text import read_makefile  # noqa: E402


class TestPackageNotices(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        self.pkg_dir = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_packaging_fails_when_bundled_binary_lacks_license_record(self) -> None:
        """Gate: packaging fails with named error when any bundled binary lacks a license record."""
        # Create a synthetic package with a dummy unlicensed DLL
        (self.pkg_dir / "unlicensed_component.dll").write_bytes(b"MZ\0\0dummy")

        with self.assertRaises(PackageRouteError) as ctx:
            package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)

        self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_RECORD_MISSING")
        self.assertIn("unlicensed_component.dll", str(ctx.exception))
        self.assertIn("lacks a license record", str(ctx.exception))

    def test_packaging_fails_when_bundled_unlicensed_dll_alongside_licensed_binary(self) -> None:
        """Gate: even if valid binaries exist, an extra unlicensed binary fails closed."""
        (self.pkg_dir / "game.exe").write_bytes(b"MZ\0\0dummy-exe")
        (self.pkg_dir / "SDL3.dll").write_bytes(b"MZ\0\0dummy-sdl3")
        (self.pkg_dir / "rogue.dll").write_bytes(b"MZ\0\0dummy-rogue")

        with self.assertRaises(PackageRouteError) as ctx:
            package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)

        self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_RECORD_MISSING")
        self.assertIn("rogue.dll", str(ctx.exception))

    def test_packaging_fails_when_lgpl_text_missing(self) -> None:
        """Gate: fails with LICENSE_TEXT_MISSING when ATRAC3+ LGPL text is missing."""
        (self.pkg_dir / "game.exe").write_bytes(b"MZ\0\0dummy-exe")
        with tempfile.TemporaryDirectory() as empty_repo:
            fake_repo = Path(empty_repo)
            (fake_repo / "LICENSE").write_text("Project GPL", encoding="utf-8")
            (fake_repo / "src" / "rt" / "atrac3p").mkdir(parents=True)
            # Deliberately do not create LICENSE.LGPLv2.1.txt

            with self.assertRaises(PackageRouteError) as ctx:
                package_notices.generate_package_notices(self.pkg_dir, repo_root=fake_repo)

            self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_TEXT_MISSING")
            self.assertIn("LICENSE_TEXT_MISSING", str(ctx.exception))
            self.assertIn("FFmpeg ATRAC3+", str(ctx.exception))

    def test_packaging_fails_when_one_required_linked_license_text_is_missing(self) -> None:
        """A missing additional notice fails even when another FFmpeg text is present."""
        inventory = package_notices.load_component_inventory()
        ffmpeg = next(
            component for component in inventory["native"]["linked_components"]
            if component["id"] == "ffmpeg-atrac3p"
        )
        required_notice = "third_party/licenses/FFMPEG_MIPS_FFT.txt"
        self.assertIn(required_notice, [entry["file"] for entry in ffmpeg["license_texts"]])

        with tempfile.TemporaryDirectory() as fake_repo_dir:
            fake_repo = Path(fake_repo_dir)
            (fake_repo / "LICENSE").write_text("Project GPL text", encoding="utf-8")
            for entry in ffmpeg["license_texts"]:
                if entry["file"] == required_notice:
                    continue
                source = ROOT / entry["file"]
                target = fake_repo / entry["file"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.read_bytes())

            (self.pkg_dir / "game.exe").write_bytes(
                b"MZ" + bytes((0, 0)) + b"dummy-exe"
            )
            with self.assertRaises(PackageRouteError) as ctx:
                package_notices.generate_package_notices(
                    self.pkg_dir, repo_root=fake_repo
                )

        self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_TEXT_MISSING")
        self.assertIn(required_notice, str(ctx.exception))
        self.assertIn("FFmpeg ATRAC3+ subset", str(ctx.exception))

    def test_malformed_copyright_notices_fail_closed(self) -> None:
        """A copyright_notices value that is not a list of strings is a named failure, not garbled notices."""
        (self.pkg_dir / "mygame.exe").write_bytes(b"MZ\0\0exe")
        real = package_notices.load_component_inventory()
        for bad in ("Copyright (c) PPSSPP Project", {"line": "x"}, ["ok", 3]):
            inventory = json.loads(json.dumps(real))
            linked = next(c for c in inventory["native"]["linked_components"] if c.get("copyright_notices"))
            linked["copyright_notices"] = bad
            with self.subTest(bad=bad), \
                    mock.patch.object(package_notices, "load_component_inventory", return_value=inventory):
                with self.assertRaises(PackageRouteError) as ctx:
                    package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)
                self.assertIn("LICENSE_RECORD_MALFORMED", str(ctx.exception))

    def test_notices_bundle_generation_with_in_tree_license_texts(self) -> None:
        """Bundle generation reads the checked-in texts, not a host toolchain."""
        # Synthetic package contents
        (self.pkg_dir / "mygame.exe").write_bytes(b"MZ\0\0exe")
        (self.pkg_dir / "SDL3.dll").write_bytes(b"MZ\0\0sdl")
        (self.pkg_dir / "libwinpthread-1.dll").write_bytes(b"MZ\0\0pthread")

        result = package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)

        self.assertEqual(result["schema_version"], 1)
        self.assertEqual(result["bundle_type"], "native_package_third_party_notices")
        comp_names = [c["name"] for c in result["components"]]
        self.assertIn("FFmpeg ATRAC3+ subset", comp_names)
        self.assertIn("SDL3", comp_names)
        self.assertIn("winpthreads", comp_names)

        # Check files created on disk
        notices_dir = self.pkg_dir / "THIRD_PARTY_NOTICES"
        self.assertTrue(notices_dir.is_dir())
        self.assertTrue((notices_dir / "index.json").is_file())
        self.assertTrue((notices_dir / "SDL3.txt").is_file())
        self.assertTrue((notices_dir / "FFmpeg-ATRAC3P.txt").is_file())
        self.assertTrue((notices_dir / "winpthreads.txt").is_file())
        self.assertTrue((notices_dir / "RELINK.md").is_file())

        self.assertTrue((self.pkg_dir / "RELINK.md").is_file())
        # Every package names the exact source it was built from.
        self.assertTrue((self.pkg_dir / "SOURCE.txt").is_file())
        self.assertTrue((notices_dir / "SOURCE.txt").is_file())
        self.assertEqual(result["source"]["repository"], package_notices.PROJECT_REPOSITORY_URL)
        self.assertTrue((self.pkg_dir / "THIRD_PARTY_NOTICES.txt").is_file())

        # The in-tree text is authoritative, so the emitted source_path is the
        # repository-relative third_party/licenses/ path and never a host path.
        index_json = json.loads((notices_dir / "index.json").read_text(encoding="utf-8"))
        for comp in index_json["components"]:
            self.assertNotIn(":", comp["source_path"])  # No drive letters / absolute paths
            self.assertNotIn("\\", comp["source_path"])  # POSIX forward slashes
            self.assertTrue((notices_dir / comp["license_file"]).is_file())

        ffmpeg = next(
            c for c in index_json["components"]
            if c["name"] == "FFmpeg ATRAC3+ subset"
        )
        mips_notice_name = "FFmpeg-ATRAC3P-FFMPEG_MIPS_FFT.txt"
        self.assertEqual(ffmpeg["spdx_id"], "LGPL-2.1-or-later")
        self.assertEqual(ffmpeg["additional_license_files"], [mips_notice_name])
        mips_notice = notices_dir / mips_notice_name
        self.assertEqual(
            mips_notice.read_text(encoding="utf-8").strip(),
            (ROOT / "third_party" / "licenses" / "FFMPEG_MIPS_FFT.txt")
            .read_text(encoding="utf-8").strip(),
        )

        sdl = next(c for c in index_json["components"] if c.get("binary") == "SDL3.dll")
        self.assertEqual(sdl["source_path"], "third_party/licenses/sdl3/LICENSE.txt")
        self.assertEqual(
            (notices_dir / "SDL3.txt").read_text(encoding="utf-8").strip(),
            (ROOT / "third_party" / "licenses" / "sdl3" / "LICENSE.txt").read_text(encoding="utf-8").strip(),
        )

        # Check RELINK.md content
        relink_text = (self.pkg_dir / "RELINK.md").read_text(encoding="utf-8")
        self.assertIn("FFmpeg ATRAC3+", relink_text)
        self.assertIn("LGPL-2.1", relink_text)
        self.assertIn("runtime-objects", relink_text)
        self.assertNotIn("atrac3p-objects", relink_text)
        self.assertIn("compile", relink_text)
        self.assertIn("atrac3p_bridge.o", relink_text)
        self.assertIn("atrac3p_libavcodec/atrac3plus.o", relink_text)

        # Check THIRD_PARTY_NOTICES.txt contains the verbatim license texts
        combined_text = (self.pkg_dir / "THIRD_PARTY_NOTICES.txt").read_text(encoding="utf-8")
        self.assertIn("Sam Lantinga", combined_text)
        self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", combined_text)
        self.assertIn("MIPS Technologies, Inc.", combined_text)
        self.assertIn("mingw-w64 project", combined_text)
        # The PPSSPP lineage records (N10) are named, with their copyright lines.
        self.assertIn("Component: sal063 / PPSSPP lineage runtime code", combined_text)
        self.assertIn("Component: PPSSPP VFPU lookup tables", combined_text)
        self.assertIn("Copyright: Copyright (c) 2012- PPSSPP Project and contributors", combined_text)
        self.assertIn("GNU GENERAL PUBLIC LICENSE", combined_text)
        self.assertIn("Credits and Third-Party Attribution", combined_text)

    def test_source_notice_offers_corresponding_sources_for_staged_lgpl_dlls(self) -> None:
        records = package_notices.load_component_inventory()["native"]["dlls"]
        versions = {
            "glib2": "2.90.1-1",
            "gettext-runtime": "1.0-1",
            "libiconv": "1.19-1",
            "gcc-libs": "16.2.0-6",
        }
        msys_root = self.pkg_dir / "synthetic-msys64"
        toolchain_root = msys_root / "ucrt64"
        local_db = msys_root / "var" / "lib" / "pacman" / "local"
        for package, version in versions.items():
            entry = local_db / f"mingw-w64-ucrt-x86_64-{package}-{version}"
            entry.mkdir(parents=True)
            (entry / "desc").write_text(
                f"%NAME%\nmingw-w64-ucrt-x86_64-{package}\n\n%VERSION%\n{version}\n",
                encoding="utf-8",
            )
        for dll_name, record in records.items():
            if record.get("pacman_package") not in versions:
                continue
            (self.pkg_dir / dll_name).write_bytes(b"synthetic staged DLL")
            for license_entry in record["license_texts"]:
                source = ROOT / license_entry["file"]
                target = toolchain_root / license_entry["toolchain_license_rel"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.read_bytes())

        package_notices.generate_package_notices(
            self.pkg_dir, repo_root=ROOT, toolchain_root=toolchain_root
        )
        source_notice = (self.pkg_dir / "SOURCE.txt").read_text(encoding="utf-8")
        package_notice = (self.pkg_dir / "THIRD_PARTY_NOTICES.txt").read_text(encoding="utf-8")
        for _dll_name, record in records.items():
            package = record.get("pacman_package")
            if package not in versions:
                continue
            version = versions[package]
            self.assertIn(record["name"], source_notice)
            self.assertIn(f"MSYS2 package: {package} {version}", source_notice)
            self.assertIn(record["upstream_origin"], source_notice)
            self.assertIn(f"MSYS2 package: {package} {version}", package_notice)
            self.assertIn(record["upstream_origin"], package_notice)
        self.assertIn("written request", source_notice.lower())
        self.assertIn(
            "https://github.com/Jstar269/Nakagawa-Recomp/discussions/categories/help",
            source_notice,
        )

    def test_h264_notice_and_relink_section_follow_the_build_feature(self) -> None:
        for enabled in (False, True):
            with self.subTest(h264_enabled=enabled), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                repo_root = root / "repo"
                package_dir = root / "package"
                repo_root.mkdir()
                package_dir.mkdir()
                (repo_root / "Makefile").write_text(
                    "H264_SUB_SRCS := src/rt/h264_ffmpeg_sub/libavcodec/h264dec.c \\\n "
                    "\tsrc/rt/h264_ffmpeg_sub/libavutil/utils.c\n",
                    encoding="utf-8",
                )
                if enabled:
                    objects = repo_root / "build" / "synthetic"
                    objects.mkdir(parents=True)
                    (objects / "h264sub_fixture.o").write_bytes(b"synthetic object")
                h264_license = "third_party/licenses/ffmpeg-h264.txt"
                project_license = "LICENSE"
                (repo_root / h264_license).parent.mkdir(parents=True)
                (repo_root / h264_license).write_text("synthetic LGPL text", encoding="utf-8")
                (repo_root / project_license).write_text("synthetic GPL text", encoding="utf-8")
                inventory = {
                    "native": {
                        "dlls": {},
                        "system_dll_patterns": [],
                        "project_output": {
                            "name": "Nakagawa Recomp",
                            "spdx_id": "GPL-3.0-or-later",
                            "disposition": "project_output",
                            "license_texts": [{"file": project_license}],
                        },
                        "linked_components": [{
                            "id": "ffmpeg-h264",
                            "name": "FFmpeg H.264 decoder subset",
                            "spdx_id": "LGPL-2.1-or-later",
                            "upstream_origin": "https://github.com/FFmpeg/FFmpeg",
                            "upstream_revision": "n4.4 / synthetic revision",
                            "disposition": "statically_linked_subset",
                            "notice_file": "FFmpeg-H264.txt",
                            "object_glob": "build/*/h264sub_*.o",
                            "relink_doc": "RELINK.md",
                            "license_texts": [{"file": h264_license}],
                        }],
                    }
                }
                (package_dir / "synthetic.exe").write_bytes(b"synthetic executable")
                with mock.patch.object(
                    package_notices, "load_component_inventory", return_value=inventory
                ), mock.patch.dict(
                    package_notices.os.environ, {"H264_FFMPEG": "1" if enabled else "0"}
                ):
                    result = package_notices.generate_package_notices(
                        package_dir, repo_root=repo_root, toolchain_root=root / "missing-toolchain"
                    )

                names = [component["name"] for component in result["components"]]
                relink = (package_dir / "RELINK.md").read_text(encoding="utf-8")
                if enabled:
                    self.assertIn("FFmpeg H.264 decoder subset", names)
                    self.assertIn("src/rt/h264_ffmpeg_sub/libavcodec/h264dec.c", relink)
                    self.assertIn("build/*/h264sub_*.o", relink)
                    self.assertIn("FFmpeg H.264 decoder subset", (package_dir / "SOURCE.txt")
                                  .read_text(encoding="utf-8"))
                else:
                    self.assertNotIn("FFmpeg H.264 decoder subset", names)
                    self.assertNotIn("H.264", relink)

    def test_known_runtime_dlls_uses_the_selected_repository_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            repo_root = Path(temp)
            inventory_path = repo_root / package_notices.COMPONENTS_IDENTITY
            inventory_path.parent.mkdir(parents=True)
            inventory_path.write_text(json.dumps({
                "native": {"dlls": {"fixture.dll": {"name": "Fixture"}}}
            }), encoding="utf-8")

            known = package_notices.known_runtime_dlls(repo_root)

        self.assertIn("fixture.dll", known)

    def test_component_inventory_is_cached_by_resolved_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "components.json"
            path.write_text(json.dumps({
                "native": {"dlls": {"fixture.dll": {"name": "Fixture"}}}
            }), encoding="utf-8")

            first = package_notices.load_component_inventory(path)
            second = package_notices.load_component_inventory(path)

        self.assertIs(first, second)

    def test_module_import_defers_component_inventory_loading(self) -> None:
        spec = importlib.util.spec_from_file_location(
            "package_notices_import_probe", ROOT / "tools" / "package_notices.py"
        )
        self.assertIsNotNone(spec)
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        with mock.patch.object(
            package_notices.json,
            "loads",
            side_effect=AssertionError("inventory parsing is lazy"),
        ):
            spec.loader.exec_module(module)

        self.assertNotIn("KNOWN_RUNTIME_DLLS", module.__dict__)
        self.assertNotIn("SYSTEM_DLL_PATTERNS", module.__dict__)

    def test_cli_reports_package_route_error_without_traceback(self) -> None:
        package_dir = self.pkg_dir / "package"
        package_dir.mkdir()
        stderr = StringIO()
        with mock.patch.object(
            package_notices,
            "generate_package_notices",
            side_effect=PackageRouteError("PACKAGE_LICENSE_TEXT_MISSING", "synthetic missing license"),
        ), mock.patch("sys.stderr", new=stderr):
            result = package_notices.main([str(package_dir)])

        self.assertEqual(result, 2)
        self.assertEqual(stderr.getvalue().strip(),
                         "PACKAGE_LICENSE_TEXT_MISSING: synthetic missing license")

    def test_toolchain_licenses_are_the_fallback_when_in_tree_text_is_absent(self) -> None:
        """A host whose toolchain is newer than the checked-in text still resolves one."""
        inventory = package_notices.load_component_inventory()
        sdl_record = inventory["native"]["dlls"]["sdl3.dll"]
        with tempfile.TemporaryDirectory() as fake_repo_dir:
            fake_repo = Path(fake_repo_dir)
            for record in [inventory["native"]["project_output"],
                           *inventory["native"].get("linked_components", [])]:
                for license_entry in record["license_texts"]:
                    target = fake_repo / license_entry["file"]
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text("verbatim text for " + record["name"], encoding="utf-8")
            for license_entry in sdl_record["license_texts"]:
                (fake_repo / license_entry["file"]).parent.mkdir(parents=True, exist_ok=True)

            with tempfile.TemporaryDirectory() as tc_dir:
                toolchain_root = Path(tc_dir)
                for license_entry in sdl_record["license_texts"]:
                    target = toolchain_root / license_entry["toolchain_license_rel"]
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes((ROOT / license_entry["file"]).read_bytes())

                (self.pkg_dir / "SDL3.dll").write_bytes(b"MZ\0\0sdl")
                result = package_notices.generate_package_notices(
                    self.pkg_dir, repo_root=fake_repo, toolchain_root=toolchain_root
                )
            sdl = next(c for c in result["components"] if c.get("binary") == "SDL3.dll")
            # A toolchain-only source must still resolve, and the recorded source
            # path must not leak a host absolute path into the shipped notice.
            self.assertNotIn(":", sdl["source_path"])
            self.assertNotIn("\\", sdl["source_path"])
            combined = (self.pkg_dir / "THIRD_PARTY_NOTICES.txt").read_text(encoding="utf-8")
            self.assertIn(
                (ROOT / sdl_record["license_texts"][0]["file"])
                .read_text(encoding="utf-8").strip(),
                combined,
            )

    def test_toolchain_license_drift_is_named_when_in_tree_text_exists(self) -> None:
        record = package_notices.load_component_inventory()["native"]["dlls"]["sdl3.dll"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo_root = root / "repo"
            toolchain_root = root / "ucrt64"
            in_tree = repo_root / record["license_texts"][0]["file"]
            in_tree.parent.mkdir(parents=True)
            in_tree.write_bytes((ROOT / record["license_texts"][0]["file"]).read_bytes())
            toolchain_copy = toolchain_root / record["license_texts"][0]["toolchain_license_rel"]
            toolchain_copy.parent.mkdir(parents=True)
            toolchain_copy.write_text("synthetic changed toolchain license", encoding="utf-8")

            with self.assertRaises(PackageRouteError) as ctx:
                package_notices.component_license_sources(record, toolchain_root, repo_root)

        self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_TEXT_MISSING")
        self.assertIn("LICENSE_TEXT_DRIFT", str(ctx.exception))

    def test_binary_import_scan_refuses_unparsable_pe_but_accepts_no_import_pe(self) -> None:
        invalid = {
            "non-PE bytes": b"synthetic non-PE executable",
            "bad PE signature": b"MZ" + bytes(64),
        }
        unmapped_import = bytearray(88 + 224)
        unmapped_import[:2] = b"MZ"
        struct.pack_into("<I", unmapped_import, 0x3C, 64)
        unmapped_import[64:68] = b"PE\0\0"
        struct.pack_into("<H", unmapped_import, 64 + 20, 224)
        struct.pack_into("<H", unmapped_import, 64 + 24, 0x10B)
        struct.pack_into("<I", unmapped_import, 64 + 24 + 92, 16)
        struct.pack_into("<I", unmapped_import, 64 + 24 + 96 + 8, 0x1234)
        struct.pack_into("<I", unmapped_import, 64 + 24 + 96 + 12, 20)
        invalid["unmapped import table"] = bytes(unmapped_import)

        valid_no_imports = bytearray(88 + 224)
        valid_no_imports[:2] = b"MZ"
        struct.pack_into("<I", valid_no_imports, 0x3C, 64)
        valid_no_imports[64:68] = b"PE\0\0"
        struct.pack_into("<H", valid_no_imports, 64 + 20, 224)
        struct.pack_into("<H", valid_no_imports, 64 + 24, 0x10B)
        struct.pack_into("<I", valid_no_imports, 64 + 24 + 92, 16)

        with tempfile.TemporaryDirectory() as temp, mock.patch.object(
            package_notices.shutil, "which", return_value=None
        ):
            for label, data in invalid.items():
                with self.subTest(binary=label):
                    path = Path(temp) / f"{label.replace(' ', '-')}.dll"
                    path.write_bytes(data)
                    with self.assertRaises(PackageRouteError) as ctx:
                        package_notices.get_binary_imports(path)
                    self.assertEqual(ctx.exception.code, "PACKAGE_BUILD_FAILED")
                    self.assertIn("cannot parse PE imports", str(ctx.exception))

            valid_path = Path(temp) / "valid-empty-import-table.dll"
            valid_path.write_bytes(valid_no_imports)
            self.assertEqual(package_notices.get_binary_imports(valid_path), [])

    def test_notice_generation_does_not_stage_runtime_dlls(self) -> None:
        package_dir = self.pkg_dir / "package"
        toolchain_root = self.pkg_dir / "toolchain"
        package_dir.mkdir()
        (package_dir / "synthetic.exe").write_bytes(b"synthetic executable")
        bin_dir = toolchain_root / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "SDL3.dll").write_bytes(b"synthetic runtime DLL")

        with mock.patch.object(
            package_notices, "get_binary_imports", return_value=["SDL3.dll"]
        ) as imports:
            package_notices.generate_package_notices(
                package_dir, repo_root=ROOT, toolchain_root=toolchain_root
            )

        self.assertFalse((package_dir / "SDL3.dll").exists())
        imports.assert_not_called()

    def test_notice_regeneration_removes_stale_component_notices(self) -> None:
        package_dir = self.pkg_dir / "package"
        package_dir.mkdir()
        (package_dir / "SDL3.dll").write_bytes(b"synthetic runtime DLL")
        notices_dir = package_dir / "THIRD_PARTY_NOTICES"
        notices_dir.mkdir()
        stale = notices_dir / "retired-component.txt"
        stale.write_text("stale synthetic notice", encoding="utf-8")

        package_notices.generate_package_notices(
            package_dir, repo_root=ROOT, toolchain_root=self.pkg_dir / "missing-toolchain"
        )

        self.assertFalse(stale.exists())

    def test_failed_notice_regeneration_preserves_previous_bundle(self) -> None:
        package_dir = self.pkg_dir / "package"
        package_dir.mkdir()
        custom_dll = package_dir / "custom_math.dll"
        custom_dll.write_bytes(b"synthetic custom DLL")
        empty_license = self.pkg_dir / "empty-license.txt"
        empty_license.write_text("", encoding="utf-8")
        notices_dir = package_dir / "THIRD_PARTY_NOTICES"
        notices_dir.mkdir()
        (notices_dir / "previous.txt").write_text("previous bundle", encoding="utf-8")
        previous_root_files = {
            "SOURCE.txt": "previous source",
            "RELINK.md": "previous relink",
            "THIRD_PARTY_NOTICES.txt": "previous combined notices",
        }
        for name, contents in previous_root_files.items():
            (package_dir / name).write_text(contents, encoding="utf-8")
        custom_map = {
            "custom_math.dll": {
                "name": "Custom Math",
                "spdx_id": "Apache-2.0",
                "license_path": str(empty_license),
            }
        }

        with self.assertRaises(PackageRouteError):
            package_notices.generate_package_notices(
                package_dir,
                repo_root=ROOT,
                toolchain_root=self.pkg_dir / "missing-toolchain",
                custom_license_map=custom_map,
            )

        self.assertEqual(sorted(path.name for path in notices_dir.iterdir()), ["previous.txt"])
        self.assertEqual((notices_dir / "previous.txt").read_text(encoding="utf-8"), "previous bundle")
        for name, contents in previous_root_files.items():
            self.assertEqual((package_dir / name).read_text(encoding="utf-8"), contents)

    def test_custom_license_map_resolves_custom_bundled_dll(self) -> None:
        """Custom license map allows explicit declaration of third-party DLLs."""
        custom_lic = self.pkg_dir / "custom_lic.txt"
        custom_lic.write_text("Custom Apache-2.0 License Text", encoding="utf-8")

        (self.pkg_dir / "custom_math.dll").write_bytes(b"MZ\0\0math")
        custom_map = {
            "custom_math.dll": {
                "name": "Custom Math Library",
                "spdx_id": "Apache-2.0",
                "license_path": str(custom_lic),
                "version": "1.0.0",
            }
        }

        result = package_notices.generate_package_notices(
            self.pkg_dir,
            repo_root=ROOT,
            custom_license_map=custom_map,
        )
        comp_names = [c["name"] for c in result["components"]]
        self.assertIn("Custom Math Library", comp_names)
        self.assertTrue((self.pkg_dir / "THIRD_PARTY_NOTICES" / "Custom_Math_Library.txt").is_file())

    def test_source_notice_names_repository_commit_and_local_changes(self) -> None:
        """SOURCE.txt is the relink mechanism: repository, commit, tag, changes."""
        text = package_notices.render_source_notice({
            "repository": package_notices.PROJECT_REPOSITORY_URL,
            "commit": "0123456789abcdef0123456789abcdef01234567",
            "tag": "v0.0.1",
            "local_changes": True,
        })
        self.assertIn("Repository: https://github.com/Jstar269/nakagawa-recomp", text)
        self.assertIn("Commit:     0123456789abcdef0123456789abcdef01234567", text)
        self.assertIn("Tag:        v0.0.1", text)
        self.assertIn("working tree with local changes", text)
        self.assertIn("GNU Lesser General Public", text)
        clean = package_notices.render_source_notice({
            "repository": package_notices.PROJECT_REPOSITORY_URL,
            "commit": None, "tag": None, "local_changes": None,
        })
        self.assertIn("unknown (built without git metadata)", clean)
        self.assertNotIn("Tag:", clean)
        self.assertNotIn("local changes", clean)

    def test_source_reference_reads_this_checkout(self) -> None:
        reference = package_notices.source_reference(ROOT)
        self.assertEqual(reference["repository"], package_notices.PROJECT_REPOSITORY_URL)
        if reference["commit"] is not None:
            self.assertRegex(reference["commit"], r"^[0-9a-f]{40}$")

    def test_system_dll_detection(self) -> None:
        """Standard Windows system DLLs are recognized; recorded loaders are not system DLLs."""
        self.assertTrue(package_notices.is_system_dll("kernel32.dll"))
        self.assertTrue(package_notices.is_system_dll("KERNEL32.DLL"))
        self.assertTrue(package_notices.is_system_dll("user32.dll"))
        self.assertTrue(package_notices.is_system_dll("api-ms-win-crt-runtime-l1-1-0.dll"))
        self.assertTrue(package_notices.is_system_dll("ext-ms-win-gdi-draw-l1-1-0.dll"))
        self.assertTrue(package_notices.is_system_dll("mfplat.dll"))
        # The player imports both (update check, #324 runtime manifest closure).
        self.assertTrue(package_notices.is_system_dll("bcrypt.dll"))
        self.assertTrue(package_notices.is_system_dll("WINHTTP.dll"))

        self.assertFalse(package_notices.is_system_dll("SDL3.dll"))
        self.assertFalse(package_notices.is_system_dll("libwinpthread-1.dll"))
        self.assertFalse(package_notices.is_system_dll("libgcc_s_seh-1.dll"))
        self.assertFalse(package_notices.is_system_dll("custom.dll"))
        # The Vulkan loader is a recorded host-resolved component, so a loader a
        # user places beside the package receives a named notice rather than
        # being waved through as an operating-system library.
        self.assertFalse(package_notices.is_system_dll("vulkan-1.dll"))
        self.assertIn("vulkan-1.dll", package_notices.KNOWN_RUNTIME_DLLS)

    def test_relink_objects_follow_makefile_sources(self) -> None:
        """RELINK.md lists exactly the ATRAC3+ objects the Makefile builds, plus the bridge."""
        objects = package_notices.atrac3p_object_names(ROOT)
        makefile = read_makefile(ROOT)
        self.assertGreater(len(objects), 1)
        for obj in objects[:-1]:
            source = "src/rt/atrac3p/" + obj[len("$(BUILD_DIR)/atrac3p_"):-2] + ".c"
            self.assertIn(source, makefile)
        self.assertEqual(objects[-1], "$(BUILD_DIR)/atrac3p_bridge.o")

    def test_installed_package_version_reads_pacman_database(self) -> None:
        """Component versions come from the toolchain's package database, never a literal."""
        with tempfile.TemporaryDirectory() as msys_dir:
            toolchain_root = Path(msys_dir) / "ucrt64"
            toolchain_root.mkdir()
            local_db = Path(msys_dir) / "var" / "lib" / "pacman" / "local"
            entry = local_db / "mingw-w64-ucrt-x86_64-sdl3-9.8.7-1"
            entry.mkdir(parents=True)
            (entry / "desc").write_text(
                "%NAME%\nmingw-w64-ucrt-x86_64-sdl3\n\n%VERSION%\n9.8.7-1\n", encoding="utf-8"
            )
            self.assertEqual(package_notices.installed_package_version(toolchain_root, "sdl3"), "9.8.7-1")
            self.assertEqual(package_notices.installed_package_version(toolchain_root, "sdl3_ttf"), "unknown")
            self.assertEqual(package_notices.installed_package_version(None, "sdl3"), "unknown")

    def test_gcc_runtime_ships_gplv3_with_exception(self) -> None:
        """GCC runtime DLLs carry both the GPLv3 text and the runtime library exception."""
        (self.pkg_dir / "libgcc_s_seh-1.dll").write_bytes(b"MZ\0\0gcc")
        with tempfile.TemporaryDirectory() as msys_dir:
            msys_root = Path(msys_dir)
            toolchain_root = msys_root / "ucrt64"
            package_version = "16.2.0-6"
            entry = (
                msys_root / "var" / "lib" / "pacman" / "local"
                / f"mingw-w64-ucrt-x86_64-gcc-libs-{package_version}"
            )
            entry.mkdir(parents=True)
            (entry / "desc").write_text(
                f"%NAME%\nmingw-w64-ucrt-x86_64-gcc-libs\n\n%VERSION%\n{package_version}\n",
                encoding="utf-8",
            )
            result = package_notices.generate_package_notices(
                self.pkg_dir, repo_root=ROOT, toolchain_root=toolchain_root
            )
        gcc = next(c for c in result["components"] if c.get("binary") == "libgcc_s_seh-1.dll")
        self.assertEqual(gcc["spdx_id"], "GPL-3.0-or-later WITH GCC-exception-3.1")
        self.assertEqual(len(gcc["additional_license_files"]), 1)
        notices_dir = self.pkg_dir / "THIRD_PARTY_NOTICES"
        combined = (self.pkg_dir / "THIRD_PARTY_NOTICES.txt").read_text(encoding="utf-8")
        self.assertIn("GCC RUNTIME LIBRARY EXCEPTION", combined)
        self.assertIn("GNU GENERAL PUBLIC LICENSE", combined)
        # Both texts came from the checked-in third_party/licenses/ copies.
        for name in (gcc["license_file"], gcc["additional_license_files"][0]):
            self.assertTrue((notices_dir / name).is_file())
        self.assertEqual(gcc["source_path"], "third_party/licenses/gcc-libs/COPYING3.txt")
        self.assertEqual(
            gcc["additional_license_files"][0], "gcc-libs-libgcc-COPYING.RUNTIME.txt"
        )

    def test_toolchain_root_requires_license_directory(self) -> None:
        """A gcc without packaged license texts is not accepted as the toolchain root."""
        with tempfile.TemporaryDirectory() as tc_dir:
            bare = Path(tc_dir) / "bare"
            (bare / "bin").mkdir(parents=True)
            (bare / "bin" / "gcc.exe").write_bytes(b"MZ")
            with mock.patch.dict(package_notices.os.environ, {"MINGW_PREFIX": str(bare)}), \
                    mock.patch.object(package_notices.shutil, "which", return_value=None):
                root = package_notices.resolve_toolchain_root()
                self.assertNotEqual(root, bare.resolve())
            (bare / "share" / "licenses").mkdir(parents=True)
            with mock.patch.dict(package_notices.os.environ, {"MINGW_PREFIX": str(bare)}):
                self.assertEqual(package_notices.resolve_toolchain_root(), bare.resolve())

    def test_toolchain_root_uses_explicit_msys_path(self) -> None:
        with tempfile.TemporaryDirectory() as tc_dir:
            root = Path(tc_dir) / "custom-ucrt64"
            msys_bin = root / "bin"
            msys_bin.mkdir(parents=True)
            (msys_bin / "gcc.exe").write_bytes(b"synthetic executable path marker")
            (root / "share" / "licenses").mkdir(parents=True)
            with mock.patch.dict(
                    package_notices.os.environ, {"MSYS_PATH": str(msys_bin)}, clear=True), \
                    mock.patch.object(package_notices.shutil, "which", return_value=None), \
                    mock.patch.object(package_notices, "_prerequisite_toolchain_root", return_value=None):
                self.assertEqual(package_notices.resolve_toolchain_root(), root.resolve())




class IsolatedInterpreterTests(unittest.TestCase):
    """tools/copy_build_assets.ps1 runs this script with the release's bundled embeddable Python, whose ._pth
    file keeps the script's folder off sys.path; `-I` gives a normal interpreter the same import path."""

    def test_script_imports_nk_core_without_the_script_folder_on_sys_path(self) -> None:
        import subprocess

        result = subprocess.run(
            [sys.executable, "-I", str(ROOT / "tools" / "package_notices.py"), "--help"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("usage", result.stdout.lower())

if __name__ == "__main__":
    unittest.main()
