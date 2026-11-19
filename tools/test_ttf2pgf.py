#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Determinism, refusal, and reader round-trip coverage for tools/ttf2pgf.py (issue #313).

Layer 1 (behavioural): convert the pinned public fixture font
``tests/fixtures/fonts/gudea/Gudea-Regular.ttf`` and open the result through the
public C reader ``src/rt/pgf_public.c`` with the harness
``tests/native/test_pgf_writer_roundtrip.c``.  Every guest record and every
drawn sample must equal the converter's own raster, the source-owned ``H``
shape must render as two stems plus a crossbar, the space must stay a
zero-area glyph, and a composite-glyph range must round-trip as well.

Layer 2 (reproducibility): the same pinned input, parameters, and converter
revision must produce byte-identical PGF and manifest output across repeated
runs, and the manifest must carry the digests, coverage, metrics, converter
identity, and the pin's complete licence material.

Layer 3 (refusals): malformed sfnt structures, unsupported outline and font
forms, out-of-range parameters, and policy-violating output names are refused
by name, and the CLI writes nothing on any refusal.

The fixture bytes are pinned by ``tests/fixtures/fonts/gudea/PIN.json``; no test
downloads anything, and no firmware, retail, or third-party PGF bytes are
read anywhere.
"""

from __future__ import annotations

import dataclasses
import functools
import hashlib
import json
import math
import os
import random
import re
import shutil
import string
import struct
import subprocess
import sys
import tempfile
import unicodedata
import unittest
from fractions import Fraction
from unittest import mock
from pathlib import Path
from typing import ClassVar, cast

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import ttf2pgf  # noqa: E402
from makefile_text import read_makefile  # noqa: E402
from nk_core.fonts import validate_pgf_data  # noqa: E402
from pgf_writer import Glyph  # noqa: E402
from ttf2pgf import DEFAULT_FONT_NAME, TtfConvertError, convert, load_pin, manifest_bytes  # noqa: E402

RT = ROOT / "src" / "rt"
HELPER_C = ROOT / "tests" / "native" / "test_pgf_writer_roundtrip.c"
READER_C = RT / "pgf_public.c"
TOOL = ROOT / "tools" / "ttf2pgf.py"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

FONT_DIR = ROOT / "tests" / "fixtures" / "fonts" / "gudea"
FONT = FONT_DIR / "Gudea-Regular.ttf"
PIN = FONT_DIR / "PIN.json"
LICENSE_TEXT = FONT_DIR / "OFL.txt"
JAPANESE_FONT_DIR = ROOT / "tests" / "fixtures" / "fonts" / "mplus1p"
JAPANESE_FONT = JAPANESE_FONT_DIR / "MPLUS1p-Regular.ttf"
JAPANESE_PIN = JAPANESE_FONT_DIR / "PIN.json"
KOREAN_FONT_DIR = ROOT / "tests" / "fixtures" / "fonts" / "gowundodum"
KOREAN_FONT = KOREAN_FONT_DIR / "GowunDodum-Regular.ttf"
KOREAN_PIN = KOREAN_FONT_DIR / "PIN.json"
LIBERATION_DIR = ROOT / "tests" / "fixtures" / "fonts" / "liberation"
NOTO_JP_FONT = ROOT / "assets" / "fonts" / "ui" / "noto-sans-jp" / "NotoSansJP-Regular.ttf"
NOTO_JP_PIN = ROOT / "tests" / "fixtures" / "fonts" / "notosansjp" / "PIN.json"
#: The console's Latin index order within a size group (#2005): family, then style.
LATIN_FACES = (
    ("LiberationSans-Regular", "Regular"), ("LiberationSerif-Regular", "Regular"),
    ("LiberationSans-Italic", "Italic"), ("LiberationSerif-Italic", "Italic"),
    ("LiberationSans-Bold", "Bold"), ("LiberationSerif-Bold", "Bold"),
    ("LiberationSans-BoldItalic", "Bold Italic"), ("LiberationSerif-BoldItalic", "Bold Italic"),
)

DEFAULT_CODES = (0x20, 0x7E)
PPEM_SHAPE = 16  # large enough that the source-owned H shape is unambiguous


def _directory(data: bytes) -> dict[str, int]:
    """Map table tag -> byte offset of its directory entry (for crafting mutations)."""
    count = struct.unpack_from(">H", data, 4)[0]
    return {
        data[12 + 16 * index : 16 + 16 * index].decode("ascii"): 12 + 16 * index
        for index in range(count)
    }


def _patch(data: bytes, offset: int, blob: bytes) -> bytes:
    mutated = bytearray(data)
    mutated[offset : offset + len(blob)] = blob
    return bytes(mutated)


def _cli(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TOOL), *[str(arg) for arg in args]],
        capture_output=True,
        text=True,
    )


def _glyph_map(glyphs: tuple[Glyph, ...]) -> dict[int, Glyph]:
    return {glyph.code: glyph for glyph in glyphs}


def _untrimmed(left: int, top: int, width: int, height: int, samples: tuple[int, ...]):
    """Stands in for ``ttf2pgf._trim_to_ink`` to convert as before rule 15: the raster box unchanged."""
    return left, top, width, height, samples


def _without_vertical(glyphs: list, **_: object) -> list:
    """Stands in for ``ttf2pgf._vertical_metrics`` to convert as before rule 16: a vertical advance
    of 0 and the writer's default vertical bearings."""
    return [dataclasses.replace(glyph, advance_y=0) for glyph in glyphs]


def _clusters(samples: list[int], row: int, width: int) -> list[int]:
    """Lengths of maximal non-zero runs in one raster row."""
    runs: list[int] = []
    current = 0
    for value in samples[row * width : (row + 1) * width]:
        if value:
            current += 1
        elif current:
            runs.append(current)
            current = 0
    if current:
        runs.append(current)
    return runs


class CliRefusalMixin(unittest.TestCase):
    """One assertion per named refusal: exit 2, stable reason, no output file."""

    def assert_cli_refused(self, reason: str, output: Path, *args: object) -> None:
        result = _cli(output, *args)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(f"refused: {reason}:", result.stderr)
        self.assertFalse(output.exists(), "a refusal must write no output")


class ConversionDeterminismTests(unittest.TestCase):
    """Layer 2: byte-identical output and a complete conversion manifest."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.pin = load_pin(PIN, self.data)

    def _convert(self, **overrides: object):
        arguments: dict = {
            "input_path": str(FONT),
            "output_path": "gudea_latin.pgf",
            "codes": DEFAULT_CODES,
            "pin": self.pin,
        }
        arguments.update(overrides)
        return convert(self.data, **arguments)

    def test_same_pinned_input_produces_identical_bytes_and_manifest(self) -> None:
        first = self._convert()
        second = self._convert()
        self.assertEqual(first.image, second.image)
        self.assertEqual(manifest_bytes(first.manifest), manifest_bytes(second.manifest))
        self.assertEqual(first.glyphs, second.glyphs)

    def test_repeated_cli_runs_are_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ttf2pgf_det_") as tmp:
            image = Path(tmp) / "out.pgf"
            manifest = Path(tmp) / "out.json"
            args = (image, FONT, "--pin", PIN, "--manifest", manifest, "--ppem", PPEM_SHAPE)
            first = _cli(*args)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            image_bytes, manifest_bytes_first = image.read_bytes(), manifest.read_bytes()
            second = _cli(*args)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertEqual(image.read_bytes(), image_bytes)
            self.assertEqual(manifest.read_bytes(), manifest_bytes_first)

    def test_cli_manifest_is_byte_identical_across_output_directories(self) -> None:
        """The same build run from two host directories records the same manifest (plan 6.2)."""
        manifests = []
        with tempfile.TemporaryDirectory(prefix="ttf2pgf_host_a_") as first_dir, \
                tempfile.TemporaryDirectory(prefix="ttf2pgf_host_b_") as second_dir:
            for directory in (first_dir, second_dir):
                image = Path(directory) / "gudea.pgf"
                manifest = Path(directory) / "gudea.json"
                result = _cli(image, FONT, "--pin", PIN, "--manifest", manifest, "--ppem", PPEM_SHAPE)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                manifests.append(manifest.read_bytes())
        self.assertEqual(manifests[0], manifests[1], "the manifest must not embed the output directory")

    def test_manifest_paths_are_basenames_under_either_host_separator(self) -> None:
        posix = self._convert(
            input_path="/srv/build/ofl/Gudea-Regular.ttf", output_path="/srv/build/out/gudea.pgf",
        )
        windows = self._convert(
            input_path="C:\\Work\\ofl\\Gudea-Regular.ttf", output_path="D:\\out\\gudea.pgf",
        )
        self.assertEqual(manifest_bytes(posix.manifest), manifest_bytes(windows.manifest))
        self.assertEqual(posix.manifest["input"]["path"], "Gudea-Regular.ttf")
        self.assertEqual(posix.manifest["output"]["path"], "gudea.pgf")

    def test_manifest_carries_digests_coverage_and_full_licence_material(self) -> None:
        conversion = self._convert()
        manifest = conversion.manifest
        self.assertEqual(
            manifest_bytes(manifest),
            manifest_bytes(json.loads(manifest_bytes(manifest))),
            "manifest JSON must round-trip byte-identically",
        )

        self.assertEqual(manifest["converter"]["name"], "ttf2pgf")
        self.assertEqual(manifest["converter"]["spec"], ttf2pgf.SPEC_PATH)
        self.assertEqual(manifest["converter"]["version"], ttf2pgf.CONVERTER_VERSION)

        self.assertEqual(manifest["input"]["sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertEqual(manifest["input"]["sha256"], self.pin.sha256)
        self.assertEqual(manifest["input"]["size_bytes"], len(self.data))
        self.assertEqual(manifest["input"]["outline_format"], "glyf")
        self.assertEqual(
            sorted(manifest["input"]["tables_used"]), sorted(ttf2pgf.REQUIRED_TABLES)
        )

        self.assertEqual(manifest["output"]["sha256"], hashlib.sha256(conversion.image).hexdigest())
        self.assertEqual(manifest["output"]["size_bytes"], len(conversion.image))
        self.assertEqual(manifest["output"]["revision"], 2)
        self.assertEqual(manifest["output"]["font_name"], DEFAULT_FONT_NAME)

        coverage = manifest["coverage"]
        self.assertEqual((coverage["first"], coverage["last"]), DEFAULT_CODES)
        self.assertEqual(coverage["count"], DEFAULT_CODES[1] - DEFAULT_CODES[0] + 1)
        self.assertEqual(coverage["range"], "U+0020..U+007E")
        self.assertEqual(manifest["glyphs"]["count"], coverage["count"])
        self.assertEqual(manifest["glyphs"]["zero_area_codes"], [0x20])

        license_block = manifest["license"]
        self.assertEqual(license_block["spdx"], "OFL-1.1")
        self.assertEqual(license_block["reserved_font_names"], ["Gudea"])
        self.assertEqual(license_block["license_text"], LICENSE_TEXT.read_text(encoding="utf-8"))
        self.assertEqual(
            license_block["license_text_sha256"],
            hashlib.sha256(LICENSE_TEXT.read_bytes()).hexdigest(),
        )
        self.assertTrue(license_block["copyright"].startswith("Copyright (c)"))
        self.assertIn("google/fonts", license_block["source"]["repository"])
        self.assertEqual(license_block["source"]["path"], "ofl/gudea/Gudea-Regular.ttf")

        forbidden = {"timestamp", "generated_at", "created_at", "now"}

        def walk(value: object) -> None:
            if isinstance(value, dict):
                for key, child in value.items():
                    self.assertNotIn(str(key).lower(), forbidden, "manifest must not carry run-time fields")
                    walk(child)
            elif isinstance(value, list):
                for child in value:
                    walk(child)

        walk(manifest)

    def test_manifest_header_fields_match_the_emitted_bytes(self) -> None:
        conversion = self._convert(ppem=PPEM_SHAPE)
        image = conversion.image
        manifest = manifest_bytes(conversion.manifest)
        parsed = json.loads(manifest)
        self.assertEqual(
            struct.unpack_from("<i", image, 0x08)[0], parsed["output"]["revision"]
        )
        self.assertEqual(struct.unpack_from("<H", image, 0xB6)[0], parsed["output"]["first_glyph"])
        self.assertEqual(struct.unpack_from("<H", image, 0xB8)[0], parsed["output"]["last_glyph"])
        for offset, key in (
            (0xD4, "ascender_26_6"),
            (0xD8, "descender_26_6"),
            (0xF4, "max_width_26_6"),
            (0xF8, "max_height_26_6"),
            (0xEC, "max_advance_26_6"),
        ):
            self.assertEqual(struct.unpack_from("<i", image, offset)[0], parsed["output"][key], key)
        name = DEFAULT_FONT_NAME.encode("ascii")
        self.assertEqual(image[0x35 : 0x35 + len(name)], name)
        self.assertEqual(parsed["parameters"]["supersample"], ttf2pgf.SUPERSAMPLE)
        self.assertEqual(parsed["parameters"]["ppem"], conversion.ppem)

    def test_independent_structural_validator_accepts_the_output(self) -> None:
        conversion = self._convert()
        report = validate_pgf_data(conversion.image)
        self.assertIsInstance(report, dict)


class SourcePinTests(unittest.TestCase):
    """The pin binds the exact input bytes and licence text."""

    def test_pin_binds_the_exact_fixture_bytes(self) -> None:
        pin = load_pin(PIN, FONT.read_bytes())
        self.assertEqual(pin.sha256, hashlib.sha256(FONT.read_bytes()).hexdigest())
        self.assertEqual(pin.spdx, "OFL-1.1")
        self.assertEqual(pin.reserved_font_names, ("Gudea",))

    def test_a_single_flipped_byte_fails_the_pin(self) -> None:
        data = bytearray(FONT.read_bytes())
        data[5000] ^= 0xFF
        with self.assertRaises(TtfConvertError) as caught:
            load_pin(PIN, bytes(data))
        self.assertEqual(caught.exception.reason, "pin-digest-mismatch")

    def test_a_corrupt_pin_is_refused_by_name(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ttf2pgf_pin_") as tmp:
            bad = Path(tmp) / "PIN.json"
            bad.write_text('{"schema": "something-else"}', encoding="utf-8")
            with self.assertRaises(TtfConvertError) as caught:
                load_pin(bad, FONT.read_bytes())
            self.assertEqual(caught.exception.reason, "pin-invalid")


class RefusalTests(CliRefusalMixin):
    """Layer 3: every malformed, unsupported, or policy-violating input is named."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.entries = _directory(self.data)
        self._tmp = tempfile.TemporaryDirectory(prefix="ttf2pgf_refuse_")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.output = self.tmp / "out.pgf"

    def _write(self, name: str, data: bytes) -> Path:
        path = self.tmp / name
        path.write_bytes(data)
        return path

    def test_otto_scaler_is_refused_as_postscript_outlines(self) -> None:
        path = self._write("otto.ttf", _patch(self.data, 0, b"OTTO"))
        self.assert_cli_refused("unsupported-outline-format", self.output, path)

    def test_collection_scaler_is_refused_by_version(self) -> None:
        path = self._write("ttcf.ttf", _patch(self.data, 0, b"ttcf"))
        self.assert_cli_refused("unsupported-sfnt-version", self.output, path)

    def test_missing_required_table_is_named(self) -> None:
        offset = self.entries["glyf"]
        path = self._write("noglyf.ttf", _patch(self.data, offset, b"XXXX"))
        self.assert_cli_refused("missing-table", self.output, path)

    def test_injected_variable_font_table_is_refused(self) -> None:
        offset = self.entries["GDEF"]
        path = self._write("fvar.ttf", _patch(self.data, offset, b"fvar"))
        self.assert_cli_refused("unsupported-variable-font", self.output, path)

    def test_injected_cff_table_is_refused(self) -> None:
        offset = self.entries["OS/2"]
        path = self._write("cff.ttf", _patch(self.data, offset, b"CFF "))
        self.assert_cli_refused("unsupported-outline-format", self.output, path)

    def test_table_extending_past_end_of_file_is_refused(self) -> None:
        offset = self.entries["name"] + 12
        path = self._write("trunc.ttf", _patch(self.data, offset, (0xFFFFFF).to_bytes(4, "big")))
        self.assert_cli_refused("truncated-table", self.output, path)

    def test_duplicate_directory_tag_is_refused(self) -> None:
        offset = self.entries["post"]
        path = self._write("dup.ttf", _patch(self.data, offset, b"name"))
        self.assert_cli_refused("malformed-font", self.output, path)

    def test_non_monotonic_loca_is_refused(self) -> None:
        entry = self.entries["loca"]
        table_offset = struct.unpack_from(">I", self.data, entry + 8)[0]
        path = self._write("loca.ttf", _patch(self.data, table_offset, b"\x00\x05\x00\x03"))
        self.assert_cli_refused("malformed-loca", self.output, path)

    def test_unmapped_code_point_is_refused(self) -> None:
        self.assert_cli_refused(
            "missing-code-point", self.output, FONT, "--codes", "0x00-0x20"
        )

    def test_inverted_code_range_is_refused(self) -> None:
        self.assert_cli_refused(
            "bad-code-range", self.output, FONT, "--codes", "0x50-0x40"
        )

    def test_out_of_range_ppem_is_refused(self) -> None:
        self.assert_cli_refused("ppem-out-of-range", self.output, FONT, "--ppem", "0")

    def test_oversized_glyph_is_refused_by_name(self) -> None:
        # A tiny unitsPerEm makes every outline huge at a legal ppem, which
        # reaches the writer's 7-bit record limit instead of any clamp.
        head_entry = self.entries["head"]
        table_offset = struct.unpack_from(">I", self.data, head_entry + 8)[0]
        mutated = _patch(self.data, table_offset + 18, struct.pack(">H", 16))
        with self.assertRaises(TtfConvertError) as caught:
            convert(
                mutated,
                input_path="crafted.ttf",
                output_path="out.pgf",
                codes=DEFAULT_CODES,
                ppem=48,
            )
        self.assertEqual(caught.exception.reason, "glyph-too-large")

    def test_output_name_using_a_pin_reserved_name_is_refused(self) -> None:
        result = _cli(self.output, FONT, "--pin", PIN, "--font-name", "Gudea Display")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: reserved-font-name:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_camouflage_font_name_is_refused(self) -> None:
        result = _cli(self.output, FONT, "--font-name", _joined("F", "TT-", "New", "Rodin Pro Latin"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: camouflage-font-name:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_adobe_reserved_font_name_source_is_refused(self) -> None:
        result = _cli(self.output, FONT, "--font-name", "Source Compatible")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: reserved-font-name:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_codepoints_and_codes_are_mutually_exclusive(self) -> None:
        points = self._write("one.txt", b"0x41\n")
        result = _cli(self.output, FONT, "--codes", "0x41", "--codepoints", points)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not allowed with", result.stderr)
        self.assertFalse(self.output.exists())

    def test_unsorted_or_repeated_codepoints_are_refused(self) -> None:
        for name, text in (("unsorted.txt", "0x48\n0x41\n"), ("repeated.txt", "0x41\n0x41\n")):
            with self.subTest(name=name):
                path = self._write(name, text.encode("ascii"))
                self.assert_cli_refused("codepoints-unsorted", self.output, FONT, "--codepoints", path)

    def test_malformed_or_empty_codepoint_list_is_refused(self) -> None:
        path = self._write("garbage.txt", b"0x41\nnot-a-code\n")
        self.assert_cli_refused("bad-codepoint-list", self.output, FONT, "--codepoints", path)
        path = self._write("empty.txt", b"# no code points\n\n")
        self.assert_cli_refused("empty-codepoint-set", self.output, FONT, "--codepoints", path)

    def test_listed_code_point_without_a_glyph_is_refused(self) -> None:
        path = self._write("unmapped.txt", b"0x01\n0x41\n")
        self.assert_cli_refused("missing-code-point", self.output, FONT, "--codepoints", path)

    def test_vendor_marker_in_the_font_name_is_refused_before_writing(self) -> None:
        result = _cli(self.output, FONT, "--font-name", "No" "to Sans")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: reserved-font-name:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_firmware_font_name_is_refused_by_the_writer(self) -> None:
        result = _cli(self.output, FONT, "--font-name", "ltn0")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: font-field-invalid:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_pin_mismatch_writes_nothing(self) -> None:
        data = bytearray(self.data)
        data[5000] ^= 0xFF
        path = self._write("flipped.ttf", bytes(data))
        self.assert_cli_refused("pin-digest-mismatch", self.output, path, "--pin", PIN)

    def test_manifest_without_pin_is_a_usage_error(self) -> None:
        result = _cli(self.output, FONT, "--manifest", self.tmp / "out.json")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--manifest requires --pin", result.stderr)
        self.assertFalse(self.output.exists())

    def test_unreachable_metric_target_is_refused(self) -> None:
        targets = self.tmp / "targets.json"
        targets.write_text(
            json.dumps(
                {"ascender_px": 9, "descender_px": -9, "tolerance_px": 0.25, "ppem_min": 4, "ppem_max": 24}
            ),
            encoding="utf-8",
        )
        self.assert_cli_refused(
            "metric-target-unreachable", self.output, FONT, "--metric-targets", targets
        )

    def test_invalid_metric_target_document_is_refused(self) -> None:
        targets = self.tmp / "targets.json"
        targets.write_text('{"ascender_px": 9, "descender_px": -2, "tolerance_px": 0}', encoding="utf-8")
        self.assert_cli_refused(
            "metric-targets-invalid", self.output, FONT, "--metric-targets", targets
        )

    def test_explicit_ppem_and_metric_targets_conflict(self) -> None:
        targets = self.tmp / "targets.json"
        targets.write_text('{"ascender_px": 9, "descender_px": -2}', encoding="utf-8")
        with self.assertRaises(TtfConvertError) as caught:
            convert(
                self.data,
                input_path=str(FONT),
                output_path="out.pgf",
                codes=DEFAULT_CODES,
                ppem=12,
                metric_targets=ttf2pgf.load_metric_targets(targets),
            )
        self.assertEqual(caught.exception.reason, "conflicting-parameters")

    def test_mac_roman_and_symbol_cmaps_are_not_accepted_as_unicode(self) -> None:
        cmap_entry = self.entries["cmap"]
        cmap_offset = struct.unpack_from(">I", self.data, cmap_entry + 8)[0]
        count = struct.unpack_from(">H", self.data, cmap_offset + 2)[0]
        for platform, encoding in ((1, 0), (3, 0)):
            with self.subTest(platform=platform, encoding=encoding):
                mutated = bytearray(self.data)
                for index in range(count):
                    rec_off = cmap_offset + 4 + 8 * index
                    struct.pack_into(">HH", mutated, rec_off, platform, encoding)
                with self.assertRaises(TtfConvertError) as caught:
                    convert(bytes(mutated), input_path="crafted.ttf", output_path="out.pgf", codes=(0x41, 0x41))
                self.assertEqual(caught.exception.reason, "malformed-cmap")

    def test_metric_targets_with_embolden_y_match_written_header_ascender(self) -> None:
        targets = ttf2pgf._MetricTargets(
            ascender_px=10, descender_px=-2, tolerance_px=1, ppem_min=6, ppem_max=24,
        )
        conversion = convert(
            self.data,
            input_path=str(FONT),
            output_path="out.pgf",
            codes=(0x41, 0x48),
            metric_targets=targets,
            embolden_y=8,
        )
        self.assertEqual(
            conversion.manifest["metric_targets"]["achieved_ascender_26_6"],
            conversion.manifest["output"]["ascender_26_6"],
        )
        self.assertLessEqual(
            abs(conversion.manifest["output"]["ascender_26_6"] / 64 - targets.ascender_px),
            targets.tolerance_px,
        )

    def test_overlapping_cmap_segments_and_groups_are_refused(self) -> None:
        sub4 = _format4_subtable([(0x41, 0x45, 0), (0x43, 0x46, 0), (0xFFFF, 0xFFFF, 1)])
        with self.assertRaises(TtfConvertError) as caught:
            ttf2pgf._parse_cmap4(sub4, 0)
        self.assertEqual(caught.exception.reason, "malformed-cmap")
        sub12 = struct.pack(">HHIII", 12, 0, 16 + 24, 0, 2) + struct.pack(">IIIIII", 0x41, 0x45, 1, 0x45, 0x48, 1)
        with self.assertRaises(TtfConvertError) as caught:
            ttf2pgf._parse_cmap12(sub12, 0)
        self.assertEqual(caught.exception.reason, "malformed-cmap")

    def test_font_type_using_camouflage_or_reserved_name_is_refused(self) -> None:
        self.assert_cli_refused(
            "camouflage-font-name", self.output, FONT, "--font-type", _joined("Font", "works")
        )
        self.assert_cli_refused(
            "reserved-font-name", self.output, FONT, "--pin", PIN, "--font-type", "Gudea"
        )

    def test_failed_output_write_leaves_no_partial_pgf(self) -> None:
        orig_write_bytes = Path.write_bytes

        def fail_pgf_write(self_path: Path, data: bytes) -> int:
            if self_path.name.startswith("out.pgf"):
                self_path.parent.mkdir(parents=True, exist_ok=True)
                with open(self_path, "wb") as handle:
                    handle.write(data[:16])
                raise OSError("disk full")
            return orig_write_bytes(self_path, data)

        with mock.patch.object(Path, "write_bytes", fail_pgf_write):
            code = ttf2pgf._main([str(self.output), str(FONT), "--codes", "0x41-0x41"])
        self.assertEqual(code, 1)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.tmp.glob("out.pgf*")), [])


class CompositeRefusalTests(unittest.TestCase):
    """Crafted composite mutations exercise the named unsupported paths."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.font = ttf2pgf._Ttf(self.data)
        self._tmp = tempfile.TemporaryDirectory(prefix="ttf2pgf_comp_")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def _first_composite_code(self) -> int:
        for code in range(0xC0, 0x100):
            gid = self.font.glyph_id(code)
            if gid is None:
                continue
            start = self.font.loca[gid]
            if self.font.loca[gid + 1] > start and struct.unpack_from(">h", self.font.glyf, start)[0] == -1:
                return code
        raise AssertionError("the pinned fixture has no composite glyph in U+00C0..U+00FF")

    def _mutate_component(self, code: int, offset_delta: int, blob: bytes) -> bytes:
        gid = self.font.glyph_id(code)
        assert gid is not None
        start = self.font.loca[gid]
        glyf_entry = _directory(self.data)["glyf"]
        glyf_offset = struct.unpack_from(">I", self.data, glyf_entry + 8)[0]
        return _patch(self.data, glyf_offset + start + offset_delta, blob)

    def test_point_matching_component_arguments_are_refused(self) -> None:
        code = self._first_composite_code()
        gid = self.font.glyph_id(code)
        assert gid is not None
        start = self.font.loca[gid]
        flags = struct.unpack_from(">H", self.font.glyf, start + 10)[0]
        patched = flags & ~ttf2pgf._ARGS_XY
        data = self._mutate_component(code, 10, struct.pack(">H", patched))
        with self.assertRaises(TtfConvertError) as caught:
            convert(data, input_path="crafted.ttf", output_path="out.pgf", codes=(code, code))
        self.assertEqual(caught.exception.reason, "unsupported-composite-point-args")

    def test_two_by_two_component_transform_is_refused(self) -> None:
        code = self._first_composite_code()
        gid = self.font.glyph_id(code)
        assert gid is not None
        start = self.font.loca[gid]
        flags = struct.unpack_from(">H", self.font.glyf, start + 10)[0]
        self.assertEqual(flags & (ttf2pgf._A_SCALE | ttf2pgf._XY_SCALE), 0, "fixture assumption")
        patched = flags | ttf2pgf._TWO_BY_TWO
        data = self._mutate_component(code, 10, struct.pack(">H", patched))
        with self.assertRaises(TtfConvertError) as caught:
            convert(data, input_path="crafted.ttf", output_path="out.pgf", codes=(code, code))
        self.assertEqual(caught.exception.reason, "unsupported-composite-transform")

    def test_self_referential_component_is_refused_at_the_depth_bound(self) -> None:
        code = self._first_composite_code()
        gid = self.font.glyph_id(code)
        assert gid is not None
        data = self._mutate_component(code, 12, struct.pack(">H", gid))
        with self.assertRaises(TtfConvertError) as caught:
            convert(data, input_path="crafted.ttf", output_path="out.pgf", codes=(code, code))
        self.assertEqual(caught.exception.reason, "composite-too-deep")


class MalformedGlyphTests(unittest.TestCase):
    """Crafted endPtsOfContours values in the pinned glyf table are refused by name."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.font = ttf2pgf._Ttf(self.data)

    def _with_end_points(self, code: int, ends: tuple[int, ...]) -> bytes:
        gid = self.font.glyph_id(code)
        assert gid is not None
        start = self.font.loca[gid]
        self.assertEqual(struct.unpack_from(">h", self.font.glyf, start)[0], len(ends), "fixture assumption")
        glyf_offset = struct.unpack_from(">I", self.data, _directory(self.data)["glyf"] + 8)[0]
        return _patch(self.data, glyf_offset + start + 10, struct.pack(f">{len(ends)}H", *ends))

    def _assert_glyph_refused(self, code: int, ends: tuple[int, ...]) -> None:
        data = self._with_end_points(code, ends)
        with self.assertRaises(TtfConvertError) as caught:
            convert(data, input_path="crafted.ttf", output_path="out.pgf", codes=(code, code))
        self.assertEqual(caught.exception.reason, "malformed-glyph")

    def test_duplicate_end_point_index_is_refused(self) -> None:
        # 'O' has endPts (10, 21); a repeated index would silently drop the second contour.
        self._assert_glyph_refused(0x4F, (10, 10))

    def test_decreasing_middle_end_point_index_is_refused(self) -> None:
        # 'B' has endPts (14, 22, 30); a decreasing middle index would overlap contours.
        self._assert_glyph_refused(0x42, (14, 10, 30))

    def test_last_end_point_beyond_the_glyph_data_is_refused(self) -> None:
        # The last index implies 41 points, but 'O' carries only 22.
        self._assert_glyph_refused(0x4F, (10, 40))


class ExactArithmeticTests(unittest.TestCase):
    """Implied midpoints and de Casteljau halvings are exact rationals; no float reaches the rasterizer."""

    def test_implied_midpoint_of_large_odd_coordinates_is_exact(self) -> None:
        # 2**60 + 1 is not representable as a float; a float midpoint would round it to 2**59.
        big = 2**60 + 1
        segments = ttf2pgf._expand_contour([(big, 0, False), (0, 0, False)])
        implied_x = segments[0][3][0]
        self.assertEqual(implied_x, Fraction(big, 2))
        self.assertIsInstance(implied_x, Fraction)

    def test_expanded_contour_contains_no_float(self) -> None:
        segments = ttf2pgf._expand_contour([(0, 0, False), (3, 0, False), (3, 3, False), (0, 3, False)])
        self.assertTrue(segments)
        for segment in segments:
            for point in segment[1:]:
                for coordinate in point:
                    self.assertNotIsInstance(coordinate, float)

    def test_flattened_quadratic_contains_only_exact_coordinates(self) -> None:
        out: list[tuple] = []
        ttf2pgf._flatten_quad((0, 0), (1, 2), (2, 0), 0, out)
        self.assertGreater(len(out), 1, "the curve must subdivide for this check to mean anything")
        for point in out:
            for coordinate in point:
                self.assertIsInstance(coordinate, (int, Fraction))
                self.assertNotIsInstance(coordinate, float)

    def test_flattened_vertices_round_half_toward_positive_infinity(self) -> None:
        self.assertEqual(ttf2pgf._round26_6(Fraction(5, 2)), 3)
        self.assertEqual(ttf2pgf._round26_6(Fraction(-5, 2)), -2)
        self.assertEqual(ttf2pgf._round26_6(7), 7)

    def test_float_coordinates_are_rejected_not_rounded(self) -> None:
        with self.assertRaises(TypeError):
            ttf2pgf._round26_6(2.5)


def _rasterize_rational_reference(raster, embolden_x: int = 0, embolden_y: int = 0):
    """The specification's 8x8 non-zero-winding rasterizer written with Fractions, as the
    converter computed it before its crossings moved to integer arithmetic. The tests compare
    the production rasterizer against this, bit for bit."""
    width, height = raster.width, raster.height
    if width <= 0 or height <= 0 or not raster.edges:
        return 0, 0, ()
    sub_pixel, supersample = ttf2pgf.SUB_PIXEL, ttf2pgf.SUPERSAMPLE
    left26, top26 = raster.left * 64, raster.top * 64
    sub_columns = width * supersample
    sub_rows = []
    for row in range(height):
        for step in range(supersample):
            y = top26 - (row * supersample + step) * sub_pixel - sub_pixel // 2
            crossings = []
            for x0, y0, x1, y1 in raster.edges:
                if y0 == y1:
                    continue
                if y0 <= y < y1:
                    direction = 1
                elif y1 <= y < y0:
                    direction = -1
                else:
                    continue
                numer = x0 * (y1 - y0) + (y - y0) * (x1 - x0)
                den = y1 - y0
                if den < 0:
                    numer, den = -numer, -den
                crossings.append((Fraction(numer, den) - left26, direction))
            crossings.sort(key=lambda entry: (entry[0], entry[1]))
            bits = winding = 0
            for index, (position, direction) in enumerate(crossings):
                winding += direction
                if winding == 0 or index + 1 >= len(crossings):
                    continue
                nxt = crossings[index + 1][0]
                j_min = max((position - (sub_pixel // 2)) // sub_pixel + 1, 0)
                j_max = min(-((sub_pixel // 2 - nxt) // sub_pixel) - 1, sub_columns - 1)
                if j_max >= j_min:
                    bits |= ((1 << (j_max - j_min + 1)) - 1) << j_min
            sub_rows.append(bits)
    if embolden_x:
        mask = (1 << sub_columns) - 1
        sub_rows = [functools_reduce_or(bits, embolden_x) & mask for bits in sub_rows]
    if embolden_y:
        sub_rows = [
            functools.reduce(lambda acc, value: acc | value, sub_rows[index : index + embolden_y + 1], 0)
            for index in range(len(sub_rows))
        ]
    counts = [0] * (width * height)
    for index, bits in enumerate(sub_rows):
        row = index // supersample
        for column in range(width):
            active = (bits >> (column * supersample)) & 0xFF
            counts[row * width + column] += active.bit_count()
    scale = supersample * supersample
    return width, height, tuple((count * 15 + 32) // scale for count in counts)


def functools_reduce_or(bits: int, shifts: int) -> int:
    grown = bits
    for shift in range(1, shifts + 1):
        grown |= bits << shift
    return grown


class IntegerRasterEquivalenceTests(unittest.TestCase):
    """The flattener, the 26.6 scaling and the crossing rasterizer run on integers for speed; each
    must produce exactly what the specification's rational arithmetic produces."""

    def test_scaled_flattening_matches_the_rational_flattener(self) -> None:
        rng = random.Random(1296)
        bits, half = ttf2pgf._SCALE_BITS, 1 << (ttf2pgf._SCALE_BITS - 1)
        for _ in range(3000):
            a, c, b = ((rng.randint(-40000, 40000), rng.randint(-40000, 40000)) for _ in range(3))
            reference: list[tuple] = []
            ttf2pgf._flatten_quad(a, c, b, 0, reference)
            scaled: list[tuple] = []
            ttf2pgf._flatten_quad_scaled(*((x << bits, y << bits) for x, y in (a, c, b)), 0, scaled)
            self.assertEqual(
                [((x + half) >> bits, (y + half) >> bits) for x, y in scaled],
                [(ttf2pgf._round26_6(x), ttf2pgf._round26_6(y)) for x, y in reference],
            )

    def test_scaled_contour_expansion_keeps_implied_midpoints_exact(self) -> None:
        rng = random.Random(363)
        bits = ttf2pgf._SCALE_BITS
        for _ in range(500):
            points = [(rng.randint(-5000, 5000), rng.randint(-5000, 5000), rng.random() < 0.4)
                      for _ in range(rng.randint(2, 9))]
            reference = ttf2pgf._expand_contour(points)
            scaled = ttf2pgf._expand_contour(
                [(x << bits, y << bits, on) for x, y, on in points], ttf2pgf._scaled_midpoint
            )
            unscale = lambda point: (Fraction(point[0], 1 << bits), Fraction(point[1], 1 << bits))
            self.assertEqual(
                [(segment[0], *map(unscale, segment[1:])) for segment in scaled],
                [(segment[0], *((Fraction(x), Fraction(y)) for x, y in segment[1:])) for segment in reference],
            )

    def test_integer_26_6_scaling_matches_the_rational_rule(self) -> None:
        rng = random.Random(1309)
        for _ in range(5000):
            value, ppem, upem = rng.randint(-70000, 70000), rng.randint(1, 64), rng.randint(16, 16384)
            self.assertEqual(
                ttf2pgf._to26_6(value, ppem, upem),
                math.floor(Fraction(value) * ppem * 64 / upem + Fraction(1, 2)),
            )

    def test_crossing_rasterizer_matches_the_rational_reference(self) -> None:
        rng = random.Random(2026)
        large_den_cases = 0
        for case in range(400):
            # Large outlines push edge denominators past 2**16 and take the Fraction sort.
            span = 1 << (8 if case % 4 else 18)
            edges: list[tuple[int, int, int, int]] = []
            for _ in range(rng.randint(1, 6)):
                points = [(rng.randint(-span, span), rng.randint(-span, span)) for _ in range(rng.randint(3, 6))]
                edges.extend((x0, y0, x1, y1) for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1], strict=True))
            xs = [x for edge in edges for x in (edge[0], edge[2])]
            ys = [y for edge in edges for y in (edge[1], edge[3])]
            left, bottom = min(xs) // 64, min(ys) // 64
            right, top = -((-max(xs)) // 64), -((-max(ys)) // 64)
            if (right - left) * (top - bottom) > 4096:
                # Clip to a 16x16 pixel window around the origin so large-denominator edges run.
                left, bottom, right, top = -8, -8, 8, 8
            raster = ttf2pgf._Raster(edges=tuple(edges), left=left, bottom=bottom, right=right, top=top)
            if any(
                abs(y1 - y0) >= (1 << 16) and min(y0, y1) < top * 64 and max(y0, y1) > bottom * 64
                for _, y0, _, y1 in edges
            ):
                large_den_cases += 1
            embolden = (case % 3, case % 2)
            self.assertEqual(ttf2pgf._rasterize(raster, *embolden),
                             _rasterize_rational_reference(raster, *embolden))
        self.assertGreater(large_den_cases, 0)

    def test_latin_project_font_output_is_pinned(self) -> None:
        # The digest the converter has produced for the Latin project font since #1309's build, with
        # the raster box as it was before rule 15's ink trim: the integer arithmetic must reproduce
        # it byte for byte. The second digest is the same font cut to its ink (converter 0.7.0), the
        # third that font with the console's vertical metrics (rule 16, converter 0.8.0).
        fixture = ROOT / "tests" / "fixtures" / "fonts" / "mplus1p"

        def make() -> ttf2pgf.Conversion:
            return convert(
                (fixture / "MPLUS1p-Regular.ttf").read_bytes(),
                input_path="tests/fixtures/fonts/mplus1p/MPLUS1p-Regular.ttf",
                output_path="nkltn.pgf",
                pin=load_pin(fixture / "PIN.json", (fixture / "MPLUS1p-Regular.ttf").read_bytes()),
                code_points=ttf2pgf.latin_1_text_code_points(),
                ppem=13, embolden_x=6, embolden_y=4, embolden_advance=3,
                font_name="Nakagawa Open Latin", font_type="Regular",
            )

        with mock.patch.object(ttf2pgf, "_vertical_metrics", _without_vertical):
            with mock.patch.object(ttf2pgf, "_trim_to_ink", _untrimmed):
                whole = make()
            trimmed = make()
        self.assertEqual(hashlib.sha256(whole.image).hexdigest(),
                         "e10068b52f117295792fe29698a97ab52faedaac1aabfee59a8cf498437b1d15")
        self.assertEqual(hashlib.sha256(trimmed.image).hexdigest(),
                         "532bfec6aa133cb323a3cc11b136d31449270ec7058e96a843c4d6e74af04057")
        self.assertEqual(hashlib.sha256(make().image).hexdigest(),
                         "5c4e76bfbeec133dbae3eb2894cae1a6d34fa20c35537a536d2bb1177087c425")


def _format4_subtable(segments: list[tuple[int, int, int]]) -> bytes:
    """A format 4 cmap subtable of (start, end, idDelta) segments with no glyphIdArray."""
    count = len(segments)
    header = struct.pack(">HHHHHHH", 4, 16 + 8 * count, 0, 2 * count, 0, 0, 0)
    ends = struct.pack(f">{count}H", *(end for _, end, _ in segments))
    starts = struct.pack(f">{count}H", *(start for start, _, _ in segments))
    deltas = struct.pack(f">{count}h", *(delta for _, _, delta in segments))
    return header + ends + struct.pack(">H", 0) + starts + deltas + struct.pack(f">{count}H", *([0] * count))


class CmapTerminatorTests(unittest.TestCase):
    """U+FFFF is a noncharacter; the format 4 terminator segment must never map it."""

    def test_terminator_segment_does_not_map_u_ffff(self) -> None:
        # If the terminator were mapped, idDelta 5 would send U+FFFF to glyph 4.
        subtable = _format4_subtable([(0x41, 0x43, 0), (0xFFFF, 0xFFFF, 5)])
        self.assertEqual(ttf2pgf._parse_cmap4(subtable, 0), {0x41: 0x41, 0x42: 0x42, 0x43: 0x43})

    def test_range_ending_at_u_ffff_stops_before_u_ffff(self) -> None:
        mapping = ttf2pgf._parse_cmap4(_format4_subtable([(0xFFF0, 0xFFFF, 0)]), 0)
        self.assertEqual(sorted(mapping), list(range(0xFFF0, 0xFFFF)))
        self.assertNotIn(0xFFFF, mapping)


class MetricTargetTests(unittest.TestCase):
    """Metric targets are a separate policy input that selects the size."""

    def test_targets_select_a_ppem_whose_header_lands_in_tolerance(self) -> None:
        data = FONT.read_bytes()
        with tempfile.TemporaryDirectory(prefix="ttf2pgf_target_") as tmp:
            targets = Path(tmp) / "targets.json"
            targets.write_text(
                json.dumps(
                    {"ascender_px": 9, "descender_px": -2, "tolerance_px": 1, "ppem_min": 6, "ppem_max": 24}
                ),
                encoding="utf-8",
            )
            policy = ttf2pgf.load_metric_targets(targets)
            conversion = convert(
                data,
                input_path=str(FONT),
                output_path="target.pgf",
                codes=DEFAULT_CODES,
                metric_targets=policy,
                pin=load_pin(PIN, data),
            )
        header = ttf2pgf._read_header_fields(conversion.image)
        self.assertLessEqual(abs(header["ascender_26_6"] / 64 - 9.0), 1.0)
        self.assertLessEqual(abs(header["descender_26_6"] / 64 - (-2.0)), 1.0)
        recorded = conversion.manifest["metric_targets"]
        self.assertIsNotNone(recorded)
        assert recorded is not None
        self.assertEqual(recorded["achieved_ascender_26_6"], header["ascender_26_6"])
        self.assertEqual(recorded["achieved_descender_26_6"], header["descender_26_6"])
        self.assertEqual(conversion.manifest["parameters"]["ppem"], conversion.ppem)
        self.assertGreaterEqual(conversion.ppem, 6)
        self.assertLessEqual(conversion.ppem, 24)


@unittest.skipUnless(CC, "no C compiler on PATH")
class CodePointSetTests(unittest.TestCase):
    """--codepoints converts exactly the listed code points, and a sparse set is written sparse."""

    SET = (0x41, 0x48, 0x67, 0x7E)

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.pin = load_pin(PIN, self.data)
        self._tmp = tempfile.TemporaryDirectory(prefix="ttf2pgf_codepoints_")
        self.addCleanup(self._tmp.cleanup)
        self.points = Path(self._tmp.name) / "points.txt"
        self.points.write_text("".join(f"0x{code:04X}\n" for code in self.SET), encoding="ascii")

    def _convert(self, ppem: int = PPEM_SHAPE):
        code_points = ttf2pgf.load_code_points(self.points)
        return convert(
            self.data,
            input_path=str(FONT),
            output_path="sparse.pgf",
            code_points=code_points,
            ppem=ppem,
            pin=self.pin,
        )

    def test_listed_codes_only_are_converted_and_the_map_is_sparse(self) -> None:
        conversion = self._convert()
        image = conversion.image
        self.assertEqual(sorted(glyph.code for glyph in conversion.glyphs), list(self.SET))
        self.assertEqual(struct.unpack_from("<HH", image, 0xB6), (0x41, 0x7E))
        self.assertEqual(struct.unpack_from("<I", image, 0x10)[0], 0x7E - 0x41 + 1)
        self.assertEqual(struct.unpack_from("<I", image, 0x14)[0], len(self.SET))
        validate_pgf_data(image, "sparse.pgf")

    def test_manifest_records_the_point_list_by_digest_and_leaf_name(self) -> None:
        conversion = self._convert()
        points = conversion.manifest["code_points"]
        self.assertEqual(points["sha256"], hashlib.sha256(self.points.read_bytes()).hexdigest())
        self.assertEqual(points["count"], len(self.SET))
        self.assertEqual(points["path"], "points.txt")
        self.assertEqual(conversion.manifest["coverage"]["count"], len(self.SET))
        self.assertEqual(conversion.manifest["coverage"]["span"], 0x7E - 0x41 + 1)
        self.assertEqual(manifest_bytes(conversion.manifest), manifest_bytes(self._convert().manifest))

    def test_nominal_size_is_the_em_size_at_the_chosen_ppem_not_the_extrema(self) -> None:
        conversion = self._convert(ppem=16)
        image = conversion.image
        self.assertEqual(struct.unpack_from("<i", image, 0x24)[0], 16 * 64)
        self.assertEqual(struct.unpack_from("<i", image, 0x28)[0], 16 * 64)
        self.assertEqual(conversion.manifest["parameters"]["nominal_em_26_6"], 16 * 64)
        self.assertEqual(conversion.manifest["output"]["horizontal_size_26_6"], 16 * 64)
        self.assertNotEqual(
            conversion.manifest["output"]["max_advance_26_6"],
            16 * 64,
            "the fixture's advance extremum must differ from the nominal size for this check to mean anything",
        )


class JapaneseCodePointCoverageTests(unittest.TestCase):
    """The built-in Japanese repertoire is complete and the selected pin covers every point."""

    def test_builtin_jis_set_pins_kana_symbols_and_level1_kanji(self) -> None:
        points = ttf2pgf.jis_x_0208_japanese_code_points()
        self.assertEqual(len(points.codes), 3647)
        self.assertEqual(points.codes, tuple(sorted(set(points.codes))))
        self.assertEqual(points.source, "builtin:jis-x-0208-jis-x-0201-japanese")
        self.assertEqual(points.sha256, "abc5302681a3c700179f29ac22a7c8082ed087a07b4cf8c3de4de5f73e7e0889")
        self.assertEqual(points.sha256, ttf2pgf.JAPANESE_CODE_POINTS_SHA256)
        self.assertTrue({0x3000, 0x3042, 0x30A2, 0x03B1, 0x2212, 0x4E00}.issubset(points.codes))
        self.assertTrue(set(range(0x20, 0x7F)).issubset(points.codes), "JIS X 0201 Roman coverage")
        self.assertTrue(set(range(0xFF61, 0xFFA0)).issubset(points.codes), "half-width katakana coverage")
        self.assertEqual(len(set(ttf2pgf._jis_x_0208_rows(range(1, 9)))), 524)
        self.assertEqual(len(set(ttf2pgf._jis_x_0208_rows(range(16, 48)))), 2965)

    def test_pinned_project_font_covers_the_complete_jis_set(self) -> None:
        data = JAPANESE_FONT.read_bytes()
        pin = load_pin(JAPANESE_PIN, data)
        self.assertEqual(pin.spdx, "OFL-1.1")
        self.assertEqual(pin.reserved_font_names, ())
        ttf2pgf.validate_output_name("Nakagawa Open Japanese", pin)
        font = ttf2pgf._Ttf(data)
        codes = ttf2pgf.jis_x_0208_japanese_code_points().codes
        missing = [code for code in codes if font.glyph_id(code) is None]
        self.assertEqual(missing, [], "the selected Japanese font must cover every assigned JIS code point")

    def test_japanese_text_set_holds_both_kanji_levels_and_all_of_cp932(self) -> None:
        """#2005: the first set stopped at JIS level 1; the text set adds level 2 and CP932."""
        points = ttf2pgf.japanese_text_code_points()
        self.assertEqual(len(points.codes), 7575)
        self.assertEqual(len(points.codes), ttf2pgf.JAPANESE_TEXT_CODE_POINT_COUNT)
        self.assertEqual(points.codes, tuple(sorted(set(points.codes))))
        self.assertEqual(points.source, "builtin:jis-x-0208-cp932-japanese")
        self.assertEqual(points.sha256, ttf2pgf.JAPANESE_TEXT_CODE_POINTS_SHA256)
        codes = set(points.codes)
        kanji = set(ttf2pgf._jis_x_0208_rows(range(16, 85)))
        self.assertEqual(len(kanji), 6355, "JIS X 0208 levels 1 and 2")
        self.assertTrue(kanji <= codes)
        self.assertTrue(set(ttf2pgf._cp932_double_byte_codes()) <= codes)
        self.assertTrue(set(ttf2pgf.jis_x_0208_japanese_code_points().codes) <= codes)
        self.assertTrue(set(range(0xA0, 0x100)) <= codes)
        # Level 2 kanji, NEC row 13 and an IBM extension are all in it.
        self.assertTrue({0x5F0C, 0x9ED1, 0x2460, 0x2252, 0x7E8A}.issubset(codes))
        self.assertFalse(any(0xE000 <= code <= 0xF8FF for code in codes), "no user-defined area")

    def test_jis_x_0213_set_is_codec_derived(self) -> None:
        codes = ttf2pgf.jis_x_0213_code_points()
        self.assertEqual(len(codes), ttf2pgf.JIS_X_0213_CODE_POINT_COUNT)
        self.assertEqual(len(codes), 10905)
        self.assertEqual(codes, tuple(sorted(set(codes))))
        self.assertTrue(set(ttf2pgf._jis_x_0208_rows(range(16, 85))) <= set(codes))
        # Plane 1's additions: a kanji outside JIS X 0208, katakana for Ainu and a Latin letter.
        self.assertTrue({0x4FF1, 0x31F0, 0x0101} <= set(codes))
        self.assertFalse(any(code > 0xFFFF for code in codes))

    @staticmethod
    def _stand_in_faces() -> tuple[ttf2pgf._Ttf, ttf2pgf._Ttf]:
        return ttf2pgf._Ttf(NOTO_JP_FONT.read_bytes()), ttf2pgf._Ttf(JAPANESE_FONT.read_bytes())

    def test_japanese_text_set_adds_jis_x_0213_and_the_named_blocks_the_faces_draw(self) -> None:
        """#1300: the stand-in drew the replacement character for every character outside CP932,
        the trade mark sign among them (Ace Combat: Joint Assault writes "Memory Stick™" with it).
        The set the build converts adds every character of JIS X 0213 and of the named Unicode
        blocks that the faces draw; it is computed from the faces' character maps."""
        noto, mplus = self._stand_in_faces()

        def draws(code: int) -> bool:
            return noto.glyph_id(code) is not None or mplus.glyph_id(code) is not None

        points = ttf2pgf.japanese_text_code_points(draws)
        codes = set(points.codes)
        self.assertEqual(points.codes, tuple(sorted(codes)))
        self.assertEqual(points.source, "builtin:jis-x-0208-cp932-japanese+jis-x-0213+blocks")
        core = set(ttf2pgf.japanese_text_code_points().codes)
        jis_x_0213 = {code for code in ttf2pgf.jis_x_0213_code_points() if draws(code)}
        blocks = {code for _, first, last in ttf2pgf.JAPANESE_TEXT_BLOCKS for code in range(first, last + 1)
                  if draws(code)}
        self.assertTrue(core <= codes)
        self.assertTrue(jis_x_0213 <= codes)
        self.assertTrue(blocks <= codes)
        self.assertEqual(codes, core | jis_x_0213 | blocks)
        self.assertTrue(all(draws(code) for code in codes - core))
        self.assertEqual(len(codes), 15631)
        # The trade mark sign, a Kangxi radical, a kanji outside JIS X 0208, a squared word,
        # parenthesized numbers, a Latin letter with a macron and a box-drawing arc.
        self.assertTrue({0x2122, 0x2F00, 0x4E04, 0x3300, 0x2474, 0x0100, 0x256D} <= codes)
        # Two blocks stay out (their wider and taller glyphs would grow the font's glyph box); what
        # CP932 and JIS X 0213 hold of them is in.
        names = {name for name, _, _ in ttf2pgf.JAPANESE_TEXT_BLOCKS}
        self.assertFalse({"Mathematical Operators", "CJK Symbols and Punctuation"} & names)
        self.assertFalse({0x2230, 0x3031} & codes)
        self.assertTrue({0x2212, 0x221E, 0x3005, 0x3033} <= codes)
        self.assertFalse(any(0xE000 <= code <= 0xF8FF for code in codes), "no user-defined area")

    def test_noto_sans_jp_draws_the_text_set_with_m_plus_for_what_it_lacks(self) -> None:
        data = NOTO_JP_FONT.read_bytes()
        pin = load_pin(NOTO_JP_PIN, data)
        self.assertEqual(pin.spdx, "OFL-1.1")
        ttf2pgf.validate_output_name("Nakagawa Open Japanese", pin)
        noto, mplus = self._stand_in_faces()
        core = ttf2pgf.japanese_text_code_points().codes
        self.assertEqual([code for code in core if noto.glyph_id(code) is None], [0x2252])
        codes = ttf2pgf.japanese_text_code_points(
            lambda code: noto.glyph_id(code) is not None or mplus.glyph_id(code) is not None).codes
        lacking = [code for code in codes if noto.glyph_id(code) is None]
        self.assertEqual(len(lacking), 341)
        self.assertEqual([code for code in lacking if mplus.glyph_id(code) is None], [])
        self.assertIsNotNone(noto.glyph_id(0x2122), "Noto Sans JP draws the trade mark sign")


class ProjectCodePointCoverageTests(unittest.TestCase):
    """Built-in Latin and Korean repertoires stay stable and pinned fonts cover them."""

    def test_latin_set_is_exact_and_mplus_covers_every_assigned_point(self) -> None:
        points = ttf2pgf.latin_1_text_code_points()
        self.assertEqual(len(points.codes), 348)
        self.assertEqual(points.codes, tuple(sorted(set(points.codes))))
        self.assertEqual(points.source, "builtin:latin-1-text")
        self.assertEqual(points.sha256, "7fbcaac92248acdb1c2704c3fef2181e7bc49f2b0e9d0f970cd029fb58057340")
        expected = (
            set(range(0x20, 0x7F))
            | set(range(0xA0, 0x100))
            | set(range(0x100, 0x180))
            | set(range(0x2010, 0x2028))
            | {0x2030, 0x2039, 0x203A, 0x20AC, 0x2122}
        )
        self.assertEqual(set(points.codes), expected)
        data = JAPANESE_FONT.read_bytes()
        pin = load_pin(JAPANESE_PIN, data)
        self.assertEqual(pin.spdx, "OFL-1.1")
        font = ttf2pgf._Ttf(data)
        missing = [code for code in points.codes if font.glyph_id(code) is None]
        self.assertEqual(missing, [], "M PLUS 1p must cover all 348 Latin text points")

    def test_latin_cyrillic_set_is_the_named_blocks_and_code_pages_the_face_draws(self) -> None:
        """#2005, #1300: the Latin text set is every character of the named Unicode blocks and code
        pages that the face draws, computed from the face's character map."""
        font = ttf2pgf._Ttf((LIBERATION_DIR / "LiberationSans-Regular.ttf").read_bytes())

        def draws(code: int) -> bool:
            return font.glyph_id(code) is not None

        points = ttf2pgf.latin_cyrillic_text_code_points(draws)
        codes = set(points.codes)
        self.assertEqual(points.codes, tuple(sorted(codes)))
        self.assertEqual(points.source, "builtin:latin-text-blocks")
        self.assertEqual(len(codes), 556)
        self.assertEqual(points.sha256, "7eb9461a9b5e1012ff653746e9b0e3ff7947b8d51b27cc9fa224003a1705647d")
        blocks = {code for _, first, last in ttf2pgf.LATIN_TEXT_BLOCKS for code in range(first, last + 1)}
        pages: set[int] = set()
        for codec in ttf2pgf.LATIN_TEXT_CODE_PAGES:
            pages |= ttf2pgf._code_page_characters(codec)
        expected = {code for code in blocks | pages if draws(code)}
        format_controls = {code for code in expected if unicodedata.category(chr(code)) == "Cf"}
        self.assertEqual(codes, (expected - format_controls) | {0x00AD})
        self.assertIn(0x200F, format_controls, "the bidirectional marks are format controls")
        # The 348-point Latin-1 text set, every letter of Windows-1251 Cyrillic and the
        # Romanian letters with a comma below are in it.
        self.assertTrue({code for code in ttf2pgf.latin_1_text_code_points().codes if draws(code)} <= codes)
        self.assertTrue(set(range(0x0410, 0x0450)) | {0x0401, 0x0451, 0x0490, 0x0491} <= codes)
        self.assertTrue({0x0218, 0x0219, 0x021A, 0x021B, 0x0192, 0x20AC, 0x2116, 0x2122} <= codes)
        # Hot Shots Tennis writes the minus sign U+2212 in its dialog and draws its own character
        # where the font has none: the set stays out of Mathematical Operators beyond the code pages.
        self.assertNotIn(0x2212, codes)
        self.assertFalse(set(range(0x0300, 0x0370)) & codes, "no combining marks")

    def test_every_liberation_face_draws_the_same_latin_cyrillic_set(self) -> None:
        license_text = (LIBERATION_DIR / "OFL.txt").read_text(encoding="utf-8")
        digests = set()
        for stem, _style in LATIN_FACES:
            with self.subTest(face=stem):
                data = (LIBERATION_DIR / f"{stem}.ttf").read_bytes()
                pin = load_pin(LIBERATION_DIR / f"{stem}.pin.json", data)
                self.assertEqual(pin.spdx, "OFL-1.1")
                self.assertEqual(pin.license_text, license_text)
                self.assertIn("Liberation", pin.reserved_font_names)
                ttf2pgf.validate_output_name("Nakagawa Open Latin", pin)
                font = ttf2pgf._Ttf(data)
                points = ttf2pgf.latin_cyrillic_text_code_points(lambda code, font=font: font.glyph_id(code) is not None)
                self.assertTrue(set(range(0x20, 0x7F)) <= set(points.codes))
                digests.add(points.sha256)
        self.assertEqual(len(digests), 1, "every face of the build draws the same Latin text set")

    def test_latin_set_keeps_the_small_sans_glyph_box(self) -> None:
        """#1300: Hot Shots Tennis places the second line of its dialogs from the font's glyph box.
        A wider set whose wide or tall glyphs (Latin Extended-B digraphs, historic Cyrillic letters,
        box drawing) grew the box moved that line; the set must leave the small sans face's box as
        the Latin-1, Cyrillic and punctuation text set had it."""
        stem = _makefile_flags("PROJECT_LATIN_0_FACE")[0]
        data = (LIBERATION_DIR / f"{stem}.ttf").read_bytes()
        font = ttf2pgf._Ttf(data)
        points = ttf2pgf.latin_cyrillic_text_code_points(lambda code: font.glyph_id(code) is not None)
        tokens = _makefile_flags("PROJECT_LATIN_SMALL_STRIKE_ARGS") + _makefile_flags("PROJECT_LATIN_8_SPACING")
        image = convert(data, input_path=f"{stem}.ttf", output_path="nkltns.pgf", code_points=points,
                        font_name="Nakagawa Open Latin", font_type="Regular",
                        pin=load_pin(LIBERATION_DIR / f"{stem}.pin.json", data), **_convert_arguments(tokens)).image
        box = (struct.unpack_from("<i", image, 0xE0)[0], struct.unpack_from("<i", image, 0xE8)[0],
               struct.unpack_from("<i", image, 0xEC)[0], struct.unpack_from("<2i", image, 0xF4),
               struct.unpack_from("<2H", image, 0xFC))
        self.assertEqual(box, (832, 11, 975, (896, 896), (14, 14)))

    def test_latin_set_refuses_a_face_without_ascii(self) -> None:
        with self.assertRaises(ttf2pgf.TtfConvertError) as caught:
            ttf2pgf.latin_cyrillic_text_code_points(lambda code: code != 0x41)
        self.assertEqual(caught.exception.reason, "latin-mapping-invalid")

    def test_korean_set_is_codec_derived_and_gowun_covers_every_assigned_point(self) -> None:
        points = ttf2pgf.ks_x_1001_korean_code_points()
        rows = (1, 2, 3, 4, 6, 7, 8, 9)
        row_counts = tuple(len(ttf2pgf._ks_x_1001_rows((row,))) for row in rows)
        self.assertEqual(row_counts, (94, 71, 94, 93, 68, 79, 91, 94))
        self.assertEqual(sum(row_counts), 684)
        self.assertEqual(len(points.codes), 11951)
        self.assertEqual(points.codes, tuple(sorted(set(points.codes))))
        self.assertEqual(points.source, "builtin:ks-x-1001-hangul-syllables-korean")
        self.assertEqual(points.sha256, "50596a7f6e0e5158561147eae40a38edc12c28b1a4be3efff6d717c74b638bc4")
        self.assertTrue(set(range(0x20, 0x7F)).issubset(points.codes))
        self.assertTrue(set(range(0xAC00, 0xD7A4)).issubset(points.codes))
        self.assertTrue({0x3000, 0x3131, 0x2500, 0x3395, 0xFFE6}.issubset(points.codes))
        self.assertFalse({0x2160, 0x3042, 0x0391, 0x0410}.intersection(points.codes))

        data = KOREAN_FONT.read_bytes()
        pin = load_pin(KOREAN_PIN, data)
        self.assertEqual(pin.spdx, "OFL-1.1")
        self.assertEqual(pin.reserved_font_names, ())
        self.assertNotRegex(pin.license_text, r"(?im)^\s*Reserved Font Name\s*:")
        ttf2pgf.validate_output_name("Nakagawa Open Korean", pin)
        font = ttf2pgf._Ttf(data)
        missing = [code for code in points.codes if font.glyph_id(code) is None]
        self.assertEqual(missing, [], "Gowun Dodum must cover the complete assigned Korean set")


class ReaderRoundTripTests(unittest.TestCase):
    """Layer 1: the generated font opens through the production reader path."""

    tmp: ClassVar[str]
    exe: ClassVar[str]

    @classmethod
    def setUpClass(cls) -> None:
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="ttf2pgf_roundtrip_")
        cls.exe = os.path.join(cls.tmp, "ttf2pgf_roundtrip.exe")
        result = subprocess.run(
            [
                CC,
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{RT}",
                "-o",
                cls.exe,
                str(READER_C),
                str(HELPER_C),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError("the PGF round-trip harness did not build:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _read_back(self, image: bytes, codes: list[int]) -> list[str]:
        path = Path(self.tmp) / "converted.pgf"
        path.write_bytes(image)
        result = subprocess.run(
            [self.exe, str(path), *[str(code) for code in codes]],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return cast(list[str], result.stdout.splitlines())

    def _convert(self, codes: tuple[int, int] = DEFAULT_CODES, ppem: int = PPEM_SHAPE):
        data = FONT.read_bytes()
        return convert(
            data,
            input_path=str(FONT),
            output_path="converted.pgf",
            codes=codes,
            ppem=ppem,
            pin=load_pin(PIN, data),
        )

    def test_every_drawn_sample_equals_the_converter_raster(self) -> None:
        conversion = self._convert()
        glyphs = _glyph_map(conversion.glyphs)
        probe = [0x48, 0x67, 0x32, 0x20, 0xE9]  # H, g, 2, space, absent U+00E9
        lines = self._read_back(conversion.image, probe)
        self.assertEqual(lines[0], "open ok")
        font = bytes.fromhex(lines[1].removeprefix("font "))
        self.assertEqual(len(font), 0x108)
        name = DEFAULT_FONT_NAME.encode("ascii")
        self.assertEqual(font[0x7C : 0x7C + len(name)], name)
        self.assertEqual(struct.unpack_from("<I", font, 0x54)[0], 0x7E - 0x20 + 1)

        records = lines[2:]
        self.assertEqual(len(records), len(probe) * 2)
        for index, code in enumerate(probe):
            char_line = records[index * 2].split(" ")
            draw_line = records[index * 2 + 1].split(" ")
            self.assertEqual(char_line[:2], ["char", str(code)])
            if code == 0xE9:
                self.assertEqual(char_line[2], "0")
                self.assertEqual(bytes.fromhex(char_line[3]), bytes(0x3C))
                self.assertEqual(draw_line[:3], ["draw", str(code), "none"])
                continue
            glyph = glyphs[code]
            if code == 0x20:
                self.assertEqual(char_line[2], "0", "a zero-area space is not present")
                self.assertEqual(draw_line[2:4], ["0", "0"])
                self.assertEqual(draw_line[4], "skipped")
                continue
            self.assertEqual(char_line[2], "1", f"U+{code:04X} presence")
            info = bytes.fromhex(char_line[3])
            self.assertEqual(len(info), 0x3C)
            self.assertEqual(struct.unpack_from("<I", info, 0x00)[0], glyph.width)
            self.assertEqual(struct.unpack_from("<I", info, 0x04)[0], glyph.height)
            self.assertEqual(struct.unpack_from("<i", info, 0x18)[0], glyph.y_base)
            self.assertEqual(struct.unpack_from("<i", info, 0x30)[0], glyph.advance_x)
            self.assertEqual(draw_line[2:4], [str(glyph.width), str(glyph.height)])
            drawn = list(bytes.fromhex(draw_line[4]))
            self.assertEqual(len(drawn), glyph.width * glyph.height)
            self.assertEqual(drawn, list(glyph.samples), f"U+{code:04X} drawn samples")

    def test_source_owned_h_renders_as_two_stems_and_a_crossbar(self) -> None:
        conversion = self._convert()
        glyph = _glyph_map(conversion.glyphs)[0x48]
        samples = list(glyph.samples)
        width, height = glyph.width, glyph.height
        self.assertGreaterEqual(width, 7)
        self.assertGreaterEqual(height, 8)
        for row in range(height):
            self.assertTrue(_clusters(samples, row, width), f"H row {row} must carry ink")
        self.assertGreaterEqual(len(_clusters(samples, 0, width)), 2, "H opens with two stems")
        self.assertGreaterEqual(len(_clusters(samples, height - 1, width)), 2, "H closes with two stems")
        middle = height // 2
        self.assertEqual(len(_clusters(samples, middle, width)), 1, "the crossbar joins the stems")
        self.assertGreaterEqual(_clusters(samples, middle, width)[0], width - 2, "the crossbar spans the H")
        self.assertEqual(max(samples), 15, "the stems reach full coverage")
        self.assertEqual(min(samples), 0, "the counters stay fully transparent")

    def test_latin1_composite_range_round_trips_through_the_reader(self) -> None:
        codes = (0xC0, 0xFF)
        conversion = self._convert(codes=codes)
        glyph = _glyph_map(conversion.glyphs)[0xE9]  # e-acute: a composite glyph
        lines = self._read_back(conversion.image, [0xE9, 0xC0])
        self.assertEqual(lines[0], "open ok")
        char_line = lines[2].split(" ")
        draw_line = lines[3].split(" ")
        self.assertEqual(char_line[2], "1")
        self.assertEqual(
            list(bytes.fromhex(draw_line[4])),
            list(glyph.samples),
            "composited outlines survive the writer and the reader bit-exactly",
        )
        self.assertTrue(any(glyph.samples), "the composite glyph has ink")

    def _vertical_fields(self, conversion: ttf2pgf.Conversion, probe: list[int]) -> dict[int, dict[str, int]]:
        """The character information's 26.6 metrics (0x10-0x37) the reader returns for each drawn code."""
        lines = self._read_back(conversion.image, probe)
        self.assertEqual(lines[0], "open ok")
        names = ("width", "height", "ascender", "descender", "bearing_hx", "bearing_hy", "bearing_vx",
                 "bearing_vy", "advance_h", "advance_v")
        fields = {}
        for index, code in enumerate(probe):
            char_line = lines[2 + index * 2].split(" ")
            self.assertEqual(char_line[2], "1", f"U+{code:04X} presence")
            fields[code] = dict(zip(names, struct.unpack_from("<10i", bytes.fromhex(char_line[3]), 0x10), strict=True))
        return fields

    def test_vertical_fields_follow_the_console_rule(self) -> None:
        """#1300 (rule 16): the stand-ins wrote vertical bearing X as the box's left edge plus half
        the advance, vertical bearing Y as the box's top and a vertical advance of 0. Every console
        font centres the box on the vertical origin (bearing X minus half the width); its vertical
        advance is a fraction of the em; its Latin and Korean fonts put each glyph's bearing Y at the
        margin between that advance and the ascender-to-descender height, and its Japanese font at
        the distance from 0.88 em above the baseline down to the glyph's top."""
        data = FONT.read_bytes()
        pin = load_pin(PIN, data)
        probe = [0x48, 0x67, 0x2E, 0x69, 0x2D]
        common = {"input_path": str(FONT), "output_path": "converted.pgf", "codes": DEFAULT_CODES, "ppem": 20,
                  "pin": pin, "nominal_size_26_6": 648, "resolution_26_6": 8192}
        # The em is 10.125 points at 128 dpi: 18 px, 1152 in 26.6. Without an option the vertical
        # advance is one em and bearing Y the margin above the ascender.
        plain = convert(data, **common, header_ascender_26_6=863, header_descender_26_6=-196)
        for code, info in self._vertical_fields(plain, probe).items():
            with self.subTest(rule="default", code=f"U+{code:04X}"):
                self.assertEqual(info["bearing_vx"], -(info["width"] // 2))
                self.assertEqual(info["advance_v"], 1152)
                self.assertEqual(info["bearing_vy"], (1152 - (863 + 196)) // 2)
        latin = convert(data, **common, header_ascender_26_6=863, header_descender_26_6=-196,
                        vertical_advance=950)
        for code, info in self._vertical_fields(latin, probe).items():
            with self.subTest(rule="margin", code=f"U+{code:04X}"):
                self.assertEqual(info["bearing_vx"], -(info["width"] // 2))
                self.assertEqual(info["advance_v"], 1094, "950/1000 of 1152, rounded")
                self.assertEqual(info["bearing_vy"], (1094 - (863 + 196)) // 2)
        self.assertEqual(struct.unpack_from("<i", latin.image, 0xF0)[0], 1094, "header vertical advance")
        self.assertEqual(latin.manifest["parameters"]["vertical_advance_thousandths"], 950)

        ideographic = convert(data, **common, vertical_origin=880)
        for code, info in self._vertical_fields(ideographic, probe).items():
            with self.subTest(rule="origin", code=f"U+{code:04X}"):
                self.assertEqual(info["bearing_vx"], -(info["width"] // 2))
                self.assertEqual(info["advance_v"], 1152, "the default is one em")
                self.assertEqual(info["bearing_vy"], 1014 - info["bearing_hy"], "880/1000 of 1152 is 1013.76")
        self.assertEqual(ideographic.manifest["parameters"]["vertical_origin_thousandths"], 880)
        self.assertNotIn("vertical_advance_thousandths", ideographic.manifest["parameters"])

    def _render(self, conversion: ttf2pgf.Conversion, text: str) -> dict[tuple[int, int], int]:
        """Draw ``text`` from what the reader returns: each bitmap at the pen plus its bitmap left
        and top (CharInfo 0x08 and 0x0C), the pen moved by the advance. Keys are pixels (x right,
        y down from the baseline); a pixel left blank is absent."""
        glyphs = _glyph_map(conversion.glyphs)
        codes = sorted({ord(char) for char in text})
        lines = self._read_back(conversion.image, codes)
        self.assertEqual(lines[0], "open ok")
        records = lines[2:]
        bitmaps: dict[int, tuple[int, int, int, int, int, bytes]] = {}
        for index, code in enumerate(codes):
            char_line = records[index * 2].split(" ")
            draw_line = records[index * 2 + 1].split(" ")
            if char_line[2] == "0":  # a zero-area glyph: only its advance
                self.assertEqual(draw_line[4], "skipped", f"U+{code:04X}")
                bitmaps[code] = (0, 0, 0, 0, cast(int, glyphs[code].advance_x), b"")
                continue
            info = bytes.fromhex(char_line[3])
            width, height, left, top = struct.unpack_from("<IIii", info, 0x00)
            advance = struct.unpack_from("<i", info, 0x30)[0]
            bitmaps[code] = (width, height, left, top, advance, bytes.fromhex(draw_line[4]))
        canvas: dict[tuple[int, int], int] = {}
        pen = 0
        for char in text:
            width, height, left, top, advance, samples = bitmaps[ord(char)]
            for row in range(height):
                for column in range(width):
                    sample = samples[row * width + column]
                    if sample:
                        pixel = ((pen >> 6) + left + column, row - top)
                        canvas[pixel] = canvas.get(pixel, 0) + sample
            pen += advance
        return canvas

    def test_ink_trim_draws_every_pixel_where_it_was(self) -> None:
        """#1300, rule 15: each image is cut to its inked columns and rows and its bitmap left and
        top move with the cut, so a string placed by advance draws the same pixels as before."""
        data = FONT.read_bytes()
        liberation = (LIBERATION_DIR / "LiberationSans-Regular.ttf").read_bytes()
        cases = {
            "plain": lambda: self._convert(),
            "emboldened": lambda: convert(
                data, input_path=str(FONT), output_path="converted.pgf", codes=DEFAULT_CODES, ppem=13,
                pin=load_pin(PIN, data), embolden_x=4, embolden_y=4, embolden_advance=2, tracking=-2),
            "scaled": lambda: convert(
                liberation, input_path="LiberationSans-Regular.ttf", output_path="converted.pgf",
                codes=DEFAULT_CODES, ppem=13, pin=load_pin(LIBERATION_DIR / "LiberationSans-Regular.pin.json",
                                                           liberation),
                scale_x=1080, embolden_y=4, snap_overshoot=3, digit_advance_26_6=571),
        }
        text = "The quick brown fox jumps over the lazy dog. NEW GAME 0123456789 {|}~ Reload? No save data found."
        for name, make in cases.items():
            with self.subTest(case=name):
                trimmed = make()
                with mock.patch.object(ttf2pgf, "_trim_to_ink", _untrimmed):
                    whole = make()
                self.assertNotEqual(trimmed.image, whole.image, "the trim changed no image")
                before = self._render(whole, text)
                self.assertTrue(before)
                self.assertEqual(self._render(trimmed, text), before)

    def test_a_second_read_is_identical(self) -> None:
        conversion = self._convert()
        codes = [0x41, 0x67, 0x20]
        first = self._read_back(conversion.image, codes)
        second = self._read_back(conversion.image, codes)
        self.assertEqual(first, second)

    def test_sparse_codepoint_set_round_trips_and_absent_codes_are_misses(self) -> None:
        codes = CodePointSetTests.SET
        points = Path(self.tmp) / "set.txt"
        points.write_text("".join(f"0x{code:04X}\n" for code in codes), encoding="ascii")
        data = FONT.read_bytes()
        conversion = convert(
            data,
            input_path=str(FONT),
            output_path="converted.pgf",
            code_points=ttf2pgf.load_code_points(points),
            ppem=PPEM_SHAPE,
            pin=load_pin(PIN, data),
        )
        glyphs = _glyph_map(conversion.glyphs)
        probe = [0x41, 0x42, 0x48, 0x7E]  # U+0042 is outside the set
        lines = self._read_back(conversion.image, probe)
        self.assertEqual(lines[0], "open ok")
        font = bytes.fromhex(lines[1].removeprefix("font "))
        self.assertEqual(struct.unpack_from("<I", font, 0x54)[0], 0x7E - 0x41 + 1)
        records = lines[2:]
        for index, code in enumerate(probe):
            char_line = records[index * 2].split(" ")
            draw_line = records[index * 2 + 1].split(" ")
            self.assertEqual(char_line[:2], ["char", str(code)])
            if code not in glyphs:
                self.assertEqual(char_line[2], "0", f"U+{code:04X} is outside the set")
                self.assertEqual(draw_line[:3], ["draw", str(code), "none"])
                continue
            self.assertEqual(char_line[2], "1", f"U+{code:04X} presence")
            self.assertEqual(list(bytes.fromhex(draw_line[4])), list(glyphs[code].samples))

    def test_japanese_sparse_subset_round_trips_through_native_reader(self) -> None:
        data = JAPANESE_FONT.read_bytes()
        codes = (0x41, 0x3042, 0x30A2, 0x4E00, 0x9FA0, 0xFF76)
        code_points = ttf2pgf.CodePointSet(
            codes=codes,
            sha256=hashlib.sha256(b"japanese sparse reader fixture").hexdigest(),
            source="test:japanese-sparse",
        )
        conversion = convert(
            data,
            input_path=str(JAPANESE_FONT),
            output_path="japanese-sparse.pgf",
            code_points=code_points,
            ppem=12,
            font_name="Nakagawa Open Japanese",
            pin=load_pin(JAPANESE_PIN, data),
        )
        self.assertEqual(struct.unpack_from("<I", conversion.image, 0x14)[0], len(codes))
        probes = [*codes, 0x3043, 0xFF77]
        lines = self._read_back(conversion.image, probes)
        self.assertEqual(lines[0], "open ok")
        records = lines[2:]
        glyphs = _glyph_map(conversion.glyphs)
        for index, code in enumerate(probes):
            char_line = records[index * 2].split(" ")
            draw_line = records[index * 2 + 1].split(" ")
            if code in (0x3043, 0xFF77):
                self.assertEqual(char_line[2], "0")
                self.assertEqual(draw_line[:3], ["draw", str(code), "none"])
                continue
            glyph = glyphs[code]
            self.assertEqual(char_line[2], "1")
            self.assertEqual(draw_line[2:4], [str(glyph.width), str(glyph.height)])
            self.assertEqual(list(bytes.fromhex(draw_line[4])), list(glyph.samples))

    def test_korean_sparse_map_round_trips_through_native_reader(self) -> None:
        data = KOREAN_FONT.read_bytes()
        codes = (0x41, 0xAC00, 0xD55C, 0xFFE6)
        code_points = ttf2pgf.CodePointSet(
            codes=codes,
            sha256=hashlib.sha256(b"Korean sparse reader fixture").hexdigest(),
            source="test:korean-sparse",
        )
        conversion = convert(
            data,
            input_path=str(KOREAN_FONT),
            output_path="korean-sparse.pgf",
            code_points=code_points,
            ppem=12,
            font_name="Nakagawa Open Korean",
            pin=load_pin(KOREAN_PIN, data),
        )
        first, last = struct.unpack_from("<HH", conversion.image, 0xB6)
        self.assertEqual((first, last), (codes[0], codes[-1]))
        self.assertEqual(struct.unpack_from("<I", conversion.image, 0x10)[0], last - first + 1)
        self.assertEqual(struct.unpack_from("<I", conversion.image, 0x14)[0], len(codes))
        probes = [*codes, 0xAC01, 0x3042]
        lines = self._read_back(conversion.image, probes)
        self.assertEqual(lines[0], "open ok")
        font = bytes.fromhex(lines[1].removeprefix("font "))
        name = b"Nakagawa Open Korean"
        self.assertEqual(font[0x7C : 0x7C + len(name)], name)
        self.assertEqual(struct.unpack_from("<I", font, 0x54)[0], last - first + 1)
        records = lines[2:]
        for index, code in enumerate(probes):
            char_line = records[index * 2].split(" ")
            draw_line = records[index * 2 + 1].split(" ")
            if code in (0xAC01, 0x3042):
                self.assertEqual(char_line[2], "0", f"U+{code:04X} absence")
                self.assertEqual(draw_line[:3], ["draw", str(code), "none"])
            else:
                self.assertEqual(char_line[2], "1", f"U+{code:04X} presence")


def _joined(*parts: str) -> str:
    """A name assembled from literal pieces, so this file never holds a vendor name whole."""
    return "".join(parts)


class EmboldenTests(CliRefusalMixin):
    """Specification section 3 rule 10: optional synthetic emboldening on the sub-sample grid."""

    CODES = (0x48, 0x6F)  # 'H'..'o': stems, bowls, and a crossbar

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.pin = load_pin(PIN, self.data)
        self.tmp = Path(tempfile.mkdtemp(prefix="ttf2pgf-embolden-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.output = self.tmp / "out.pgf"

    def _convert(self, **overrides: object):
        arguments: dict = {
            "input_path": str(FONT),
            "output_path": "embolden.pgf",
            "codes": self.CODES,
            "ppem": PPEM_SHAPE,
            "pin": self.pin,
        }
        arguments.update(overrides)
        return convert(self.data, **arguments)

    def test_zero_strength_is_byte_identical_to_the_plain_rasterizer(self) -> None:
        plain = self._convert()
        zero = self._convert(embolden_x=0, embolden_y=0, embolden_advance=0)
        self.assertEqual(plain.image, zero.image)
        self.assertEqual(
            plain.manifest["parameters"]["embolden"],
            {"unit": "sub-sample (1/8 pixel)", "x": 0, "y": 0, "advance": 0},
        )

    def test_strokes_thicken_rightwards_and_upwards_without_moving_the_baseline(self) -> None:
        # Rule 10 grows the raster box; rule 15 then cuts each image to its ink, which this test
        # leaves out so that it sees the box (InkTrimTests covers the cut).
        with mock.patch.object(ttf2pgf, "_trim_to_ink", _untrimmed):
            plain = _glyph_map(self._convert().glyphs)
            bold = _glyph_map(self._convert(embolden_x=6, embolden_y=4, embolden_advance=3).glyphs)
        for code in range(self.CODES[0], self.CODES[1] + 1):
            before, after = plain[code], bold[code]
            if before.width == 0:
                continue
            with self.subTest(code=f"U+{code:04X}"):
                self.assertEqual(after.width, before.width + 1)
                self.assertEqual(after.height, before.height + 1)
                self.assertEqual(after.adjust_x, before.adjust_x, "the left edge stays put")
                self.assertEqual(after.adjust_y, before.adjust_y + 1, "the box grows on the top")
                self.assertEqual(after.y_base - after.dimension_height,
                                 before.y_base - before.dimension_height, "the baseline stays put")
                self.assertEqual(after.advance_x, before.advance_x + 3 * ttf2pgf.SUB_PIXEL)
                self.assertGreater(sum(after.samples), sum(before.samples))
        # The H stems: each run of the plain glyph's middle row is at least as wide when bold.
        h_plain, h_bold = plain[0x48], bold[0x48]
        row = h_plain.height // 4
        runs_plain = _clusters(list(h_plain.samples), row, h_plain.width)
        runs_bold = _clusters(list(h_bold.samples), row + 1, h_bold.width)
        self.assertEqual(len(runs_plain), len(runs_bold), "emboldening must not merge the two stems")
        for narrow, wide in zip(runs_plain, runs_bold, strict=True):
            self.assertGreaterEqual(wide, narrow)

    def test_manifest_records_the_strengths(self) -> None:
        conversion = self._convert(embolden_x=6, embolden_y=4, embolden_advance=3)
        self.assertEqual(
            conversion.manifest["parameters"]["embolden"],
            {"unit": "sub-sample (1/8 pixel)", "x": 6, "y": 4, "advance": 3},
        )

    def test_out_of_range_strengths_are_refused(self) -> None:
        for field in ("embolden_x", "embolden_y", "embolden_advance"):
            for value in (-1, ttf2pgf.MAX_EMBOLDEN + 1, True):
                with self.subTest(field=field, value=value):
                    with self.assertRaises(TtfConvertError) as caught:
                        self._convert(**{field: value})
                    self.assertEqual(caught.exception.reason, "embolden-out-of-range")
        self.assert_cli_refused("embolden-out-of-range", self.output, FONT, "--embolden-x", "9")

    def test_cli_options_match_the_api(self) -> None:
        result = _cli(self.output, FONT, "--codes", "0x48-0x6F", "--ppem", PPEM_SHAPE,
                      "--embolden-x", "6", "--embolden-y", "4", "--embolden-advance", "3")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        api = self._convert(embolden_x=6, embolden_y=4, embolden_advance=3, pin=None,
                            output_path=str(self.output))
        self.assertEqual(self.output.read_bytes(), api.image)
        self.assertIsNotNone(validate_pgf_data(api.image, "embolden.pgf"))


class HeaderAndEllipsisOptionTests(CliRefusalMixin):
    """The header sizes and extents a font declares, and a baseline ellipsis, are explicit options."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.tmp = Path(tempfile.mkdtemp(prefix="ttf2pgf-header-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _convert(self, **overrides: object):
        arguments: dict = {"input_path": str(FONT), "output_path": "out.pgf", "codes": (0x48, 0x48), "ppem": 13}
        arguments.update(overrides)
        return convert(self.data, **arguments)

    def test_defaults_keep_the_derived_header(self) -> None:
        conversion = self._convert()
        self.assertEqual(struct.unpack_from("<iiii", conversion.image, 0x24), (13 * 64, 13 * 64, 1920, 1920))
        self.assertIsNone(conversion.manifest["parameters"]["header_ascender_26_6"])

    def test_options_reach_the_header_and_the_manifest(self) -> None:
        conversion = self._convert(nominal_size_26_6=448, resolution_26_6=8192, header_ascender_26_6=576,
                                   header_descender_26_6=-131)
        self.assertEqual(struct.unpack_from("<iiii", conversion.image, 0x24), (448, 448, 8192, 8192))
        self.assertEqual(struct.unpack_from("<ii", conversion.image, 0xD4), (576, -131))
        parameters = conversion.manifest["parameters"]
        self.assertEqual((parameters["nominal_em_26_6"], parameters["resolution_26_6"]), (448, 8192))
        self.assertEqual(parameters["header_descender_26_6"], -131)

    def test_baseline_ellipsis_needs_the_ellipsis_in_the_code_set(self) -> None:
        with self.assertRaises(TtfConvertError) as caught:
            self._convert(baseline_ellipsis=True)
        self.assertEqual(caught.exception.reason, "baseline-ellipsis-unavailable")
        self.assert_cli_refused("baseline-ellipsis-unavailable", self.tmp / "out.pgf", FONT, "--codes", "0x48-0x48",
                                "--baseline-ellipsis")


def _makefile_flags(variable: str) -> list[str]:
    match = re.search(rf"^{variable} := (.+)$", read_makefile(ROOT), re.MULTILINE)
    assert match is not None, f"the Makefile does not define {variable}"
    return match.group(1).split()


_CONVERT_FLAGS = {
    "--ppem": "ppem", "--embolden-x": "embolden_x", "--embolden-y": "embolden_y",
    "--embolden-advance": "embolden_advance", "--nominal-size": "nominal_size_26_6",
    "--resolution": "resolution_26_6", "--header-ascender": "header_ascender_26_6",
    "--header-descender": "header_descender_26_6", "--tracking": "tracking",
    "--digit-advance": "digit_advance_26_6", "--snap-overshoot": "snap_overshoot", "--scale-x": "scale_x",
    "--vertical-advance": "vertical_advance", "--vertical-origin": "vertical_origin",
}


def _convert_arguments(tokens: list[str]) -> dict:
    arguments: dict = {}
    tokens = list(tokens)
    if "--baseline-ellipsis" in tokens:
        tokens.remove("--baseline-ellipsis")
        arguments["baseline_ellipsis"] = True
    for flag, value in zip(tokens[0::2], tokens[1::2], strict=True):
        arguments[_CONVERT_FLAGS[flag]] = int(value)
    return arguments


def _code_set(codes: set[int]) -> ttf2pgf.CodePointSet:
    ordered = tuple(sorted(codes))
    canonical = "".join(f"U+{code:04X}\n" for code in ordered).encode("ascii")
    return ttf2pgf.CodePointSet(codes=ordered, sha256=hashlib.sha256(canonical).hexdigest(), source="test")


class ProjectLatinStrikeTests(unittest.TestCase):
    """#1300 and #2005: each of the build's sixteen Latin faces meets the console font of its index in
    the numbers that make text look different: where the ink sits, how thick the strokes are, and
    how far each letter moves.

    The targets are aggregates measured on console Latin fonts (font/README.md, "Measured
    differences"); no glyph shape is taken from them. A line width is the sum of the advances of a
    sentence and a menu line without their spaces. The bounds are 3% of a line (the source fonts'
    capital-to-lowercase ratio is not the console's, so no spacing meets both lines exactly), one
    pixel for an ink edge and a fifth of a pixel for a stem.
    """

    SENTENCE = "The quick brown fox jumps over the lazy dog.".replace(" ", "")
    MENU = "NEW GAME  CONTINUE  OPTIONS  0123456789".replace(" ", "")
    #: Console line widths in pixels (sentence, menu) for Latin indices 0-15.
    LINES = (
        (413.4, 463.5), (363.1, 406.5), (413.4, 463.5), (356.1, 405.2),
        (432.3, 473.6), (382.4, 416.2), (432.3, 473.6), (363.8, 412.8),
        (275.6, 309.0), (242.1, 271.1), (275.6, 309.0), (237.5, 270.2),
        (288.1, 315.7), (254.9, 277.4), (288.1, 315.7), (242.5, 275.2),
    )
    #: The regular sans faces' class averages in pixels (lowercase, capitals, digits).
    CLASSES = {0: (11.435, 14.64, 14.69), 8: (7.63, 9.76, 9.80)}
    #: Mean ink per letter of the console's sans-serif faces, A-Z and a-z, in pixels of full coverage
    #: (the sum of each glyph's 4-bit samples over 15). Measured on the console's own files (#1300);
    #: only the number is recorded. Horizontal strokes of the source faces are thinner than the
    #: console's, so without a per-face --embolden-y a letter carries 9 to 25 % less ink.
    LETTER_INK = {0: 68.5, 2: 68.4, 4: 104.5, 6: 104.4, 8: 30.8, 10: 30.8, 12: 46.9, 14: 46.9}
    #: Ink of the regular sans faces: nominal, header ascender and descender, x-height, cap height,
    #: descender drop and stem.
    INK = {0: (648, 863, -196, 11, 14, -3, 2.13), 8: (448, 576, -131, 7, 9, -2, 1.40)}
    CODES = {ord(c) for c in SENTENCE + MENU} | set(range(0x41, 0x5B)) | set(range(0x61, 0x7B)) \
        | set(range(0x30, 0x3A)) | {0x2E, 0x2026}
    _cache: ClassVar[dict[int, ttf2pgf.Conversion]] = {}

    @staticmethod
    def _face(index: int) -> tuple[str, str]:
        group = index % 8
        return (_makefile_flags(f"PROJECT_LATIN_{group}_FACE")[0],
                " ".join(_makefile_flags(f"PROJECT_LATIN_{group}_STYLE")))

    @classmethod
    def _convert(cls, index: int) -> ttf2pgf.Conversion:
        if index not in cls._cache:
            strike = "PROJECT_LATIN_STRIKE_ARGS" if index < 8 else "PROJECT_LATIN_SMALL_STRIKE_ARGS"
            tokens = _makefile_flags(strike) + _makefile_flags(f"PROJECT_LATIN_{index}_SPACING")
            stem, style = cls._face(index)
            data = (LIBERATION_DIR / f"{stem}.ttf").read_bytes()
            cls._cache[index] = convert(
                data, input_path=f"{stem}.ttf", output_path="nkltn.pgf", code_points=_code_set(cls.CODES),
                font_name="Nakagawa Open Latin", font_type=style,
                pin=load_pin(LIBERATION_DIR / f"{stem}.pin.json", data), **_convert_arguments(tokens))
        return cls._cache[index]

    @staticmethod
    def _ink_rows(glyph: Glyph) -> tuple[int, int]:
        """Top and bottom edges of the rows that reach half coverage, above the baseline."""
        rows = [max(glyph.samples[row * glyph.width:(row + 1) * glyph.width]) for row in range(glyph.height)]
        inked = [row for row, peak in enumerate(rows) if peak >= 8]
        return glyph.adjust_y - inked[0], glyph.adjust_y - inked[-1] - 1

    @staticmethod
    def _stem(glyph: Glyph) -> float:
        row = glyph.height // 2
        return float(sum(glyph.samples[row * glyph.width:(row + 1) * glyph.width])) / 15

    @staticmethod
    def _average(glyphs: dict[int, Glyph], codes: range) -> float:
        return sum(cast(int, glyphs[code].advance_x) for code in codes) / 64 / len(codes)

    @staticmethod
    def _width(glyphs: dict[int, Glyph], text: str) -> float:
        return sum(cast(int, glyphs[ord(c)].advance_x) for c in text) / 64

    def test_each_index_has_its_own_face_in_the_console_order(self) -> None:
        """#2005: one face used to serve all eight styles of a size group."""
        for index in range(16):
            with self.subTest(index=index):
                self.assertEqual(self._face(index), LATIN_FACES[index % 8])
                conversion = self._convert(index)
                field = conversion.image[0x75:0x75 + 64].split(b"\0", 1)[0].decode("ascii")
                self.assertEqual(field, LATIN_FACES[index % 8][1])
                # #1300, rule 16: the console's vertical advance, 0.950 em of the larger size (18 px)
                # and 0.917 em of the smaller (7 points at 128 dpi), and the margin above the
                # ascender as every glyph's vertical bearing Y.
                advance_y, margin = (1094, 17) if index < 8 else (730, 11)
                self.assertEqual(struct.unpack_from("<i", conversion.image, 0xF0)[0], advance_y)
                for glyph in conversion.glyphs:
                    self.assertEqual((glyph.advance_y, glyph.y_top), (advance_y, margin))

    def test_each_index_meets_the_console_line_widths(self) -> None:
        for index, (sentence, menu) in enumerate(self.LINES):
            with self.subTest(index=index):
                glyphs = _glyph_map(self._convert(index).glyphs)
                self.assertLessEqual(abs(self._width(glyphs, self.SENTENCE) - sentence) / sentence, 0.03)
                self.assertLessEqual(abs(self._width(glyphs, self.MENU) - menu) / menu, 0.03)

    def test_regular_faces_meet_the_console_letter_capital_and_digit_widths(self) -> None:
        """#2005: capitals and digits were narrower than the console's; digits are tabular."""
        for index, (lower, capital, digit) in self.CLASSES.items():
            with self.subTest(index=index):
                glyphs = _glyph_map(self._convert(index).glyphs)
                average = functools.partial(self._average, glyphs)
                self.assertLessEqual(abs(average(range(0x61, 0x7B)) - lower) / lower, 0.03)
                self.assertLessEqual(abs(average(range(0x41, 0x5B)) - capital) / capital, 0.03)
                self.assertLessEqual(abs(average(range(0x30, 0x3A)) - digit) / digit, 0.01)
                self.assertEqual(len({glyphs[code].advance_x for code in range(0x30, 0x3A)}), 1)

    def test_regular_faces_meet_the_console_ink(self) -> None:
        for index, (nominal, ascender, descender, x_height, cap, drop, stem) in self.INK.items():
            with self.subTest(index=index):
                conversion = self._convert(index)
                glyphs = _glyph_map(conversion.glyphs)
                self.assertEqual(struct.unpack_from("<iiii", conversion.image, 0x24),
                                 (nominal, nominal, 8192, 8192))
                self.assertEqual(struct.unpack_from("<ii", conversion.image, 0xD4), (ascender, descender))
                self.assertLessEqual(abs(self._ink_rows(glyphs[ord("x")])[0] - x_height), 1)
                self.assertLessEqual(abs(self._ink_rows(glyphs[ord("H")])[0] - cap), 1)
                self.assertLessEqual(abs(self._ink_rows(glyphs[ord("p")])[1] - drop), 1)
                self.assertLessEqual(abs(self._stem(glyphs[ord("l")]) - stem), 0.2)

    def test_sans_faces_carry_the_console_ink_per_letter(self) -> None:
        """#1300: the stand-ins' horizontal strokes were thinner than the console's, so game text
        drawn with a stand-in looked light. Each sans-serif face is within 2 % of the console's ink."""
        letters = [ord(c) for c in string.ascii_letters]
        for index, target in self.LETTER_INK.items():
            with self.subTest(index=index):
                glyphs = _glyph_map(self._convert(index).glyphs)
                ink = sum(sum(glyphs[code].samples) for code in letters) / 15 / len(letters)
                self.assertLessEqual(abs(ink - target) / target, 0.02, f"ink per letter {ink:.1f}")

    def test_sans_faces_set_the_console_x_height_and_cap_height_rows(self) -> None:
        """#1300: the sans-serif faces' --embolden-y thickened strokes upwards far enough that the
        x-height and cap height of the bold faces of both sizes, and the x-height of the smaller
        regular face, came out one row above the console's, so game text looked taller and
        narrower than the console's. Each sans face now tops x and H on the console rows of
        ``INK`` (shared by every sans face of a size); the parameters were tuned against a
        private comparison with the console's own fonts."""
        for index in (0, 2, 4, 6, 8, 10, 12, 14):
            _nominal, _ascender, _descender, x_height, cap, _drop, _stem = self.INK[0 if index < 8 else 8]
            with self.subTest(index=index):
                glyphs = _glyph_map(self._convert(index).glyphs)
                self.assertEqual(self._ink_rows(glyphs[ord("x")])[0], x_height, "x-height row")
                self.assertEqual(self._ink_rows(glyphs[ord("H")])[0], cap, "cap-height row")

    def test_the_ellipsis_sits_on_the_line_of_the_full_stop(self) -> None:
        # No strike passes --baseline-ellipsis: every Liberation face already draws U+2026 there.
        for index in range(16):
            with self.subTest(index=index):
                glyphs = _glyph_map(self._convert(index).glyphs)
                self.assertEqual(glyphs[0x2026].adjust_y, glyphs[ord(".")].adjust_y)

    def test_round_and_flat_letters_share_their_rows(self) -> None:
        """#1300: in "Reload? No save data found." the round letters ended a row below the flat ones
        (their outlines overshoot the baseline by a fraction of a pixel). Every face of both sizes
        sets them on the baseline, the x-height and the cap height of its flat letters."""
        for index in range(16):
            with self.subTest(index=index):
                glyphs = _glyph_map(self._convert(index).glyphs)

                # Top row and bottom edge of each letter's bitmap, in pixels above the baseline.
                rows = {char: (glyphs[ord(char)].adjust_y, glyphs[ord(char)].adjust_y - glyphs[ord(char)].height)
                        for char in "eoacsuRNHlnxOCGS"}
                for char in "eoacsuRNHlnx":
                    self.assertEqual(rows[char][1], 0, f"{char!r} ends below the baseline")
                for char in "eoacs":
                    self.assertEqual(rows[char][0], rows["x"][0], f"{char!r} rises above the x-height")
                for char in "OCGS":
                    self.assertEqual(rows[char][0], rows["H"][0], f"{char!r} rises above the cap height")
                # Ascenders are not overshoots: at 13 ppem a band of half a pixel would have set the
                # top of l, 75 units above the cap height, on it. l is drawn as without the option.
                strike = "PROJECT_LATIN_STRIKE_ARGS" if index < 8 else "PROJECT_LATIN_SMALL_STRIKE_ARGS"
                arguments = _convert_arguments(_makefile_flags(strike) + _makefile_flags(f"PROJECT_LATIN_{index}_SPACING"))
                self.assertEqual(arguments.pop("snap_overshoot"), 3)
                stem, style = self._face(index)
                data = (LIBERATION_DIR / f"{stem}.ttf").read_bytes()
                plain = convert(data, input_path=f"{stem}.ttf", output_path="nkltn.pgf",
                                code_points=_code_set({ord("l")} | set(range(0x30, 0x3A))), font_type=style,
                                **arguments)
                self.assertEqual(glyphs[ord("l")], _glyph_map(plain.glyphs)[ord("l")])


class InkTrimTests(unittest.TestCase):
    """Specification section 3 rule 15 (#1300): the console's glyph images are cut to their ink in
    both directions, and a game that places the next glyph one pixel after the image's right edge
    (Hot Shots Tennis does) spaced the stand-ins' text by their blank columns too. Every image is now
    tight; the advances are those of the raster box."""

    @staticmethod
    def _conversions() -> dict[str, ttf2pgf.Conversion]:
        data = FONT.read_bytes()
        found = {
            "fixture": convert(data, input_path=str(FONT), output_path="converted.pgf", codes=DEFAULT_CODES,
                               ppem=PPEM_SHAPE, pin=load_pin(PIN, data)),
            "fixture-emboldened": convert(data, input_path=str(FONT), output_path="converted.pgf",
                                          codes=DEFAULT_CODES, ppem=13, pin=load_pin(PIN, data),
                                          embolden_x=8, embolden_y=8, embolden_advance=2),
        }
        for index in range(16):
            found[f"latin-{index}"] = ProjectLatinStrikeTests._convert(index)
        return found

    def test_no_image_has_an_empty_edge_column_or_row(self) -> None:
        for name, conversion in self._conversions().items():
            with self.subTest(conversion=name):
                loose: list[str] = []
                for glyph in conversion.glyphs:
                    width, height, samples = glyph.width, glyph.height, glyph.samples
                    if width == 0 or height == 0:
                        self.assertEqual((width, height, samples), (0, 0, ()), f"U+{glyph.code:04X}")
                        continue
                    edges = {
                        "first column": samples[0::width],
                        "last column": samples[width - 1::width],
                        "first row": samples[:width],
                        "last row": samples[-width:],
                    }
                    loose += [f"U+{glyph.code:04X} {edge}" for edge, line in edges.items() if not any(line)]
                self.assertEqual(loose, [], f"{len(loose)} empty image edges")

    def test_the_trim_keeps_every_advance(self) -> None:
        data = FONT.read_bytes()
        for embolden in (0, 8):
            with self.subTest(embolden=embolden):
                def make(strength: int = embolden) -> ttf2pgf.Conversion:
                    return convert(data, input_path=str(FONT), output_path="converted.pgf", codes=DEFAULT_CODES,
                                   ppem=13, pin=load_pin(PIN, data), embolden_x=strength, embolden_y=strength,
                                   tracking=3)
                trimmed = make()
                with mock.patch.object(ttf2pgf, "_trim_to_ink", _untrimmed):
                    whole = make()
                self.assertEqual([(glyph.code, glyph.advance_x, glyph.advance_y) for glyph in trimmed.glyphs],
                                 [(glyph.code, glyph.advance_x, glyph.advance_y) for glyph in whole.glyphs])
                # The header's nominal size (0x24, 0x28) and advance maxima (0xEC, 0xF0) as well.
                for offset in (0x24, 0x28, 0xEC, 0xF0):
                    self.assertEqual(struct.unpack_from("<i", trimmed.image, offset),
                                     struct.unpack_from("<i", whole.image, offset), hex(offset))
                trimmed_space = _glyph_map(trimmed.glyphs)[0x20]
                self.assertEqual((trimmed_space.width, trimmed_space.height, trimmed_space.samples), (0, 0, ()))


class OvershootSnapTests(CliRefusalMixin):
    """#1300: ``--snap-overshoot`` (spec section 3, rule 13) sets round letters' overshoots on the
    flat edges measured from the font's own ``H`` and ``x``, and moves nothing else."""

    SIZES = (13, 20)  # the project's two Latin strikes, 7 and 10.125 points at 128 dpi
    CODES = _code_set({ord(c) for c in "eoRHxlpvO.,"})

    def setUp(self) -> None:
        self.data = (LIBERATION_DIR / "LiberationSans-Regular.ttf").read_bytes()
        self.pin = load_pin(LIBERATION_DIR / "LiberationSans-Regular.pin.json", self.data)
        self.tmp = Path(tempfile.mkdtemp(prefix="ttf2pgf-snap-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _convert(self, ppem: int, **overrides: object) -> ttf2pgf.Conversion:
        arguments: dict = {"input_path": "LiberationSans-Regular.ttf", "output_path": "out.pgf",
                           "code_points": self.CODES, "ppem": ppem, "pin": self.pin}
        arguments.update(overrides)
        return convert(self.data, **arguments)

    @staticmethod
    def _bottom(glyph: Glyph) -> int:
        return int(glyph.adjust_y - glyph.height)

    def test_e_o_and_r_share_a_bottom_row_at_the_psp_sizes(self) -> None:
        for ppem in self.SIZES:
            with self.subTest(ppem=ppem):
                plain = _glyph_map(self._convert(ppem).glyphs)
                # Without the option the overshoot is a faint extra row under the round letters.
                self.assertEqual([self._bottom(plain[ord(c)]) for c in "eoR"], [-1, -1, 0])
                self.assertLess(max(plain[ord("e")].samples[-plain[ord("e")].width:]), 8, "under half coverage")
                glyphs = _glyph_map(self._convert(ppem, snap_overshoot=3).glyphs)
                self.assertEqual([self._bottom(glyphs[ord(c)]) for c in "eoR"], [0, 0, 0])
                self.assertEqual(glyphs[ord("e")].adjust_y, glyphs[ord("x")].adjust_y)
                self.assertEqual(glyphs[ord("o")].adjust_y, glyphs[ord("x")].adjust_y)
                self.assertEqual(glyphs[ord("O")].adjust_y, glyphs[ord("H")].adjust_y)
                # Flat letters and the comma's tail are drawn exactly as before; p keeps its descender.
                for char in "RHxlv.,":
                    self.assertEqual(glyphs[ord(char)], plain[ord(char)], char)
                self.assertEqual(self._bottom(glyphs[ord("p")]), self._bottom(plain[ord("p")]))

    def test_only_outer_overshoots_in_the_band_move(self) -> None:
        zones = (0, 1000, 1400)  # baseline, x-height, cap height in font units
        upem, ppem, threshold = 2048, 20, 3  # band: depths under 3 * 2048 / (8 * 20) = 38.4 units

        def box(left: int, bottom: int, right: int, top: int) -> list[tuple]:
            return [(left, bottom, True), (right, bottom, True), (right, top, True), (left, top, True)]

        round_letter = box(0, -20, 500, 1020)
        counter = box(100, -10, 400, 1010)       # enclosed by round_letter: a hole, never moved
        deep = box(600, -39, 700, 500)            # 39 units is past the band
        accent = box(800, 1200, 900, 1430)        # wholly above the x-height: an accent, never moved
        capital = box(1000, 0, 1400, 1430)
        moved = ttf2pgf._snap_overshoot([round_letter, counter, deep, accent, capital], zones, threshold, ppem, upem)
        self.assertEqual(moved, [box(0, 0, 500, 1000), counter, deep, accent, box(1000, 0, 1400, 1400)])
        # Off-curve points move like on-curve ones, and x never changes.
        curve = [(0, -20, True), (250, -20, False), (500, 300, True), (250, 1030, False)]
        self.assertEqual(ttf2pgf._snap_overshoot([curve], zones, threshold, ppem, upem),
                         [[(0, 0, True), (250, 0, False), (500, 300, True), (250, 1000, False)]])

    def test_the_band_ends_just_short_of_n_eighths_of_a_pixel(self) -> None:
        # At 16 ppem and 2048 units per em, 3/8 px is exactly 48 units: depth * 16 * 8 < 3 * 2048.
        zones, upem, ppem = (0, 1000, 1400), 2048, 16
        for depth, moves in ((47, True), (48, False)):
            with self.subTest(depth=depth):
                below = [(0, -depth, True), (100, -depth, True), (100, 500, True), (0, 500, True)]
                above = [(0, 0, True), (100, 0, True), (100, 1000 + depth, True), (0, 1000 + depth, True)]
                snapped = ttf2pgf._snap_overshoot([below, above], zones, 3, ppem, upem)
                self.assertEqual(min(y for _x, y, _on in snapped[0]), 0 if moves else -depth)
                self.assertEqual(max(y for _x, y, _on in snapped[1]), 1000 if moves else 1000 + depth)
                # One sub-sample more widens the band past the same depth.
                wider = ttf2pgf._snap_overshoot([below], zones, 4, ppem, upem)
                self.assertEqual(min(y for _x, y, _on in wider[0]), 0)

    def test_the_option_is_recorded_and_absent_when_off(self) -> None:
        conversion = self._convert(13, snap_overshoot=3)
        self.assertEqual(conversion.manifest["converter"]["version"], "0.8.0")
        self.assertEqual(conversion.manifest["parameters"]["snap_overshoot"], {
            "unit": "sub-sample (1/8 pixel)", "threshold": 3,
            "zones": {"baseline": 0, "x_height": 1082, "cap_height": 1409},
        })
        self.assertNotIn("snap_overshoot", self._convert(13).manifest["parameters"])
        self.assertNotIn("snap_overshoot", self._convert(13, snap_overshoot=0).manifest["parameters"])
        self.assertEqual(conversion.image, self._convert(13, snap_overshoot=3).image, "deterministic")

    def test_misuse_is_refused(self) -> None:
        for value in (-1, 9, True):
            with self.subTest(value=value), self.assertRaises(TtfConvertError) as caught:
                self._convert(13, snap_overshoot=value)
            self.assertEqual(caught.exception.reason, "snap-overshoot-out-of-range")
        # At 2 ppem a band of one pixel above the x-height (1082) reaches the cap height (1409).
        with self.assertRaises(TtfConvertError) as caught:
            self._convert(2, snap_overshoot=8)
        self.assertEqual(caught.exception.reason, "snap-overshoot-zones-overlap")
        real = ttf2pgf._Ttf.glyph_id

        def without_x(font: ttf2pgf._Ttf, code: int) -> int | None:
            return None if code == 0x78 else real(font, code)

        with mock.patch.object(ttf2pgf._Ttf, "glyph_id", without_x), self.assertRaises(TtfConvertError) as caught:
            self._convert(13, snap_overshoot=3, code_points=_code_set({0x48}))
        self.assertEqual(caught.exception.reason, "snap-overshoot-unavailable")
        self.assert_cli_refused("snap-overshoot-out-of-range", self.tmp / "out.pgf",
                                LIBERATION_DIR / "LiberationSans-Regular.ttf", "--codes", "0x48-0x48",
                                "--snap-overshoot", "9")
        self.assert_cli_refused("snap-overshoot-zones-overlap", self.tmp / "out.pgf",
                                LIBERATION_DIR / "LiberationSans-Regular.ttf", "--codes", "0x48-0x48",
                                "--ppem", "2", "--snap-overshoot", "8")

    def test_a_fallback_font_uses_its_own_edges(self) -> None:
        gudea = FONT.read_bytes()
        mplus = JAPANESE_FONT.read_bytes()
        conversion = convert(gudea, input_path=str(FONT), output_path="out.pgf", code_points=_code_set({0x48, 0x3042}),
                             ppem=13, pin=load_pin(PIN, gudea), fallback_data=mplus, fallback_path=str(JAPANESE_FONT),
                             fallback_pin=load_pin(JAPANESE_PIN, mplus), snap_overshoot=3)
        snap = conversion.manifest["parameters"]["snap_overshoot"]
        expected = {}
        for key, data in (("zones", gudea), ("fallback_zones", mplus)):
            base, x_height, cap = ttf2pgf._overshoot_zones(ttf2pgf._Ttf(data), "font")
            expected[key] = {"baseline": base, "x_height": x_height, "cap_height": cap}
        self.assertEqual({key: snap[key] for key in expected}, expected)
        self.assertNotEqual(snap["zones"], snap["fallback_zones"])


class ProjectCjkSizeTests(unittest.TestCase):
    """#2005: the Japanese and Korean fonts are set at the console's 10.125 pt (18 px at 128 dpi),
    where they used to be 12 px. A kanji or kana moves 18 px, as on the console, and a Hangul
    syllable about 18.36 px on average (the console's measured average)."""

    KANA_KANJI = {0x3042, 0x30A2, 0x4E00, 0x6F22, 0x5B57, 0x9ED1, 0x5F0C, 0x3001}

    def _convert(self, variable: str, data: bytes, codes: set[int], **extra: object) -> ttf2pgf.Conversion:
        arguments = _convert_arguments(_makefile_flags(variable))
        arguments.pop("baseline_ellipsis", None)
        arguments.update(extra)
        return convert(data, input_path="font.ttf", output_path="cjk.pgf", code_points=_code_set(codes),
                       **arguments)

    def test_japanese_font_is_set_at_ten_point_one_two_five(self) -> None:
        conversion = self._convert("PROJECT_JAPANESE_FONT_ARGS", NOTO_JP_FONT.read_bytes(), self.KANA_KANJI,
                                   font_name="Nakagawa Open Japanese")
        self.assertEqual(struct.unpack_from("<iiii", conversion.image, 0x24), (648, 648, 8192, 8192))
        self.assertEqual(struct.unpack_from("<ii", conversion.image, 0xD4), (1013, -139))
        # #1300, rule 16: one em down, from an origin 0.88 em above the baseline.
        self.assertEqual(struct.unpack_from("<i", conversion.image, 0xF0)[0], 18 * 64)
        for glyph in conversion.glyphs:
            with self.subTest(code=f"U+{glyph.code:04X}"):
                self.assertEqual(glyph.advance_x, 18 * 64)
                self.assertEqual((glyph.advance_y, glyph.y_top), (18 * 64, 1014 - cast(int, glyph.y_base)))

    def test_korean_font_is_set_at_ten_point_one_two_five(self) -> None:
        hangul = set(ttf2pgf.ks_x_1001_korean_code_points().codes) & set(range(0xAC00, 0xD7A4))
        sample = set(sorted(hangul)[::10])
        conversion = self._convert("PROJECT_KOREAN_FONT_ARGS", KOREAN_FONT.read_bytes(), sample,
                                   font_name="Nakagawa Open Korean")
        self.assertEqual(struct.unpack_from("<iiii", conversion.image, 0x24), (648, 648, 8192, 8192))
        self.assertEqual(struct.unpack_from("<ii", conversion.image, 0xD4), (1000, -250))
        # #1300, rule 16: 1.185 em down, bearing Y at the margin above the ascender.
        self.assertEqual({(glyph.advance_y, glyph.y_top) for glyph in conversion.glyphs}, {(1365, 57)})
        average = sum(cast(int, glyph.advance_x) for glyph in conversion.glyphs) / 64 / len(conversion.glyphs)
        self.assertLessEqual(abs(average - 18.356), 0.25)


class SpacingAndFallbackOptionTests(CliRefusalMixin):
    """#2005: tracking, tabular digits and a second font are explicit, recorded and bounded options."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.pin = load_pin(PIN, self.data)
        self.tmp = Path(tempfile.mkdtemp(prefix="ttf2pgf-spacing-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _convert(self, **overrides: object) -> ttf2pgf.Conversion:
        arguments: dict = {"input_path": str(FONT), "output_path": "out.pgf", "codes": (0x30, 0x48), "ppem": 13,
                           "pin": self.pin}
        arguments.update(overrides)
        return convert(self.data, **arguments)

    def test_tracking_adds_sub_samples_to_every_advance_and_is_recorded(self) -> None:
        plain = _glyph_map(self._convert().glyphs)
        tracked = self._convert(tracking=3)
        self.assertEqual(tracked.manifest["parameters"]["tracking_sub_samples"], 3)
        self.assertNotIn("tracking_sub_samples", self._convert().manifest["parameters"])
        for code, glyph in _glyph_map(tracked.glyphs).items():
            self.assertEqual(glyph.advance_x, cast(int, plain[code].advance_x) + 3 * 8)
            self.assertEqual(glyph.samples, plain[code].samples)
        tighter = _glyph_map(self._convert(tracking=-2).glyphs)
        self.assertEqual(tighter[0x48].advance_x, cast(int, plain[0x48].advance_x) - 16)

    def test_digit_advance_makes_tabular_digits_centred_in_their_change(self) -> None:
        plain = _glyph_map(self._convert().glyphs)
        conversion = self._convert(digit_advance_26_6=640)
        self.assertEqual(conversion.manifest["parameters"]["digit_advance_26_6"], 640)
        glyphs = _glyph_map(conversion.glyphs)
        for code in range(0x30, 0x3A):
            natural = cast(int, plain[code].advance_x)
            self.assertEqual(glyphs[code].advance_x, 640)
            # The outline moves by half the change, so the ink stays centred.
            self.assertLessEqual(abs(glyphs[code].adjust_x - plain[code].adjust_x - (640 - natural) // 128), 1)
        self.assertEqual(glyphs[0x48].advance_x, plain[0x48].advance_x, "letters keep their own advance")

    def test_digit_advance_is_centred_when_metric_targets_choose_the_ppem(self) -> None:
        # The ppem search rasterizes the digits before their shift is known; the shift must still apply.
        targets = ttf2pgf._MetricTargets(
            ascender_px=9, descender_px=-2, tolerance_px=1, ppem_min=6, ppem_max=24,
        )
        searched = self._convert(ppem=None, metric_targets=targets, digit_advance_26_6=640)
        ppem = searched.manifest["parameters"]["ppem"]
        direct = _glyph_map(self._convert(ppem=ppem, digit_advance_26_6=640).glyphs)
        plain = _glyph_map(self._convert(ppem=ppem).glyphs)
        glyphs = _glyph_map(searched.glyphs)
        self.assertTrue(
            any(direct[code].adjust_x != plain[code].adjust_x for code in range(0x30, 0x3A)),
            "the chosen ppem must move at least one digit, or this test proves nothing",
        )
        for code in range(0x30, 0x3A):
            with self.subTest(code=hex(code)):
                self.assertEqual(glyphs[code].advance_x, 640)
                self.assertEqual(glyphs[code].adjust_x, direct[code].adjust_x)
                self.assertEqual(glyphs[code].samples, direct[code].samples)

    def test_scale_x_widens_outlines_and_advances_only_and_is_recorded(self) -> None:
        """#1300: ``--scale-x`` (spec section 3, rule 14) scales x in font units before rule 1."""
        plain = self._convert()
        self.assertNotIn("scale_x_thousandths", plain.manifest["parameters"])
        neutral = self._convert(scale_x=ttf2pgf.SCALE_X_NEUTRAL)
        self.assertEqual(neutral.image, plain.image, "1000 is byte-identical to no option")
        wide = self._convert(scale_x=1200)
        self.assertEqual(wide.manifest["parameters"]["scale_x_thousandths"], 1200)
        font = ttf2pgf._Ttf(self.data)
        before, after = _glyph_map(plain.glyphs), _glyph_map(wide.glyphs)
        for code, glyph in after.items():
            with self.subTest(code=hex(code)):
                units = font.advance_units(cast(int, font.glyph_id(code)))
                self.assertEqual(glyph.advance_x,
                                 ttf2pgf._to26_6(Fraction(units * 1200, 1000), 13, font.units_per_em))
                # Rows are untouched: same top row and height.
                self.assertEqual((glyph.adjust_y, glyph.height), (before[code].adjust_y, before[code].height))
        h_plain, h_wide = before[0x48], after[0x48]
        self.assertGreater(h_wide.width, h_plain.width)
        self.assertGreater(sum(h_wide.samples), sum(h_plain.samples))
        self.assertEqual(self._convert(scale_x=1200).image, wide.image, "deterministic")

    def test_scale_x_cli_matches_the_api_and_misuse_is_refused(self) -> None:
        output = self.tmp / "wide.pgf"
        result = _cli(output, FONT, "--codes", "0x30-0x48", "--ppem", "13", "--scale-x", "1080")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(output.read_bytes(), self._convert(scale_x=1080, pin=None, output_path=str(output)).image)
        for value in (ttf2pgf.MIN_SCALE_X - 1, ttf2pgf.MAX_SCALE_X + 1, True):
            with self.subTest(value=value), self.assertRaises(TtfConvertError) as caught:
                self._convert(scale_x=value)
            self.assertEqual(caught.exception.reason, "scale-x-out-of-range")
        self.assert_cli_refused("scale-x-out-of-range", self.tmp / "out.pgf", FONT, "--codes", "0x48-0x48",
                                "--scale-x", "1251")

    def test_vertical_options_cli_match_the_api_and_misuse_is_refused(self) -> None:
        output = self.tmp / "vertical.pgf"
        result = _cli(output, FONT, "--codes", "0x30-0x48", "--ppem", "13", "--vertical-advance", "917",
                      "--vertical-origin", "880")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            output.read_bytes(),
            self._convert(vertical_advance=917, vertical_origin=880, pin=None, output_path=str(output)).image,
        )
        for kwargs in (
            {"vertical_advance": ttf2pgf.MIN_VERTICAL - 1},
            {"vertical_advance": ttf2pgf.MAX_VERTICAL + 1},
            {"vertical_origin": ttf2pgf.MIN_VERTICAL - 1},
            {"vertical_origin": True},
        ):
            with self.subTest(**kwargs), self.assertRaises(TtfConvertError) as caught:
                self._convert(**kwargs)
            self.assertEqual(caught.exception.reason, "vertical-metric-out-of-range")
        self.assert_cli_refused("vertical-metric-out-of-range", self.tmp / "out.pgf", FONT, "--codes", "0x48-0x48",
                                "--vertical-advance", "0")

    def test_spacing_options_out_of_range_are_refused(self) -> None:
        for kwargs, reason in (
            ({"tracking": ttf2pgf.MAX_TRACKING + 1}, "tracking-out-of-range"),
            ({"tracking": ttf2pgf.MIN_TRACKING - 1}, "tracking-out-of-range"),
            ({"tracking": True}, "tracking-out-of-range"),
            ({"digit_advance_26_6": 63}, "digit-advance-out-of-range"),
            ({"digit_advance_26_6": 640, "codes": (0x41, 0x48)}, "digit-advance-unavailable"),
        ):
            with self.subTest(kwargs=kwargs), self.assertRaises(TtfConvertError) as caught:
                self._convert(**kwargs)
            self.assertEqual(caught.exception.reason, reason)
        self.assert_cli_refused("tracking-out-of-range", self.tmp / "out.pgf", FONT, "--codes", "0x48-0x48",
                                "--tracking", "17")

    def test_fallback_draws_only_what_the_first_font_lacks_and_is_recorded(self) -> None:
        mplus = JAPANESE_FONT.read_bytes()
        mplus_pin = load_pin(JAPANESE_PIN, mplus)
        codes = _code_set({0x41, 0x48, 0x3042})
        conversion = self._convert(codes=None, code_points=codes, fallback_data=mplus,
                                   fallback_path=str(JAPANESE_FONT), fallback_pin=mplus_pin)
        fallback = conversion.manifest["fallback"]
        self.assertEqual(fallback["codes"], [0x3042])
        self.assertEqual(fallback["input"]["sha256"], hashlib.sha256(mplus).hexdigest())
        self.assertEqual(fallback["input"]["path"], "MPLUS1p-Regular.ttf")
        self.assertEqual(fallback["license"]["spdx"], "OFL-1.1")
        alone = convert(mplus, input_path=str(JAPANESE_FONT), output_path="out.pgf",
                        code_points=_code_set({0x3042}), ppem=13, pin=mplus_pin)
        self.assertEqual(_glyph_map(conversion.glyphs)[0x3042].samples, alone.glyphs[0].samples)
        plain = _glyph_map(self._convert(codes=(0x41, 0x48)).glyphs)
        self.assertEqual(_glyph_map(conversion.glyphs)[0x48].samples, plain[0x48].samples)
        self.assertNotIn("fallback", self._convert().manifest)

    def test_fallback_misuse_is_refused(self) -> None:
        mplus = JAPANESE_FONT.read_bytes()
        mplus_pin = load_pin(JAPANESE_PIN, mplus)
        for kwargs, reason in (
            ({"fallback_data": mplus, "fallback_path": "m.ttf", "fallback_pin": mplus_pin}, "fallback-unused"),
            ({"fallback_data": mplus, "fallback_path": None}, "conflicting-parameters"),
            ({"fallback_data": mplus, "fallback_path": "m.ttf"}, "conflicting-parameters"),
            ({"fallback_pin": mplus_pin}, "conflicting-parameters"),
        ):
            with self.subTest(kwargs=sorted(kwargs)), self.assertRaises(TtfConvertError) as caught:
                self._convert(**kwargs)
            self.assertEqual(caught.exception.reason, reason)

    def test_a_static_font_with_a_stat_table_is_accepted(self) -> None:
        """Noto Sans JP's static instance carries STAT, which static family members carry too."""
        font = ttf2pgf._Ttf(NOTO_JP_FONT.read_bytes())
        self.assertIsNotNone(font.glyph_id(0x4E00))
        self.assertNotIn("STAT", ttf2pgf.VARIABLE_FONT_TABLES)


class NameDenylistTests(unittest.TestCase):
    """Output names: vendor camouflage and reserved or trademark names are refused by name."""

    def test_vendor_camouflage_names_are_refused(self) -> None:
        names = (
            _joined("F", "TT-", "New", "Rodin Pro Latin"),
            _joined("f", "tt-", "new", "rodin"),
            _joined("Asia", "NHH", "(512Johab)"),
            _joined("Asia", "KNHH", "-", "SO", "NY", "-uni"),
            _joined("Font", "works", " Latin"),
            _joined("SO", "NY", " Latin"),
        )
        for name in names:
            with self.subTest(name=name):
                with self.assertRaises(TtfConvertError) as caught:
                    ttf2pgf.validate_output_name(name, None)
                self.assertEqual(caught.exception.reason, "camouflage-font-name")

    def test_reserved_and_trademark_names_are_refused(self) -> None:
        names = (
            "Noto Sans JP", "Lato Display", "Atkinson Hyperlegible Next", "Nanum Gothic",
            "Sawarabi Gothic", "Gowun Dodum", "M PLUS 1p", "Ume Hy Gothic", "Source Han Sans",
        )
        for name in names:
            with self.subTest(name=name):
                with self.assertRaises(TtfConvertError) as caught:
                    ttf2pgf.validate_output_name(name, None)
                self.assertEqual(caught.exception.reason, "reserved-font-name")

    def test_project_names_and_ordinary_words_are_accepted(self) -> None:
        for name in ("Nakagawa Open Japanese", "Nakagawa Open Latin", "Nakagawa Open Korean",
                     "Platonic Serif", "Volume Mono", "Sonyx Latin"):
            with self.subTest(name=name):
                ttf2pgf.validate_output_name(name, None)  # must not raise


class VendorFontNameScanTests(unittest.TestCase):
    """Publication scan: no tracked source file names a vendor font.

    The scan covers every tracked file except Markdown and binary files. Markdown is the
    provenance record (docs/provenance/FONT_ORIGINS.md) that documents where each name was
    found, so it names the vendor fonts on purpose.

    The marker set is this class's own literal list, not read from the converter's
    denylist, so weakening that denylist cannot hide a name from this scan. It is narrower
    than the denylist on one point, deliberately: the console maker's name is not a
    whole-word marker here. That name is the platform's own identity and appears
    legitimately across tracked source (the PSP's device identifiers, the non-affiliation
    notice, third-party attribution), so a whole-word match would fail on those lines.
    Consequently a maker-branded font name in tracked source passes this scan; the
    converter's denylist refuses it at output instead.
    Each name is split across literals so this file does not contain it whole.
    """

    PATTERN = re.compile(
        "|".join(
            re.escape(part)
            for part in (_joined("New", "Rodin"), _joined("Asia", "KNHH"),
                         _joined("Asia", "NHH"), _joined("Font", "works"))
        )
        + r"|(?<![0-9A-Za-z])" + re.escape(_joined("F", "TT")) + r"(?![0-9A-Za-z])",
        re.IGNORECASE,
    )

    @classmethod
    def _hit_lines(cls, text: str) -> list[int]:
        """1-based numbers of the lines in ``text`` that the scan flags."""
        return [number for number, line in enumerate(text.splitlines(), start=1)
                if cls.PATTERN.search(line)]

    def test_scan_catches_each_listed_marker(self) -> None:
        # The tree test passes vacuously if the pattern is broken, so prove it fires.
        markers = (
            _joined("New", "Rodin"), _joined("Asia", "KNHH"), _joined("Asia", "NHH"),
            _joined("Font", "works"), _joined("F", "TT"),
        )
        for marker in markers:
            with self.subTest(marker=marker):
                self.assertEqual(self._hit_lines("font = '" + marker + "'\n"), [1])

    def test_whole_word_marker_needs_a_word_boundary(self) -> None:
        self.assertEqual(self._hit_lines("x = 'X" + _joined("F", "TT") + "Y'\n"), [])

    def test_platform_prose_naming_the_console_maker_is_not_flagged(self) -> None:
        # The maker's name is deliberately not a marker here (see the class docstring).
        prose = "# Not affiliated with " + _joined("SO", "NY") + " Interactive Entertainment.\n"
        self.assertEqual(self._hit_lines(prose), [])

    def test_tracked_source_names_no_vendor_font(self) -> None:
        if shutil.which("git") is None:
            self.skipTest("git is required to enumerate the tracked tree")
        raw_listing = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "-z"], capture_output=True, check=True,
        ).stdout
        offenders: list[str] = []
        unreadable: list[str] = []
        for raw_entry in sorted(filter(None, raw_listing.split(b"\0"))):
            entry = raw_entry.decode("utf-8", errors="surrogateescape")
            if entry.lower().endswith(".md"):
                continue
            try:
                data = (ROOT / entry).read_bytes()
            except OSError as exc:
                unreadable.append(f"{entry}: {exc}")
                continue
            if b"\0" in data:
                continue
            text = data.decode("utf-8", errors="replace")
            offenders.extend(f"{entry}:{number}" for number in self._hit_lines(text))
        self.assertEqual(unreadable, [], "every tracked file must be readable by the vendor font scan")
        self.assertEqual(offenders, [], "tracked source names a vendor font; use a project-owned name")



class IsolatedInterpreterTests(unittest.TestCase):
    """The release bundles Python's embeddable distribution, whose ._pth file keeps the
    script's folder off sys.path. `-I` gives a normal interpreter the same import path, so
    BUILD PACKAGE's project-font step must still find its sibling pgf_writer module."""

    def test_script_imports_its_sibling_module_without_the_script_folder_on_sys_path(self) -> None:
        result = subprocess.run(
            [sys.executable, "-I", str(TOOL), "--help"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("usage", result.stdout.lower())

if __name__ == "__main__":
    unittest.main()
