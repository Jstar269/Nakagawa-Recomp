# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for enhancement package declaration schema and validator.

Tests fail-closed boundaries:
- Valid synthetic package passes.
- Unknown category is rejected (including obsolete duplicate guest_hook).
- Incompatible schema version is rejected.
- Address-literal rejection across hook symbols and capability names (0x-hex,
  decompiler-style sub_/loc_/func_ stems, bare 6-8 digit hex strings including
  all-letter ones like 'deadbeef', and numeric addresses).
- Speculative category payload objects and undeclared properties fail closed.
- Zero jsonschema dependency: normative validator runs in pure Python.
"""

from __future__ import annotations

import copy
import decimal
import difflib
import hashlib
import itertools
from io import StringIO
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import codegen
import codegen_llvm
import enhancement_package
import lowering_plan
from enhancement_package import (
    AOT_ENHANCEMENT_PACKAGE_EPOCH,
    HOOK_KEYS,
    HOOK_TYPES,
    REQUIRED_ROOT_KEYS,
    RNG_POLICY_MODES,
    ROOT_KEYS,
    SITE_TRANSFORMS,
    SUPPORTED_SCHEMA_VERSION,
    UPDATE_BOUNDARY_KEYS,
    VALID_CATEGORIES,
    EnhancementPackageError,
    _gpr_written,
    _scale_value,
    _validate_data_files,
    _validate_site_extras,
    compute_stock_words_digest,
    draft_package_declaration,
    draft_timing_package,
    find_data_file,
    load_package,
    plan_data_value,
    plan_float_pair_rewrite,
    plan_site_rewrite,
    transform_data_file,
    validate_package,
    verify_and_resolve_data_sites,
    verify_and_resolve_sites,
)
from nk_core import package_cache

SCHEMA_PATH = ROOT / "assets" / "enhancement_package.schema.json"
GCC = shutil.which("gcc") or shutil.which("cc")


def _f32_decay_reference(k: float) -> int:
    """The f32 bits nearest 1 - sqrt(1 - k), evaluated to 60 decimal digits."""
    with decimal.localcontext() as ctx:
        ctx.prec = 60
        exact = 1 - (1 - decimal.Decimal(k)).sqrt()
    return _bits_of(float(exact))



class TestEnhancementPackageSchema(unittest.TestCase):
    """Test enhancement package schema validation and fail-closed rules."""

    def setUp(self) -> None:
        self.valid_presentation_pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.overlay.fps",
            "display_name": "Synthetic Frame Rate Overlay",
            "version": "1.0.0",
            "category": "presentation_only",
            "target_title_id": "*",
            "capabilities": ["hud_overlay", "fps_counter"],
        }

        self.valid_asset_pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.textures.hd",
            "display_name": "Synthetic HD Texture Pack",
            "version": "0.1.0",
            "category": "asset_replacement",
            "target_title_id": "synthetic-allegrex-v1",
            "capabilities": ["texture_override"],
        }

        self.valid_hook_pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.hook.intro_skip",
            "display_name": "Synthetic Intro Skip Hook",
            "version": "1.0.0",
            "category": "guest_memory_code_hook",
            "target_title_id": "synthetic-allegrex-v1",
            "capabilities": ["milestone_skip"],
            "hooks": [
                {
                    "symbol": "title_intro_movie_start",
                    "type": "pre_call",
                }
            ],
        }

    def test_schema_file_matches_python_contract(self) -> None:
        """The checked-in schema must be valid JSON Draft 2020-12 matching Python contract."""
        self.assertEqual(SUPPORTED_SCHEMA_VERSION, 1)
        self.assertTrue(SCHEMA_PATH.is_file(), f"Schema file missing: {SCHEMA_PATH}")
        with SCHEMA_PATH.open("r", encoding="utf-8") as f:
            schema = json.load(f)

        self.assertEqual(schema.get("$schema"), "https://json-schema.org/draft/2020-12/schema")
        self.assertFalse(schema.get("additionalProperties"))
        self.assertEqual(schema.get("properties", {}).get("schema_version", {}).get("const"), 1)
        self.assertEqual(set(schema.get("required", [])), REQUIRED_ROOT_KEYS)
        # A published package (section 5.1) adds its own root properties, required together.
        self.assertEqual(set(schema.get("properties", {}).keys()),
                         ROOT_KEYS | enhancement_package.PUBLISHED_KEYS)
        self.assertEqual(set(schema["dependentRequired"]["published_package"]),
                         (enhancement_package.PUBLISHED_KEYS - {"published_package"}) | {"executable_sha256"})
        self.assertEqual(schema["properties"]["digest_scheme"]["const"],
                         enhancement_package.PUBLISHED_DIGEST_SCHEME)

        # Category enum must exactly match VALID_CATEGORIES
        schema_categories = set(schema["properties"]["category"]["enum"])
        self.assertEqual(schema_categories, VALID_CATEGORIES)
        self.assertNotIn("guest_hook", schema_categories)

        # Hooks definition must be symbol + type only
        hook_item = schema["properties"]["hooks"]["items"]
        self.assertFalse(hook_item.get("additionalProperties"))
        self.assertEqual(set(hook_item.get("required", [])), HOOK_KEYS)
        self.assertEqual(set(hook_item.get("properties", {}).keys()), HOOK_KEYS)
        self.assertEqual(set(hook_item["properties"]["type"]["enum"]), HOOK_TYPES)

        # Speculative category payload objects must NOT exist in schema
        for speculative in ("assets", "presentation", "timing", "camera", "save_data"):
            self.assertNotIn(speculative, schema.get("properties", {}))

        # Site descriptors: insn_offset is a byte offset the validator requires to be 4-aligned.
        site_props = schema["properties"]["sites"]["items"]["properties"]
        self.assertEqual(site_props["insn_offset"].get("multipleOf"), 4)
        self.assertIn("Byte offset", site_props["insn_offset"]["description"])
        self.assertEqual(set(site_props["transform"]["enum"]), SITE_TRANSFORMS)

        # The update boundary is one pinned instruction: function, offset, digest.
        boundary = schema["properties"]["update_boundary"]
        self.assertFalse(boundary.get("additionalProperties"))
        self.assertEqual(set(boundary["required"]), UPDATE_BOUNDARY_KEYS)
        self.assertEqual(set(boundary["properties"]), UPDATE_BOUNDARY_KEYS)
        self.assertEqual(boundary["properties"]["insn_offset"].get("multipleOf"), 4)

    def test_load_package_from_disk(self) -> None:
        """load_package must correctly read and parse JSON packages."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            file_path = Path(tmp_dir) / "test_pkg.json"
            file_path.write_text(json.dumps(self.valid_presentation_pkg), encoding="utf-8")
            loaded = load_package(file_path)
            self.assertEqual(loaded["id"], "synth.overlay.fps")

            # Missing file raises EnhancementPackageError
            missing_path = Path(tmp_dir) / "nonexistent.json"
            with self.assertRaises(EnhancementPackageError):
                load_package(missing_path)

            # Malformed JSON raises EnhancementPackageError
            bad_json_path = Path(tmp_dir) / "bad.json"
            bad_json_path.write_text("{invalid_json", encoding="utf-8")
            with self.assertRaises(EnhancementPackageError):
                load_package(bad_json_path)

            # Non-dict root raises EnhancementPackageError
            array_json_path = Path(tmp_dir) / "array.json"
            array_json_path.write_text("[]", encoding="utf-8")
            with self.assertRaises(EnhancementPackageError):
                load_package(array_json_path)

    def test_valid_synthetic_packages_pass_validation(self) -> None:
        """Standard synthetic packages across categories must validate cleanly."""
        validated_pres = validate_package(self.valid_presentation_pkg)
        self.assertEqual(validated_pres["category"], "presentation_only")

        validated_asset = validate_package(self.valid_asset_pkg)
        self.assertEqual(validated_asset["category"], "asset_replacement")

        validated_hook = validate_package(self.valid_hook_pkg)
        self.assertEqual(validated_hook["category"], "guest_memory_code_hook")

        # Package with optional min_runtime_version passes
        pkg_with_min_rt = copy.deepcopy(self.valid_presentation_pkg)
        pkg_with_min_rt["min_runtime_version"] = "1.0.0"
        self.assertEqual(validate_package(pkg_with_min_rt)["min_runtime_version"], "1.0.0")

    def test_unknown_category_rejected(self) -> None:
        """Unknown or arbitrary category strings must fail closed."""
        pkg = copy.deepcopy(self.valid_presentation_pkg)
        pkg["category"] = "unsupported_magic_accelerator"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg)
        self.assertIn("Unknown enhancement category", str(ctx.exception))

        # Obsolete duplicate 'guest_hook' is explicitly rejected
        pkg_guest_hook = copy.deepcopy(self.valid_presentation_pkg)
        pkg_guest_hook["category"] = "guest_hook"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_guest_hook)
        self.assertIn("Unknown enhancement category", str(ctx.exception))

    def test_incompatible_version_rejected(self) -> None:
        """Schema versions other than 1 must be rejected."""
        # Future version
        pkg_future = copy.deepcopy(self.valid_presentation_pkg)
        pkg_future["schema_version"] = 2
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_future)
        self.assertIn("Incompatible schema version", str(ctx.exception))

        # Obsolete / zero version
        pkg_zero = copy.deepcopy(self.valid_presentation_pkg)
        pkg_zero["schema_version"] = 0
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_zero)
        self.assertIn("Incompatible schema version", str(ctx.exception))

        # Boolean schema version
        pkg_bool = copy.deepcopy(self.valid_presentation_pkg)
        pkg_bool["schema_version"] = True
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_bool)
        self.assertIn("Incompatible schema version", str(ctx.exception))

        # Missing schema version
        pkg_missing = copy.deepcopy(self.valid_presentation_pkg)
        del pkg_missing["schema_version"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_missing)
        self.assertIn("Missing required property 'schema_version'", str(ctx.exception))

    def test_address_literal_rejection_in_hook_symbols(self) -> None:
        """Hook symbols must reject 0x-hex, decompiler stems, bare hex, and numeric addresses."""
        cases = [
            ("0x08804000", "0x-prefixed hex address"),
            ("0xdeadbeef", "0x-prefixed all-letter hex"),
            ("0X08804000", "0X-prefixed uppercase hex"),
            ("sub_08804000", "decompiler sub_ stem with hex"),
            ("sub_deadbeef", "decompiler sub_ stem with all-letter hex"),
            ("loc_08804000", "decompiler loc_ stem with hex"),
            ("loc_deadbeef", "decompiler loc_ stem with all-letter hex"),
            ("func_08804000", "decompiler func_ stem with hex"),
            ("func_deadbeef", "decompiler func_ stem with all-letter hex"),
            ("08804000", "bare 8-digit hex string"),
            ("123456", "bare 6-digit hex string"),
            ("12345678", "bare 8-digit numeric hex string"),
            ("deadbeef", "bare 8-digit all-letter hex string"),
            ("cafebabe", "bare 8-digit all-letter hex string cafebabe"),
            ("abcdef", "bare 6-digit all-letter hex string abcdef"),
            (142622720, "numeric integer address literal"),
        ]

        for symbol_val, label in cases:
            with self.subTest(symbol=symbol_val, label=label):
                pkg = copy.deepcopy(self.valid_hook_pkg)
                pkg["hooks"][0]["symbol"] = symbol_val
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn("Address-literal hook symbol rejected", str(ctx.exception))

    def test_address_literal_rejection_in_capabilities(self) -> None:
        """Capability names must reject 0x-hex, decompiler stems, bare hex, and numeric addresses."""
        cases = [
            ("0x08804000", "0x-prefixed hex address"),
            ("0xdeadbeef", "0x-prefixed all-letter hex"),
            ("sub_08804000", "decompiler sub_ stem with hex"),
            ("sub_deadbeef", "decompiler sub_ stem with all-letter hex"),
            ("loc_08804000", "decompiler loc_ stem with hex"),
            ("loc_deadbeef", "decompiler loc_ stem with all-letter hex"),
            ("func_08804000", "decompiler func_ stem with hex"),
            ("func_deadbeef", "decompiler func_ stem with all-letter hex"),
            ("08804000", "bare 8-digit hex string"),
            ("123456", "bare 6-digit hex string"),
            ("deadbeef", "bare 8-digit all-letter hex string"),
            ("cafebabe", "bare 8-digit all-letter hex string cafebabe"),
            ("abcdef", "bare 6-digit all-letter hex string abcdef"),
            (142622720, "numeric integer address literal"),
        ]

        for cap_val, label in cases:
            with self.subTest(capability=cap_val, label=label):
                pkg = copy.deepcopy(self.valid_presentation_pkg)
                pkg["capabilities"] = [cap_val]
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn("Address-literal capability name rejected", str(ctx.exception))

    def test_speculative_payload_objects_rejected(self) -> None:
        """Speculative category payload objects and extra properties must fail closed."""
        speculative_keys = [
            ("assets", [{"asset_id": "tex1", "replacement_path": "a.png", "kind": "texture"}]),
            ("presentation", {"overlay_enabled": True}),
            ("timing", {"target_fps": 60}),
            ("camera", {"fov_multiplier": 1.5}),
            ("save_data", {"format_version": 1}),
            ("description", "A description string"),
            ("author", "Author Name"),
            ("untrusted_script", "print('hello')"),
        ]

        for key, value in speculative_keys:
            with self.subTest(key=key):
                pkg = copy.deepcopy(self.valid_presentation_pkg)
                pkg[key] = value
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn("Unknown properties forbidden", str(ctx.exception))

        # Extraneous properties in hook item must also fail closed
        hook_pkg_with_desc = copy.deepcopy(self.valid_hook_pkg)
        hook_pkg_with_desc["hooks"][0]["description"] = "A description in hook"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(hook_pkg_with_desc)
        self.assertIn("Unknown properties forbidden in hook", str(ctx.exception))

        hook_pkg_with_addr = copy.deepcopy(self.valid_hook_pkg)
        hook_pkg_with_addr["hooks"][0]["guest_addr"] = "0x08804000"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(hook_pkg_with_addr)
        self.assertIn("Unknown properties forbidden in hook", str(ctx.exception))

    def test_missing_required_fields_rejected(self) -> None:
        """Packages missing required metadata must fail validation."""
        for field in ("id", "display_name", "version", "category", "target_title_id", "capabilities"):
            with self.subTest(field=field):
                pkg = copy.deepcopy(self.valid_presentation_pkg)
                del pkg[field]
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn(f"Missing required property '{field}'", str(ctx.exception))

    def test_experimental_is_an_optional_boolean(self) -> None:
        """experimental (section 9.10) may be left out or be true or false, and nothing else."""
        for value in (True, False):
            with self.subTest(value=value):
                pkg = copy.deepcopy(self.valid_presentation_pkg)
                pkg["experimental"] = value
                self.assertIs(validate_package(pkg)["experimental"], value)
        bad_values: tuple[object, ...] = ("false", 0, None, [])
        for bad in bad_values:
            with self.subTest(value=bad):
                pkg = copy.deepcopy(self.valid_presentation_pkg)
                pkg["experimental"] = bad
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn("experimental must be true or false", str(ctx.exception))

    def test_capabilities_validation(self) -> None:
        """Capabilities list must be non-empty array of unique valid identifiers."""
        # Empty array
        pkg_empty = copy.deepcopy(self.valid_presentation_pkg)
        pkg_empty["capabilities"] = []
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_empty)
        self.assertIn("capabilities array must contain at least 1 item", str(ctx.exception))

        # Duplicates
        pkg_dup = copy.deepcopy(self.valid_presentation_pkg)
        pkg_dup["capabilities"] = ["hud_overlay", "hud_overlay"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_dup)
        self.assertIn("duplicate capability", str(ctx.exception))

        # Non-array
        pkg_str = copy.deepcopy(self.valid_presentation_pkg)
        pkg_str["capabilities"] = "hud_overlay"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_str)
        self.assertIn("capabilities must be an array", str(ctx.exception))

    def test_hooks_validation(self) -> None:
        """Hooks must be array of objects with valid symbol and type."""
        # Non-array
        pkg_not_list = copy.deepcopy(self.valid_hook_pkg)
        pkg_not_list["hooks"] = "not_a_list"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_not_list)
        self.assertIn("hooks must be an array", str(ctx.exception))

        # Missing symbol
        pkg_no_sym = copy.deepcopy(self.valid_hook_pkg)
        del pkg_no_sym["hooks"][0]["symbol"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_no_sym)
        self.assertIn("Missing required property 'symbol' in hook", str(ctx.exception))

        # Missing type
        pkg_no_type = copy.deepcopy(self.valid_hook_pkg)
        del pkg_no_type["hooks"][0]["type"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_no_type)
        self.assertIn("Missing required property 'type' in hook", str(ctx.exception))

        # Unknown type
        pkg_bad_type = copy.deepcopy(self.valid_hook_pkg)
        pkg_bad_type["hooks"][0]["type"] = "invalid_intercept"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_bad_type)
        self.assertIn("Unknown hook type", str(ctx.exception))

    def test_cli_runner_success_and_failure(self) -> None:
        """CLI tool must report OK for valid packages and exit non-zero for invalid ones."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            valid_path = Path(tmp_dir) / "valid.json"
            valid_path.write_text(json.dumps(self.valid_presentation_pkg), encoding="utf-8")

            invalid_path = Path(tmp_dir) / "invalid.json"
            bad_pkg = copy.deepcopy(self.valid_hook_pkg)
            bad_pkg["hooks"][0]["symbol"] = "0x08900000"
            invalid_path.write_text(json.dumps(bad_pkg), encoding="utf-8")

            # Valid file CLI run
            res_ok = subprocess.run(
                [sys.executable, "-m", "enhancement_package", str(valid_path)],
                cwd=ROOT / "tools",
                capture_output=True,
                text=True,
            )
            self.assertEqual(res_ok.returncode, 0, f"CLI stdout: {res_ok.stdout}, stderr: {res_ok.stderr}")
            self.assertIn("OK", res_ok.stdout)

            # Invalid file CLI run
            res_bad = subprocess.run(
                [sys.executable, "-m", "enhancement_package", str(invalid_path)],
                cwd=ROOT / "tools",
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(res_bad.returncode, 0)
            self.assertIn("Address-literal hook symbol rejected", res_bad.stderr)

    def test_timing_rescale_package_schema_and_fail_closed_validation(self) -> None:
        """Declarative 60 Hz frame-rate rescale packages must validate all fields and fail closed."""
        stock_word = 0x24020002  # li $v0, 2
        valid_60hz_pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.timing.60hz",
            "display_name": "Synthetic 60 Hz Logic Package",
            "version": "1.0.0",
            "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.01",
            "executable_sha256": "a" * 64,
            "capabilities": ["logic_60hz_rescale", "vblank_wait_2_to_1"],
            "rng_policy": {
                "mode": "rescale_probability",
                "reason": "Halve stateless per-update probability thresholds and gate stateful multi-roll routines at 30 Hz.",
            },
            "sites": [
                {
                    "id": "S001",
                    "function": "synth_vblank_sync_loop",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([stock_word]),
                    "transform": "vblank_wait_2_to_1",
                }
            ],
        }
        validated = validate_package(valid_60hz_pkg)
        self.assertEqual(validated["game_version"], "1.01")
        self.assertEqual(validated["sites"][0]["word_count"], 1)
        self.assertEqual(validated["rng_policy"]["mode"], "rescale_probability")

        # Schema must match SITE_TRANSFORMS and RNG_POLICY_MODES
        with SCHEMA_PATH.open("r", encoding="utf-8") as f:
            schema = json.load(f)
        site_schema = schema["properties"]["sites"]["items"]
        self.assertEqual(set(site_schema["properties"]["transform"]["enum"]), SITE_TRANSFORMS)
        rng_schema = schema["properties"]["rng_policy"]
        self.assertEqual(set(rng_schema["properties"]["mode"]["enum"]), RNG_POLICY_MODES)

        # Missing cross-field timing properties must fail closed
        for missing_key in ("game_version", "executable_sha256", "rng_policy", "sites"):
            with self.subTest(missing_key=missing_key):
                incomplete = copy.deepcopy(valid_60hz_pkg)
                del incomplete[missing_key]
                with self.assertRaises(EnhancementPackageError):
                    validate_package(incomplete)

        # Wildcard target_title_id '*' is forbidden when sites are present
        wildcard_pkg = copy.deepcopy(valid_60hz_pkg)
        wildcard_pkg["target_title_id"] = "*"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(wildcard_pkg)
        self.assertIn("Timing rescale package must target a specific title id, not '*'", str(ctx.exception))

        # Address-literal and FUN_/sub_/loc_/func_ rejection in site function names
        bad_functions = [
            "0x08804000",
            "FUN_12345678",
            "fun_1234abcd",
            "sub_08804000",
            "loc_08804000",
            "func_08804000",
            "12345678",
            "deadbeef",
        ]
        for bad_fn in bad_functions:
            with self.subTest(bad_function=bad_fn):
                bad_pkg = copy.deepcopy(valid_60hz_pkg)
                bad_pkg["sites"][0]["function"] = bad_fn
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(bad_pkg)
                self.assertIn("Address-literal", str(ctx.exception))

        # Non-4-byte-aligned insn_offset rejected
        misaligned_pkg = copy.deepcopy(valid_60hz_pkg)
        misaligned_pkg["sites"][0]["insn_offset"] = 2
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(misaligned_pkg)
        self.assertIn("4-byte-aligned", str(ctx.exception))

        # A package that gates must declare where each logic update begins.
        gated_pkg = copy.deepcopy(valid_60hz_pkg)
        gated_pkg["sites"].append(dict(gated_pkg["sites"][0], id="S002", insn_offset=8,
                                       transform="gate_30hz"))
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(copy.deepcopy(gated_pkg))
        self.assertIn("must declare update_boundary", str(ctx.exception))
        boundary = {
            "function": "synth_frame_body",
            "insn_offset": 0,
            "stock_digest": compute_stock_words_digest([0x27BDFFF0]),  # addiu sp, sp, -16
        }
        validate_package(dict(copy.deepcopy(gated_pkg), update_boundary=dict(boundary)))
        bad_boundaries = (
            ({**boundary, "insn_offset": 2}, "4-byte-aligned"),
            ({**boundary, "function": "FUN_00047920"}, "Address-literal"),
            ({**boundary, "stock_digest": "x"}, "SHA-256"),
            ({k: v for k, v in boundary.items() if k != "stock_digest"}, "Missing required property"),
            ({**boundary, "word_count": 1}, "Unknown properties"),
            ({"function": "synth_vblank_sync_loop", "insn_offset": 0,
              "stock_digest": boundary["stock_digest"]}, "must not be a rewritten site"),
            ("synth_frame_body", "must be an object"),
        )
        for bad, why in bad_boundaries:
            with self.subTest(boundary=bad):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(dict(copy.deepcopy(valid_60hz_pkg), update_boundary=bad))
                self.assertIn(why, str(ctx.exception))
        # Only a timing package may name a boundary.
        presentation_pkg = {k: v for k, v in valid_60hz_pkg.items()
                            if k not in ("game_version", "executable_sha256", "rng_policy", "sites")}
        presentation_pkg = dict(presentation_pkg, category="presentation_only", update_boundary=boundary)
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(presentation_pkg)
        self.assertIn("only valid for category 'timing_frame_presentation'", str(ctx.exception))

        # Unknown transform rejected
        bad_transform_pkg = copy.deepcopy(valid_60hz_pkg)
        bad_transform_pkg["sites"][0]["transform"] = "accelerate_clock_x2"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(bad_transform_pkg)
        self.assertIn("Unknown site transform", str(ctx.exception))


def _write_synthetic_mips_elf(path: Path, words: dict[int, int], entry: int,
                              sections: tuple[tuple[str, int, int, int], ...] = ()) -> bytes:
    """Write a minimal synthetic MIPS32 little-endian ET_EXEC ELF for testing.

    ``sections`` adds a section header table of (name, addr, size, flags) PROGBITS
    sections over the loaded image.
    """
    filesz = max(max(words) + 8, 0x1000)
    payload_off = 52 + 32
    blob = bytearray(payload_off + filesz)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH",
        blob,
        16,
        2,  # e_type = ET_EXEC
        8,  # e_machine = EM_MIPS
        1,
        entry,
        52,
        0,
        0,
        52,
        32,
        1,
        0,
        0,
        0,
    )
    struct.pack_into("<8I", blob, 52, 1, payload_off, 0, 0, filesz, filesz, 5, 4)
    for addr, word in words.items():
        struct.pack_into("<I", blob, payload_off + addr, word)
    if sections:
        names = b"\0.shstrtab\0" + b"".join(name.encode() + b"\0" for name, *_ in sections)
        strtab_off = len(blob)
        blob += names
        while len(blob) % 4:
            blob.append(0)
        shoff = len(blob)
        headers = [bytes(40), struct.pack("<10I", 1, 3, 0, 0, strtab_off, len(names), 0, 0, 1, 0)]
        name_at = len(b"\0.shstrtab\0")
        for name, addr, size, flags in sections:
            headers.append(struct.pack("<10I", name_at, 1, flags, addr, payload_off + addr, size, 0, 0, 4, 0))
            name_at += len(name) + 1
        blob += b"".join(headers)
        struct.pack_into("<I", blob, 32, shoff)
        struct.pack_into("<HHH", blob, 46, 40, len(headers), 1)
    raw = bytes(blob)
    path.write_bytes(raw)
    return raw


class _SynthTimingElfFixture(unittest.TestCase):
    """A synthetic MIPS ELF with six functions, one per transform class, and its package."""

    def setUp(self) -> None:
        # Build a synthetic MIPS ELF with 6 functions exercising each of the 6 transform classes:
        #   0x00001000: synth_main_entry (calls the 6 helper functions)
        #   0x00001100: synth_vblank_wait     -> vblank_wait_2_to_1: addiu $v0, $zero, 2 (0x24020002)
        #   0x00001200: synth_timer_duration  -> count_x2:           addiu $a0, $zero, 30 (0x2404001e)
        #   0x00001300: synth_velocity_step   -> step_x0.5:          lui $a1, 0x3f80 (1.0f -> 0.5f 0x3f00)
        #   0x00001400: synth_damping_decay   -> decay_k_to_1-sqrt(1-k): lui $a2, 0x3f40 (0.75f -> sqrt(0.75)=0.8660254f 0x3f5db3d7)
        #   0x00001500: synth_rng_prob_check  -> probability_per_update_x0.5: slti $v1, $a0, 100 (0x28830064 -> 50)
        #   0x00001600: synth_subtick_gate    -> gate_30hz:          jal 0x00001200 (0x0c000480)
        self.entry = 0x00001000
        self.fn_addrs = {
            "synth_main_entry": 0x00001000,
            "synth_vblank_wait": 0x00001100,
            "synth_timer_duration": 0x00001200,
            "synth_velocity_step": 0x00001300,
            "synth_damping_decay": 0x00001400,
            "synth_rng_prob_check": 0x00001500,
            "synth_subtick_gate": 0x00001600,
        }
        jr_ra = 0x03E00008
        nop = 0x00000000

        def jal(target: int) -> int:
            return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)

        self.words: dict[int, int] = {
            # synth_main_entry
            0x00001000: jal(0x00001100),
            0x00001004: nop,
            0x00001008: jal(0x00001200),
            0x0000100C: nop,
            0x00001010: jal(0x00001300),
            0x00001014: nop,
            0x00001018: jal(0x00001400),
            0x0000101C: nop,
            0x00001020: jal(0x00001500),
            0x00001024: nop,
            0x00001028: jal(0x00001600),
            0x0000102C: nop,
            0x00001030: jr_ra,
            0x00001034: nop,
            # synth_vblank_wait: addiu $v0, $zero, 2
            0x00001100: 0x24020002,
            0x00001104: jr_ra,
            0x00001108: nop,
            # synth_timer_duration: addiu $a0, $zero, 30
            0x00001200: 0x2404001E,
            0x00001204: jr_ra,
            0x00001208: nop,
            # synth_velocity_step: lui $a1, 0x3f80 (1.0f)
            0x00001300: 0x3C053F80,
            0x00001304: jr_ra,
            0x00001308: nop,
            # synth_damping_decay: lui $a2, 0x3f40 (0.75f)
            0x00001400: 0x3C063F40,
            0x00001404: jr_ra,
            0x00001408: nop,
            # synth_rng_prob_check: slti $v1, $a0, 100
            0x00001500: 0x28830064,
            0x00001504: jr_ra,
            0x00001508: nop,
            # synth_subtick_gate: jal synth_timer_duration
            0x00001600: jal(0x00001200),
            0x00001604: nop,
            0x00001608: jr_ra,
            0x0000160C: nop,
        }

    @staticmethod
    def _run_codegen(elf_path: Path, out_c: Path, extra_args: list[str] | None = None) -> tuple[int, str, dict[str, bytes]]:
        argv = [
            "codegen.py",
            str(elf_path),
            str(out_c),
            "--base=0",
            "--profile=none",
            "--funcs-per-chunk=4000",
        ]
        if extra_args:
            argv.extend(extra_args)
        orig_out, orig_err = sys.stdout, sys.stderr
        try:
            sys.stdout, sys.stderr = StringIO(), StringIO()
            rc = codegen.main(argv)
            output = sys.stdout.getvalue() + sys.stderr.getvalue()
        finally:
            sys.stdout, sys.stderr = orig_out, orig_err
        files: dict[str, bytes] = {}
        if rc == 0:
            for p in sorted(out_c.parent.glob("out*")):
                if p.is_file():
                    files[p.name] = p.read_bytes()
        return rc, output, files

    def _make_package_and_symbols(self, elf_bytes: bytes) -> tuple[dict[str, Any], dict[str, str]]:
        elf_sha = hashlib.sha256(elf_bytes).hexdigest()
        symbols = {name: f"0x{addr:08x}" for name, addr in self.fn_addrs.items()}
        pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.timing.60hz",
            "display_name": "Synthetic 60 Hz Logic Rescale",
            "version": "1.0.0",
            "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00",
            "executable_sha256": elf_sha,
            "capabilities": [
                "vblank_wait_2_to_1",
                "count_x2",
                "step_x0_5",
                "decay_k_to_1_sqrt_1_k",
                "probability_per_update_x0_5",
                "gate_30hz",
            ],
            "rng_policy": {
                "mode": "rescale_probability",
                "reason": "Halve single-roll per-frame thresholds and gate stateful multi-roll routines at 30 Hz.",
            },
            "sites": [
                {
                    "id": "SYNTH_VBLANK",
                    "function": "synth_vblank_wait",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001100]]),
                    "transform": "vblank_wait_2_to_1",
                },
                {
                    "id": "SYNTH_COUNT",
                    "function": "synth_timer_duration",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001200]]),
                    "transform": "count_x2",
                },
                {
                    "id": "SYNTH_STEP",
                    "function": "synth_velocity_step",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001300]]),
                    "transform": "step_x0.5",
                },
                {
                    "id": "SYNTH_DECAY",
                    "function": "synth_damping_decay",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001400]]),
                    "transform": "decay_k_to_1-sqrt(1-k)",
                },
                {
                    "id": "SYNTH_PROB",
                    "function": "synth_rng_prob_check",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001500]]),
                    "transform": "probability_per_update_x0.5",
                },
                {
                    "id": "SYNTH_GATE",
                    "function": "synth_subtick_gate",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001600]]),
                    "transform": "gate_30hz",
                },
            ],
            # One update runs synth_vblank_wait once; its return's delay slot counts it.
            "update_boundary": {
                "function": "synth_vblank_wait",
                "insn_offset": 8,
                "stock_digest": compute_stock_words_digest([self.words[0x00001108]]),
            },
        }
        return pkg, symbols


class TestEnhancementAotCodegenAndCache(_SynthTimingElfFixture):
    """Verify authentic byte-identity, all 6 transform classes, fail-closed digest rejection, and AOT cache epoch."""

    def test_authentic_mode_output_and_transforms_and_fail_closed_digest(self) -> None:
        """Verify authentic output stays untouched, all 6 transforms apply when enabled, and 1 wrong digest fails closed."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)

            # 1. Authentic baseline run (package off)
            auth_dir = tmp / "authentic"
            auth_dir.mkdir()
            rc_auth, out_auth, files_auth = self._run_codegen(elf_path, auth_dir / "out.c")
            self.assertEqual(rc_auth, 0, f"Authentic codegen failed: {out_auth}")
            self.assertTrue(files_auth)
            auth_combined = b"\n".join(files_auth.values()).decode("utf-8")
            self.assertNotIn("AOT Enhancement Epoch", auth_combined)
            self.assertIn("s->r[2] = (0u + 0x00000002u);", auth_combined)    # vblank wait 2
            self.assertIn("s->r[4] = (0u + 0x0000001eu);", auth_combined)    # count 30
            self.assertIn("s->r[5] = (0x3f80u << 16);", auth_combined)       # step 1.0f

            # 2. Enhanced run (package on)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            pkg_path = tmp / "synth-60hz.json"
            sym_path = tmp / "synth-60hz.symbols.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            sym_path.write_text(json.dumps(symbols), encoding="utf-8")

            enh_dir = tmp / "enhanced"
            enh_dir.mkdir()
            rc_enh, out_enh, files_enh = self._run_codegen(
                elf_path,
                enh_dir / "out.c",
                [f"--enhancement-package={pkg_path}"],
            )
            self.assertEqual(rc_enh, 0, f"Enhanced codegen failed: {out_enh}")
            enh_combined = b"\n".join(files_enh.values()).decode("utf-8")

            # Verify epoch stamp and all 6 transforms in generated C
            self.assertIn(f"AOT Enhancement Epoch: {AOT_ENHANCEMENT_PACKAGE_EPOCH}", enh_combined)
            self.assertIn("s->r[2] = (0u + 0x00000001u);", enh_combined)          # vblank_wait_2_to_1: 2 -> 1
            self.assertIn("s->r[4] = (0u + 0x0000003cu);", enh_combined)          # count_x2: 30 (0x1e) -> 60 (0x3c)
            self.assertIn("s->r[5] = (0x3f00u << 16);", enh_combined)             # step_x0.5: 1.0f (0x3f80) -> 0.5f (0x3f00)
            self.assertIn("s->r[6] = (0x3f00u << 16);", enh_combined)             # decay: k=0.75 -> 1-sqrt(1-0.75)=0.5f (0x3f00)
            self.assertIn("(int32_t)0x00000032u ? 1u : 0u", enh_combined)         # probability_per_update_x0.5: 100 (0x64) -> 50 (0x32)
            self.assertIn("if (!nk_enhancement_on() || nk_enhancement_gate_30hz())", enh_combined)  # gated call
            self.assertIn("int nk_enhancement_on(void) {", enh_combined)  # run-time opt-in, off by default
            self.assertIn("getenv(\"SR_EXPERIMENTAL_60HZ\")", enh_combined)
            self.assertNotIn("nk_enhancement", auth_combined)

            # 2b. A (transform, instruction) pair without a defined rewrite refuses the whole package
            nodef_pkg = copy.deepcopy(pkg)
            nodef_pkg["sites"][0] = dict(nodef_pkg["sites"][0], transform="gate_30hz")  # gate on addiu v0,zero,2
            nodef_path = tmp / "no-rewrite.json"
            nodef_path.write_text(json.dumps(nodef_pkg), encoding="utf-8")
            (tmp / "no-rewrite.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            nodef_dir = tmp / "no_rewrite"
            nodef_dir.mkdir()
            rc_nd, out_nd, files_nd = self._run_codegen(
                elf_path, nodef_dir / "out.c", [f"--enhancement-package={nodef_path}"]
            )
            self.assertNotEqual(rc_nd, 0)
            self.assertIn("has no defined rewrite", out_nd)
            self.assertEqual(files_nd, {})

            # 3. Re-running authentic mode after an enhanced run in the same process must stay byte-identical
            auth2_dir = tmp / "authentic2"
            auth2_dir.mkdir()
            rc_auth2, _, files_auth2 = self._run_codegen(elf_path, auth2_dir / "out.c")
            self.assertEqual(rc_auth2, 0)
            self.assertEqual(files_auth, files_auth2, "Authentic output changed after enhanced run")

            # 4. Fail-closed check: 1 wrong stock_digest must refuse the whole package
            bad_pkg = copy.deepcopy(pkg)
            bad_pkg["sites"][2]["stock_digest"] = compute_stock_words_digest([0x24020099])
            bad_pkg_path = tmp / "bad-digest.json"
            bad_sym_path = tmp / "bad-digest.symbols.json"
            bad_pkg_path.write_text(json.dumps(bad_pkg), encoding="utf-8")
            bad_sym_path.write_text(json.dumps(symbols), encoding="utf-8")

            bad_dir = tmp / "bad_run"
            bad_dir.mkdir()
            rc_bad, out_bad, files_bad = self._run_codegen(
                elf_path,
                bad_dir / "out.c",
                [f"--enhancement-package={bad_pkg_path}"],
            )
            self.assertNotEqual(rc_bad, 0, "Codegen must fail closed when a stock_digest mismatches")
            self.assertIn("Stock instruction digest mismatch", out_bad)
            self.assertEqual(files_bad, {}, "No partial enhanced output may be emitted on digest failure")

    def _codegen_with(self, tmp: Path, name: str, pkg: dict[str, Any], symbols: dict[str, str]
                      ) -> tuple[int, str, dict[str, bytes]]:
        elf_path = tmp / "synth.elf"
        pkg_path = tmp / f"{name}.json"
        pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
        (tmp / f"{name}.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
        out_dir = tmp / name
        out_dir.mkdir()
        return self._run_codegen(elf_path, out_dir / "out.c", [f"--enhancement-package={pkg_path}"])

    def test_a_run_leaves_no_package_active_after_it_returns(self) -> None:
        """The package a run loads ends with the run: code translated afterwards in the same process
        (tools/test_cpu_lle.py's fixtures, translated at the site addresses this ELF uses) is
        authentic. A package left active once rewrote those fixtures with nk_enhancement_on()."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            pkg_path = tmp / "synth-60hz.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            (tmp / "synth-60hz.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            out_dir = tmp / "out"
            out_dir.mkdir()
            rc, output, files = self._run_codegen(
                elf_path, out_dir / "out.c", [f"--enhancement-package={pkg_path}"])
            self.assertEqual(rc, 0, output)
            self.assertIn(b"nk_enhancement_on", b"\n".join(files.values()))
        self.assertEqual(codegen.ENHANCEMENT_SITES, {})
        self.assertEqual(codegen.ENHANCEMENT_ACE_SITES, {})
        self.assertIsNone(codegen.ENHANCEMENT_PKG_META)
        self.assertIsNone(codegen.ENHANCEMENT_HOOK_META)
        self.assertIsNone(codegen.ENHANCEMENT_UPDATE_BOUNDARY)
        self.assertIsNone(codegen.ENHANCEMENT_STORE_KEY)
        for table in (codegen.ENHANCEMENT_APPLIED, codegen.ENHANCEMENT_DATA_PATCHES,
                      codegen.ENHANCEMENT_DATA_FILES, codegen.ENHANCEMENT_STORE_SITES,
                      codegen.ENHANCEMENT_PHASE_STEPS, codegen.ENHANCEMENT_LAUNCH_SITES):
            self.assertFalse(table)

    def test_gated_sites_count_logic_updates_at_the_update_boundary(self) -> None:
        """The boundary statement counts each update once; gates read that count, not the display."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            rc, output, files = self._codegen_with(tmp, "boundary", pkg, symbols)
            self.assertEqual(rc, 0, output)
            combined = b"\n".join(files.values()).decode("utf-8")
            self.assertEqual(combined.count("nk_enhancement_updates++"), 1)
            self.assertIn("uint32_t nk_enhancement_updates;", combined)
            self.assertIn("(nk_enhancement_updates & 1u) == 0u", combined)
            self.assertNotIn("sr_display_vcount", combined)
            # The count runs before the boundary's own stock statement.
            count_at = combined.index("nk_enhancement_updates++")
            self.assertLess(count_at, combined.index("sr_begin(s, 0x00001108u", count_at))

    def test_update_boundary_refusals(self) -> None:
        """A gate without a boundary, or a boundary that does not verify, refuses the package."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)

            # A unit step under step_x0.5 plans a gate, so it needs a boundary too.
            words = dict(self.words)
            words[0x00001300] = 0x24A50001  # addiu a1, a1, 1
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", words, self.entry)
            step_pkg = copy.deepcopy(pkg)
            step_pkg["executable_sha256"] = hashlib.sha256(elf_bytes).hexdigest()
            step_pkg["sites"] = [dict(step_pkg["sites"][2],
                                      stock_digest=compute_stock_words_digest([0x24A50001]))]
            del step_pkg["update_boundary"]
            rc, output, files = self._codegen_with(tmp, "step-no-boundary", step_pkg, symbols)
            self.assertEqual(rc, 1, output)
            self.assertIn("declares no update_boundary", output)
            self.assertEqual(files, {})
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)

            cases = (
                ("boundary-digest", {"stock_digest": compute_stock_words_digest([0x24020099])},
                 "Stock instruction digest mismatch"),
                ("boundary-control", {"insn_offset": 4,
                                      "stock_digest": compute_stock_words_digest([0x03E00008])},
                 "control transfer cannot begin a logic update"),
            )
            for name, change, why in cases:
                with self.subTest(name):
                    bad = copy.deepcopy(pkg)
                    bad["update_boundary"].update(change)
                    rc, output, files = self._codegen_with(tmp, name, bad, symbols)
                    self.assertEqual(rc, 1, output)
                    self.assertIn(why, output)
                    self.assertEqual(files, {})

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_gates_alternate_on_updates_when_frames_drop(self) -> None:
        """Compile the generated gate and boundary count and run updates of 1 to 3 vblanks.

        A gated statement must run on every other logic update however many display
        vblanks each update takes (a frame drop, or a stock two-vblank wait).
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            rc, output, files = self._codegen_with(tmp, "drops", pkg, symbols)
            self.assertEqual(rc, 0, output)
            header = files["out_funcs.h"].decode("utf-8")
            gate_def = next(line for line in header.splitlines()
                            if "int nk_enhancement_gate_30hz(void)" in line)
            combined = b"\n".join(files.values()).decode("utf-8")
            count_stmt = next(line.strip() for line in combined.splitlines()
                              if "nk_enhancement_updates++" in line)
            harness = tmp / "drops.c"
            harness.write_text(
                "#include <stdint.h>\n#include <stdio.h>\n"
                "static uint32_t vcount;\n"
                "uint32_t sr_display_vcount(void) { return vcount; }\n"
                "int nk_enhancement_on(void) { return 1; }\n"
                "uint32_t nk_enhancement_updates;\n"
                f"{gate_def}\n"
                "int main(void) {\n"
                "    static const unsigned vblanks[] = {1, 1, 2, 2, 2, 1, 3, 2, 1, 1, 2, 2};\n"
                "    unsigned runs = 0, alternations = 0, last = 2;\n"
                "    for (unsigned u = 0; u < 1200; u++) {\n"
                f"        {count_stmt}\n"
                "        unsigned ran = 0;\n"
                "        if (nk_enhancement_gate_30hz()) { ran = 1; runs++; }\n"
                "        if (ran != last) alternations++;\n"
                "        last = ran;\n"
                "        vcount += vblanks[u % 12];\n"
                "    }\n"
                "    printf(\"%u %u\\n\", runs, alternations);\n"
                "    return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            exe = tmp / "drops.exe"
            assert GCC is not None
            subprocess.run([GCC, "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                            "-o", str(exe), str(harness)], check=True, capture_output=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
            runs, alternations = (int(v) for v in out.split())
            self.assertEqual(runs, 600)
            self.assertEqual(alternations, 1200)

    def test_data_sites_and_files_reach_the_generated_start_up(self) -> None:
        """Data sites become a verified load-time patch; data files a verified registration."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            words = dict(self.words)
            words[0x00001800] = 21           # s32 count in .data
            words[0x00001804] = 0x3F800000   # f32 1.0 in .data
            sections = (
                (".text", 0x00001000, 0x700, 6),  # alloc + exec
                (".data", 0x00001800, 0x10, 3),   # alloc + write
            )
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", words, self.entry, sections)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            symbols["synth_wait_table"] = "0x00001800"
            symbols["synth_code_as_data"] = "0x00001200"
            pkg["data_sites"] = [
                {"id": "D_COUNT", "symbol": "synth_wait_table", "offset": 0, "type": "s32",
                 "stock_digest": hashlib.sha256(struct.pack("<i", 21)).hexdigest(),
                 "transform": "count_x2"},
                {"id": "D_STEP", "symbol": "synth_wait_table", "offset": 4, "type": "f32",
                 "stock_digest": hashlib.sha256(struct.pack("<f", 1.0)).hexdigest(),
                 "transform": "step_x0.5"},
            ]
            csv = b"Name,Wait,Speed\r\nlabel,frames,rate\r\na,30,0.25\r\nb,12,1.5\r\n"
            data_root = tmp / "game" / "xbdata" / "pack.xb.d" / "data" / "param"
            data_root.mkdir(parents=True)
            (data_root / "Timing.csv").write_bytes(csv)
            pkg["data_files"] = [{
                "id": "F_TIMING", "path": "data/param/timing.csv",
                "sha256": hashlib.sha256(csv).hexdigest(), "format": "csv",
                "header_row": 0, "first_data_row": 2,
                "cells": [{"column": "Wait", "transform": "count_x2"},
                          {"column": "Speed", "transform": "step_x0.5"}],
            }]
            elf_path = tmp / "synth.elf"
            pkg_path = tmp / "data.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            (tmp / "data.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            out_dir = tmp / "data"
            out_dir.mkdir()
            rc, output, files = self._run_codegen(
                elf_path, out_dir / "out.c",
                [f"--enhancement-package={pkg_path}", f"--enhancement-data-root={tmp / 'game'}"])
            self.assertEqual(rc, 0, output)
            combined = b"\n".join(files.values()).decode("utf-8")
            self.assertIn("if (nk_enhancement_on()) nk_enhancement_apply_data();", combined)
            self.assertIn("sr_enhancement_register_updates(&nk_enhancement_updates);", combined)
            self.assertIn("nk_enh_data_new_0[] = {0x2a, 0x00, 0x00, 0x00}", combined)  # 21 -> 42
            self.assertIn("nk_enh_data_new_1[] = {0x00, 0x00, 0x00, 0x3f}", combined)  # 1.0 -> 0.5
            self.assertIn("ENHANCEMENT_DATA_MISMATCH: data site D_COUNT", combined)
            rewritten = b"Name,Wait,Speed\r\nlabel,frames,rate\r\na,60,0.125\r\nb,24,0.75\r\n"
            self.assertIn(f"nk_enh_file_data_0[] = {{{', '.join(f'0x{b:02x}' for b in rewritten)}}}",
                          combined)
            self.assertIn('sr_enhancement_file_register("data/param/timing.csv", nk_enh_file_sha_0, '
                          f"nk_enh_file_data_0, {len(rewritten)}u)", combined)
            if GCC:
                # The start-up block is C the runtime build compiles; check it is valid C.
                checked = subprocess.run(
                    [GCC, "-std=gnu11", "-fsyntax-only", "-Wall", "-Werror", "-Wno-unused-function",
                     f"-I{ROOT / 'src' / 'rt'}", f"-I{out_dir}", str(out_dir / "out.c")],
                    capture_output=True, text=True)
                self.assertEqual(checked.returncode, 0, checked.stderr[-2000:])

            refusals: tuple[tuple[str, dict[str, str] | None, list[str] | None, str], ...] = (
                ("no-data-root", None, [], "need --enhancement-data-root"),
                ("code-as-data", {"symbol": "synth_code_as_data"}, None, "initialised data"),
                ("data-digest", {"stock_digest": "0" * 64}, None, "Stock data digest mismatch"),
            )
            for name, change, extra, why in refusals:
                with self.subTest(name):
                    bad = copy.deepcopy(pkg)
                    if change:
                        bad["data_sites"][0].update(change)
                    bad_path = tmp / f"{name}.json"
                    bad_path.write_text(json.dumps(bad), encoding="utf-8")
                    (tmp / f"{name}.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
                    bad_dir = tmp / name
                    bad_dir.mkdir()
                    args = [f"--enhancement-package={bad_path}"]
                    args += extra if extra is not None else [f"--enhancement-data-root={tmp / 'game'}"]
                    rc, output, files = self._run_codegen(elf_path, bad_dir / "out.c", args)
                    self.assertEqual(rc, 1, output)
                    self.assertIn(why, output)
                    self.assertEqual(files, {})

    def test_float_pair_rewrite_scales_the_whole_constant(self) -> None:
        """lui+ori of one register names a float exactly; anything else refuses."""
        site = {"id": "PAIR", "transform": "decay_k_to_1-sqrt(1-k)"}
        lui, ori = 0x3C023F4C, 0x3442CCCD  # lui v0,0x3f4c; ori v0,v0,0xcccd = 0.8f
        hi, lo = plan_float_pair_rewrite(site, lui, ori)
        bits = ((hi["word"] & 0xFFFF) << 16) | (lo["word"] & 0xFFFF)
        expected = struct.unpack("<I", struct.pack("<f", 1.0 - (1.0 - struct.unpack(
            "<f", struct.pack("<I", 0x3F4CCCCD))[0]) ** 0.5))[0]
        self.assertEqual(bits, expected)
        self.assertEqual(hi["word"] >> 16, lui >> 16)
        self.assertEqual(lo["word"] >> 16, ori >> 16)
        step = {"id": "PAIR", "transform": "step_x0.5"}
        hi, lo = plan_float_pair_rewrite(step, lui, ori)
        self.assertEqual(((hi["word"] & 0xFFFF) << 16) | (lo["word"] & 0xFFFF), 0x3ECCCCCD)  # 0.4f
        for w0, w1, why in (
            (lui, 0x3463CCCD, "ori of the lui's register"),   # ori v1,v1 after lui v0
            (0x24020002, ori, "starts with lui"),
            (lui, 0x2442CCCD, "ori of the lui's register"),   # addiu, not ori
        ):
            with self.subTest(w0=hex(w0), w1=hex(w1)):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    plan_float_pair_rewrite(step, w0, w1)
                self.assertIn(why, str(ctx.exception))
        with self.assertRaises(EnhancementPackageError):
            plan_float_pair_rewrite({"id": "PAIR", "transform": "gate_30hz"}, lui, ori)

    def test_rewrite_planner_is_defined_or_refuses(self) -> None:
        """Each (transform, instruction) pair has one defined rewrite; anything else refuses."""
        from enhancement_package import EnhancementPackageError, plan_site_rewrite

        def plan(transform: str, word: int) -> dict[str, Any]:
            planned: dict[str, Any] = plan_site_rewrite({"id": "S", "transform": transform}, word)
            return planned

        self.assertEqual(plan("count_x2", 0x2404001E), {"kind": "word", "word": 0x2404003C})
        self.assertEqual(plan("step_x0.5", 0x3C053F80), {"kind": "word", "word": 0x3C053F00})
        self.assertEqual(plan("step_x0.5", 0x24840001), {"kind": "gate"})  # addiu a0,a0,1
        self.assertEqual(plan("count_x2", 0x2484FFFF), {"kind": "gate"})  # addiu a0,a0,-1
        self.assertEqual(plan("retention_m_to_sqrt(m)", 0x3C023F40)["word"], 0x3C023F5E)
        # k = 2^-126: 1 - sqrt(1 - k) cancels to 0 in double; the result is k / 2 = 2^-127
        self.assertEqual(plan("decay_k_to_1-sqrt(1-k)", 0x3C020080)["word"], 0x3C020040)
        self.assertEqual(plan("decay_k_to_1-sqrt(1-k)", 0xC4430010),  # lwc1 f3: map the loaded factor
                         {"kind": "map_f_reg", "reg": 3, "map": "decay"})
        self.assertEqual(plan("retention_m_to_sqrt(m)", 0xC4430010),
                         {"kind": "map_f_reg", "reg": 3, "map": "retention"})
        # mov.s f20, f14 copies the factor: the copy is mapped, never the source register
        self.assertEqual(plan("decay_k_to_1-sqrt(1-k)", 0x46007506),
                         {"kind": "map_f_reg", "reg": 20, "map": "decay"})
        self.assertEqual(plan("retention_m_to_sqrt(m)", 0x46007506),
                         {"kind": "map_f_reg", "reg": 20, "map": "retention"})
        self.assertEqual(plan("count_x2", 0xC8404F84),  # lv.s S000.s, 0x4f84(v0)
                         {"kind": "scale_v_reg", "vreg": 0, "factor": 2.0})
        self.assertEqual(plan("step_x0.5", 0xC8470011),  # lv.s S131.s (vt 7 | 1 << 5)
                         {"kind": "scale_v_reg", "vreg": 39, "factor": 0.5})
        self.assertEqual(plan("decay_k_to_1-sqrt(1-k)", 0xC844C410),  # lv.s S100.s, -0x3bf0(v0)
                         {"kind": "map_v_reg", "vreg": 4, "map": "decay"})
        self.assertEqual(plan("gate_30hz", 0x0C000480), {"kind": "gate"})  # jal
        self.assertEqual(plan("count_x2", 0xC4430010), {"kind": "scale_f_reg", "reg": 3, "factor": 2.0})
        self.assertEqual(
            plan("step_x0.5", 0x46041080),  # add.s f2,f2,f4 accumulates: the addend is scaled
            {"kind": "scale_f_addend", "reg": 4, "factor": 0.5},
        )
        for transform, word in (
            ("step_x0.5", 0xAFA40008),  # sw
            ("count_x2", 0x3C040010),  # lui of a non-float value
            ("count_x2", 0x0C000480),  # jal
            ("count_x2", 0x2485FFFA),  # negative immediate
            ("gate_30hz", 0xC4820008),  # gate on a load
            ("step_x0.5", 0x4604103C),  # FPU compare
            ("step_x0.5", 0x34841234),  # ori low half
            ("count_x2", 0x44841000),  # mtc1
            ("vblank_wait_2_to_1", 0x24020003),
            ("decay_k_to_1-sqrt(1-k)", 0x3C023F80),  # 1.0 is outside (0, 1)
            ("decay_k_to_1-sqrt(1-k)", 0xE4430010),  # swc1 stores, it does not load the factor
            ("retention_m_to_sqrt(m)", 0x24020003),
            ("decay_k_to_1-sqrt(1-k)", 0x46207506),  # mov.d: a double, not a single factor
            ("decay_k_to_1-sqrt(1-k)", 0x46007507),  # neg.s does not copy the factor
            ("decay_k_to_1-sqrt(1-k)", 0x46007502),  # mul.s: the decay is not taken from arithmetic
            ("decay_k_to_1-sqrt(1-k)", 0x460018C6),  # mov.s f3, f3: no copy, the source would change
            ("retention_m_to_sqrt(m)", 0x460018C6),
            ("decay_k_to_1-sqrt(1-k)", 0x46017506),  # mov.s with a nonzero ft field is not a valid word
            ("count_x2", 0x46017506),
            ("step_x0.5", 0x46017507),  # neg.s likewise
        ):
            with self.subTest(transform=transform, word=f"{word:08x}"):
                with self.assertRaises(EnhancementPackageError):
                    plan(transform, word)

    def test_count_x2_even_and_gate_30hz_hold_plans(self) -> None:
        """count_x2_even doubles a slti/sltiu bound and keeps even counts; gate_30hz_hold holds f12."""
        from enhancement_package import EnhancementPackageError, plan_site_rewrite

        def plan(transform: str, word: int) -> dict:
            rewrite: dict = plan_site_rewrite({"id": "S", "transform": transform}, word)
            return rewrite

        self.assertEqual(plan("count_x2_even", 0x28410008),  # slti at, v0, 8
                         {"kind": "word_even", "word": 0x28410010, "reg": 2, "rt": 1})
        self.assertEqual(plan("count_x2_even", 0x2C410008),  # sltiu at, v0, 8
                         {"kind": "word_even", "word": 0x2C410010, "reg": 2, "rt": 1})
        self.assertEqual(plan("count_x2_even", 0x28413FFF),  # the largest bound that doubles in 16 bits
                         {"kind": "word_even", "word": 0x28417FFE, "reg": 2, "rt": 1})
        self.assertEqual(plan("gate_30hz_hold", 0x0C000480), {"kind": "gate", "hold_f": 12})  # jal
        for transform, word in (
            ("count_x2_even", 0x28420008),  # slti v0, v0, 8: the counter is overwritten
            ("count_x2_even", 0x28400008),  # slti zero, v0, 8
            ("count_x2_even", 0x2841FFFF),  # negative bound
            ("count_x2_even", 0x28410000),  # a bound of 0 is never true for a count
            ("count_x2_even", 0x2C410000),  # sltiu with a bound of 0
            ("count_x2_even", 0x28414000),  # 2 * 0x4000 does not fit
            ("count_x2_even", 0x24410008),  # addiu: not a compare
            ("gate_30hz_hold", 0x24840001),  # an accumulating update is not a call
            ("gate_30hz_hold", 0x0320F809),  # jalr: codegen gates only a jal
        ):
            with self.subTest(transform=transform, word=f"{word:08x}"):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    plan(transform, word)
                self.assertIn("has no defined rewrite", str(ctx.exception))

        pkg, _ = self._make_package_and_symbols(b"synthetic_elf_bytes")
        pkg["sites"] = [dict(pkg["sites"][-1], transform="gate_30hz_hold")]
        del pkg["update_boundary"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg)
        self.assertIn("gate_30hz_alt or gate_30hz_store sites must declare", str(ctx.exception))

    def _even_and_hold_build(self, tmp: Path) -> str:
        # The resolver's static checks: the held call goes to an ease, a leaf that reads f12
        # and writes f0 (f0 = f12 + f14 * (f13 - f12)), and the even bound compares a count
        # loaded just before it.
        self.words.update({
            0x00001500: 0x8CA40000,  # lw a0, 0(a1)
            0x00001504: 0x28830064,  # slti v1, a0, 100
            0x00001508: 0x03E00008, 0x0000150C: 0,
            0x00001600: 0x0C000000 | (0x00001700 >> 2),  # jal synth_ease
            0x00001700: 0x460C6841,  # sub.s f1, f13, f12
            0x00001704: 0x460E0842,  # mul.s f1, f1, f14
            0x00001708: 0x46016000,  # add.s f0, f12, f1
            0x0000170C: 0x03E00008, 0x00001710: 0,
        })
        self.fn_addrs["synth_ease"] = 0x00001700
        elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
        pkg, symbols = self._make_package_and_symbols(elf_bytes)
        for site in pkg["sites"]:
            if site["id"] == "SYNTH_PROB":  # slti v1, a0, 100, after the count's load
                site.update(transform="count_x2_even", insn_offset=4,
                            stock_digest=compute_stock_words_digest([0x28830064]))
            elif site["id"] == "SYNTH_GATE":  # jal synth_ease
                site["transform"] = "gate_30hz_hold"
        rc, output, files = self._codegen_with(tmp, "even-hold", pkg, symbols)
        self.assertEqual(rc, 0, output)
        return b"\n".join(files.values()).decode("utf-8")

    def test_count_x2_even_and_gate_30hz_hold_emit_both_forms(self) -> None:
        """The enhanced slti doubles its bound and drops odd counts; the held call copies f12 to f0."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            combined = self._even_and_hold_build(Path(tmp_dir))
            self.assertIn("(int32_t)0x000000c8u ? 1u : 0u", combined)  # 100 -> 200
            self.assertIn("(int32_t)0x00000064u ? 1u : 0u", combined)  # the stock form is kept
            self.assertIn("uint32_t _nk_count = s->r[4];", combined)
            # slti: a negative count stays true, as in stock.
            self.assertIn("if ((_nk_count & 1u) && (int32_t)_nk_count >= 0) { s->r[3] = 0u; }", combined)
            self.assertIn("if (!nk_enhancement_on() || nk_enhancement_gate_30hz())", combined)
            self.assertIn("} else { s->fi[0] = s->fi[12]; }", combined)

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_count_x2_even_and_gate_30hz_hold_match_30hz_when_run(self) -> None:
        """Compile the generated statements and run 30 Hz against 60 Hz updates.

        count_x2_even: a counter of updates since an event meets the bound at the stock
        instants only (60 Hz count 2k where the stock count is k), the same number of times.
        gate_30hz_hold: three values advanced by an ease call each update, as a menu pane
        does, take the 30 Hz values, each held for two updates; none takes another's result.
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            combined = self._even_and_hold_build(tmp)
            lines = combined.splitlines()
            at = next(i for i, line in enumerate(lines) if "uint32_t _nk_count" in line)
            compare = "\n".join(lines[at:at + 2])
            # The call statement: from the jal's trace hook to the line that closes its gate,
            # whatever that gate's skipped branch holds.
            at = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001600u" in line)
            gate = next(i for i in range(at, len(lines)) if "nk_enhancement_gate_30hz()) {" in lines[i])
            end = next(i for i in range(gate + 1, len(lines)) if lines[i].lstrip().startswith("}"))
            call = "\n".join(lines[at:end + 1])
            authentic_dir = tmp / "authentic"
            authentic_dir.mkdir()
            rc, output, files = self._run_codegen(tmp / "synth.elf", authentic_dir / "out.c", [])
            self.assertEqual(rc, 0, output)
            stock_lines = b"\n".join(files.values()).decode("utf-8").splitlines()
            at = next(i for i, line in enumerate(stock_lines) if "sr_begin(s, 0x00001504u" in line)
            stock_compare = stock_lines[at]
            at = next(i for i, line in enumerate(stock_lines) if "sr_begin(s, 0x00001600u" in line)
            end = next(i for i in range(at, len(stock_lines))
                       if "sr_aot_return_boundary_end" in stock_lines[i])
            stock_call = "\n".join(stock_lines[at:end + 1])
            # Toggle off, each site runs the Authentic statement, byte for byte.
            self.assertIn(f"else {{ {stock_compare.strip()} }}", compare)
            self.assertIn(stock_lines[end], call)  # the checked call itself

            def program(name: str, compare: str, call: str) -> list[str]:
                callee_match = re.search(r"return; (\w+)\(s\);", call)
                self.assertIsNotNone(callee_match, call)
                assert callee_match is not None
                callee = callee_match.group(1)
                harness = tmp / f"{name}.c"
                harness.write_text(
                    "#include \"recomp.h\"\n"
                    "/* The real CpuState and trace hooks; the statement runs alone, with no caller\n"
                    " * frame to return through, so only the return boundary is stubbed. */\n"
                    "#undef SR_TAIL_DRAIN\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
                    "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
                    "#define sr_aot_return_boundary_end(s) 0\n"
                    "static int on;\nint nk_enhancement_on(void) { return on; }\n"
                    "uint32_t nk_enhancement_updates;\n"
                    "static inline int nk_enhancement_gate_30hz(void) "
                    "{ return (nk_enhancement_updates & 1u) == 0u; }\n"
                    "/* The ease: f0 = f12 + f14 * (f13 - f12), as the consumer's helper computes. */\n"
                    f"static void {callee}(CpuState *s) "
                    "{ s->f[0] = s->f[12] + s->f[14] * (s->f[13] - s->f[12]); }\n"
                    f"static void compare(CpuState *s) {{\n{compare}\n}}\n"
                    f"static void call(CpuState *s) {{\n{call}\n}}\n"
                    "static float ease(CpuState *s, float cur, float target) {\n"
                    "    s->f[12] = cur; s->f[13] = target; s->f[14] = 0.25f; call(s); return s->f[0];\n"
                    "}\n"
                    "int main(void) {\n"
                    "    CpuState st; CpuState *s = &st; memset(&st, 0, sizeof st);\n"
                    "    for (on = 0; on < 2; on++) {\n"
                    "        for (int32_t n = -4; n < 300; n++) {\n"
                    "            s->r[4] = (uint32_t)n; compare(s);\n"
                    "            if (s->r[3]) printf(\"%s %d\\n\", on ? \"hit60\" : \"hit30\", (int)n);\n"
                    "        }\n"
                    "    }\n"
                    "    for (on = 0; on < 2; on++) {\n"
                    "        float x = 0.0f, y = 100.0f, a = 1.0f;\n"
                    "        nk_enhancement_updates = 0;\n"
                    "        for (int u = 0; u < (on ? 40 : 20); u++) {\n"
                    "            nk_enhancement_updates++;\n"
                    "            x = ease(s, x, 64.0f); y = ease(s, y, 0.0f); a = ease(s, a, 0.0f);\n"
                    "            printf(\"%s %d %.6f %.6f %.6f\\n\", on ? \"ease60\" : \"ease30\", u, x, y, a);\n"
                    "        }\n"
                    "    }\n"
                    "    return 0;\n"
                    "}\n",
                    encoding="utf-8",
                )
                exe = tmp / f"{name}.exe"
                # The generated call statement keeps its guards on one line, as codegen emits it.
                assert GCC is not None, "skipUnless(GCC) runs this test only with a compiler"
                built = subprocess.run([GCC, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                        "-Wno-misleading-indentation", "-Wno-unused-variable",
                                        f"-I{ROOT / 'src' / 'rt'}", "-o", str(exe), str(harness)],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
                lines: list[str] = run.stdout.split("\n")
                return lines

            out = program("even_hold", compare, call)
            # Section 11.2, item 1: the package build with the toggle off prints what the
            # Authentic build prints, byte for byte.
            authentic = program("authentic", stock_compare, stock_call)
            off = [line for line in out if line.startswith(("hit30 ", "ease30 "))]
            self.assertEqual(off, [line for line in authentic if line.startswith(("hit30 ", "ease30 "))])
            self.assertEqual(len(off), 104 + 20)
            hit30 = [int(line.split()[1]) for line in out if line.startswith("hit30 ")]
            hit60 = [int(line.split()[1]) for line in out if line.startswith("hit60 ")]
            # A negative count is below the slti bound in stock and stays so (an "inactive"
            # sentinel such as -1 keeps its meaning); 0..99 at 30 Hz meet it at 0..198 at 60 Hz.
            self.assertEqual(hit30, [-4, -3, -2, -1] + list(range(100)))
            self.assertEqual(hit60, [-4, -3, -2, -1] + [2 * k for k in range(100)])
            ease30 = [tuple(line.split()[2:]) for line in out if line.startswith("ease30 ")]
            ease60 = [tuple(line.split()[2:]) for line in out if line.startswith("ease60 ")]
            # Update 1 of 60 is odd and holds the start; then each 30 Hz value shows for two updates.
            self.assertEqual(ease60[0], ("0.000000", "100.000000", "1.000000"))
            for k, values in enumerate(ease30[:19]):
                with self.subTest(update=k):
                    self.assertEqual(ease60[2 * k + 1], values)
                    self.assertEqual(ease60[2 * k + 2], values)

    def test_gate_30hz_store_plans(self) -> None:
        """gate_30hz_store gates an sb, sh, sw or swc1 off the stack frame; anything else refuses."""
        from enhancement_package import EnhancementPackageError, plan_site_rewrite

        def plan(word: int) -> dict:
            rewrite: dict = plan_site_rewrite({"id": "S", "transform": "gate_30hz_store"}, word)
            return rewrite

        for word in (
            0xA2420010,  # sb v0, 0x10(s2)
            0xA64200EC,  # sh v0, 0xec(s2)
            0xAE030078,  # sw v1, 0x78(s0)
            0xE602006C,  # swc1 f2, 0x6c(s0)
            0xAE000078,  # sw zero, 0x78(s0): a stored constant is held as well
        ):
            with self.subTest(word=f"{word:08x}"):
                self.assertEqual(plan(word), {"kind": "gate"})
        for word, why in (
            (0xAFA40008, "stack frame"),  # sw a0, 8(sp)
            (0xE7A00010, "stack frame"),  # swc1 f0, 0x10(sp)
            (0x8E030078, "takes an sb"),  # lw: a load is not a store
            (0x00621821, "takes an sb"),  # addu v1, v1, v0: gate_30hz's form, not this one
            (0x0C000480, "takes an sb"),  # jal
            (0xEA000000, "takes an sb"),  # sv.s: VFPU stores are not admitted
            (0xAA420000, "takes an sb"),  # swl v0, 0(s2)
            (0xBA420000, "takes an sb"),  # swr v0, 0(s2)
            (0xE2420000, "takes an sb"),  # sc v0, 0(s2): it also writes rt
        ):
            with self.subTest(word=f"{word:08x}"):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    plan(word)
                self.assertIn(why, str(ctx.exception))

        pkg, _ = self._make_package_and_symbols(b"synthetic_elf_bytes")
        pkg["sites"] = [dict(pkg["sites"][-1], transform="gate_30hz_store")]
        del pkg["update_boundary"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg)
        self.assertIn("gate_30hz_alt or gate_30hz_store sites must declare", str(ctx.exception))

        pkg, _ = self._make_package_and_symbols(b"synthetic_elf_bytes")
        pkg["sites"][-1] = dict(pkg["sites"][-1], transform="gate_30hz_store", word_count=2)
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg)
        self.assertIn("gate_30hz_store names one store (word_count 1)", str(ctx.exception))

    def test_gate_30hz_store_gates_a_store_in_a_delay_slot(self) -> None:
        """A gated store in a jr's delay slot is emitted gated before the return, both forms kept."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            self.words.update({
                0x00001500: 0x84A20000,  # lh v0, 0(a1)
                0x00001504: 0x24420020,  # addiu v0, v0, 32
                0x00001508: 0x03E00008,  # jr ra
                0x0000150C: 0xA4A20000,  # sh v0, 0(a1), in the delay slot
            })
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            for site in pkg["sites"]:
                if site["id"] == "SYNTH_PROB":
                    site.update(transform="gate_30hz_store", insn_offset=12,
                                stock_digest=compute_stock_words_digest([0xA4A20000]))
            rc, output, files = self._codegen_with(tmp, "slot", pkg, symbols)
            self.assertEqual(rc, 0, output)
            lines = b"\n".join(files.values()).decode("utf-8").splitlines()
            at = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x0000150cu" in line)
            stock = "sr_begin(s, 0x0000150cu, 0xa4a20000u); MEM_W16_PC("
            self.assertIn(f"if (nk_enhancement_on()) {{ if (nk_enhancement_gate_30hz()) {{ {stock}", lines[at])
            self.assertIn(f"else {{ {stock}", lines[at + 1])
            # The jr's target is latched before the slot, and the slot runs before the return.
            self.assertTrue(any("_sr_return_target" in line for line in lines[at - 4:at]), lines[at - 4:at])
            self.assertIn("return;", lines[at + 2])

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_gate_30hz_store_holds_each_30hz_value_for_two_updates(self) -> None:
        """Compile a field's per-update step with its store gated and run 30 Hz against 60 Hz.

        The step is x = x / 2 + 32 on a halfword, as a cursor's inline ease does: with the
        store run on even updates only, the field takes the 30 Hz values, each for two
        updates; with the toggle off the statements are the Authentic ones byte for byte.
        The harness uses the gate test codegen writes into the generated header. Without a host
        C compiler (gcc on PATH) the test is skipped, as the other compiled tests are.
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            self.words.update({
                0x00001500: 0x84A20000,  # lh v0, 0(a1)
                0x00001504: 0x00021043,  # sra v0, v0, 1
                0x00001508: 0x24420020,  # addiu v0, v0, 32
                0x0000150C: 0xA4A20000,  # sh v0, 0(a1)
                0x00001510: 0x03E00008, 0x00001514: 0,
            })
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            for site in pkg["sites"]:
                if site["id"] == "SYNTH_PROB":
                    site.update(transform="gate_30hz_store", insn_offset=12,
                                stock_digest=compute_stock_words_digest([0xA4A20000]))
            rc, output, files = self._codegen_with(tmp, "store", pkg, symbols)
            self.assertEqual(rc, 0, output)
            authentic_dir = tmp / "authentic"
            authentic_dir.mkdir()
            rc, output, stock_files = self._run_codegen(tmp / "synth.elf", authentic_dir / "out.c", [])
            self.assertEqual(rc, 0, output)

            def step(files: dict[str, bytes]) -> str:
                lines = b"\n".join(files.values()).decode("utf-8").splitlines()
                at = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001500u" in line)
                # The step ends where the jr $ra's return sequence begins.
                end = next(i for i in range(at, len(lines)) if "_sr_return_target" in lines[i])
                return "\n".join(lines[at:end])

            enhanced, stock = step(files), step(stock_files)
            gate_def = next(line for line in files["out_funcs.h"].decode("utf-8").splitlines()
                            if "int nk_enhancement_gate_30hz(void)" in line)
            store = next(line.strip() for line in stock.splitlines() if "0x0000150cu" in line)
            self.assertIn("MEM_W16_PC", store)
            self.assertIn(f"if (nk_enhancement_gate_30hz()) {{ {store} }}", enhanced)
            self.assertIn(f"else {{ {store} }}", enhanced)  # toggle off: the Authentic store

            def program(name: str, body: str) -> list[str]:
                harness = tmp / f"{name}.c"
                harness.write_text(
                    "#include \"recomp.h\"\n"
                    "/* The real CpuState and trace hooks; guest memory is one halfword here. */\n"
                    "#undef MEM_R16\n#undef MEM_W16_PC\n"
                    "static uint16_t field;\n"
                    "#define MEM_R16(a) ((void)(a), field)\n"
                    "#define MEM_W16_PC(a, v, pc) ((void)(a), (void)(pc), field = (uint16_t)(v))\n"
                    "static int on;\nint nk_enhancement_on(void) { return on; }\n"
                    "uint32_t nk_enhancement_updates;\n"
                    f"{gate_def}\n"
                    f"static void update(CpuState *s) {{\n{body}\n}}\n"
                    "int main(void) {\n"
                    "    CpuState st; CpuState *s = &st; memset(&st, 0, sizeof st);\n"
                    "    for (on = 0; on < 2; on++) {\n"
                    "        field = 0; nk_enhancement_updates = 0;\n"
                    "        for (int u = 0; u < (on ? 40 : 20); u++) {\n"
                    "            nk_enhancement_updates++;\n"
                    "            update(s);\n"
                    "            printf(\"%s %d %u\\n\", on ? \"x60\" : \"x30\", u, (unsigned)field);\n"
                    "        }\n"
                    "    }\n"
                    "    return 0;\n"
                    "}\n",
                    encoding="utf-8",
                )
                exe = tmp / f"{name}.exe"
                assert GCC is not None, "skipUnless(GCC) runs this test only with a compiler"
                built = subprocess.run([GCC, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                        "-Wno-misleading-indentation", "-Wno-unused-variable",
                                        "-Wno-unused-value",
                                        f"-I{ROOT / 'src' / 'rt'}", "-o", str(exe), str(harness)],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
                return run.stdout.split("\n")

            out = program("store_gate", enhanced)
            authentic = program("authentic", stock)
            off = [line for line in out if line.startswith("x30 ")]
            self.assertEqual(off, [line for line in authentic if line.startswith("x30 ")])
            x30 = [int(line.split()[2]) for line in off]
            x60 = [int(line.split()[2]) for line in out if line.startswith("x60 ")]
            self.assertEqual(x30[:4], [32, 48, 56, 60])
            # Update 1 of 60 is odd and holds the start; then each 30 Hz value shows for two updates.
            self.assertEqual(x60[0], 0)
            for k, value in enumerate(x30[:19]):
                with self.subTest(update=k):
                    self.assertEqual(x60[2 * k + 1], value)
                    self.assertEqual(x60[2 * k + 2], value)

    def test_aot_cache_epoch_separation_and_authentic_invariance(self) -> None:
        """With package off, AOT cache digest is untouched; with package on, it keys a distinct epoch."""
        base_opts = {
            "base": "0x08804000",
            "codegen_profile": "none",
            "entry": "0x08804000",
            "extra_modules": [],
            "extra_spans": [],
            "funcs_per_chunk": 500,
            "game_name": "synthetic",
            "psp_header_mode": "none",
            "static_verify": False,
        }
        # Authentic options with empty environment must add zero keys
        enh_off = package_cache.enhancement_package_options({})
        self.assertEqual(enh_off, {})

        with tempfile.TemporaryDirectory() as tmp_dir:
            pkg_path = Path(tmp_dir) / "synth-60hz.json"
            pkg, _ = self._make_package_and_symbols(b"synthetic_elf_bytes")
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            enh_on = package_cache.enhancement_package_options({"ENHANCEMENT_PACKAGE": str(pkg_path)})
            self.assertEqual(enh_on["enhancement_package_epoch"], AOT_ENHANCEMENT_PACKAGE_EPOCH)
            self.assertEqual(enh_on["enhancement_package_id"], "synth.timing.60hz")
            self.assertEqual(len(enh_on["enhancement_package_sha256"]), 64)

            opts_authentic = dict(base_opts)
            opts_authentic.update(enh_off)
            opts_enhanced = dict(base_opts)
            opts_enhanced.update(enh_on)

            self.assertEqual(opts_authentic, base_opts)
            self.assertNotEqual(
                hashlib.sha256(package_cache.canonical_json(opts_authentic).encode("utf-8")).hexdigest(),
                hashlib.sha256(package_cache.canonical_json(opts_enhanced).encode("utf-8")).hexdigest(),
            )

    def _argument_package(self, tmp: Path, helper_words: dict[int, int]) -> tuple[dict[str, Any], dict[str, str]]:
        """A package with one argument_x2 site on a helper the entry calls."""
        words = dict(self.words)
        words[0x00001030] = 0x0C000000 | (0x00001700 >> 2)  # jal helper, then the stock return
        words[0x00001034] = 0x00000000
        words[0x00001038] = 0x03E00008
        words[0x0000103C] = 0x00000000
        words.update(helper_words)
        elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", words, self.entry)
        pkg, symbols = self._make_package_and_symbols(elf_bytes)
        symbols["synth_repeat_helper"] = "0x00001700"
        pkg["sites"].append({
            "id": "SYNTH_ARGS",
            "function": "synth_repeat_helper",
            "insn_offset": 0,
            "stock_digest": compute_stock_words_digest([helper_words[0x00001700]]),
            "transform": "argument_x2",
            "arguments": ["a3", "t0"],
        })
        return pkg, symbols

    def test_argument_x2_doubles_the_named_arguments_only_when_enabled(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            pkg, symbols = self._argument_package(tmp, {
                0x00001700: 0x8C820080,  # lw v0,128(a0)
                0x00001704: 0x03E00008,
                0x00001708: 0x00000000,
            })
            rc, output, files = self._codegen_with(tmp, "args", pkg, symbols)
            self.assertEqual(rc, 0, output)
            combined = b"\n".join(files.values()).decode("utf-8")
            lines = combined.splitlines()
            at = next(i for i, line in enumerate(lines) if "s->r[7] <<= 1; s->r[8] <<= 1;" in line)
            self.assertIn("if (nk_enhancement_on()) {", lines[at])
            # The doubling follows the stock load, and the other arm is the stock load alone.
            self.assertLess(lines[at].index("0x8c820080"), lines[at].index("s->r[7] <<= 1"))
            self.assertIn("else {", lines[at + 1])
            self.assertIn("0x8c820080", lines[at + 1])
            self.assertNotIn("<<= 1", lines[at + 1])

    def test_argument_x2_refuses_a_function_that_branches_back_to_its_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            pkg, symbols = self._argument_package(tmp, {
                0x00001700: 0x8C820080,  # lw v0,128(a0)
                0x00001704: 0x1040FFFE,  # beqz v0, entry: a second pass would double again
                0x00001708: 0x00000000,
                0x0000170C: 0x03E00008,
                0x00001710: 0x00000000,
            })
            with self.assertRaises(EnhancementPackageError) as ctx:
                self._codegen_with(tmp, "loop", pkg, symbols)
            self.assertIn("branches back to its entry", str(ctx.exception))

    def test_argument_x2_refuses_a_function_that_jumps_back_to_its_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            pkg, symbols = self._argument_package(tmp, {
                0x00001700: 0x8C820080,  # lw v0,128(a0)
                0x00001704: 0x08000000 | (0x00001700 >> 2),  # j entry: a second pass would double again
                0x00001708: 0x00000000,
            })
            with self.assertRaises(EnhancementPackageError) as ctx:
                self._codegen_with(tmp, "selfj", pkg, symbols)
            self.assertIn("branches back to its entry", str(ctx.exception))

    def test_a_site_that_is_never_emitted_refuses_the_package(self) -> None:
        """Every verified site must be emitted; a site whose function is stubbed refuses."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            pkg_path = tmp / "synth-60hz.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            (tmp / "synth-60hz.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            real_emit = codegen.emit_function

            def emit_or_stub(elf, start, *args, **kwargs):
                lines = real_emit(elf, start, *args, **kwargs)
                if start == self.fn_addrs["synth_velocity_step"]:
                    # The site was already rewritten when the function failed late.
                    raise codegen.Unsupported("synthetic late failure")
                return lines

            out_dir = tmp / "stubbed"
            out_dir.mkdir()
            with mock.patch.object(codegen, "emit_function", side_effect=emit_or_stub):
                rc, output, files = self._run_codegen(
                    elf_path, out_dir / "out.c", [f"--enhancement-package={pkg_path}"]
                )
            self.assertEqual(rc, 1, output)
            self.assertIn("SYNTH_STEP", output)
            self.assertIn("not emitted", output)
            self.assertEqual(files, {})

    def test_guest_symbol_map_names_functions_and_package_sites(self) -> None:
        """--symbol-map names guest functions and resolves the package's sites, over the ELF symbols."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)
            pkg, _ = self._make_package_and_symbols(elf_bytes)
            pkg_path = tmp / "synth-60hz.json"  # no synth-60hz.symbols.json sidecar
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            package = f"--enhancement-package={pkg_path}"

            # The map wins over the ELF symbol table: pointing a site's function at another
            # function's entry refuses the package on its stock digest.
            wrong_map = tmp / "wrong-symbols.txt"
            wrong_map.write_text("synth_vblank_wait = 0x00001200; // type:func\n",
                                 encoding="utf-8")
            wrong_dir = tmp / "wrong"
            wrong_dir.mkdir()
            rc, output, files = self._run_codegen(
                elf_path, wrong_dir / "out.c", [package, f"--symbol-map={wrong_map}"])
            self.assertEqual(rc, 1, output)
            self.assertIn("Stock instruction digest mismatch", output)
            self.assertEqual(files, {})

            # A map that agrees with the ELF resolves the same sites and names the function.
            guest_map = tmp / "guest-symbols.txt"
            guest_map.write_text("synth_vblank_wait = 0x00001100; // type:func size:0xC\n",
                                 encoding="utf-8")
            for name, extra in (("elf-symbols", []), ("named", [f"--symbol-map={guest_map}"])):
                with self.subTest(name):
                    out_dir = tmp / name
                    out_dir.mkdir()
                    rc, output, files = self._run_codegen(
                        elf_path, out_dir / "out.c", [package, *extra])
                    self.assertEqual(rc, 0, output)
                    combined = b"\n".join(files.values()).decode("utf-8")
                    self.assertIn("sites=6", combined)
                    self.assertIn("s->r[2] = (0u + 0x00000001u);", combined)
                    self.assertEqual("f_00001100_synth_vblank_wait" in combined, bool(extra))

    def test_site_names_leave_out_a_name_given_to_two_addresses(self) -> None:
        """A name the guest map gives to two functions resolves no site at all."""
        names = codegen.site_names_from_symbol_map({
            0x1100: ("synth_vblank_wait", 0xC),
            0x1200: ("helper", 0),
            0x1300: ("helper", 0),
        })
        self.assertEqual(names, {"synth_vblank_wait": 0x1100})

    def test_mapped_factor_runs_only_when_enabled_and_inside_the_unit_interval(self) -> None:
        """An lwc1 factor is mapped after the stock load, behind the toggle, and only inside (0, 1);
        outside it keeps its stock value and the site reports it."""
        lwc1 = 0xC4430010  # lwc1 $f3, 0x10($v0)
        saved = codegen.ENHANCEMENT_SITES
        try:
            for kind, expr in (("decay", "(float)((double)s->f[3] / (1.0 + sqrt(1.0 - (double)s->f[3])))"),
                               ("retention", "sqrtf(s->f[3])")):
                with self.subTest(map=kind):
                    codegen.ENHANCEMENT_SITES = {
                        0x1300: {"id": "F3", "rewrite": {"kind": "map_f_reg", "reg": 3, "map": kind}}
                    }
                    codegen.ENHANCEMENT_APPLIED.clear()
                    line = codegen.normal_line(0x1300, lwc1, lle_cpu=False)
                    enhanced, stock = line.rsplit("else", 1)
                    self.assertIn("nk_enhancement_on()", enhanced)
                    self.assertIn(f"if (s->f[3] > 0.0f && s->f[3] < 1.0f) {{ s->f[3] = {expr}; }} "
                                  'else { static int nk_kept; nk_enhancement_factor_kept(&nk_kept, "F3", '
                                  "0x00001300u, s->f[3]); }", enhanced)
                    self.assertLess(enhanced.index("s->fi[3]"), enhanced.index(expr))
                    self.assertNotIn("sqrt", stock)
                    self.assertNotIn("nk_enhancement_factor_kept", stock)
        finally:
            codegen.ENHANCEMENT_SITES = saved
            codegen.ENHANCEMENT_APPLIED.clear()

    def test_invalid_resolved_rewrite_fails_before_marking_the_site_applied(self) -> None:
        saved_sites = codegen.ENHANCEMENT_SITES
        saved_applied = set(codegen.ENHANCEMENT_APPLIED)
        saved_space = codegen.ADDRESS_SPACE
        try:
            codegen.ADDRESS_SPACE = codegen.AddressSpace()
            codegen.ENHANCEMENT_SITES = {0x1300: {"rewrite": "not-an-object"}}
            codegen.ENHANCEMENT_APPLIED.clear()
            with self.assertRaises(enhancement_package.EnhancementPackageError):
                codegen._enhancement_rewrite(0x1300)
            self.assertNotIn(0x1300, codegen.ENHANCEMENT_APPLIED)
        finally:
            codegen.ENHANCEMENT_SITES = saved_sites
            codegen.ENHANCEMENT_APPLIED.clear()
            codegen.ENHANCEMENT_APPLIED.update(saved_applied)
            codegen.ADDRESS_SPACE = saved_space

    def test_missing_llvm_transfer_fields_fail_closed(self) -> None:
        transfer_without_slot = lowering_plan.Transfer(0x1000, 0, "call")
        transfer_without_condition = lowering_plan.Transfer(0x1004, 0, "branch-tail")
        with self.assertRaises(codegen_llvm.LLVMUnsupported):
            codegen_llvm._require_delay_slot(transfer_without_slot)
        with self.assertRaises(codegen_llvm.LLVMUnsupported):
            codegen_llvm._require_condition(transfer_without_condition)

    def test_runtime_module_translation_collections_default_independently(self) -> None:
        first = codegen.RuntimeModuleTranslation(0, "synthetic", 0, "0" * 64, 0)
        second = codegen.RuntimeModuleTranslation(1, "other", 0, "1" * 64, 1)
        self.assertEqual((first.texts, first.functions, first.spans, first.stubbed), ([], [], [], []))
        self.assertEqual((second.texts, second.functions, second.spans, second.stubbed), ([], [], [], []))
        first.texts.append(codegen.ModuleText("synthetic body"))
        self.assertEqual(second.texts, [])

    def test_vfpu_single_load_is_scaled_or_mapped_after_the_load(self) -> None:
        """lv.s loads the VFPU register first; the rewrite then scales or maps that register."""
        lvs = 0xC8470011  # lv.s S131.s, 0x10($v0): vt 7 | 1 << 5
        v = codegen.vreg_indices(39, 1)[0]
        saved = codegen.ENHANCEMENT_SITES
        try:
            for rewrite, expr in (({"kind": "scale_v_reg", "vreg": 39, "factor": 2.0}, f"s->v[{v}] *= 2.0f;"),
                                  ({"kind": "map_v_reg", "vreg": 39, "map": "decay"},
                                   f"if (s->v[{v}] > 0.0f && s->v[{v}] < 1.0f) "
                                   f"{{ s->v[{v}] = (float)((double)s->v[{v}] / "
                                   f"(1.0 + sqrt(1.0 - (double)s->v[{v}]))); }}")):
                with self.subTest(kind=rewrite["kind"]):
                    codegen.ENHANCEMENT_SITES = {0x1300: {"id": "S131", "rewrite": rewrite}}
                    codegen.ENHANCEMENT_APPLIED.clear()
                    enhanced, stock = codegen.normal_line(0x1300, lvs, lle_cpu=False).rsplit("else", 1)
                    self.assertIn(expr, enhanced)
                    self.assertLess(enhanced.index(f"s->vi[{v}] = MEM_R32("), enhanced.index(expr))
                    self.assertNotIn(f"s->v[{v}]", stock)
        finally:
            codegen.ENHANCEMENT_SITES = saved
            codegen.ENHANCEMENT_APPLIED.clear()

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_mapped_factor_values_match_the_lui_path(self) -> None:
        """Compile the emitted map and run it. Inside (0, 1) each result is bit for bit the f32 the
        lui path's formula gives, and that is the f32 nearest the exact map down to the smallest
        normal result; 1 - sqrt(1 - k) cancels to 0 there even in double. A subnormal result may
        round once more, so it is within one step of the nearest. Outside (0, 1) the value keeps
        its stock bits and the site reports once."""
        lwc1 = 0xC4430010  # lwc1 $f3, 0x10($v0)
        rng = random.Random(1932)
        ks = [2.0 ** -25, 1e-7, 1e-4, 0.2, 0.5, 0.75, 0.999, _f32_of(0x3F7FFFFF),
              1e-17, 1e-30, 2.0 ** -126, _f32_of(0x00000001), _f32_of(0x007FFFFF)]
        ks += [rng.random() for _ in range(2000)]
        ks += [2.0 ** rng.uniform(-149, -20) for _ in range(500)]
        ks = [struct.unpack("<f", struct.pack("<f", k))[0] for k in ks]
        outside = [0.0, 1.0, 1.5, -0.25, -0.0, _f32_of(0x7FC00000)]  # the last is a quiet NaN
        lvs = 0xC844C410  # lv.s S100.s, -0x3bf0($v0)
        v4 = codegen.vreg_indices(4, 1)[0]
        forms = (("map_f_reg", {"reg": 3}, lwc1, "s->f[3]"), ("map_v_reg", {"vreg": 4}, lvs, f"s->v[{v4}]"))
        saved = codegen.ENHANCEMENT_SITES
        try:
            for (rkind, operand, word, cell), kind in itertools.product(forms, ("decay", "retention")):
                with self.subTest(rewrite=rkind, map=kind):
                    codegen.ENHANCEMENT_SITES = {
                        0x1300: {"id": "K", "rewrite": {"kind": rkind, **operand, "map": kind}}
                    }
                    codegen.ENHANCEMENT_APPLIED.clear()
                    enhanced = codegen.normal_line(0x1300, word, lle_cpu=False).rsplit("else", 1)[0]
                    mapped = enhanced[enhanced.index(f"if ({cell} >"):].rsplit("}", 1)[0]
                    values = ks + outside
                    with tempfile.TemporaryDirectory() as tmp_dir:
                        tmp = Path(tmp_dir)
                        (tmp / "k.txt").write_text("\n".join(struct.pack("<f", k).hex() for k in values),
                                                   encoding="utf-8")
                        harness = tmp / "map.c"
                        harness.write_text(
                            "#include <math.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
                            "typedef struct { float f[32]; float v[128]; } S;\n"
                            "static int reports;\n"
                            "static void nk_enhancement_factor_kept(int *reported, const char *site, "
                            "uint32_t pc, float value) {\n"
                            "    (void)site; (void)pc; (void)value;\n"
                            "    if (!*reported) { *reported = 1; reports++; }\n"
                            "}\n"
                            "int main(int argc, char **argv) {\n"
                            "    FILE *in = fopen(argv[1], \"r\"); char hex[16]; S st, *s = &st;\n"
                            "    if (argc < 2 || !in) return 1;\n"
                            "    while (fscanf(in, \"%15s\", hex) == 1) {\n"
                            "        uint8_t b[4]; uint32_t bits;\n"
                            "        for (int i = 0; i < 4; i++) sscanf(hex + 2 * i, \"%2hhx\", &b[i]);\n"
                            f"        memcpy(&{cell}, b, 4);\n"
                            f"        {mapped}\n"
                            f"        memcpy(&bits, &{cell}, 4); printf(\"%08x\\n\", (unsigned)bits);\n"
                            "    }\n"
                            "    printf(\"reports=%d\\n\", reports);\n"
                            "    fclose(in); return 0;\n"
                            "}\n",
                            encoding="utf-8",
                        )
                        exe = tmp / "map.exe"
                        subprocess.run([str(GCC), "-std=c11", "-O2", "-Wall", "-Werror", "-o", str(exe),
                                        str(harness), "-lm"], check=True, capture_output=True, text=True)
                        out = subprocess.run([str(exe), str(tmp / "k.txt")], check=True, capture_output=True,
                                             text=True).stdout.split()
                    self.assertEqual(out.pop(), "reports=1")
                    transform = {"decay": "decay_k_to_1-sqrt(1-k)", "retention": "retention_m_to_sqrt(m)"}[kind]
                    for k, got in zip(values, out, strict=True):
                        if not 0.0 < k < 1.0:
                            self.assertEqual(int(got, 16), struct.unpack("<I", struct.pack("<f", k))[0])
                            continue
                        want = struct.unpack("<I", struct.pack("<f", _scale_value(transform, k)))[0]
                        self.assertEqual(int(got, 16), want, f"k={k!r}")
                        if kind == "decay":
                            nearest = _f32_decay_reference(k)
                            self.assertLessEqual(abs(want - nearest), 0 if nearest >= 0x00800000 else 1,
                                                 f"k={k!r}")
        finally:
            codegen.ENHANCEMENT_SITES = saved
            codegen.ENHANCEMENT_APPLIED.clear()

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_mapped_register_copy_keeps_its_source_when_run(self) -> None:
        """A decay or retention site on mov.s fd, fs, as the planner plans it: compile the whole emitted
        statement and run it.
        With the toggle on, fd is the mapped factor and fs keeps its stock bits (the source may also
        be a duration); with the toggle off both are the stock copy."""
        mov_s = 0x46007506  # mov.s $f20, $f14
        k = struct.unpack("<f", struct.pack("<f", 0.2))[0]
        saved = codegen.ENHANCEMENT_SITES
        try:
            for kind in ("decay", "retention"):
                with self.subTest(map=kind):
                    transform = {"decay": "decay_k_to_1-sqrt(1-k)", "retention": "retention_m_to_sqrt(m)"}[kind]
                    rewrite = enhancement_package.plan_site_rewrite({"id": "K", "transform": transform}, mov_s)
                    codegen.ENHANCEMENT_SITES = {0x1300: {"id": "K", "rewrite": rewrite}}
                    codegen.ENHANCEMENT_APPLIED.clear()
                    line = codegen.normal_line(0x1300, mov_s, lle_cpu=False)
                    with tempfile.TemporaryDirectory() as tmp_dir:
                        tmp = Path(tmp_dir)
                        harness = tmp / "copy.c"
                        harness.write_text(
                            "#include <math.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
                            "typedef struct { float f[32]; } S;\n"
                            "static int on;\n"
                            "static int nk_enhancement_on(void) { return on; }\n"
                            "static void sr_begin(S *s, uint32_t pc, uint32_t w) { (void)s; (void)pc; (void)w; }\n"
                            "static void sr_end(S *s, uint32_t a, int n) { (void)s; (void)a; (void)n; }\n"
                            "static void nk_enhancement_factor_kept(int *reported, const char *site, "
                            "uint32_t pc, float value) { (void)reported; (void)site; (void)pc; (void)value; }\n"
                            "int main(void) {\n"
                            "    for (on = 0; on < 2; on++) {\n"
                            "        S st, *s = &st; uint32_t a, b;\n"
                            "        memset(&st, 0, sizeof st);\n"
                            f"        s->f[14] = {k!r}f; s->f[20] = 7.0f;\n"
                            f"    {line}\n"
                            "        memcpy(&a, &s->f[14], 4); memcpy(&b, &s->f[20], 4);\n"
                            "        printf(\"%08x %08x\\n\", (unsigned)a, (unsigned)b);\n"
                            "    }\n"
                            "    return 0;\n"
                            "}\n",
                            encoding="utf-8",
                        )
                        exe = tmp / "copy.exe"
                        subprocess.run([str(GCC), "-std=c11", "-O2", "-Wall", "-Werror", "-o", str(exe),
                                        str(harness), "-lm"], check=True, capture_output=True, text=True)
                        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()
                    stock = struct.unpack("<I", struct.pack("<f", k))[0]
                    mapped = struct.unpack("<I", struct.pack("<f", _scale_value(transform, k)))[0]
                    self.assertNotEqual(mapped, stock)
                    self.assertEqual([int(v, 16) for v in out], [stock, stock, stock, mapped])
        finally:
            codegen.ENHANCEMENT_SITES = saved
            codegen.ENHANCEMENT_APPLIED.clear()

    def test_lle_scaled_store_never_writes_the_fpr(self) -> None:
        """Under --lle-cpu a faulting swc1 returns early, so the rewrite must not touch s->f."""
        swc1 = 0xE4A40010  # swc1 $f4, 0x10($a1)
        saved_sites = codegen.ENHANCEMENT_SITES
        try:
            codegen.ENHANCEMENT_SITES = {
                0x1300: {"id": "F4", "rewrite": {"kind": "scale_f_store", "reg": 4, "factor": 0.5}}
            }
            codegen.ENHANCEMENT_APPLIED.clear()
            for lle in (False, True):
                with self.subTest(lle_cpu=lle):
                    options = codegen.CodegenOptions(lle_cpu=lle)
                    line = codegen.normal_line(0x1300, swc1, options=options)
                    enhanced = line.split("else", 1)[0]
                    self.assertNotIn("s->f[4] =", enhanced)
                    self.assertNotIn("s->f[4] *=", enhanced)
                    self.assertNotIn("s->fi[4]", enhanced)
                    self.assertIn("s->f[4] * 0.5f", enhanced)
                    if lle:
                        self.assertIn("return;", enhanced)
        finally:
            codegen.ENHANCEMENT_SITES = saved_sites
            codegen.ENHANCEMENT_APPLIED.clear()


class TestEnhancementDataTransforms(unittest.TestCase):
    """Data values and CSV cells rescale exactly or refuse."""

    def test_data_values(self) -> None:
        self.assertEqual(plan_data_value("count_x2", "s32", struct.pack("<i", -3)), struct.pack("<i", -6))
        self.assertEqual(plan_data_value("step_x0.5", "u16", struct.pack("<H", 40)), struct.pack("<H", 20))
        self.assertEqual(plan_data_value("retention_m_to_sqrt(m)", "f32", struct.pack("<f", 0.25)),
                         struct.pack("<f", 0.5))
        tiny = struct.unpack("<f", struct.pack("<f", 1e-30))[0]
        self.assertEqual(plan_data_value("decay_k_to_1-sqrt(1-k)", "f32", struct.pack("<f", tiny)),
                         struct.pack("<I", _f32_decay_reference(tiny)))
        for transform, dtype, raw in (
            ("step_x0.5", "s32", struct.pack("<i", 7)),          # odd count
            ("count_x2", "u8", struct.pack("<B", 200)),          # overflow
            ("decay_k_to_1-sqrt(1-k)", "s32", struct.pack("<i", 1)),
            ("decay_k_to_1-sqrt(1-k)", "f32", struct.pack("<f", 1.5)),
        ):
            with self.subTest(transform=transform, dtype=dtype), self.assertRaises(ValueError):
                plan_data_value(transform, dtype, raw)

    def test_data_sites_resolve_fail_closed(self) -> None:
        stock = struct.pack("<2i", 15, 45)
        pkg: dict[str, Any] = {
            "schema_version": 1, "id": "synth.data", "display_name": "Synthetic data", "version": "1.0.0",
            "category": "timing_frame_presentation", "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00", "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "Every roll stays on alternate updates."},
            "sites": [{"id": "S1", "function": "f", "insn_offset": 0,
                       "stock_digest": "b" * 64, "transform": "count_x2"}],
            "data_sites": [{"id": "D1", "symbol": "table", "offset": 4, "type": "s32", "count": 2,
                            "stock_digest": hashlib.sha256(stock).hexdigest(), "transform": "count_x2"}],
        }
        image = {0x104: stock}
        resolved = verify_and_resolve_data_sites(
            pkg, symbol_to_addr={"table": 0x100}, read_data_fn=lambda a, n: image.get(a))
        self.assertEqual(resolved, [{"id": "D1", "addr": 0x104, "stock": stock,
                                     "data": struct.pack("<2i", 30, 90)}])
        with self.assertRaisesRegex(EnhancementPackageError, "Cannot resolve data symbol"):
            verify_and_resolve_data_sites(pkg, symbol_to_addr={}, read_data_fn=lambda a, n: image.get(a))
        with self.assertRaisesRegex(EnhancementPackageError, "initialised data"):
            verify_and_resolve_data_sites(pkg, symbol_to_addr={"table": 0x100}, read_data_fn=lambda a, n: None)
        overlap = copy.deepcopy(pkg)
        overlap["data_sites"].append(dict(overlap["data_sites"][0], id="D2", offset=8))
        with self.assertRaisesRegex(EnhancementPackageError, "overlaps"):
            validate_package(overlap)
        misaligned = copy.deepcopy(pkg)
        misaligned["data_sites"][0]["offset"] = 2
        with self.assertRaisesRegex(EnhancementPackageError, "not aligned"):
            validate_package(misaligned)
        gate = copy.deepcopy(pkg)
        gate["data_sites"][0]["transform"] = "gate_30hz"
        with self.assertRaisesRegex(EnhancementPackageError, "has no data rescale"):
            validate_package(gate)

    def _csv_entry(self, csv: bytes, cells: list[dict[str, Any]]) -> dict[str, Any]:
        return {"id": "F1", "path": "data/p/t.csv", "sha256": hashlib.sha256(csv).hexdigest(),
                "format": "csv", "header_row": 0, "first_data_row": 1, "cells": cells}

    def test_csv_cells(self) -> None:
        csv = b"Key,A,B\nx,3,0.1\ny,10,2.50\n\n"
        out = transform_data_file(self._csv_entry(csv, [
            {"column": "A", "transform": "count_x2", "rows": [1]},
            {"column": "B", "transform": "decay_k_to_1-sqrt(1-k)", "rows": [0]},
        ]), csv)
        lines = out.split(b"\n")
        self.assertEqual(lines[1].split(b",")[1], b"3")            # row 0 left alone
        self.assertEqual(lines[2].split(b",")[1], b"20")
        decayed = struct.unpack("<f", struct.pack("<f", 1.0 - (1.0 - struct.unpack(
            "<f", struct.pack("<f", 0.1))[0]) ** 0.5))[0]
        cell = float(lines[1].split(b",")[2])
        self.assertEqual(struct.unpack("<f", struct.pack("<f", cell))[0], decayed)
        self.assertEqual(lines[2].split(b",")[2], b"2.50")          # row 1 left alone
        self.assertEqual(lines[3:], [b"", b""])                     # trailing lines kept
        for cells, why in (
            ([{"column": "C", "transform": "count_x2"}], "not exactly one header"),
            ([{"column": "Key", "transform": "count_x2"}], "not a plain decimal"),
            ([{"column": "A", "transform": "step_x0.5"}], "no exact rescale"),
            ([{"column": "A", "transform": "count_x2", "rows": [5]}], "past the last data row"),
        ):
            with self.subTest(cells=cells), self.assertRaises(EnhancementPackageError) as ctx:
                transform_data_file(self._csv_entry(csv, cells), csv)
            self.assertIn(why, str(ctx.exception))
        with self.assertRaisesRegex(EnhancementPackageError, "does not match its sha256"):
            transform_data_file(self._csv_entry(csv, [{"column": "A", "transform": "count_x2"}]), csv + b"x")

    def test_csv_cells_by_column_index(self) -> None:
        # A Shift-JIS header no ASCII name can match; its bytes never include ',' or '\n'.
        csv = b"Key,\x92\x6c,\x94\xf5\x8d\x6c\r\nx,20,a\r\ny,0.05,b\r\n"
        entry = self._csv_entry(csv, [{"column_index": 1, "transform": "count_x2", "rows": [0]},
                                      {"column_index": 1, "transform": "step_x0.5", "rows": [1]}])
        self.assertEqual(transform_data_file(entry, csv),
                         b"Key,\x92\x6c,\x94\xf5\x8d\x6c\r\nx,40,a\r\ny,0.025,b\r\n")
        entry["cells"] = [{"column_index": 1, "transform": "count_x2"},
                          {"column_index": 1, "transform": "step_x0.5", "rows": [1]}]
        with self.assertRaisesRegex(EnhancementPackageError, "line 3, column 1 is rescaled twice"):
            transform_data_file(entry, csv)
        entry["cells"] = [{"column_index": 2, "transform": "count_x2", "rows": [0]}]
        with self.assertRaisesRegex(EnhancementPackageError, "line 2, column 2: .*not a plain decimal"):
            transform_data_file(entry, csv)
        entry["cells"] = [{"column_index": 3, "transform": "count_x2"}]
        with self.assertRaisesRegex(EnhancementPackageError, "has no column 3"):
            transform_data_file(entry, csv)
        entry["cells"] = [{"column": "Key", "transform": "count_x2"},
                          {"column_index": 0, "transform": "count_x2"}]
        with self.assertRaisesRegex(EnhancementPackageError, "rescaled twice"):
            transform_data_file(entry, csv)

    def test_csv_cell_type(self) -> None:
        # The title reads every value in a column by its own type: an f32 written
        # without a decimal point halves exactly, which an integer cannot.
        csv = (b"Key,V\nx,5\ny,0.5\nz,2000000000\nw,-1\nv,300\nbig," + b"9" * 50
               + b"\nmax,340282346638528859811704183484516925440\n")
        cases: list[tuple[dict[str, Any], bytes | str]] = [
            ({"column": "V", "transform": "step_x0.5", "rows": [0]}, "integer cell 5 has no exact rescale"),
            ({"column": "V", "type": "f32", "transform": "step_x0.5", "rows": [0]}, b"x,2.5"),
            ({"column": "V", "type": "f32", "transform": "count_x2", "rows": [0]}, b"x,10.0"),
            ({"column": "V", "type": "s32", "transform": "count_x2", "rows": [0]}, b"x,10"),
            ({"column": "V", "type": "s32", "transform": "count_x2", "rows": [1]}, "s32 cell 0.5 has a fractional"),
            ({"column": "V", "type": "s32", "transform": "count_x2", "rows": [2]}, "out of range"),
            ({"column": "V", "type": "u32", "transform": "count_x2", "rows": [2]}, b"z,4000000000"),
            ({"column": "V", "type": "u8", "transform": "count_x2", "rows": [3]}, "u8 cell -1 is out of range"),
            # The stock value must fit the type too, not only the rescaled result.
            ({"column": "V", "type": "u8", "transform": "step_x0.5", "rows": [4]}, "u8 cell 300 is out of range"),
            ({"column": "V", "type": "f32", "transform": "count_x2", "rows": [5]}, "outside the float32 range"),
            ({"column": "V", "type": "f32", "transform": "count_x2", "rows": [6]}, "outside the float32 range"),
        ]
        for cell, want in cases:
            with self.subTest(cell=cell):
                if isinstance(want, bytes):
                    self.assertIn(want, transform_data_file(self._csv_entry(csv, [cell]), csv).split(b"\n"))
                else:
                    with self.assertRaisesRegex(EnhancementPackageError, want):
                        transform_data_file(self._csv_entry(csv, [cell]), csv)
        with self.assertRaisesRegex(EnhancementPackageError, "Unknown data type 'f64'"):
            _validate_data_files([self._csv_entry(csv, [{"column": "V", "type": "f64", "transform": "count_x2"}])])

        pkg: dict[str, Any] = {
            "schema_version": 1, "id": "synth.data", "display_name": "Synthetic data", "version": "1.0.0",
            "category": "timing_frame_presentation", "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00", "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "Every roll stays on alternate updates."},
            "sites": [{"id": "S1", "function": "f", "insn_offset": 0,
                       "stock_digest": "b" * 64, "transform": "count_x2"}],
            "data_files": [self._csv_entry(csv, [{"column_index": 1, "transform": "count_x2"}])],
        }
        validate_package(pkg)
        for cell, why in (
            ({"transform": "count_x2"}, "column or column_index"),
            ({"column": "A", "column_index": 1, "transform": "count_x2"}, "column or column_index"),
            ({"column_index": 256, "transform": "count_x2"}, r"must be an integer in \[0, 255\]"),
            ({"column_index": True, "transform": "count_x2"}, "must be an integer"),
            ({"column_index": 1, "transform": "step_x0.5", "rows": [0]}, "rescaled twice in the same row"),
        ):
            bad = copy.deepcopy(pkg)
            bad["data_files"][0]["cells"].append(cell)
            with self.subTest(cell=cell), self.assertRaisesRegex(EnhancementPackageError, why):
                validate_package(bad)
        disjoint = copy.deepcopy(pkg)
        disjoint["data_files"][0]["cells"] = [{"column_index": 1, "transform": "count_x2", "rows": [0]},
                                              {"column_index": 1, "transform": "step_x0.5", "rows": [1]}]
        validate_package(disjoint)

    def test_find_data_file_matches_path_without_case_and_digest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            root = Path(tmp_dir)
            for i, body in enumerate((b"old", b"new")):
                folder = root / f"a{i}.xb.d" / "Data" / "P"
                folder.mkdir(parents=True)
                (folder / "T.CSV").write_bytes(body)
            entry = self._csv_entry(b"new", [{"column": "A", "transform": "count_x2"}])
            self.assertEqual(find_data_file(entry, root), b"new")
            entry["sha256"] = hashlib.sha256(b"other").hexdigest()
            with self.assertRaisesRegex(EnhancementPackageError, "No file data/p/t.csv"):
                find_data_file(entry, root)
            entry["path"] = "p/t.csv/../t.csv"
            with self.assertRaisesRegex(EnhancementPackageError, "relative disc path"):
                validate_package({
                    "schema_version": 1, "id": "synth.data", "display_name": "Synthetic data",
                    "version": "1.0.0", "category": "timing_frame_presentation",
                    "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
                    "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
                    "rng_policy": {"mode": "subtick_30hz", "reason": "Every roll stays on alternate updates."},
                    "sites": [{"id": "S1", "function": "f", "insn_offset": 0,
                               "stock_digest": "b" * 64, "transform": "count_x2"}],
                    "data_files": [entry],
                })


def _f32_of(bits: int) -> float:
    return float(struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0])


def _bits_of(value: float) -> int:
    return int(struct.unpack("<I", struct.pack("<f", value))[0])


class TestEnhancementPhysicsAndOffsetTransforms(unittest.TestCase):
    """offset_x2, window_start_x2, accel_x0.25, launch_velocity_x0.5 and f64_high."""

    def _plan(self, transform: str, word: int, **extra: Any) -> dict[str, Any]:
        planned: dict[str, Any] = plan_site_rewrite(dict({"id": "S", "transform": transform}, **extra), word)
        return planned

    def test_offsets_and_windows(self) -> None:
        # addiu v0,v0,2 (a lookahead D + 2): offset_x2 doubles the term even though rt == rs.
        self.assertEqual(self._plan("offset_x2", 0x24420002)["word"], 0x24420004)
        self.assertEqual(self._plan("offset_x2", 0x2442FFFD)["word"], 0x2442FFFA)  # -3 -> -6
        # addiu v1,a1,-4 (window head-4 .. head-1, 3 updates) -> -7 (6 updates).
        self.assertEqual(self._plan("window_start_x2", 0x24A3FFFC)["word"], 0x24A3FFF9)
        for transform, word, why in (
            ("offset_x2", 0x24420000, "nonzero"),
            ("offset_x2", 0x28420002, "addi/addiu"),          # slti
            ("window_start_x2", 0x24A3FFFF, "-2 or less"),     # -1 has no span
            ("window_start_x2", 0x24A30004, "-2 or less"),
        ):
            with self.subTest(transform=transform, word=hex(word)):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    self._plan(transform, word)
                self.assertIn(why, str(ctx.exception))

    def test_argument_x2_plans_and_refuses(self) -> None:
        site = {"arguments": ["a3", "t0"], "insn_offset": 0, "word_count": 1}
        # lw v0,128(a0) leaves the arguments alone.
        self.assertEqual(self._plan("argument_x2", 0x8C820080, **site),
                         {"kind": "scale_int_args", "regs": [7, 8]})
        # lw a3,128(a0) writes one: doubling after it would double the loaded value.
        with self.assertRaises(EnhancementPackageError) as ctx:
            self._plan("argument_x2", 0x8C870080, **site)
        self.assertIn("writes an argument", str(ctx.exception))
        # Every other form that writes a3 refuses too, not only the plain loads.
        for name, word in self._A3_WRITERS:
            with self.subTest(form=name):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    self._plan("argument_x2", word, **site)
                self.assertIn("writes an argument", str(ctx.exception))

    # (form, encoding) of instructions that write a3 (r7).
    _A3_WRITERS = (
        ("lwl", 0x88870000),
        ("lwr", 0x98870000),
        ("ll", 0xC0870000),
        ("sc", 0xE0870000),
        ("mfc1", 0x44071000),
        ("cfc1", 0x4447F800),
        ("mfc0", 0x40076000),
        ("ext", 0x7C873800),
        ("ins", 0x7C873804),
        ("seb", 0x7C043C20),
        ("seh", 0x7C043E20),
        ("clz", 0x00803816),
        ("clo", 0x00803817),
        ("mfv", 0x48670000),
        ("mfvc", 0x48670080),
    )

    def test_gpr_written_table(self) -> None:
        for name, word in self._A3_WRITERS:
            with self.subTest(form=name):
                self.assertEqual(_gpr_written(word), 7)
        for name, word in (
            ("sw", 0xAFA70000),
            ("mtc1", 0x44843800),
            ("mtv", 0x48E70000),
            ("mtvc", 0x48E70080),
            ("mthi", 0x00800011),
            ("mtlo", 0x00800013),
            ("mult", 0x00870018),
            ("jr", 0x03E00008),
            ("syscall", 0x0000000C),
            ("sync", 0x0000000F),
            ("beq", 0x10870002),
        ):
            with self.subTest(form=name):
                self.assertIsNone(_gpr_written(word))

    def test_argument_x2_validation(self) -> None:
        for extra, why in (
            ({}, "needs a list"),
            ({"arguments": []}, "needs a list"),
            ({"arguments": ["s0"]}, "needs a list"),
            ({"arguments": ["a3", "a3"]}, "needs a list"),
            ({"arguments": "a3"}, "needs a list"),
            ({"arguments": ["a3"], "insn_offset": 4}, "function entry"),
            ({"arguments": ["a3"], "word_count": 2}, "function entry"),
        ):
            with self.subTest(extra=extra):
                site = dict({"transform": "argument_x2", "insn_offset": 0, "word_count": 1}, **extra)
                with self.assertRaises(EnhancementPackageError) as ctx:
                    _validate_site_extras(site, "sites[0]")
                self.assertIn(why, str(ctx.exception))
        with self.assertRaises(EnhancementPackageError) as ctx:
            _validate_site_extras({"transform": "count_x2", "insn_offset": 0, "word_count": 1,
                                   "arguments": ["a3"]}, "sites[0]")
        self.assertIn("belongs only to argument_x2", str(ctx.exception))

    def test_accel_and_double_upper_words(self) -> None:
        # lui v0,0x3d05 (the upper half of 0.0326667): two exponent steps are exact.
        word = self._plan("accel_x0.25", 0x3C023D05)["word"]
        self.assertEqual(_f32_of((word & 0xFFFF) << 16), _f32_of(0x3D050000) / 4)
        with self.assertRaises(EnhancementPackageError):
            self._plan("accel_x0.25", 0x24020002)
        # lui a0,0x4011 is the upper word of the double 4.25: halved is 2.125 (0x4001...).
        hi = self._plan("step_x0.5", 0x3C044011, operand="f64_high")["word"] & 0xFFFF
        self.assertEqual(hi, 0x4001)
        hi = self._plan("count_x2", 0x3C04C011, operand="f64_high")["word"] & 0xFFFF  # -4.25 -> -8.5
        self.assertEqual(hi, 0xC021)
        self.assertEqual(struct.unpack(">d", bytes.fromhex("4021000000000000"))[0], 8.5)
        with self.assertRaises(EnhancementPackageError):
            self._plan("decay_k_to_1-sqrt(1-k)", 0x3C043FE0, operand="f64_high")

    def test_launch_velocity_matches_the_30hz_arc_every_second_update(self) -> None:
        """v0' = v0/2 + g/8 with g' = g/4 puts a semi-implicit Euler arc on the same points."""
        v0, g = _f32_of(0xBE99999A), _f32_of(0x3D05CD7C)   # -0.3 and 0.032666668
        words = {
            0x1000: 0x3C02BE99, 0x1004: 0x3442999A,       # lui/ori v0 = v0
            0x1100: 0x3C023D05, 0x1140: 0x3442CD7C,       # lui ... ori (not adjacent) = g
        }
        pkg: dict[str, Any] = {
            "schema_version": 1, "id": "synth.jump", "display_name": "Synthetic jump", "version": "1.0.0",
            "category": "timing_frame_presentation", "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00", "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "sites": [
                {"id": "V0", "function": "jump", "insn_offset": 0, "word_count": 2,
                 "stock_digest": compute_stock_words_digest([words[0x1000], words[0x1004]]),
                 "transform": "launch_velocity_x0.5", "acceleration_site": "G"},
                {"id": "G", "function": "jump", "insn_offset": 0x100,
                 "stock_digest": compute_stock_words_digest([words[0x1100]]),
                 "low_insn_offset": 0x140, "low_stock_digest": compute_stock_words_digest([words[0x1140]]),
                 "transform": "accel_x0.25"},
            ],
        }
        resolved = verify_and_resolve_sites(
            pkg, executable_sha256="a" * 64, symbol_to_addr={"jump": 0x1000},
            read_words_fn=lambda a, n: [words[a + 4 * i] for i in range(n)] if a in words else None)
        new_v0 = _f32_of(((resolved[0x1000]["rewrite"]["word"] & 0xFFFF) << 16)
                         | (resolved[0x1004]["rewrite"]["word"] & 0xFFFF))
        new_g = _f32_of((resolved[0x1100]["rewrite"]["word"] & 0xFFFF) << 16 | 0xCD7C)
        self.assertEqual(new_v0, _f32_of(_bits_of(v0 / 2 + g / 8)))
        self.assertNotIn(0x1140, resolved)   # the pinned low word is read, never rewritten

        def arc(v: float, a: float, steps: int) -> list[float]:
            y, out = 0.0, []
            for _ in range(steps):
                v += a
                y += v
                out.append(y)
            return out
        stock, enhanced = arc(v0, g, 20), arc(new_v0, new_g, 40)
        for n in range(20):
            self.assertAlmostEqual(stock[n], enhanced[2 * n + 1], places=5)

        no_pin = copy.deepcopy(pkg)
        del no_pin["sites"][1]["low_insn_offset"], no_pin["sites"][1]["low_stock_digest"]
        with self.assertRaisesRegex(EnhancementPackageError, "does not pin its whole float"):
            verify_and_resolve_sites(
                no_pin, executable_sha256="a" * 64, symbol_to_addr={"jump": 0x1000},
                read_words_fn=lambda a, n: [words[a + 4 * i] for i in range(n)] if a in words else None)
        wrong = copy.deepcopy(pkg)
        wrong["sites"][1]["transform"] = "step_x0.5"
        with self.assertRaisesRegex(EnhancementPackageError, "must be an accel_x0.25 site"):
            validate_package(wrong)
        missing = copy.deepcopy(pkg)
        del missing["sites"][0]["acceleration_site"]
        with self.assertRaisesRegex(EnhancementPackageError, "needs acceleration_site"):
            validate_package(missing)


def _i(op: int, rs: int, rt: int, imm: int) -> int:
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def _jal(target: int) -> int:
    return (0x03 << 26) | ((target >> 2) & 0x3FFFFFF)


def _move(rd: int, rs: int) -> int:
    return (rs << 21) | (rd << 11) | 0x21     # addu rd, rs, zero


_NOP, _JR_RA = 0, 0x03E00008
_ZERO, _AT, _V0, _V1, _A0, _A1, _A2, _SP = 0, 1, 2, 3, 4, 5, 6, 29


def _program(blocks: dict[int, list[int]]) -> dict[int, int]:
    return {base + 4 * i: w for base, ws in blocks.items() for i, w in enumerate(ws)}


class TestEnhancementDraft(unittest.TestCase):
    """The 60 Hz draft finds the frame-pacing waits of a synthetic title."""

    STUB_REG, STUB_WAIT, STUB_MULTI = 0x9000, 0x9008, 0x9010
    IMPORTS = {STUB_REG: 0xCA04A2B9, STUB_WAIT: 0x984C27E7, STUB_MULTI: 0x40F1469C}

    def _counter_title(self, *, increment: bool = True, wrapper: bool = True) -> dict[int, int]:
        handler = [
            _i(0x0F, 0, _V1, 0x0031), _i(0x23, _V1, _A1, 0x105C),          # lw a1, counter
            _i(0x0F, 0, _A0, 0x0031),
            _i(0x09, _A1, _A1, 1 if increment else 0),                      # addiu a1, a1, 1
            _i(0x2B, _A0, _A1, 0x105C), _JR_RA, _NOP,
        ]
        if wrapper:   # register_vblank(handler) -> sceKernelRegisterSubIntrHandler(30, 0, handler, 0)
            registration = {
                0x1100: [_i(0x0F, 0, _A0, 0), _i(0x09, _A0, _A0, 0x1000), _jal(0x1180), _NOP, _JR_RA, _NOP],
                0x1180: [_i(0x09, _SP, _SP, -16), _move(_V1, _A0), _i(0x09, _ZERO, _A0, 30),
                         _move(_A2, _V1), _jal(self.STUB_REG), _NOP, _JR_RA, _NOP],
            }
        else:
            registration = {
                0x1100: [_i(0x0F, 0, _A2, 0), _i(0x09, _A2, _A2, 0x1000), _i(0x09, _ZERO, _A0, 30),
                         _jal(self.STUB_REG), _NOP, _JR_RA, _NOP],
            }
        wait = [
            _jal(self.STUB_WAIT), _NOP,                                       # wait one VBLANK
            _i(0x0F, 0, _V0, 0x0031), _i(0x23, _V0, _V0, 0x105C),
            _i(0x0B, _V0, _AT, 2),                                            # sltiu at, v0, 2
            _i(0x05, _AT, 0, -6 & 0xFFFF), _NOP,                              # bnez at, back
            _i(0x0F, 0, _V0, 0x0031), _i(0x2B, _V0, _ZERO, 0x105C),           # sw zero, counter
            _JR_RA, _NOP,
        ]
        return _program({0x1000: handler, **registration, 0x1200: wait,
                         0x1300: [_jal(0x1200), _NOP, _JR_RA, _NOP],          # frame body
                         0x1400: [_jal(0x1300), _NOP, _JR_RA, _NOP]})         # main loop

    def test_counter_poll_through_a_registration_wrapper(self) -> None:
        for wrapper in (True, False):
            with self.subTest(wrapper=wrapper):
                words = self._counter_title(wrapper=wrapper)
                draft = draft_timing_package(words, self.IMPORTS)
                self.assertEqual(len(draft["pacing_sites"]), 1)
                site = draft["pacing_sites"][0]
                self.assertEqual((site["kind"], site["address"], site["function"], site["insn_offset"]),
                                 ("vblank_counter_poll", 0x1210, 0x1200, 0x10))
                self.assertEqual(site["counter"], 0x0031105C)
                self.assertTrue(site["handler_registered"])
                self.assertEqual(draft["boundary_candidates"], [{"function": 0x1300, "calls": [0x1300]}])
                self.assertEqual(draft["unexplained_waits"], [])   # the poll's own one-VBLANK wait

    def test_package_validates_and_rewrites_the_poll(self) -> None:
        words = self._counter_title()
        package, symbols = draft_package_declaration(
            draft_timing_package(words, self.IMPORTS), words, package_id="synth-60hz",
            title_id="synthetic-allegrex-v1", executable_sha256="a" * 64, game_version="1.00")
        validate_package(package)
        self.assertEqual(symbols, {"draft_fn_00001200": "0x00001200", "draft_fn_00001300": "0x00001300"})
        self.assertEqual(package["update_boundary"]["function"], "draft_fn_00001300")
        site = package["sites"][0]
        self.assertEqual(site["transform"], "vblank_wait_2_to_1")
        self.assertEqual(plan_site_rewrite(site, words[0x1210])["word"], _i(0x0B, _V0, _AT, 1))
        resolved = verify_and_resolve_sites(
            package, executable_sha256="a" * 64, symbol_to_addr={k: int(v, 16) for k, v in symbols.items()},
            read_words_fn=lambda a, n: [words[a + 4 * i] for i in range(n)] if a in words else None)
        self.assertEqual(sorted(resolved), [0x1210])

    def test_wait_multi_and_unexplained_waits(self) -> None:
        words = _program({
            0x2000: [_i(0x09, _ZERO, _A0, 2), _jal(self.STUB_MULTI), _NOP, _JR_RA, _NOP],
            0x2100: [_jal(self.STUB_WAIT), _NOP, _jal(self.STUB_WAIT), _NOP, _JR_RA, _NOP],
            0x2200: [_jal(0x2000), _NOP, _jal(0x2000), _NOP, _JR_RA, _NOP],
            0x2300: [_jal(0x2000), _NOP, _jal(0x2100), _NOP, _JR_RA, _NOP],
        })
        draft = draft_timing_package(words, self.IMPORTS, function_starts=[0x2200, 0x2300])  # symbol entries
        self.assertEqual([(s["kind"], s["address"], s["api"]) for s in draft["pacing_sites"]],
                         [("wait_multi_2", 0x2000, "sceDisplayWaitVblankStartMulti")])
        self.assertEqual(draft["boundary_candidates"], [{"function": 0x2200, "calls": [0x2200, 0x2208]},
                                                        {"function": 0x2300, "calls": [0x2300]}])
        self.assertEqual([(w["call"], w["function"]) for w in draft["unexplained_waits"]],
                         [(0x2100, 0x2100), (0x2108, 0x2100)])
        package, _ = draft_package_declaration(draft, words, package_id="synth-60hz",
                                               title_id="synthetic-allegrex-v1", executable_sha256="a" * 64,
                                               game_version="1.00")
        self.assertNotIn("update_boundary", package)   # two candidates: a person chooses
        self.assertEqual(plan_site_rewrite(package["sites"][0], words[0x2000])["word"], _i(0x09, _ZERO, _A0, 1))

    def test_no_pacing_without_an_increment_or_a_clear(self) -> None:
        words = self._counter_title(increment=False)
        draft = draft_timing_package(words, self.IMPORTS)
        self.assertEqual(draft["pacing_sites"], [])
        self.assertEqual([w["call"] for w in draft["unexplained_waits"]], [0x1200])
        with self.assertRaisesRegex(EnhancementPackageError, "No frame-pacing wait"):
            draft_package_declaration(draft, words, package_id="synth-60hz", title_id="synthetic-allegrex-v1",
                                      executable_sha256="a" * 64, game_version="1.00")
        cleared_elsewhere = self._counter_title()
        cleared_elsewhere[0x1220] = _NOP     # drop the `sw zero, counter`
        self.assertEqual(draft_timing_package(cleared_elsewhere, self.IMPORTS)["pacing_sites"], [])


class TestEnhancementBuildPackageWiring(unittest.TestCase):
    """White-box tests of the build-package refusals and the package directory choice."""

    DISC = "ULUS99999"
    TITLE = "synthetic-title"
    EXE_SHA = "ab" * 32

    def setUp(self) -> None:
        import nk_cli
        import title_codegen_plan

        self.nk_cli = nk_cli
        self.plan = title_codegen_plan
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.pkg = {
            "schema_version": 1,
            "id": "synth.timing.60hz",
            "display_name": "Synthetic 60 Hz Logic Rescale",
            "version": "1.0.0",
            "category": "timing_frame_presentation",
            "target_title_id": self.TITLE,
            "game_version": "1.00",
            "executable_sha256": self.EXE_SHA,
            "capabilities": ["count_x2"],
            "rng_policy": {"mode": "rescale_probability", "reason": "Synthetic."},
            "sites": [{
                "id": "SYNTH_COUNT",
                "function": "synth_timer_duration",
                "insn_offset": 0,
                "stock_digest": compute_stock_words_digest([0x2404001E]),
                "transform": "count_x2",
            }],
        }

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_pkg(self, name: str, **changes: Any) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(dict(self.pkg, **changes)), encoding="utf-8")
        return path

    def _select(self, package: Path | None = None, experimental_60hz: bool = False,
                environment: dict[str, str] | None = None):
        return self.nk_cli._select_enhancement_package(
            self.root,
            self.DISC,
            title_id=self.TITLE,
            executable_sha256=self.EXE_SHA,
            enhancement_package=package,
            experimental_60hz=experimental_60hz,
            environment={} if environment is None else environment,
            shipped_enhancements=self.root / "shipped",
        )

    def test_no_package_builds_the_authentic_directory(self) -> None:
        self.assertEqual(self._select(), (None, self.root / "packages" / self.DISC))

    def test_explicit_package_builds_its_own_directory(self) -> None:
        path = self._write_pkg("pkg.json")
        selected, target = self._select(path)
        self.assertEqual(selected, path.resolve())
        self.assertEqual(target, self.root / "packages" / f"{self.DISC}-enhanced-synth.timing.60hz")

    def test_target_title_mismatch_refuses(self) -> None:
        path = self._write_pkg("pkg.json", target_title_id="other-title")
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "target_title_id"):
            self._select(path)

    def test_executable_digest_mismatch_refuses(self) -> None:
        path = self._write_pkg("pkg.json", executable_sha256="cd" * 32)
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "executable_sha256"):
            self._select(path)

    def test_experimental_60hz_without_a_package_refuses(self) -> None:
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "requires an enhancement package"):
            self._select(experimental_60hz=True)

    def test_experimental_60hz_finds_the_lowercase_package(self) -> None:
        path = self._write_pkg(f"enhancements/{self.DISC.lower()}-60hz.json")
        selected, target = self._select(experimental_60hz=True)
        self.assertEqual(selected, path.resolve())
        self.assertEqual(target.name, f"{self.DISC}-enhanced-synth.timing.60hz")

    def test_ambient_package_never_enhances_a_build(self) -> None:
        path = self._write_pkg("pkg.json")
        for environment in (
            {"ENHANCEMENT_PACKAGE": str(path)},
            {"CODEGEN_USER_ARGS": f"--static-verify --enhancement-package={path}"},
            {"MAKEFLAGS": f"-j3 ENHANCEMENT_PACKAGE={path}"},
        ):
            for explicit in (None, path):
                with self.subTest(environment=environment, explicit=explicit):
                    with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "environment"):
                        self._select(explicit, environment=environment)
        # Empty values name no package.
        self.assertEqual(
            self._select(environment={"ENHANCEMENT_PACKAGE": " ", "CODEGEN_USER_ARGS": "--static-verify"}),
            (None, self.root / "packages" / self.DISC),
        )

    def test_planner_refuses_an_ambient_package(self) -> None:
        path = self._write_pkg("pkg.json")
        for environment in (
            {"ENHANCEMENT_PACKAGE": str(path)},
            {"CODEGEN_USER_ARGS": f"--enhancement-package={path}"},
        ):
            for explicit in (None, path):
                with self.subTest(environment=environment, explicit=explicit):
                    with mock.patch.dict(os.environ, environment):
                        with self.assertRaises(self.plan.PackageRouteError) as ctx:
                            self.plan.build_package(
                                {},
                                manifest_path=self.root / "manifest.json",
                                game_elf=self.root / "game.elf",
                                output_dir=self.root / "out",
                                enhancement_package_path=explicit,
                            )
                    self.assertEqual(ctx.exception.code, "PACKAGE_AMBIENT_ENHANCEMENT")

    def test_the_build_route_names_the_code_hook_package(self) -> None:
        timing = self._write_pkg("pkg.json")
        hook = self._write_pkg("hook.json", category="guest_memory_code_hook", id="synth.widen.clip",
                               capabilities=["widen_visibility"], sites=[{
                                   "id": "SYNTH_WIDEN", "function": "synth_scene_cache", "insn_offset": 0,
                                   "stock_digest": compute_stock_words_digest([0x0C000440]),
                                   "transform": "clip_x_widen", "clip_register": "v0", "clip_offsets": [640]}])
        for key in ("rng_policy",):
            data = json.loads(hook.read_text(encoding="utf-8"))
            data.pop(key, None)
            hook.write_text(json.dumps(data), encoding="utf-8")
        # nk_cli: the planner arguments and the cache key's environment, unchanged without one.
        self.assertEqual(self.nk_cli._planner_package_args(timing, None, ""), ["--enhancement-package", str(timing)])
        self.assertEqual(self.nk_cli._planner_package_args(timing, hook, ""),
                         ["--enhancement-package", str(timing), "--code-hook-package", str(hook)])
        self.assertEqual(self.nk_cli._planner_package_args(None, None, str(self.root)), [])
        self.assertEqual(self.nk_cli._package_environment(timing, None), {"ENHANCEMENT_PACKAGE": str(timing)})
        self.assertEqual(self.nk_cli._package_environment(timing, hook),
                         {"ENHANCEMENT_PACKAGE": str(timing), "ENHANCEMENT_CODE_HOOK_PACKAGE": str(hook)})
        # nk_cli's cache key is computed with both packages named.
        class Stop(Exception):
            pass

        seen: list[dict[str, str]] = []

        def capture(_manifest: Any, environment: dict[str, str]) -> Any:
            seen.append(dict(environment))
            raise Stop

        manifest_path = self.root / "manifest.json"
        manifest_path.write_text("{}", encoding="utf-8")
        with mock.patch.object(self.nk_cli, "_package_codegen_options", side_effect=capture):
            with self.assertRaises(Stop):
                self.nk_cli._current_package_cache_key(
                    {"modules": []}, manifest_path, self.EXE_SHA, None, None, {},
                    enhancement_package_path=timing, code_hook_package_path=hook)
        self.assertEqual(seen[0]["ENHANCEMENT_PACKAGE"], str(timing))
        self.assertEqual(seen[0]["ENHANCEMENT_CODE_HOOK_PACKAGE"], str(hook))
        # The planner's cache options: the code-hook package is named only when there is one.
        plan = {"environment": {"GAME_BASE": "0x08804000", "GAME_ENTRY": "0x08804000", "TITLE_EXTRA_SPANS": ""},
                "codegen_profile": "none"}
        with mock.patch.dict(os.environ, {}, clear=False):
            for name in ("ENHANCEMENT_PACKAGE", "ENHANCEMENT_CODE_HOOK_PACKAGE", "CODEGEN_USER_ARGS"):
                os.environ.pop(name, None)
            alone = self.plan._cache_codegen_options(plan, selected_optional=set(), funcs_per_chunk=2000,
                                                     enhancement_package_path=timing)
            both = self.plan._cache_codegen_options(plan, selected_optional=set(), funcs_per_chunk=2000,
                                                    enhancement_package_path=timing, code_hook_package_path=hook)
        self.assertNotIn("code_hook_package_id", alone)
        self.assertEqual({k: v for k, v in both.items() if not k.startswith("code_hook_")}, alone)
        self.assertEqual(both["code_hook_package_id"], "synth.widen.clip")

    def test_planner_refuses_a_code_hook_package_without_an_enhancement_package(self) -> None:
        hook = self._write_pkg("hook.json", category="guest_memory_code_hook")
        with self.assertRaises(self.plan.PackageRouteError) as ctx:
            self.plan.build_package(
                {}, manifest_path=self.root / "manifest.json", game_elf=self.root / "game.elf",
                output_dir=self.root / "out", code_hook_package_path=hook)
        self.assertEqual(ctx.exception.code, "PACKAGE_INVALID_ENHANCEMENT")
        self.assertIn("beside an enhancement package", str(ctx.exception))
        with mock.patch.dict(os.environ, {"ENHANCEMENT_CODE_HOOK_PACKAGE": str(hook)}):
            with self.assertRaises(self.plan.PackageRouteError) as ctx:
                self.plan.build_package(
                    {}, manifest_path=self.root / "manifest.json", game_elf=self.root / "game.elf",
                    output_dir=self.root / "out", enhancement_package_path=self._write_pkg("pkg.json"))
        self.assertEqual(ctx.exception.code, "PACKAGE_AMBIENT_ENHANCEMENT")

    def test_invalid_explicit_package_refuses_the_cache_key(self) -> None:
        path = self._write_pkg("pkg.json", sites=[])
        with self.assertRaises(package_cache.PackageCacheError):
            package_cache.enhancement_package_options({"ENHANCEMENT_PACKAGE": str(path)})

    def _published(self, **changes: Any) -> dict[str, Any]:
        return dict(self.pkg, published_package=1, disc_id=self.DISC, disc_version="1.01",
                    digest_scheme=enhancement_package.PUBLISHED_DIGEST_SCHEME,
                    site_map={"synth_timer_duration": "0x00001200"}, **changes)

    def _select_published(self, sfo_disc_id: str | None, disc_version: str | None,
                          executable_sha256: str | None = None, **changes: Any):
        path = self.root / "published.json"
        path.write_text(json.dumps(self._published(**changes)), encoding="utf-8")
        return self.nk_cli._select_enhancement_package(
            self.root, self.DISC, title_id=self.TITLE,
            executable_sha256=self.EXE_SHA if executable_sha256 is None else executable_sha256,
            enhancement_package=path, experimental_60hz=False, environment={},
            sfo_disc_id=sfo_disc_id, disc_version=disc_version)

    def test_published_package_for_this_disc_builds_its_own_directory(self) -> None:
        selected, target = self._select_published(self.DISC, "1.01")
        self.assertEqual(selected, (self.root / "published.json").resolve())
        self.assertEqual(target.name, f"{self.DISC}-enhanced-synth.timing.60hz")

    def test_published_package_refuses_another_disc_revision_or_image_by_name(self) -> None:
        cases = {
            "other_disc": dict(sfo_disc_id="ULES99999", disc_version="1.01"),
            "other_revision": dict(sfo_disc_id=self.DISC, disc_version="1.00"),
            "modified_executable": dict(sfo_disc_id=self.DISC, disc_version="1.01", executable_sha256="cd" * 32),
        }
        for code, kwargs in cases.items():
            with self.subTest(code):
                with self.assertRaisesRegex(self.nk_cli.PackageBuildError, rf"refused \({code}\)") as ctx:
                    self._select_published(**kwargs)
                self.assertIn("Nothing was built", str(ctx.exception))
        # The disc is named before the title: another region's disc is other_disc, not a title mismatch.
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, r"refused \(other_disc\)"):
            self._select_published("ULES99999", "1.01", target_title_id="other-title")

    def _select_shipped(self) -> Any:
        return self.nk_cli._select_enhancement_package(
            self.root, self.DISC, title_id=self.TITLE, executable_sha256=self.EXE_SHA,
            enhancement_package=None, experimental_60hz=True, environment={},
            sfo_disc_id=self.DISC, disc_version="1.01", shipped_enhancements=self.root / "shipped")

    def test_experimental_60hz_falls_back_to_the_shipped_published_package(self) -> None:
        shipped = self.root / "shipped" / f"{self.DISC.lower()}-60hz.json"
        shipped.parent.mkdir(parents=True)
        shipped.write_text(json.dumps(self._published()), encoding="utf-8")
        selected, target = self._select_shipped()
        self.assertEqual(selected, shipped.resolve())
        self.assertEqual(target.name, f"{self.DISC}-enhanced-synth.timing.60hz")
        # The user's own package comes first.
        own = self._write_pkg(f"enhancements/{self.DISC.lower()}-60hz.json")
        self.assertEqual(self._select_shipped()[0], own.resolve())

    def test_an_unusable_own_package_is_not_replaced_by_the_shipped_one(self) -> None:
        shipped = self.root / "shipped" / f"{self.DISC.lower()}-60hz.json"
        shipped.parent.mkdir(parents=True)
        shipped.write_text(json.dumps(self._published()), encoding="utf-8")
        own = self._write_pkg(f"enhancements/{self.DISC.lower()}-60hz.json")
        own.write_text("{ not json", encoding="utf-8")
        with self.assertRaises((self.nk_cli.PackageBuildError, enhancement_package.EnhancementPackageError)):
            self._select_shipped()

    def test_no_package_in_either_place_names_both(self) -> None:
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "or a published one at") as ctx:
            self._select_shipped()
        self.assertIn(str(self.root / "shipped"), str(ctx.exception))

    def test_a_shipped_package_must_be_published(self) -> None:
        shipped = self.root / "shipped" / f"{self.DISC.lower()}-60hz.json"
        shipped.parent.mkdir(parents=True)
        shipped.write_text(json.dumps(self.pkg), encoding="utf-8")
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "not in the published form"):
            self._select_shipped()

    def test_every_shipped_package_is_found_for_its_own_disc(self) -> None:
        shipped = sorted(self.nk_cli.SHIPPED_ENHANCEMENTS.glob("*-60hz.json"))
        self.assertTrue(shipped)
        for path in shipped:
            with self.subTest(path.name):
                package = enhancement_package.load_and_validate_package(path)
                self.assertTrue(enhancement_package.is_published_package(package))
                self.assertEqual(path.name, f"{package['disc_id'].lower()}-60hz.json")

    def test_published_package_needs_the_disc_identity(self) -> None:
        for sfo_disc_id, disc_version in ((None, "1.01"), (self.DISC, None), ("", "")):
            with self.subTest(sfo_disc_id=sfo_disc_id, disc_version=disc_version):
                with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "DISC_ID and DISC_VERSION"):
                    self._select_published(sfo_disc_id, disc_version)

    # The code-hook package beside the timing package (contract section 9.9).

    def _hook_pkg(self, name: str, **changes: Any) -> Path:
        hook = {
            "schema_version": 1, "id": "synth.widen.clip", "display_name": "Synthetic clip widen",
            "version": "1.0.0", "category": "guest_memory_code_hook", "target_title_id": self.TITLE,
            "game_version": "1.00", "executable_sha256": self.EXE_SHA, "capabilities": ["widen_visibility"],
            "sites": [{"id": "SYNTH_WIDEN", "function": "synth_scene_cache", "insn_offset": 0,
                       "stock_digest": compute_stock_words_digest([0x0C000440]),
                       "transform": "clip_x_widen", "clip_register": "v0", "clip_offsets": [640]}],
        }
        hook.update(changes)
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(hook), encoding="utf-8")
        return path

    def _select_hook(self, package: Path | None = None, ace_cull: bool = False) -> Any:
        return self.nk_cli._select_code_hook_package(
            self.root, self.DISC, title_id=self.TITLE, executable_sha256=self.EXE_SHA,
            code_hook_package=package, ace_cull=ace_cull, shipped_enhancements=self.root / "shipped")

    def _stacked(self, timing: Path | None, hook: Path | None) -> Any:
        selected, target = self._select(timing)
        return self.nk_cli._stack_code_hook_package(self.root, self.DISC, selected, self._select_hook(hook), target)

    def test_without_a_code_hook_package_the_selection_is_unchanged(self) -> None:
        self.assertIsNone(self._select_hook())
        timing = self._write_pkg("pkg.json")
        self.assertEqual(self._stacked(timing, None),
                         (timing.resolve(), None, self.root / "packages" / f"{self.DISC}-enhanced-synth.timing.60hz"))
        self.assertEqual(self._stacked(None, None), (None, None, self.root / "packages" / self.DISC))

    def test_ace_cull_finds_the_lowercase_culling_package(self) -> None:
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "--ace-cull requires a culling package"):
            self._select_hook(ace_cull=True)
        own = self._hook_pkg(f"enhancements/{self.DISC.lower()}-cull.json")
        self.assertEqual(self._select_hook(ace_cull=True), own.resolve())

    def test_a_code_hook_package_beside_a_timing_package_builds_their_joint_directory(self) -> None:
        timing, hook = self._write_pkg("pkg.json"), self._hook_pkg("hook.json")
        self.assertEqual(self._stacked(timing, hook), (
            timing.resolve(), hook.resolve(),
            self.root / "packages" / f"{self.DISC}-enhanced-synth.timing.60hz+synth.widen.clip"))

    def test_a_code_hook_package_alone_is_the_one_package(self) -> None:
        hook = self._hook_pkg("hook.json")
        self.assertEqual(self._stacked(None, hook), (
            hook.resolve(), None, self.root / "packages" / f"{self.DISC}-enhanced-synth.widen.clip"))

    def test_a_code_hook_package_with_no_sites_is_refused(self) -> None:
        path = self._hook_pkg("no-sites.json")
        data = json.loads(path.read_text(encoding="utf-8"))
        for key in ("sites", "game_version", "executable_sha256"):
            del data[key]
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "has no sites"):
            self._select_hook(path)

    def test_a_code_hook_package_is_checked_like_the_enhancement_package(self) -> None:
        cases = {
            "not 'guest_memory_code_hook'": self._write_pkg("timing-as-hook.json"),
            "target_title_id": self._hook_pkg("other-title.json", target_title_id="other-title"),
            "executable_sha256": self._hook_pkg("other-exe.json", executable_sha256="cd" * 32),
        }
        for pattern, path in cases.items():
            with self.subTest(pattern):
                with self.assertRaisesRegex(self.nk_cli.PackageBuildError, pattern):
                    self._select_hook(path)

    def test_two_code_hook_packages_are_refused(self) -> None:
        first = self._hook_pkg("first.json", id="synth.first.hook")
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "is a code-hook package too"):
            self.nk_cli._stack_code_hook_package(self.root, self.DISC, first.resolve(),
                                                 self._hook_pkg("hook.json").resolve(), self.root)

    def test_a_joint_name_over_64_characters_is_refused(self) -> None:
        timing = self._write_pkg("pkg.json", id="t" * 40)
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "more than the 64"):
            self._stacked(timing, self._hook_pkg("hook.json", id="h" * 30))

    def test_the_cache_key_names_the_code_hook_package_only_when_there_is_one(self) -> None:
        from nk_core import package_cache

        timing, hook = self._write_pkg("pkg.json"), self._hook_pkg("hook.json")
        alone = package_cache.enhancement_package_options({"ENHANCEMENT_PACKAGE": str(timing)})
        self.assertEqual(sorted(alone), ["enhancement_package_epoch", "enhancement_package_id",
                                         "enhancement_package_sha256"])
        both = package_cache.enhancement_package_options(
            {"ENHANCEMENT_PACKAGE": str(timing), "ENHANCEMENT_CODE_HOOK_PACKAGE": str(hook)})
        self.assertEqual({k: v for k, v in both.items() if k in alone}, alone)
        self.assertEqual(both["code_hook_package_id"], "synth.widen.clip")
        self.assertEqual(both["code_hook_package_sha256"],
                         enhancement_package.compute_package_digest(enhancement_package.load_and_validate_package(hook)))
        self.assertNotIn("code_hook_symbol_map_sha256", both)
        # Its symbols sidecar names its sites, so an edited one is a different build.
        hook.with_suffix(".symbols.json").write_text('{"synth_scene_cache": "0x00001200"}', encoding="utf-8")
        named = package_cache.enhancement_package_options(
            {"ENHANCEMENT_PACKAGE": str(timing), "ENHANCEMENT_CODE_HOOK_PACKAGE": str(hook)})
        self.assertEqual(named["code_hook_symbol_map_sha256"], package_cache.sha256_file(hook.with_suffix(".symbols.json")))

    def test_an_inherited_code_hook_package_is_ambient(self) -> None:
        from nk_core import package_cache

        for env, named in (({"ENHANCEMENT_CODE_HOOK_PACKAGE": "x.json"}, "ENHANCEMENT_CODE_HOOK_PACKAGE"),
                           ({"MAKEFLAGS": "-j4 ENHANCEMENT_CODE_HOOK_PACKAGE=x.json"},
                            "MAKEFLAGS ENHANCEMENT_CODE_HOOK_PACKAGE"),
                           ({"CODEGEN_USER_ARGS": "--code-hook-package=x.json"},
                            "CODEGEN_USER_ARGS --code-hook-package")):
            with self.subTest(named):
                self.assertEqual(package_cache.ambient_enhancement_package(env), named)
        self.assertIsNone(package_cache.ambient_enhancement_package({"MAKEFLAGS": "-j4"}))


def _sample_csv(path: Path, *, hz: int, seconds: int, columns: dict[str, Any]) -> None:
    """Write a synthetic SR_SAMPLE_OUT file: one row per vblank, logic at `hz` updates a second.

    `columns` maps a label to a function of (update count, vblank) giving the cell text.
    """
    lines = ["vblank,updates," + ",".join(columns)]
    vblanks = round(seconds * 60000 / 1001)
    for v in range(1, vblanks + 1):
        n = (v * hz * 1001) // 60000
        lines.append(f"{v},{n & 0xFFFFFFFF}," + ",".join(fn(n, v) for fn in columns.values()))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class TestEnhancementRateComparison(unittest.TestCase):
    """Section 9.7: the side-by-side timing check of a stock and an enhanced run."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def _run(self, stock: dict[str, Any], enhanced: dict[str, Any], *extra: str,
             enhanced_hz: int = 60, seconds: int = 60) -> tuple[int, str, dict[str, Any]]:
        _sample_csv(self.tmp / "stock.csv", hz=30, seconds=seconds, columns=stock)
        _sample_csv(self.tmp / "enh.csv", hz=enhanced_hz, seconds=seconds, columns=enhanced)
        out = StringIO()
        with mock.patch("sys.stdout", out):
            rc = enhancement_package.main(["--compare-rates", str(self.tmp / "stock.csv"), str(self.tmp / "enh.csv"),
                                           "--json", str(self.tmp / "r.json"), *extra])
        return rc, out.getvalue(), json.loads((self.tmp / "r.json").read_text(encoding="utf-8"))

    def test_rescaled_events_match_and_pass(self) -> None:
        # One serve every 45 stock updates; the package doubles the count, so every 90 enhanced updates.
        rc, text, data = self._run({"serves": lambda n, v: str(n // 45)}, {"serves": lambda n, v: str(n // 90)},
                                   "--event", "count:serves")
        self.assertEqual(rc, 0, text)
        self.assertIn("rates: PASS", text)
        whole = data["report"]["phases"][0]
        self.assertEqual(whole["logic"], "OK")
        self.assertEqual(whole["events"][0]["verdict"], "PASS")
        self.assertAlmostEqual(whole["events"][0]["ratio"], 1.0, delta=0.05)

    def test_an_unrescaled_count_fails(self) -> None:
        # A frame count the package missed: the event comes twice as often per second.
        rc, text, data = self._run({"serves": lambda n, v: str(n // 45)}, {"serves": lambda n, v: str(n // 45)},
                                   "--event", "count:serves")
        self.assertEqual(rc, 1, text)
        self.assertIn("rates: FAIL", text)
        self.assertAlmostEqual(data["report"]["phases"][0]["events"][0]["ratio"], 2.0, delta=0.1)

    def test_lcg_steps_are_counted_and_compared_with_the_declared_ratio(self) -> None:
        a, c = 1103515245, 12345

        def lcg(steps: int) -> str:
            x = 7
            for _ in range(steps):
                x = (a * x + c) & 0xFFFFFFFF
            return str(x)

        cache: dict[int, str] = {}

        def rolls(n: int, v: int) -> str:  # one roll per update
            if n not in cache:
                cache[n] = lcg(n)
            return cache[n]

        rc, text, data = self._run({"seed": rolls}, {"seed": rolls}, "--event", f"lcg:seed:{a}:{c}=2",
                                   seconds=10)
        self.assertEqual(rc, 0, text)
        ev = data["report"]["phases"][0]["events"][0]
        self.assertEqual((ev["stock"], ev["verdict"], ev["unresolved"]), (data["stock"]["phases"]["all"]["updates"],
                                                                           "PASS", 0))
        rc, text, _ = self._run({"seed": rolls}, {"seed": rolls}, "--event", f"lcg:seed:{a}:{c}", seconds=10)
        self.assertEqual(rc, 1, text)

    def test_an_lcg_jump_past_the_limit_is_unresolved_not_zero(self) -> None:
        rc, text, data = self._run({"seed": lambda n, v: str(v * 977)}, {"seed": lambda n, v: str(v * 977)},
                                   "--event", "lcg:seed:1103515245:12345", "--lcg-limit", "8", seconds=2)
        self.assertEqual(rc, 1, text)
        self.assertGreater(data["report"]["unresolved"], 0)

    def test_slowed_logic_and_rare_events_are_incomplete_not_pass(self) -> None:
        rc, text, data = self._run({"serves": lambda n, v: str(n // 45)}, {"serves": lambda n, v: str(n // 90)},
                                   "--event", "count:serves", enhanced_hz=45)
        self.assertEqual(rc, 2, text)
        self.assertIn("rates: INCOMPLETE", text)
        self.assertEqual(data["report"]["phases"][0]["logic"], "SLOWED")
        rc, text, data = self._run({"rare": lambda n, v: str(n // 3000)}, {"rare": lambda n, v: str(n // 6000)},
                                   "--event", "count:rare")
        self.assertEqual(rc, 2, text)
        self.assertEqual(data["report"]["phases"][0]["events"][0]["verdict"], "NOT_JUDGED")

    def test_phases_split_by_column_and_the_counter_may_wrap(self) -> None:
        base = 0xFFFFFFC0  # wraps after 64 of the 120 updates
        _sample_csv(self.tmp / "s.csv", hz=30, seconds=4, columns={"mode": lambda n, v: "1" if v < 120 else "2"})
        lines = (self.tmp / "s.csv").read_text(encoding="utf-8").splitlines()
        wrapped = [lines[0]] + [",".join([c[0], str((int(c[1]) + base) & 0xFFFFFFFF), c[2]])
                                for c in (row.split(",") for row in lines[1:])]
        (self.tmp / "w.csv").write_text("\n".join(wrapped) + "\n", encoding="utf-8")
        rows = enhancement_package.load_sample_rows(self.tmp / "w.csv", ["updates", "mode"])
        m = enhancement_package.measure_rates(rows, updates="updates", nominal_hz=30, phase="mode")
        self.assertEqual(set(m["phases"]), {"1", "2"})
        total = sum(b["updates"] for b in m["phases"].values())
        self.assertEqual(total, int(lines[-1].split(",")[1]) - int(lines[1].split(",")[1]))
        self.assertAlmostEqual(m["phases"]["1"]["updates_per_second"], 30.0, delta=0.5)

    def test_change_events_and_a_loading_phase(self) -> None:
        # A flag that flips every 45 stock updates; the package doubles the period. The enhanced
        # run opens with a phase where no logic runs, which must not mark the run as slowed.
        flip = {"mode": lambda n, v: "1", "flag": lambda n, v: str((n // 45) % 2)}
        _sample_csv(self.tmp / "stock.csv", hz=30, seconds=60, columns=flip)
        _sample_csv(self.tmp / "enh.csv", hz=60, seconds=60,
                    columns={"mode": lambda n, v: "1", "flag": lambda n, v: str((n // 90) % 2)})
        lines = (self.tmp / "enh.csv").read_text(encoding="utf-8").splitlines()
        first_updates = lines[1].split(",")[1]
        loading = [f"{v},{first_updates},0,0" for v in range(1, 601)]
        shifted = [",".join([str(int(c[0]) + 600), *c[1:]]) for c in (row.split(",") for row in lines[1:])]
        (self.tmp / "enh.csv").write_text("\n".join([lines[0], *loading, *shifted]) + "\n", encoding="utf-8")
        out = StringIO()
        with mock.patch("sys.stdout", out):
            rc = enhancement_package.main(["--compare-rates", str(self.tmp / "stock.csv"), str(self.tmp / "enh.csv"),
                                           "--phase", "mode", "--event", "change:flag",
                                           "--json", str(self.tmp / "r.json")])
        data = json.loads((self.tmp / "r.json").read_text(encoding="utf-8"))
        self.assertEqual(rc, 0, out.getvalue())
        whole, loading_phase = data["report"]["phases"][0], data["report"]["phases"][1]
        self.assertEqual((loading_phase["phase"], loading_phase["logic"]), ("0", "ABSENT"))
        self.assertLess(whole["enhanced"]["updates_per_second"], 57.0)
        self.assertEqual(whole["events"][0]["verdict"], "PASS")
        self.assertGreater(whole["events"][0]["stock"], 30)

    def test_a_failing_phase_fails_a_run_whose_totals_pass(self) -> None:
        # Phase 1 (120 s) is rescaled right; phase 2 (20 s) runs its event at the stock period, so
        # twice as often per game second. The totals dilute it; the phase row must still fail.
        boundary = round(120 * 60000 / 1001)
        n_b = (boundary * 60 * 1001) // 60000

        def mode(n: int, v: int) -> str:
            return "1" if v < boundary else "2"

        def enhanced_count(n: int, v: int) -> str:
            return str(n // 18 if v < boundary else n_b // 18 + (n - n_b) // 9)

        rc, text, data = self._run({"mode": mode, "hits": lambda n, v: str(n // 9)},
                                   {"mode": mode, "hits": enhanced_count},
                                   "--phase", "mode", "--event", "count:hits", seconds=140)
        whole, first, second = data["report"]["phases"]
        self.assertEqual(whole["events"][0]["verdict"], "PASS", text)
        self.assertEqual(first["events"][0]["verdict"], "PASS", text)
        self.assertEqual(second["events"][0]["verdict"], "FAIL", text)
        self.assertEqual(rc, 1, text)
        self.assertIn("rates: FAIL (1 event(s): 1 result(s) differ [2 count:hits]", text)

    def test_a_truncated_or_one_sided_run_is_incomplete(self) -> None:
        serves = {"serves": lambda n, v: str(n // 45)}
        rc, text, data = self._run(serves, {"serves": lambda n, v: str(n // 90)}, "--event", "count:serves")
        self.assertEqual(rc, 0, text)
        # The enhanced capture stops halfway: its rates still agree, but it covers half the route.
        _sample_csv(self.tmp / "stock.csv", hz=30, seconds=60, columns=serves)
        _sample_csv(self.tmp / "enh.csv", hz=60, seconds=30, columns={"serves": lambda n, v: str(n // 90)})
        out = StringIO()
        with mock.patch("sys.stdout", out):
            rc = enhancement_package.main(["--compare-rates", str(self.tmp / "stock.csv"),
                                           str(self.tmp / "enh.csv"), "--event", "count:serves"])
        self.assertEqual(rc, 2, out.getvalue())
        self.assertIn("the shorter run covers 50% of the longer one's game time (below 90%)", out.getvalue())
        # A phase that runs logic in one run only means the runs did not follow the same route.
        rc, text, data = self._run({"mode": lambda n, v: "1", **serves},
                                   {"mode": lambda n, v: "1" if v < 3000 else "2",
                                    "serves": lambda n, v: str(n // 90)},
                                   "--phase", "mode", "--event", "count:serves")
        self.assertEqual(rc, 2, text)
        self.assertEqual(data["report"]["one_sided_phases"], ["2"])

    def test_repeated_rows_are_skipped_and_empty_cells_scope_the_event(self) -> None:
        # The sampler can write one vblank twice: an exact repeat is skipped and counted. An empty
        # event cell leaves that step out of the event's count and out of its game time.
        (self.tmp / "s.csv").write_text("vblank,updates,flag\n1,0,0\n2,1,0\n2,1,0\n3,2,1\n4,3,\n5,4,1\n",
                                        encoding="utf-8")
        rows = enhancement_package.load_sample_rows(self.tmp / "s.csv", ["updates", "flag"])
        m = enhancement_package.measure_rates(rows, updates="updates", nominal_hz=30,
                                              events=[enhancement_package.parse_rate_event("change:flag")])
        b = m["phases"]["all"]
        self.assertEqual((m["duplicate_rows"], m["empty_steps"]["change:flag"]), (1, 2))
        self.assertEqual((b["vblanks"], b["updates"], b["events"]["change:flag"]), (4, 4, 1))
        self.assertEqual(b["event_updates"]["change:flag"], 2)

    def test_a_capture_that_loses_time_refuses(self) -> None:
        cases = (
            ("vblank,updates\n1,0\nx,1\n", r"s.csv:3: column vblank: 'x' is not an integer"),
            ("vblank,updates\n1,0\n1,1\n", r"s.csv:3: vblank 1 does not follow vblank 1 with new values"),
            ("vblank,updates\n2,0\n1,1\n", r"s.csv:3: vblank 1 does not follow vblank 2"),
            ("vblank,updates\n1,0\n2,\n", r"s.csv:3: no updates value"),
            ("vblank,updates,serves\n1,0,0\n2,1,two\n", r"s.csv:3: column serves: 'two' is not an integer"),
        )
        for body, why in cases:
            (self.tmp / "s.csv").write_text(body, encoding="utf-8")
            columns = ["updates"] + (["serves"] if "serves" in body else [])
            rows = enhancement_package.load_sample_rows(self.tmp / "s.csv", columns)
            events = [enhancement_package.parse_rate_event("count:serves")] if "serves" in body else []
            with self.subTest(why=why), self.assertRaisesRegex(EnhancementPackageError, why):
                enhancement_package.measure_rates(rows, updates="updates", nominal_hz=30, events=events,
                                                  source=str(self.tmp / "s.csv"))

    def test_bad_inputs_and_options_give_a_rates_fail_line(self) -> None:
        _sample_csv(self.tmp / "good.csv", hz=30, seconds=1, columns={"serves": lambda n, v: str(n // 9)})
        (self.tmp / "latin1.csv").write_bytes(b"vblank,updates\n1,\xe9\n")
        good, event = str(self.tmp / "good.csv"), ["--event", "count:serves"]
        cases = (
            ([str(self.tmp / "missing.csv"), good, *event], "cannot read the sample file"),
            ([str(self.tmp / "latin1.csv"), good], "cannot read the sample file"),
            ([good, good, *event, "--stock-hz", "0"], "the nominal rate must be a positive number"),
            ([good, good, *event, "--enhanced-hz", "nan"], "the nominal rate must be a positive number"),
            ([good, good, *event, "--lcg-limit", "0"], "--lcg-limit must be at least 1"),
            ([good, good, *event, "--min-coverage", "2"], r"--min-coverage must lie in \[0, 1\]"),
        )
        for argv, why in cases:
            err = StringIO()
            with self.subTest(why=why), mock.patch("sys.stderr", err), mock.patch("sys.stdout", StringIO()):
                rc = enhancement_package.main(["--compare-rates", *argv])
                self.assertEqual(rc, 1)
                self.assertRegex(err.getvalue(), "rates: FAIL: .*" + why)

    def test_bad_sample_files_refuse(self) -> None:
        for body, why in (("", "empty sample file"), ("frame,updates\n1,0\n", "first column must be vblank"),
                          ("vblank,updates\n1,0\n2\n", r"s.csv:3: 1 cells, the header has 2")):
            (self.tmp / "s.csv").write_text(body, encoding="utf-8")
            with self.subTest(why=why), self.assertRaisesRegex(EnhancementPackageError, why):
                enhancement_package.load_sample_rows(self.tmp / "s.csv", ["updates"])
        _sample_csv(self.tmp / "s.csv", hz=30, seconds=1, columns={})
        err = StringIO()
        with mock.patch("sys.stderr", err):
            rc = enhancement_package.main(["--compare-rates", str(self.tmp / "s.csv"), str(self.tmp / "s.csv"),
                                           "--event", "count:serves"])
        self.assertEqual(rc, 1)
        self.assertIn("rates: FAIL:", err.getvalue())
        self.assertIn("no column serves", err.getvalue())

    def test_bad_event_specs_and_missing_columns_refuse(self) -> None:
        for spec in ("count", "sum:x", "lcg:x:1", "count:x=0", "change:x:y", "lcg:x:1:1:65"):
            with self.subTest(spec=spec), self.assertRaises(EnhancementPackageError):
                enhancement_package.parse_rate_event(spec)
        _sample_csv(self.tmp / "s.csv", hz=30, seconds=1, columns={})
        with self.assertRaisesRegex(EnhancementPackageError, "no column serves"):
            enhancement_package.load_sample_rows(self.tmp / "s.csv", ["updates", "serves"])


def _f32_bits_of(value: float) -> int:
    return int(struct.unpack("<I", struct.pack("<f", value))[0])


class TestEnhancementStoreSites(unittest.TestCase):
    """Load-time store sites: stock_values, the applied and mismatch lines, and requires."""

    SWC1 = 0xE4820000   # swc1 f2, 0(a0)
    SW = 0xAC850000     # sw a1, 0(a0)
    SW_SELF = 0xAC840000  # sw a0, 0(a0): the value register is also the base
    SW_ZERO = 0xAC800000  # sw zero, 0(a0)
    ADDIU = 0x2406001E  # addiu a2, zero, 30

    def _site(self, sid: str, word: int, **extra: Any) -> dict[str, Any]:
        return dict({"id": sid, "function": "synth_fn", "insn_offset": 0,
                     "stock_digest": compute_stock_words_digest([word]), "transform": "count_x2"},
                    **extra)

    def _package(self, sites: list[dict[str, Any]], **extra: Any) -> dict[str, Any]:
        return dict({
            "schema_version": 1, "id": "synth.store", "display_name": "Synthetic store sites",
            "version": "1.0.0", "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
            "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "sites": sites,
        }, **extra)

    def _plan(self, word: int, **extra: Any) -> dict[str, Any]:
        plan: dict[str, Any] = plan_site_rewrite(dict({"id": "S", "transform": "count_x2"}, **extra), word)
        return plan

    def _refused(self, word: int, why: str, **extra: Any) -> None:
        with self.subTest(word=hex(word), extra=extra):
            with self.assertRaisesRegex(EnhancementPackageError, why):
                self._plan(word, **extra)

    def test_validator_accepts_store_sites_with_stock_values(self) -> None:
        pkg = self._package([
            self._site("F_STORE", self.SWC1, stock_values=[30, 60.5]),
            self._site("I_STORE", self.SW, function="synth_other", stock_values=[9, -9]),
        ])
        validate_package(pkg)
        rw = self._plan(self.SWC1, stock_values=[30, 60.5])
        self.assertEqual(rw["kind"], "scale_f_store")
        self.assertEqual(rw["reg"], 2)
        self.assertEqual(rw["stock_values"], [[_f32_bits_of(30.0), _f32_bits_of(60.0)],
                                              [_f32_bits_of(60.5), _f32_bits_of(121.0)]])
        rw = self._plan(self.SW, stock_values=[9, -9])
        self.assertEqual(rw["kind"], "scale_int_store")
        self.assertEqual(rw["reg"], 5)
        self.assertEqual(rw["stock_values"], [[9, 18], [0xFFFFFFF7, 0xFFFFFFEE]])
        # A store without stock_values keeps its plain rewrite.
        self.assertEqual(self._plan(self.SWC1), {"kind": "scale_f_store", "reg": 2, "factor": 2.0})

    def test_stock_values_refusals(self) -> None:
        self._refused(self.ADDIU, "stock_values belongs only to", stock_values=[30])
        self._refused(self.SW, "needs stock_values")
        self._refused(self.SW, "no defined rewrite", transform="step_x0.5", stock_values=[8])
        self._refused(self.SW_SELF, "base register", stock_values=[8])
        self._refused(self.SW_ZERO, r"must not be \$zero", stock_values=[0])
        self._refused(self.SWC1, "duplicate", stock_values=[1.0, 1.0])
        self._refused(self.SWC1, "duplicate", stock_values=[0.1, 0.10000000149011612])
        self._refused(self.SWC1, "not finite", stock_values=[float("nan")])
        self._refused(self.SWC1, "not finite", stock_values=[float("inf")])
        self._refused(self.SWC1, "not finite", stock_values=[1e39])
        self._refused(self.SWC1, "not finite", stock_values=[3.0e38])   # doubled, past f32
        self._refused(self.SW, "duplicate", stock_values=[9, 9])
        self._refused(self.SW, "s32", stock_values=[2 ** 31])
        self._refused(self.SW, "s32", stock_values=[2 ** 30])           # doubled, 2^31 does not fit
        self._refused(self.SW, "s32", stock_values=[-2 ** 30 - 1])
        self._refused(self.SW, "integer", stock_values=[1.5])
        # The shape is checked before any instruction is read.
        for bad in ([], "30", [True], ["30"], [None], 30):
            with self.subTest(stock_values=bad):
                with self.assertRaisesRegex(EnhancementPackageError, "stock_values must be a non-empty list of numbers"):
                    validate_package(self._package([self._site("F_STORE", self.SWC1, stock_values=bad)]))

    def test_requires_validation(self) -> None:
        store = self._site("F_STORE", self.SWC1, stock_values=[30])
        other = self._site("G_COUNT", self.ADDIU, function="synth_gate", requires=["F_STORE"])
        validate_package(self._package([store, other]))
        cases = (
            ("unknown", [store, dict(other, requires=["NOPE"])], "names sites that declare stock_values"),
            ("self", [dict(store, requires=["F_STORE"])], "its own"),
            ("duplicate", [store, dict(other, requires=["F_STORE", "F_STORE"])], "duplicate"),
            ("no stock_values", [self._site("PLAIN", self.ADDIU, function="synth_plain"),
                                 dict(other, requires=["PLAIN"])],
             "names sites that declare stock_values"),
            ("empty", [store, dict(other, requires=[])], "requires must be a non-empty list"),
            ("not a list", [store, dict(other, requires="F_STORE")], "requires must be a non-empty list"),
        )
        for name, sites, why in cases:
            with self.subTest(name):
                with self.assertRaisesRegex(EnhancementPackageError, why):
                    validate_package(self._package(sites))
        # A required site may not itself wait on another site.
        first = self._site("A_STORE", self.SW, function="synth_a", stock_values=[9])
        chained = self._site("B_STORE", self.SWC1, function="synth_b", stock_values=[30],
                             requires=["A_STORE"])
        with self.assertRaisesRegex(EnhancementPackageError, "itself declares requires"):
            validate_package(self._package([first, chained, dict(other, requires=["B_STORE"])]))
        # A data entry is not a site.
        data_site = {"id": "D_COUNT", "symbol": "synth_table", "offset": 0, "type": "s32",
                     "stock_digest": hashlib.sha256(struct.pack("<i", 21)).hexdigest(),
                     "transform": "count_x2"}
        with self.assertRaisesRegex(EnhancementPackageError, "names sites that declare stock_values"):
            validate_package(self._package([store, dict(other, requires=["D_COUNT"])],
                                           data_sites=[data_site]))
        data_file = {"id": "F_TABLE", "path": "data/param/table.csv", "sha256": "c" * 64,
                     "format": "csv", "header_row": 0, "first_data_row": 1,
                     "cells": [{"column": "Wait", "transform": "count_x2"}]}
        with self.assertRaisesRegex(EnhancementPackageError, "names sites that declare stock_values"):
            validate_package(self._package([store, dict(other, requires=["F_TABLE"])],
                                           data_files=[data_file]))

    def test_stock_values_refused_on_a_two_word_site(self) -> None:
        lui, ori = 0x3C0141F0, 0x34210000   # lui at, 0x41f0; ori at, at, 0: 30.0f
        words = {0x1000: lui, 0x1004: ori}
        pair = self._site("P_PAIR", lui, function="pair", word_count=2, transform="step_x0.5",
                          stock_digest=compute_stock_words_digest([lui, ori]))
        pkg = self._package([pair])

        def resolve(package: dict[str, Any]) -> dict[int, Any]:
            resolved: dict[int, Any] = verify_and_resolve_sites(
                package, executable_sha256="a" * 64, symbol_to_addr={"pair": 0x1000},
                read_words_fn=lambda a, n: [words[a + 4 * i] for i in range(n)] if a in words else None)
            return resolved

        self.assertEqual(sorted(resolve(pkg)), [0x1000, 0x1004])
        pkg["sites"][0]["stock_values"] = [30]
        with self.assertRaisesRegex(EnhancementPackageError, "stock_values belongs only to swc1 and sw"):
            resolve(pkg)

    # -- compile and run ---------------------------------------------------------------

    def _generate(self, tmp: Path, gate_transform: str | None = None, published: bool = False) -> dict[str, str]:
        """Generate the store-site fixture. With ``published``, from its published form, whose store
        sites carry only keyed digests of their stock values. With ``gate_transform``, the entry also calls
        0x1400, whose first word begins each logic update and whose ``jal 0x1500`` at 0x1404 (an ease
        leaf, as gate_30hz_hold needs) is a gate site of that transform that requires F_STORE."""
        def jal(target: int) -> int:
            return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)

        words = {
            0x1000: jal(0x1100), 0x1004: 0, 0x1008: jal(0x1200), 0x100C: 0,
            0x1010: jal(0x1300), 0x1014: 0, 0x1018: 0x03E00008, 0x101C: 0,
            0x1100: self.SWC1, 0x1104: 0x03E00008, 0x1108: 0,
            0x1200: self.SW, 0x1204: 0x03E00008, 0x1208: 0,
            0x1300: self.ADDIU, 0x1304: 0x03E00008, 0x1308: 0,
        }
        symbols = {"fstore": "0x1100", "istore": "0x1200", "gcount": "0x1300"}
        if gate_transform is not None:
            words.update({0x1018: jal(0x1400), 0x101C: 0, 0x1020: 0x03E00008, 0x1024: 0,
                          0x1400: 0x24070001, 0x1404: jal(0x1500), 0x1408: 0,  # addiu a3, zero, 1
                          0x140C: 0x03E00008, 0x1410: 0,
                          # sub.s f1, f13, f12; add.s f0, f12, f1; jr ra; nop
                          0x1500: 0x460C6841, 0x1504: 0x46016000, 0x1508: 0x03E00008, 0x150C: 0})
            symbols["hgate"] = "0x1400"
        elf = _write_synthetic_mips_elf(tmp / "synth.elf", words, 0x1000)
        sites = [
            dict(self._site("F_STORE", self.SWC1, stock_values=[30, 60]), function="fstore"),
            dict(self._site("I_STORE", self.SW, stock_values=[9, -9]), function="istore"),
            dict(self._site("G_COUNT", self.ADDIU, requires=["F_STORE"]), function="gcount"),
        ]
        extra: dict[str, Any] = {}
        if gate_transform is not None:
            sites.append(dict(self._site("H_GATE", jal(0x1500), requires=["F_STORE"]),
                              function="hgate", insn_offset=4, transform=gate_transform))
            extra["update_boundary"] = {"function": "hgate", "insn_offset": 0,
                                        "stock_digest": compute_stock_words_digest([0x24070001])}
        pkg = self._package(sites, executable_sha256=hashlib.sha256(elf).hexdigest(), **extra)
        overlap = ("site F_STORE: stock value 30 scales to 60, which is also a stock value; a path that "
                   "stores an already scaled value again would scale it twice")
        if published:
            from analyze import Elf

            read_words, read_data = enhancement_package.elf_readers(Elf(elf, base=0))
            notes: list[str] = []
            pkg = enhancement_package.publish_package(
                pkg, {k: int(v, 16) for k, v in symbols.items()}, executable=elf, disc_id="ULUS99999",
                disc_version="1.00", read_words_fn=read_words, read_data_fn=read_data, notes=notes)
            # The overlap is found where the values are known: when the package is published.
            self.assertEqual(notes, [overlap])
        (tmp / "store.json").write_text(json.dumps(pkg), encoding="utf-8")
        (tmp / "store.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
        out_dir = tmp / "out"
        out_dir.mkdir()
        rc, output, files = TestEnhancementAotCodegenAndCache._run_codegen(
            tmp / "synth.elf", out_dir / "out.c", [f"--enhancement-package={tmp / 'store.json'}"])
        self.assertEqual(rc, 0, output)
        if not published:
            # 30 scales to 60, which is also a stock value: a build-time warning, not a refusal.
            self.assertIn(f"warning: enhancement {overlap}", output)
            self.assertEqual(output.count("warning: enhancement site"), 1, output)
        return {name: data.decode("utf-8") for name, data in files.items()}

    def _store_harness(self, tmp: Path, files: dict[str, str], name: str) -> dict[int, Path]:
        """The store-site block and the three rewritten statements in a host program, built with the
        runtime's SHA-256 (vfpu_tables.c), toggle off (0) and on (1)."""
        main, chunk = files["out.c"], files["out_0.c"]
        block = main[main.index("/* Enhancement store sites. */"):main.index("/* End of enhancement store sites. */")]
        harness = (
            "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n"
            "typedef struct { uint32_t r[32]; union { float f[32]; uint32_t fi[32]; }; } CpuState;\n"
            "static uint32_t mem[4];\n"
            "#define sr_begin(s, pc, op) ((void)0)\n#define sr_end(s, a, n) ((void)0)\n"
            "#define MEM_W32_PC(a, v, pc) (mem[((a) >> 2) & 3u] = (uint32_t)(v))\n"
            "void sr_at_process_end(void (*report)(void)) { (void)report; }\n"
            f"int nk_enhancement_on(void) {{ return NK_ON; }}\n{block}\n"
            f"static void run_f(CpuState *s) {{\n{self._statement(chunk, 0x1100)}\n}}\n"
            f"static void run_i(CpuState *s) {{\n{self._statement(chunk, 0x1200)}\n}}\n"
            f"static void run_g(CpuState *s) {{\n{self._statement(chunk, 0x1300)}\n}}\n"
            "int main(int argc, char **argv) {\n"
            "    CpuState s; memset(&s, 0, sizeof s); s.r[4] = 4;\n"
            "    nk_enh_sites_start();\n"
            "    for (int i = 1; i < argc; i++) {\n"
            "        char k = argv[i][0]; long v = k == 'G' ? 0 : strtol(argv[i] + 2, NULL, 0);\n"
            "        mem[1] = 0xdeadbeefu;\n"
            "        if (k == 'F') { s.fi[2] = (uint32_t)v; run_f(&s); "
            "printf(\"F %08x %08x\\n\", mem[1], s.fi[2]); }\n"
            "        if (k == 'I') { s.r[5] = (uint32_t)v; run_i(&s); "
            "printf(\"I %08x %08x\\n\", mem[1], s.r[5]); }\n"
            "        if (k == 'G') { s.r[6] = 0; run_g(&s); printf(\"G %u\\n\", s.r[6]); }\n"
            "        fflush(stdout);\n"
            "    }\n    return 0;\n}\n")
        src = tmp / f"{name}_harness.c"
        src.write_text(harness, encoding="utf-8")
        exes = {}
        for on in (0, 1):
            exes[on] = tmp / f"{name}{on}.exe"
            assert GCC is not None
            built = subprocess.run([str(GCC), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                    f"-DNK_ON={on}", f"-I{ROOT / 'src' / 'rt'}", "-o", str(exes[on]), str(src),
                                    str(ROOT / "src" / "rt" / "vfpu_tables.c"), "-lm"],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
        return exes

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_published_store_sites_check_the_same_values_by_keyed_digest(self) -> None:
        """A published package keeps no stock value; its store sites accept, scale, refuse and report
        exactly as the private package's do, by digesting the value being stored."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            (tmp / "plain").mkdir()
            (tmp / "published").mkdir()
            plain = self._generate(tmp / "plain")
            keyed = self._generate(tmp / "published", published=True)
            published = json.loads((tmp / "published" / "store.json").read_text(encoding="utf-8"))
            for site in published["sites"][:2]:
                self.assertNotIn("stock_values", site)
                self.assertEqual(len(site["stock_value_digests"]), 2)
            self.assertEqual(enhancement_package.published_package_findings(published), [])
            # No stock value reaches the published build's source: no table, a key and digests instead.
            self.assertIn("nk_enh_stock_0", plain["out.c"])
            self.assertNotIn("nk_enh_stock_", keyed["out.c"])
            self.assertIn("static const unsigned char nk_enh_key[32]", keyed["out.c"])
            exes = {"plain": self._store_harness(tmp, plain, "plain"),
                    "keyed": self._store_harness(tmp, keyed, "keyed")}
            f30, f60, f120 = (f"F:0x{_f32_bits_of(v):08x}" for v in (30.0, 60.0, 120.0))
            runs: list[tuple[int, list[str]]] = [
                (1, [f30, "I:9", "I:-9"]),
                (1, [f60, f"F:0x{_f32_bits_of(1.0):08x}", f120]),   # 1.0 is no stock value: stop
                (1, ["I:10"]),
                (1, [*([f30] * 20), f60, f30]),
                (1, ["G", "G", f30, "G"]),
                (1, ["I:-9", "I:9", "I:-9", "I:9"]),
                (1, []),
                (0, [f30, "F:0x3f800000", "I:9", "I:10", "G"]),
            ]
            for on, args in runs:
                with self.subTest(on=on, args=args):
                    results = {name: subprocess.run([str(e[on]), *args], capture_output=True, text=True)
                               for name, e in exes.items()}
                    self.assertEqual(
                        (results["keyed"].returncode, results["keyed"].stdout, results["keyed"].stderr),
                        (results["plain"].returncode, results["plain"].stdout, results["plain"].stderr))
            res = subprocess.run([str(exes["keyed"][1]), "I:10"], capture_output=True, text=True)
            self.assertEqual(res.returncode, 1)
            self.assertIn("ENHANCEMENT_DATA_MISMATCH: site I_STORE at 0x00001200 stores 10 (0x0000000a), "
                          "not one of the package's stock values; the run stops here", res.stderr)

    def test_stock_value_digests_validation(self) -> None:
        digest = "d" * 64
        store = self._site("F_STORE", self.SWC1, stock_value_digests=[digest])
        other = self._site("G_COUNT", self.ADDIU, function="synth_gate", requires=["F_STORE"])
        validate_package(self._package([store, other]))
        rw = self._plan(self.SW, stock_value_digests=[digest])
        self.assertEqual(rw, {"kind": "scale_int_store", "reg": 5, "stock_value_digests": [digest]})
        self._refused(self.ADDIU, "stock_value_digests belongs only to", stock_value_digests=[digest])
        for bad, why in (([], "1 to 64"), (["D" * 64], "1 to 64"), ([digest, digest], "duplicate"),
                         ([digest] * 65, "1 to 64"), (digest, "1 to 64")):
            with self.subTest(stock_value_digests=bad):
                with self.assertRaisesRegex(EnhancementPackageError, why):
                    validate_package(self._package([self._site("F_STORE", self.SWC1, stock_value_digests=bad)]))
        with self.assertRaisesRegex(EnhancementPackageError, "not both"):
            validate_package(self._package([self._site("F_STORE", self.SWC1, stock_values=[30],
                                                       stock_value_digests=[digest])]))
        key = enhancement_package.published_digest_key(b"synthetic executable")
        # The digest binds the site: the same value at another site has another digest.
        self.assertNotEqual(enhancement_package.keyed_stock_value_digest(key, "A", 30),
                            enhancement_package.keyed_stock_value_digest(key, "B", 30))

    @staticmethod
    def _statement(chunk: str, pc: int) -> str:
        """The toggle-on and stock lines of the rewritten site at ``pc``."""
        lines = chunk.splitlines()
        marker = f"sr_begin(s, 0x{pc:08x}u"
        at = next(i for i, line in enumerate(lines)
                  if line.lstrip().startswith("else {") and marker in line)
        if not lines[at - 1].lstrip().startswith("if (nk_enhancement_on())"):
            raise AssertionError(lines[at - 1])
        return lines[at - 1] + "\n" + lines[at]

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_store_sites_check_report_and_gate_when_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            files = self._generate(tmp)
            main, chunk = files["out.c"], files["out_0.c"]
            register_all = main[main.index("void sr_register_all(void)"):]
            self.assertIn("    nk_enh_sites_start();\n", register_all[:register_all.index("\n}")])
            begin = main.index("/* Enhancement store sites. */")
            end = main.index("/* End of enhancement store sites. */")
            block = main[begin:end]
            harness = (
                "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
                "typedef struct { uint32_t r[32]; union { float f[32]; uint32_t fi[32]; }; } CpuState;\n"
                "static uint32_t mem[4];\n"
                "#define sr_begin(s, pc, op) ((void)0)\n#define sr_end(s, a, n) ((void)0)\n"
                "#define MEM_W32_PC(a, v, pc) (mem[((a) >> 2) & 3u] = (uint32_t)(v))\n"
                "void sr_perf_shutdown(void);\n"
                f"int nk_enhancement_on(void) {{ return NK_ON; }}\n{block}\n"
                f"static void run_f(CpuState *s) {{\n{self._statement(chunk, 0x1100)}\n}}\n"
                f"static void run_i(CpuState *s) {{\n{self._statement(chunk, 0x1200)}\n}}\n"
                f"static void run_g(CpuState *s) {{\n{self._statement(chunk, 0x1300)}\n}}\n"
                "int main(int argc, char **argv) {\n"
                "    CpuState s; memset(&s, 0, sizeof s); s.r[4] = 4;\n"
                "    nk_enh_sites_start();\n"
                "    for (int i = 1; i < argc; i++) {\n"
                "        char k = argv[i][0]; long v = k == 'G' || k == 'X' ? 0 : strtol(argv[i] + 2, NULL, 0);\n"
                "        mem[1] = 0xdeadbeefu;\n"
                "        if (k == 'F') { s.fi[2] = (uint32_t)v; run_f(&s); "
                "printf(\"F %08x %08x\\n\", mem[1], s.fi[2]); }\n"
                "        if (k == 'I') { s.r[5] = (uint32_t)v; run_i(&s); "
                "printf(\"I %08x %08x\\n\", mem[1], s.r[5]); }\n"
                "        if (k == 'G') { s.r[6] = 0; run_g(&s); printf(\"G %u\\n\", s.r[6]); }\n"
                "        /* X ends as hle_display.c's headless end of run does: reports, then _Exit. */\n"
                "        if (k == 'X') { fflush(stdout); sr_perf_shutdown(); _Exit(0); }\n"
                "        fflush(stdout);\n"
                "    }\n    return 0;\n}\n")
            src = tmp / "store_harness.c"
            src.write_text(harness, encoding="utf-8")
            exes = {}
            for on in (0, 1):
                exes[on] = tmp / f"store{on}.exe"
                subprocess.run([str(GCC), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                f"-DNK_ON={on}", f"-I{ROOT / 'src' / 'rt'}", "-o", str(exes[on]), str(src),
                                str(ROOT / "src" / "rt" / "perf.c"), "-lm"],
                               check=True, capture_output=True, text=True)

            def run(on: int, *args: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run([str(exes[on]), *args], capture_output=True, text=True)

            f30, f60, f120 = (f"0x{_f32_bits_of(v):08x}" for v in (30.0, 60.0, 120.0))
            f_unreached, i_unreached = (
                f"ENHANCEMENT_DATA_SITE_NOT_REACHED: site {sid} at 0x{pc} stored no value in this run; "
                "whatever it should have rescaled stayed stock"
                for sid, pc in (("F_STORE", "00001100"), ("I_STORE", "00001200")))
            # Toggle on, a stock value: the doubled value is stored, the register is not changed.
            res = run(1, f"F:{f30}", "I:9", "I:-9")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.splitlines(), [
                f"F {f60[2:]} {f30[2:]}", "I 00000012 00000009", "I ffffffee fffffff7"])
            self.assertEqual(res.stderr.splitlines(), [
                "ENHANCEMENT_DATA_APPLIED: site F_STORE at 0x00001100: 30 -> 60",
                "ENHANCEMENT_DATA_APPLIED: site I_STORE at 0x00001200: 9 -> 18",
                "ENHANCEMENT_DATA_APPLIED: site I_STORE at 0x00001200: -9 -> -18",
            ])
            # A value that is not a stock value stops the run before it is stored.
            res = run(1, f"F:{f60}", f"F:0x{_f32_bits_of(1.0):08x}", f"F:{f120}")
            self.assertEqual(res.returncode, 1)
            self.assertEqual(res.stdout.splitlines(), [f"F {f120[2:]} {f60[2:]}"])
            self.assertIn("ENHANCEMENT_DATA_MISMATCH: site F_STORE at 0x00001100 stores 1 (0x3f800000), "
                          "not one of the package's stock values; the run stops here", res.stderr)
            self.assertNotIn("NOT_REACHED", res.stderr)   # the stop is the report
            res = run(1, "I:10")
            self.assertEqual(res.returncode, 1)
            self.assertIn("ENHANCEMENT_DATA_MISMATCH: site I_STORE at 0x00001200 stores 10 (0x0000000a), "
                          "not one of the package's stock values; the run stops here", res.stderr)
            self.assertEqual(res.stdout, "")
            # Toggle off: the stock statement, no check, no output.
            res = run(0, f"F:{f30}", "F:0x3f800000", "I:9", "I:10", "G")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.splitlines(), [
                f"F {f30[2:]} {f30[2:]}", "F 3f800000 3f800000", "I 00000009 00000009",
                "I 0000000a 0000000a", "G 30"])
            self.assertEqual(res.stderr, "")
            # Only the first 16 applications of a site are logged.
            res = run(1, *(f"F:{f30}" for _ in range(20)))
            self.assertEqual(res.returncode, 0, res.stderr)
            lines = res.stderr.splitlines()
            self.assertEqual(len(lines), 18, res.stderr)
            self.assertTrue(all(line == "ENHANCEMENT_DATA_APPLIED: site F_STORE at 0x00001100: 30 -> 60"
                                for line in lines[:16]))
            self.assertEqual(lines[16],
                             "ENHANCEMENT_DATA_APPLIED: site F_STORE: later applications are not logged")
            self.assertEqual(lines[17], i_unreached)
            # requires: stock until the required site has applied, one line per run.
            res = run(1, "G", "G", f"F:{f30}", "G")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.splitlines(), ["G 30", "G 30", f"F {f60[2:]} {f30[2:]}", "G 60"])
            self.assertEqual(res.stderr.splitlines(), [
                "ENHANCEMENT_SITE_GATED: site G_COUNT at 0x00001300 runs stock: F_STORE has not applied",
                "ENHANCEMENT_DATA_APPLIED: site F_STORE at 0x00001100: 30 -> 60",
                i_unreached,
            ])
            # A store site that never runs is reported at exit, one line per site; a site
            # without stock_values (G_COUNT) is not a store site and has nothing to report.
            res = run(1, "G")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stderr.splitlines(), [
                "ENHANCEMENT_SITE_GATED: site G_COUNT at 0x00001300 runs stock: F_STORE has not applied",
                f_unreached, i_unreached,
            ])
            res = run(1)
            self.assertEqual(res.stderr.splitlines(), [f_unreached, i_unreached])
            self.assertEqual(run(0).stderr, "")
            # The headless end of run leaves through _Exit, which skips atexit: the runtime's
            # end-of-process list prints the report, once, and the exit status is unchanged.
            res = run(1, f"F:{f30}", "X")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.splitlines(), [f"F {f60[2:]} {f30[2:]}"])
            self.assertEqual(res.stderr.splitlines(), [
                "ENHANCEMENT_DATA_APPLIED: site F_STORE at 0x00001100: 30 -> 60", i_unreached])
            res = run(0, "X")
            self.assertEqual((res.returncode, res.stderr), (0, ""))

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_requires_holds_a_gated_call_stock_until_ready(self) -> None:
        """A gate_30hz or gate_30hz_hold call site with ``requires`` makes the call every
        update, as stock, until the required site has applied; then it gates."""
        for transform in ("gate_30hz", "gate_30hz_hold"):
            with self.subTest(transform=transform), tempfile.TemporaryDirectory() as tmp_dir:
                tmp = Path(tmp_dir)
                files = self._generate(tmp, gate_transform=transform)
                main, chunk = files["out.c"], files["out_0.c"]
                begin = main.index("/* Enhancement store sites. */")
                block = main[begin:main.index("/* End of enhancement store sites. */")]
                lines = chunk.splitlines()
                at = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001404u" in line)
                gate = next(i for i in range(at, len(lines)) if "nk_enhancement_gate_30hz()) {" in lines[i])
                self.assertIn("|| !nk_enh_ready(", lines[gate])
                end = next(i for i in range(gate + 1, len(lines)) if lines[i].lstrip().startswith("}"))
                call = "\n".join(lines[at:end + 1])
                callee = re.search(r"return; (\w+)\(s\);", call)
                self.assertIsNotNone(callee, call)
                assert callee is not None
                harness = (
                    "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
                    "typedef struct { uint32_t r[32]; union { float f[32]; uint32_t fi[32]; }; } CpuState;\n"
                    "#define sr_begin(s, pc, op) ((void)0)\n#define sr_end(s, a, n) ((void)0)\n"
                    "#define MEM_W32_PC(a, v, pc) ((void)(v))\n"
                    "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
                    "#define sr_aot_return_boundary_end(s) 0\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
                    "void sr_perf_shutdown(void);\n"
                    "int nk_enhancement_on(void) { return 1; }\n"
                    "uint32_t nk_enhancement_updates;\n"
                    "static inline int nk_enhancement_gate_30hz(void) "
                    "{ return (nk_enhancement_updates & 1u) == 0u; }\n"
                    f"{block}\n"
                    "static unsigned calls;\n"
                    f"static void {callee.group(1)}(CpuState *s) {{ (void)s; calls++; }}\n"
                    f"static void run_f(CpuState *s) {{\n{self._statement(chunk, 0x1100)}\n}}\n"
                    f"static void run_h(CpuState *s) {{\n{call}\n}}\n"
                    "int main(void) {\n"
                    "    CpuState s; memset(&s, 0, sizeof s); s.r[4] = 4;\n"
                    "    nk_enh_sites_start();\n"
                    "    for (nk_enhancement_updates = 1; nk_enhancement_updates <= 4; nk_enhancement_updates++)\n"
                    "        run_h(&s);\n"
                    "    printf(\"before %u\\n\", calls); calls = 0;\n"
                    "    s.fi[2] = 0x41f00000u; run_f(&s);\n"
                    "    for (nk_enhancement_updates = 5; nk_enhancement_updates <= 8; nk_enhancement_updates++)\n"
                    "        run_h(&s);\n"
                    "    printf(\"after %u\\n\", calls); fflush(stdout);\n"
                    "    return 0;\n}\n")
                src = tmp / "gate_harness.c"
                src.write_text(harness, encoding="utf-8")
                exe = tmp / "gate.exe"
                assert GCC is not None
                built = subprocess.run([str(GCC), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                        "-Wno-misleading-indentation", f"-I{ROOT / 'src' / 'rt'}", "-o",
                                        str(exe), str(src), str(ROOT / "src" / "rt" / "perf.c"), "-lm"],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                res = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, res.stderr)
                # Stock, every update, while F_STORE has not applied; then every second update.
                self.assertEqual(res.stdout.splitlines(), ["before 4", "after 2"])
                self.assertIn("ENHANCEMENT_SITE_GATED: site H_GATE at 0x00001404 runs stock: "
                              "F_STORE has not applied", res.stderr)

    def test_every_runtime_exit_path_reaches_the_end_of_process_reports(self) -> None:
        """A store site's NOT_REACHED report runs from sr_perf_shutdown(); every _Exit/_exit
        in the runtime calls it first. HLE self-tests that do not link perf.c stub it."""
        exit_call = re.compile(r"\b_(?:Exit|exit)\(")
        report = re.compile(r"\bsr_perf_shutdown\(\);")
        # verify_harness.c is its own program (#1843): it runs a synthetic module with no
        # enhancement package and does not link perf.c, so it has no store sites to report.
        not_the_runtime = {"verify_harness.c"}
        paths = 0
        for path in sorted((ROOT / "src" / "rt").glob("*.c")):
            if path.name in not_the_runtime:
                continue
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for n, line in enumerate(lines):
                code = line.split("//", 1)[0]
                if not exit_call.search(code) or code.lstrip().startswith(("*", "/*")):
                    continue
                paths += 1
                window = "\n".join(lines[max(0, n - 10):n + 1])
                with self.subTest(path=f"{path.name}:{n + 1}"):
                    self.assertRegex(window, report)
        self.assertGreaterEqual(paths, 12)

    def test_package_without_store_keys_emits_no_store_table(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            case = TestEnhancementAotCodegenAndCache(
                "test_gated_sites_count_logic_updates_at_the_update_boundary")
            case.setUp()
            elf = _write_synthetic_mips_elf(tmp / "synth.elf", case.words, case.entry)
            pkg, symbols = case._make_package_and_symbols(elf)
            rc, output, files = case._codegen_with(tmp, "plain", pkg, symbols)
            self.assertEqual(rc, 0, output)
            combined = b"\n".join(files.values()).decode("utf-8")
            self.assertNotIn("nk_enh_store", combined)
            self.assertNotIn("nk_enh_ready", combined)
            self.assertNotIn("Enhancement store sites", combined)


class TestHoldAndEvenStaticChecks(unittest.TestCase):
    """The resolver's static checks behind gate_30hz_hold and count_x2_even (section 9.2):
    a held callee drops nothing but its result, and an even bound compares a stored count."""

    JAL_EASE = 0x0C000800       # jal 0x2000
    NOP, JR_RA = 0x00000000, 0x03E00008
    SUB = 0x460C6841            # sub.s f1, f13, f12
    MUL = 0x460E0842            # mul.s f1, f1, f14
    ADD_F0 = 0x46016000         # add.s f0, f12, f1
    LW_A0 = 0x8CA40000          # lw a0, 0(a1)
    SLTI = 0x28830064           # slti v1, a0, 100

    def _resolve(self, words: dict[int, int], site: dict[str, Any]) -> dict[int, Any]:
        pkg = {
            "schema_version": 1, "id": "synth.hold", "display_name": "Synthetic hold and even sites",
            "version": "1.0.0", "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
            "executable_sha256": "a" * 64, "capabilities": ["gate_30hz"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "update_boundary": {"function": "caller", "insn_offset": 8,
                                "stock_digest": compute_stock_words_digest([self.JR_RA])},
            "sites": [site],
        }
        resolved: dict[int, Any] = verify_and_resolve_sites(
            pkg, executable_sha256="a" * 64,
            symbol_to_addr={"caller": 0x1000, "counter": 0x1100},
            read_words_fn=lambda a, n: ([words[a + 4 * i] for i in range(n)]
                                        if all(a + 4 * i in words for i in range(n)) else None))
        return resolved

    def _hold(self, callee: list[int]) -> dict[int, Any]:
        words = {0x1000: self.JAL_EASE, 0x1004: self.NOP, 0x1008: self.JR_RA, 0x100C: self.NOP}
        words.update({0x2000 + 4 * i: w for i, w in enumerate(callee)})
        return self._resolve(words, {
            "id": "HOLD", "function": "caller", "insn_offset": 0,
            "stock_digest": compute_stock_words_digest([self.JAL_EASE]), "transform": "gate_30hz_hold"})

    def _even(self, body: list[int]) -> dict[int, Any]:
        words = {0x1100 + 4 * i: w for i, w in enumerate(body)}
        offset = 4 * body.index(self.SLTI)
        return self._resolve(words, {
            "id": "EVEN", "function": "counter", "insn_offset": offset,
            "stock_digest": compute_stock_words_digest([self.SLTI]), "transform": "count_x2_even"})

    def test_an_ease_callee_is_held(self) -> None:
        ease = [self.SUB, self.MUL, self.ADD_F0, self.JR_RA, self.NOP]
        self.assertEqual(self._hold(ease)[0x1000]["rewrite"], {"kind": "gate", "hold_f": 12})
        # A forward branch inside the body (bc1f to the return) is allowed.
        self.assertIn(0x1000, self._hold([self.SUB, 0x45000002, self.MUL, self.ADD_F0, self.JR_RA, self.NOP]))

    def test_hold_callee_refusals(self) -> None:
        cases = (
            ("store", [self.SUB, 0xE4810000, self.ADD_F0, self.JR_RA, self.NOP], "writes memory at 0x00002004"),
            ("call", [self.SUB, 0x0C000C00, self.ADD_F0, self.JR_RA, self.NOP], "jumps or calls"),
            ("jalr", [self.SUB, 0x0320F809, self.ADD_F0, self.JR_RA, self.NOP], "calls through a register"),
            ("tail jump", [self.SUB, self.ADD_F0, 0x03200008, self.NOP, self.JR_RA, self.NOP],
             "jumps through a register"),
            ("syscall", [self.SUB, 0x0000000C, self.ADD_F0, self.JR_RA, self.NOP], "traps"),
            ("store in the delay slot", [self.SUB, self.ADD_F0, self.JR_RA, 0xAC850000], "writes memory"),
            ("branch out", [self.SUB, 0x10000040, self.ADD_F0, self.JR_RA, self.NOP], "branches from 0x00002004"),
            ("f12 written first", [0x46006B06, self.ADD_F0, self.JR_RA, self.NOP],
             "does not read f12 before it writes it"),
            ("no f0", [self.SUB, 0x46016080, self.JR_RA, self.NOP], "does not write f0"),
            ("no return", [self.NOP] * 300, r"has no jr \$ra in its first 256"),
            ("unreadable", [self.SUB, self.ADD_F0], "cannot be read at 0x00002008"),
            ("no delay slot", [self.SUB, self.ADD_F0, self.JR_RA], "cannot be read at its return's delay slot"),
            ("ra moved", [self.SUB, self.ADD_F0, 0x27FF0004, self.JR_RA, self.NOP], r"writes \$ra at 0x00002008"),
            ("ra set", [self.SUB, self.ADD_F0, 0x3C1F0800, self.JR_RA, self.NOP], r"writes \$ra"),
            ("ra loaded", [self.SUB, self.ADD_F0, 0x8FBF0000, self.JR_RA, self.NOP], r"writes \$ra"),
            ("sp moved", [self.SUB, 0x27BDFFF0, self.ADD_F0, self.JR_RA, self.NOP], r"writes \$sp"),
            ("fcsr", [self.SUB, 0x44C0F800, self.ADD_F0, self.JR_RA, self.NOP], "writes an FPU control register"),
            ("unknown word", [self.SUB, 0x78000000, self.ADD_F0, self.JR_RA, self.NOP], "cannot read"),
            # beq to the return's delay slot: the slot runs, then execution falls through to the sw.
            ("branch to the return's slot", [self.SUB, 0x10000003, self.NOP, self.ADD_F0, self.JR_RA, self.NOP,
                                             0xAC850000, self.JR_RA, self.NOP],
             "branches from 0x00002004 to 0x00002014, outside the body up to its jr"),
        )
        for name, callee, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._hold(callee)

    def test_even_counter_is_loaded(self) -> None:
        self.assertEqual(self._even([self.LW_A0, self.SLTI, self.JR_RA, self.NOP])[0x1104]["rewrite"],
                         {"kind": "word_even", "word": 0x288300C8, "reg": 4, "rt": 3})
        # Unrelated instructions between the load and the compare are allowed.
        self.assertIn(0x1108, self._even([self.LW_A0, 0x00031080, self.SLTI, self.JR_RA, self.NOP]))
        # A plain branch's delay slot runs on both paths, so a load there reaches the compare.
        self.assertIn(0x1108, self._even([0x14400002, self.LW_A0, self.SLTI, self.JR_RA, self.NOP]))

    def test_even_counter_refusals(self) -> None:
        cases = (
            ("computed", [0x24840001, self.SLTI, self.JR_RA, self.NOP], r"counter \$4 is computed at 0x00001100"),
            ("at the entry", [self.SLTI, self.JR_RA, self.NOP], "not loaded from memory in the straight-line"),
            ("call between", [self.LW_A0, 0x0C000C00, self.NOP, self.SLTI, self.JR_RA, self.NOP],
             "not loaded from memory in the straight-line"),
            ("branch between", [self.LW_A0, 0x10000002, self.NOP, self.SLTI, self.JR_RA, self.NOP],
             "not loaded from memory in the straight-line"),
            ("unknown word", [self.LW_A0, 0x78000000, self.SLTI, self.JR_RA, self.NOP], "cannot tell whether"),
            ("too far", [self.LW_A0] + [self.NOP] * 16 + [self.SLTI, self.JR_RA, self.NOP],
             "not loaded from memory in the straight-line"),
            # The callee runs between a call's delay slot and the compare.
            ("call's slot", [0x0C000C00, self.LW_A0, self.SLTI, self.JR_RA, self.NOP],
             "loaded at 0x00001104, in the delay slot of 0x0c000c00"),
            ("jalr's slot", [0x0320F809, self.LW_A0, self.SLTI, self.JR_RA, self.NOP], "in the delay slot of"),
            # bnel skips its slot on the fall-through path to the compare.
            ("branch-likely's slot", [0x54400002, self.LW_A0, self.SLTI, self.JR_RA, self.NOP],
             "in the delay slot of 0x54400002"),
        )
        for name, body, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._even(body)


def _r(rs: int, rt: int, rd: int, sa: int, fn: int) -> int:
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn


def _beq(rs: int, rt: int, pc: int, to: int) -> int:
    return _i(0x04, rs, rt, (to - pc - 4) >> 2)   # _i, _jal and _move are defined above


ZERO, AT, V0, V1, A0, A1, T0, S0, S1, SP, RA = 0, 1, 2, 3, 4, 5, 8, 16, 17, 29, 31
NOP, JR_RA = 0, 0x03E00008


def _addiu(rt: int, rs: int, imm: int) -> int:
    return _i(0x09, rs, rt, imm)


def _lw(rt: int, off: int, base: int) -> int:
    return _i(0x23, base, rt, off)


def _sw(rt: int, off: int, base: int) -> int:
    return _i(0x2B, base, rt, off)


def _lwc1(ft: int, off: int, base: int) -> int:
    return _i(0x31, base, ft, off)


def _swc1(ft: int, off: int, base: int) -> int:
    return _i(0x39, base, ft, off)


def _add_s(fd: int, fs: int, ft: int) -> int:
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6)


class TestAltGateStaticChecks(unittest.TestCase):
    """The resolver's static proof behind gate_30hz_alt (section 9.2), on a synthetic guest: a
    caller makes the gated call to TARGET, a three-way table switch whose arms all reach a tail
    that calls TICK1(a0) and TICK2(a0); TICK2 calls LEAF with f12 set."""

    CALLER, TARGET, TABLE, TICK1, TICK2, LEAF, OTHER, END = (
        0x1000, 0x2000, 0x3000, 0x4000, 0x4100, 0x4200, 0x4300, 0x4400)
    SITE = 0x100C

    def _caller(self) -> list[int]:
        return [_addiu(SP, SP, -8), _sw(RA, 4, SP), _addiu(A0, ZERO, 0x40), _jal(self.TARGET), NOP,
                _i(0x0F, 0, V0, 1), _jal(self.OTHER), NOP, _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)]

    def _target(self, tail: list[int] | None = None, arm1: list[int] | None = None) -> list[int]:
        t = self.TARGET
        tail_at = t + 0x50
        body = [_addiu(SP, SP, -16), _sw(RA, 12, SP), _sw(S1, 8, SP), _lw(V0, 0x1B0, A0),
                _i(0x0B, V0, AT, 3), _beq(AT, ZERO, t + 0x14, tail_at), _move(S1, A0),
                _i(0x0F, 0, V1, 0), _r(0, V0, V0, 2, 0x00), _addiu(V1, V1, self.TABLE),
                _r(V0, V1, V0, 0, 0x21), _lw(V0, 0, V0), _r(V0, 0, 0, 0, 0x08), NOP,
                # arm 0 at +0x38: branches to the tail
                _addiu(V0, ZERO, 1), _beq(ZERO, ZERO, t + 0x3C, tail_at), _sw(V0, 0x1B4, S1)]
        body += arm1 if arm1 is not None else [_addiu(V0, ZERO, 2), _sw(V0, 0x1B4, S1)]  # arm 1 at +0x44
        body += [NOP]  # arm 2 at +0x4c falls into the tail
        assert len(body) == 0x50 // 4, len(body)
        body += tail if tail is not None else [
            _move(A0, S1), _jal(self.TICK1), NOP, _jal(self.TICK2), _move(A0, S1),
            _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)]
        return body

    def _tick1(self) -> list[int]:
        return [_lwc1(0, 0x108, A0), _lwc1(1, 0x10C, A0), _add_s(0, 0, 1), JR_RA, _swc1(0, 0x108, A0)]

    def _tick2(self, call: int | None = None) -> list[int]:
        return [_addiu(SP, SP, -8), _sw(RA, 4, SP), _sw(S0, 0, SP), _lwc1(12, 0xCC, A0),
                call if call is not None else _jal(self.LEAF), _move(S0, A0), _swc1(0, 0xCC, S0),
                _lw(S0, 0, SP), _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)]

    def _leaf(self) -> list[int]:
        return [_add_s(0, 12, 12), JR_RA, NOP]

    def _words(self, **parts: list[int]) -> dict[int, int]:
        layout = {"caller": self.CALLER, "target": self.TARGET, "tick1": self.TICK1, "tick2": self.TICK2,
                  "leaf": self.LEAF, "other": self.OTHER}
        words: dict[int, int] = {}
        for name, base in layout.items():
            code = parts.get(name) or getattr(self, f"_{name}", lambda: [JR_RA, NOP])()
            words.update({base + 4 * i: w for i, w in enumerate(code)})
        for addr in range(self.CALLER, self.END, 4):
            words.setdefault(addr, NOP)  # alignment padding after each return
        table = parts.get("table", [self.TARGET + 0x38, self.TARGET + 0x44, self.TARGET + 0x4C])
        words.update({self.TABLE + 4 * i: w for i, w in enumerate(table)})
        return words

    def _other(self) -> list[int]:
        return [JR_RA, NOP]

    def _symbols(self) -> dict[str, int]:
        return {"caller": self.CALLER, "target": self.TARGET, "tick1": self.TICK1, "tick2": self.TICK2,
                "leaf": self.LEAF, "other": self.OTHER}

    def _package(self, words: dict[int, int], sha256: str, *, alternates: list[str] | None = None,
                 extra_sites: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        site = {"id": "ALT", "function": "caller", "insn_offset": self.SITE - self.CALLER,
                "stock_digest": compute_stock_words_digest([words[self.SITE]]), "transform": "gate_30hz_alt",
                "alternates": alternates if alternates is not None else ["tick1", "tick2"]}
        return {
            "schema_version": 1, "id": "synth.alt", "display_name": "Synthetic alternate gate",
            "version": "1.0.0", "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
            "executable_sha256": sha256, "capabilities": ["gate_30hz"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "update_boundary": {"function": "caller", "insn_offset": 0,
                                "stock_digest": compute_stock_words_digest([words[self.CALLER]])},
            "sites": [site] + (extra_sites or []),
        }

    def _resolve(self, words: dict[int, int], *, alternates: list[str] | None = None,
                 extra_sites: list[dict[str, Any]] | None = None) -> dict[int, Any]:
        pkg = self._package(words, "a" * 64, alternates=alternates, extra_sites=extra_sites)
        symbols = self._symbols()
        resolved: dict[int, Any] = verify_and_resolve_sites(
            pkg, executable_sha256="a" * 64, symbol_to_addr=symbols,
            read_words_fn=lambda a, n: ([words[a + 4 * i] for i in range(n)]
                                        if all(a + 4 * i in words for i in range(n)) else None),
            known_functions=[self.CALLER, self.TARGET, self.TABLE, self.TICK1, self.TICK2, self.LEAF,
                             self.OTHER, self.END])
        return resolved

    def test_the_proof_accepts_the_switch_and_its_tail(self) -> None:
        resolved = self._resolve(self._words())
        self.assertEqual(resolved[self.SITE]["rewrite"], {"kind": "gate", "alternates": [self.TICK1, self.TICK2]})

    def test_schema_refusals(self) -> None:
        cases: tuple[tuple[str, list[str], str], ...] = (
            ("none", [], "a list of 1 to 4 function symbols"),
            ("too many", ["tick1", "tick2", "leaf", "other", "target"], "a list of 1 to 4"),
            ("twice", ["tick1", "tick1"], "lists a function twice"),
            ("unknown", ["tick1", "nowhere"], "'nowhere' is not in the symbol map"),
            ("literal", ["0x00004000"], "alternate symbol"),
        )
        for name, alternates, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(), alternates=alternates)
        bad = {"id": "G", "function": "caller", "insn_offset": 24, "transform": "gate_30hz",
               "stock_digest": compute_stock_words_digest([_jal(self.OTHER)]), "alternates": ["tick1"]}
        with self.assertRaisesRegex(EnhancementPackageError, "alternates belongs only to gate_30hz_alt"):
            self._resolve(self._words(), extra_sites=[bad])
        jalr = self._caller()
        jalr[3] = _r(25, 0, RA, 0, 0x09)
        with self.assertRaisesRegex(EnhancementPackageError, "gate_30hz_alt takes a jal"):
            self._resolve(self._words(caller=jalr))

    def test_target_refusals(self) -> None:
        early_return = [JR_RA, _addiu(SP, SP, 16)]
        cases = (
            ("an arm returns early", {"target": self._target(arm1=early_return)},
             "has a path that returns without calling alternate 0x00004000"),
            ("a tick is called twice", {"target": self._target(tail=[
                _move(A0, S1), _jal(self.TICK1), NOP, _jal(self.TICK1), _move(A0, S1), _jal(self.TICK2),
                _move(A0, S1), _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)])},
             "calls alternate 0x00004000 directly 2 times"),
            ("a0 not the entry a0", {"target": self._target(tail=[
                _move(A0, S0), _jal(self.TICK1), NOP, _jal(self.TICK2), _move(A0, S1),
                _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)])},
             "does not pass alternate 0x00004000 its own entry"),
            ("s1 rewritten in an arm", {"target": self._target(arm1=[_addiu(S1, S1, 4), NOP])},
             "does not pass alternate 0x00004000 its own entry"),
            ("a0 lost across the first tick", {"target": self._target(tail=[
                _move(A0, S1), _jal(self.TICK1), NOP, _jal(self.TICK2), NOP,
                _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)])},
             "does not pass alternate 0x00004100 its own entry"),
            ("a table arm in another function", {"table": [self.TARGET + 0x38, self.TARGET + 0x44, self.OTHER]},
             "has a path that returns without calling alternate 0x00004000"),
            ("a table entry in a delay slot", {"table": [self.TARGET + 0x38, self.TARGET + 0x40, self.TARGET + 0x4C]},
             "branches into the delay slot at 0x00002040"),
        )
        for name, parts, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(**parts))
        with self.subTest("order"), self.assertRaisesRegex(EnhancementPackageError, "in the order the target calls"):
            self._resolve(self._words(), alternates=["tick2", "tick1"])
        unbounded = self._target()
        unbounded[4] = NOP  # no sltiu bound before the jr
        with self.subTest("no bound"), self.assertRaisesRegex(EnhancementPackageError, "no bounded table switch"):
            self._resolve(self._words(target=unbounded))
        with self.subTest("the target itself"), self.assertRaisesRegex(EnhancementPackageError, "gated function itself"):
            self._resolve(self._words(), alternates=["target"])

    def test_caller_refusals(self) -> None:
        reads_v0 = self._caller()
        reads_v0[5] = _addiu(V1, V0, 1)
        returns = [_addiu(SP, SP, -8), _sw(RA, 4, SP), _addiu(A0, ZERO, 0x40), _jal(self.TARGET), NOP,
                   _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)]
        branch_read = self._caller()
        branch_read[5:5] = [_beq(V0, ZERO, self.SITE + 8, self.SITE + 0x14), NOP]
        for name, caller, why in (("reads v0", reads_v0, r"reads \$v0 at 0x00001014"),
                                  ("returns", returns, r"returns after the gated call without writing \$v0"),
                                  ("branches on v0", branch_read, r"reads \$v0 at 0x00001014")):
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(caller=caller))

    def test_a_register_call_below_the_alternate_follows_the_abi(self) -> None:
        # The documented limit (section 9.2): below the alternate's own body, a call through a
        # register is taken to read only the arguments its caller set.
        leaf = [_addiu(SP, SP, -8), _sw(RA, 4, SP), _r(25, 0, RA, 0, 0x09), NOP, _lw(RA, 4, SP), JR_RA,
                _addiu(SP, SP, 8)]
        self.assertIn(self.SITE, self._resolve(self._words(leaf=leaf)))

    def test_alternate_refusals(self) -> None:
        cases = (
            ("reads a1", {"tick1": [_lw(V0, 0, A1), JR_RA, NOP]}, r"may read \$a1 as its caller left them"),
            ("its callee reads t0", {"leaf": [_add_s(0, 12, 12), _sw(T0, 0, A0), JR_RA, NOP]},
             r"0x00004100 may read \$t0"),
            ("its callee reads f13", {"leaf": [_add_s(0, 12, 13), JR_RA, NOP]}, "may read f13"),
            ("two calls down", {"leaf": [_addiu(SP, SP, -8), _sw(RA, 4, SP), _jal(self.OTHER), NOP,
                                         _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)],
                                "other": [_sw(T0, 0, A0), JR_RA, NOP]}, r"0x00004100 may read \$t0"),
            ("calls through a register", {"tick2": self._tick2(call=_r(25, 0, RA, 0, 0x09))},
             "calls through a register at 0x00004110"),
            ("re-enters the target", {"leaf": [_addiu(SP, SP, -8), _sw(RA, 4, SP), _jal(self.TARGET), NOP,
                                               _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)]},
             "reaches 0x00002000, the gated function"),
        )
        for name, parts, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(**parts))
        gated = {"id": "G", "function": "tick1", "insn_offset": 0, "transform": "gate_30hz",
                 "stock_digest": compute_stock_words_digest([_add_s(0, 0, 0)])}
        with self.subTest("holds a gated site"), self.assertRaisesRegex(
                EnhancementPackageError, "which holds gated site G"):
            self._resolve(self._words(tick1=[_add_s(0, 0, 0)] + self._tick1()[1:]), extra_sites=[gated])

    def _run_target(self) -> list[int]:
        # A straight-line update: its own work (a count at a0+0x1b0), then tick1(a0) and tick2(a0).
        t1, t2 = self.TICK1, self.TICK2
        return [_addiu(SP, SP, -16), _sw(RA, 12, SP), _sw(S1, 8, SP), _move(S1, A0), _lw(V0, 0x1B0, A0),
                _addiu(V0, V0, 1), _sw(V0, 0x1B0, S1), _move(A0, S1), _jal(t1), NOP, _jal(t2), _move(A0, S1),
                _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)]

    def _site_statement(self, tmp: Path, name: str, with_package: bool) -> str:
        """Generate the fixture with the package (or without, the Authentic build) and return the
        site's statement: from the jal's trace hook to the end of its call."""
        words = self._words(target=self._run_target(), table=[])
        elf = _write_synthetic_mips_elf(tmp / "alt.elf", words, self.CALLER)
        args = []
        if with_package:
            pkg_path = tmp / f"{name}.json"
            pkg_path.write_text(json.dumps(self._package(words, hashlib.sha256(elf).hexdigest())), encoding="utf-8")
            (tmp / f"{name}.symbols.json").write_text(
                json.dumps({k: f"0x{v:08x}" for k, v in self._symbols().items()}), encoding="utf-8")
            args = [f"--enhancement-package={pkg_path}"]
        out_dir = tmp / name
        out_dir.mkdir()
        rc, output, files = TestEnhancementAotCodegenAndCache._run_codegen(tmp / "alt.elf", out_dir / "out.c", args)
        self.assertEqual(rc, 0, output)
        lines = b"\n".join(files.values()).decode("utf-8").splitlines()
        at = next(i for i, line in enumerate(lines) if f"sr_begin(s, 0x{self.SITE:08x}u" in line)
        if with_package:
            end = next(i for i in range(at, len(lines)) if lines[i] == "    } }")
        else:
            end = next(i for i in range(at, len(lines)) if "sr_aot_return_boundary_end" in lines[i])
        return "\n".join(lines[at:end + 1])

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_gate_30hz_alt_runs_its_alternates_on_the_skipped_updates(self) -> None:
        """Compile the site statement and run it over a schedule of logic updates.

        Toggle on, the site makes the stock call in an update unless it made the stock call in the
        update just before; then it calls tick1 and tick2 in its place, each with the site's $a0 and
        $ra = site + 8, and a second visit in one update repeats that update's choice. Toggle off,
        it prints what the Authentic statement prints, byte for byte.
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            alt = self._site_statement(tmp, "alt", with_package=True)
            stock = self._site_statement(tmp, "authentic", with_package=False)
            # Toggle off, the site runs the Authentic call statement.
            stock_call = stock.splitlines()[-1]
            self.assertIn(stock_call, alt)
            # Visits per update: a gap after an alternate update (4, 5), two visits in one
            # update (7), and a gap after a stock update (9).
            visits = [1, 1, 1, 1, 0, 0, 1, 2, 1, 0, 1, 1, 1, 1]

            def program(name: str, statement: str) -> list[str]:
                callees = re.findall(r"(\w+)\(s\); SR_TAIL_DRAIN", statement)
                self.assertTrue(callees, statement)
                tags = ["target", "tick1", "tick2"]
                stubs = "".join(
                    f"static void {fn}(CpuState *s) {{ printf(\"  {tags[k]} a0=%x ra=%x\\n\", "
                    f"(unsigned)s->r[4], (unsigned)s->r[31]); s->r[4] = 0x{0xD0 + k:x}u; "
                    f"s->r[31] = 0x{0xE0 + k:x}u; s->r[2] = {k}u; }}\n"
                    for k, fn in enumerate(dict.fromkeys(callees)))
                harness = tmp / f"{name}.c"
                harness.write_text(
                    "#include \"recomp.h\"\n"
                    "/* The real CpuState and trace hooks; the callees are stubs that print what they\n"
                    " * are given and clobber $a0 and $ra, and only the return boundary is stubbed. */\n"
                    "#undef SR_TAIL_DRAIN\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
                    "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
                    "#define sr_aot_return_boundary_end(s) 0\n"
                    "static int on;\nint nk_enhancement_on(void) { return on; }\n"
                    "uint32_t nk_enhancement_updates;\n"
                    f"{stubs}"
                    f"static void site(CpuState *s) {{\n{statement}\n}}\n"
                    f"static const int visits[] = {{{', '.join(map(str, visits))}}};\n"
                    "int main(void) {\n"
                    "    CpuState st; CpuState *s = &st; memset(&st, 0, sizeof st);\n"
                    "    for (on = 0; on < 2; on++) {\n"
                    "        nk_enhancement_updates = on ? 100u : 0u;\n"
                    "        for (int u = 0; u < (int)(sizeof visits / sizeof visits[0]); u++) {\n"
                    "            nk_enhancement_updates++;  /* also while off: the toggle alone keeps it stock */\n"
                    "            for (int v = 0; v < visits[u]; v++) {\n"
                    "                printf(\"%s %d %d\\n\", on ? \"on\" : \"off\", u, v);\n"
                    "                s->r[4] = 0x40u + (uint32_t)u; s->r[31] = 0u; site(s);\n"
                    "            }\n"
                    "        }\n"
                    "    }\n"
                    "    return 0;\n"
                    "}\n",
                    encoding="utf-8",
                )
                exe = tmp / f"{name}.exe"
                assert GCC is not None, "skipUnless(GCC) runs this test only with a compiler"
                built = subprocess.run([GCC, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                        "-Wno-misleading-indentation", "-Wno-unused-variable",
                                        f"-I{ROOT / 'src' / 'rt'}", "-o", str(exe), str(harness)],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
                return run.stdout.splitlines()

            out = program("alt", alt)
            authentic = program("authentic", stock)
            ra = f"{self.SITE + 8:x}"

            def expect(mode: str, choices: str) -> list[str]:
                lines: list[str] = []
                k = 0
                for u, n in enumerate(visits):
                    for v in range(n):
                        lines.append(f"{mode} {u} {v}")
                        a0 = f"{0x40 + u:x}"
                        if choices[k] == "S":
                            lines.append(f"  target a0={a0} ra={ra}")
                        else:
                            lines += [f"  tick1 a0={a0} ra={ra}", f"  tick2 a0={a0} ra={ra}"]
                        k += 1
                return lines

            def part(lines: list[str], mode: str) -> list[str]:
                keep, kept = False, []
                for line in lines:
                    if not line.startswith("  "):
                        keep = line.startswith(mode + " ")
                    if keep:
                        kept.append(line)
                return kept

            # Section 11.2, item 1: toggle off is the Authentic statement's output, byte for byte.
            self.assertEqual(part(out, "off"), part(authentic, "off"))
            self.assertEqual(part(out, "off"), expect("off", "S" * sum(visits)))
            # Toggle on, per visit: updates 0-3 alternate; after the gap at 4-5 update 6 is stock;
            # update 7's two visits both take the alternates; after the gap at 9, update 10 is stock.
            self.assertEqual(part(out, "on"), expect("on", "SASA" "SAAS" "SASA"))


class TestClipXWiden(unittest.TestCase):
    """clip_x_widen (contract section 10.8): a guest_memory_code_hook site that runs the runtime's
    matrix widening after the call at the site returns, and nothing else."""

    ENTRY = 0x1000
    BUILD = 0x1100     # the function the hooked call enters; it returns a pointer in $v0
    CALLER = 0x1200    # the function holding the site: its first word is the jal to BUILD

    def setUp(self) -> None:
        self.words = _program({
            self.ENTRY: [_jal(self.CALLER), _NOP, _JR_RA, _NOP],
            self.BUILD: [_i(0x09, _ZERO, _V0, 0x0400), _JR_RA, _NOP],       # addiu v0, zero, 0x400
            self.CALLER: [_jal(self.BUILD), _NOP, _JR_RA, _NOP],
        })
        self.symbols = {"synth_entry": f"0x{self.ENTRY:08x}", "synth_scene_build": f"0x{self.BUILD:08x}",
                        "synth_scene_cache": f"0x{self.CALLER:08x}"}

    def _package(self, elf_bytes: bytes, **site: Any) -> dict[str, Any]:
        entry = {
            "id": "SYNTH_WIDEN",
            "function": "synth_scene_cache",
            "insn_offset": 0,
            "stock_digest": compute_stock_words_digest([self.words[self.CALLER]]),
            "transform": "clip_x_widen",
            "clip_register": "v0",
            "clip_offsets": [640, 448],
        }
        entry.update(site)
        return {
            "schema_version": 1,
            "id": "synth.widen.clip",
            "display_name": "Synthetic clip widen",
            "version": "1.0.0",
            "category": "guest_memory_code_hook",
            "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00",
            "executable_sha256": hashlib.sha256(elf_bytes).hexdigest(),
            "capabilities": ["widen_visibility"],
            "sites": [entry],
        }

    def _build(self, tmp: Path, pkg: dict[str, Any] | None, name: str) -> tuple[int, str, str]:
        elf_path = tmp / "synth.elf"
        if not elf_path.exists():
            self.elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.ENTRY)
        out_dir = tmp / name
        out_dir.mkdir()
        extra = []
        if pkg is not None:
            (tmp / f"{name}.json").write_text(json.dumps(pkg), encoding="utf-8")
            (tmp / f"{name}.symbols.json").write_text(json.dumps(self.symbols), encoding="utf-8")
            extra = [f"--enhancement-package={tmp / (name + '.json')}"]
        rc, out, files = TestEnhancementAotCodegenAndCache._run_codegen(elf_path, out_dir / "out.c", extra)
        return rc, out, b"\n".join(files.values()).decode("utf-8")

    def test_widen_runs_after_the_call_and_authentic_output_is_untouched(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            rc, out, authentic = self._build(tmp, None, "authentic")
            self.assertEqual(rc, 0, out)
            pkg = self._package(self.elf_bytes)
            rc, out, widened = self._build(tmp, pkg, "widened")
            self.assertEqual(rc, 0, out)
            self.assertNotIn("sr_enhancement_widen_clip_x", authentic)
            self.assertEqual(widened.count("sr_enhancement_widen_clip_x(s->r[2], _nk_clip_off, 2u);"), 1)
            self.assertIn("_nk_clip_off[] = {640u, 448u};", widened)
            # The stock call is still made, and the widening follows it (not the other way round).
            call = widened.index(self._call_to(self.BUILD, widened))
            self.assertLess(call, widened.index("sr_enhancement_widen_clip_x(s->r["))
            # It is not the 60 Hz toggle: no gate, no logic-update counter.
            self.assertNotIn("nk_enhancement_gate_30hz()) {", widened)

    @staticmethod
    def _call_to(target: int, text: str) -> str:
        match = re.search(r"[A-Za-z_0-9]*%08x[A-Za-z_0-9]*\(s\);" % target, text)
        assert match, f"no call to 0x{target:08x} in the generated C"
        return match.group(0)

    def test_a_site_that_is_not_a_call_refuses_the_package(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            self._build(tmp, None, "authentic")
            bad = self._package(self.elf_bytes, function="synth_scene_build", insn_offset=0,
                                stock_digest=compute_stock_words_digest([self.words[self.BUILD]]))
            rc, out, files = self._build(tmp, bad, "not_a_call")
            self.assertNotEqual(rc, 0)
            self.assertIn("clip_x_widen takes a jal", out)
            self.assertEqual(files, "")

    def test_validation_keeps_code_hook_and_timing_transforms_apart(self) -> None:
        pkg = self._package(b"synthetic")
        validate_package(copy.deepcopy(pkg))
        timing = copy.deepcopy(pkg)
        timing["category"] = "timing_frame_presentation"
        timing["rng_policy"] = {"mode": "rescale_probability", "reason": "synthetic"}
        with self.assertRaisesRegex(EnhancementPackageError, "needs category 'guest_memory_code_hook'"):
            validate_package(timing)
        hook = copy.deepcopy(pkg)
        hook["sites"][0]["transform"] = "count_x2"
        with self.assertRaisesRegex(EnhancementPackageError, "is not a code-hook transform"):
            validate_package(hook)
        for key, value, pattern in (
            ("clip_register", "a0", "clip_register"),
            ("clip_offsets", [], "clip_offsets"),
            ("clip_offsets", [641], "clip_offsets"),
            ("clip_offsets", [640, 640], "clip_offsets"),
        ):
            bad = copy.deepcopy(pkg)
            bad["sites"][0][key] = value
            with self.assertRaisesRegex(EnhancementPackageError, pattern):
                validate_package(bad)
        no_regs = copy.deepcopy(pkg)
        del no_regs["sites"][0]["clip_register"]
        with self.assertRaisesRegex(EnhancementPackageError, "clip_register"):
            validate_package(no_regs)
        with_rng = copy.deepcopy(pkg)
        with_rng["rng_policy"] = {"mode": "rescale_probability", "reason": "synthetic"}
        with self.assertRaisesRegex(EnhancementPackageError, "not valid for category 'guest_memory_code_hook'"):
            validate_package(with_rng)
        timing_site = copy.deepcopy(timing)
        timing_site["sites"][0].pop("clip_offsets")
        with self.assertRaises(EnhancementPackageError):
            validate_package(timing_site)


class TestCodeHookBesideTiming(unittest.TestCase):
    """A build carries one timing package and at most one code-hook package beside it (contract
    section 9.9): codegen's --code-hook-package. Without it the output is as before; each way the
    two could clash is refused by name, never merged."""

    ENTRY = 0x1000
    BUILD = 0x1100     # returns a pointer in $v0
    CALLER = 0x1200    # jal BUILD; its delay slot loads a count
    TIMER = 0x1300     # a count the timing package doubles; its return's delay slot is the boundary
    PAIR = 0x1400      # jal TIMER, then jal BUILD in its delay slot
    RELAY = 0x1500     # jal BUILD, passing its own $a0 on
    COUNT = 0x2404001E  # addiu $a0, $zero, 30

    def setUp(self) -> None:
        self.words = _program({
            self.ENTRY: [_jal(self.CALLER), _NOP, _jal(self.TIMER), _NOP, _jal(self.RELAY), _NOP,
                         _jal(self.PAIR), _NOP, _JR_RA, _NOP],
            self.BUILD: [_i(0x09, _ZERO, _V0, 0x0400), _JR_RA, _NOP],       # addiu v0, zero, 0x400
            self.CALLER: [_jal(self.BUILD), self.COUNT, _JR_RA, _NOP],
            self.TIMER: [self.COUNT, _JR_RA, _NOP],
            self.PAIR: [_jal(self.TIMER), _jal(self.BUILD), _NOP, _JR_RA, _NOP],
            self.RELAY: [_jal(self.BUILD), _NOP, _JR_RA, _NOP],
        })
        self.symbols = {"synth_entry": f"0x{self.ENTRY:08x}", "synth_scene_build": f"0x{self.BUILD:08x}",
                        "synth_scene_cache": f"0x{self.CALLER:08x}", "synth_timer": f"0x{self.TIMER:08x}",
                        "synth_pair": f"0x{self.PAIR:08x}", "synth_relay": f"0x{self.RELAY:08x}"}

    def _timing(self, *sites: dict[str, Any]) -> dict[str, Any]:
        return {
            "schema_version": 1,
            "id": "synth.timing.60hz",
            "display_name": "Synthetic 60 Hz",
            "version": "1.0.0",
            "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00",
            "executable_sha256": hashlib.sha256(self.elf_bytes).hexdigest(),
            "capabilities": ["count_x2", "gate_30hz"],
            "rng_policy": {"mode": "rescale_probability", "reason": "synthetic"},
            "sites": list(sites) or [self._count_site("SYNTH_TIMER", "synth_timer", 0)],
            "update_boundary": {"function": "synth_timer", "insn_offset": 8,
                                "stock_digest": compute_stock_words_digest([_NOP])},
        }

    @staticmethod
    def _count_site(site_id: str, function: str, offset: int) -> dict[str, Any]:
        return {"id": site_id, "function": function, "insn_offset": offset,
                "stock_digest": compute_stock_words_digest([TestCodeHookBesideTiming.COUNT]),
                "transform": "count_x2"}

    def _hook(self, **fields: Any) -> dict[str, Any]:
        pkg = {
            "schema_version": 1,
            "id": "synth.widen.clip",
            "display_name": "Synthetic clip widen",
            "version": "1.0.0",
            "category": "guest_memory_code_hook",
            "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00",
            "executable_sha256": hashlib.sha256(self.elf_bytes).hexdigest(),
            "capabilities": ["widen_visibility"],
            "sites": [{
                "id": "SYNTH_WIDEN", "function": "synth_scene_cache", "insn_offset": 0,
                "stock_digest": compute_stock_words_digest([self.words[self.CALLER]]),
                "transform": "clip_x_widen", "clip_register": "v0", "clip_offsets": [640, 448],
            }],
        }
        pkg.update(fields)
        return pkg

    def _build(self, tmp: Path, name: str, timing: dict[str, Any] | None = None,
               hook: dict[str, Any] | None = None) -> tuple[int, str, dict[str, bytes]]:
        elf_path = tmp / "synth.elf"
        if not elf_path.exists():
            self.elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.ENTRY)
        out_dir = tmp / name
        out_dir.mkdir()
        extra = []
        for flag, pkg, leaf in (("--enhancement-package", timing, "timing"), ("--code-hook-package", hook, "hook")):
            if pkg is not None:
                path = tmp / f"{name}-{leaf}.json"
                path.write_text(json.dumps(pkg), encoding="utf-8")
                path.with_suffix(".symbols.json").write_text(json.dumps(self.symbols), encoding="utf-8")
                extra.append(f"{flag}={path}")
        return TestEnhancementAotCodegenAndCache._run_codegen(elf_path, out_dir / "out.c", extra)

    def _elf(self, tmp: Path) -> None:
        self.elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.ENTRY)

    def test_the_hook_package_adds_only_its_site_and_its_header_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            self._elf(tmp)
            rc, out, timing_only = self._build(tmp, "timing", self._timing())
            self.assertEqual(rc, 0, out)
            rc, out, stacked = self._build(tmp, "stacked", self._timing(), self._hook())
            self.assertEqual(rc, 0, out)
            self.assertEqual(sorted(timing_only), sorted(stacked))
            text = b"\n".join(stacked.values()).decode("utf-8")
            self.assertEqual(text.count("sr_enhancement_widen_clip_x(s->r[2], _nk_clip_off, 2u);"), 1)
            self.assertIn("/* AOT Code-Hook Package: id=synth.widen.clip v=1.0.0 sites=1 sha256=", text)
            self.assertIn("/* AOT Enhancement Epoch: aot-enhancement-package-v1 id=synth.timing.60hz", text)
            # Every line the timing-only build writes is still there, in order; the stacked build adds
            # the header line and the site's widening lines, and nothing else.
            added = []
            for leaf in sorted(stacked):
                old = timing_only[leaf].decode("utf-8").splitlines()
                new = stacked[leaf].decode("utf-8").splitlines()
                matcher = difflib.SequenceMatcher(a=old, b=new, autojunk=False)
                for tag, i1, i2, j1, j2 in matcher.get_opcodes():
                    self.assertIn(tag, ("equal", "insert"), (leaf, old[i1:i2], new[j1:j2]))
                    if tag == "insert":
                        added.extend(new[j1:j2])
            self.assertTrue(added)
            for line in added:
                self.assertTrue("Code-Hook Package" in line or "_nk_clip_off" in line, line)

    def test_a_stacked_build_leaves_nothing_behind_for_the_next_build(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            self._elf(tmp)
            rc, out, first = self._build(tmp, "first", self._timing())
            self.assertEqual(rc, 0, out)
            # A stacked build leaves module state behind; the next timing-only run is byte-identical.
            rc, out, _ = self._build(tmp, "stacked", self._timing(), self._hook())
            self.assertEqual(rc, 0, out)
            rc, out, again = self._build(tmp, "again", self._timing())
            self.assertEqual(rc, 0, out)
            self.assertEqual(first, again)
            self.assertNotIn(b"Code-Hook", b"".join(first.values()))

    def _refused(self, pattern: str, timing: dict[str, Any] | None, hook: dict[str, Any],
                 status: int = 1) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            self._elf(tmp)
            rc, out, files = self._build(tmp, "refused", timing, hook)
            self.assertEqual(rc, status, out)
            self.assertRegex(out, pattern)
            self.assertEqual(files, {})

    def test_a_second_package_that_is_not_a_code_hook_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        second = self._timing(self._count_site("SYNTH_OTHER", "synth_scene_cache", 4))
        second["id"] = "synth.other.timing"
        self._refused(r"code-hook package 'synth.other.timing': category 'timing_frame_presentation' "
                      r"is not 'guest_memory_code_hook'", self._timing(), second)

    def test_two_code_hook_packages_are_refused(self) -> None:
        self._elf_bytes_for_packages()
        first = self._hook(id="synth.first.hook")
        first["sites"][0]["id"] = "SYNTH_FIRST"
        self._refused(r"the enhancement package 'synth.first.hook' is a code-hook package too",
                      first, self._hook())

    def test_an_address_collision_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        gate = {"id": "SYNTH_GATE", "function": "synth_scene_cache", "insn_offset": 0,
                "stock_digest": compute_stock_words_digest([self.words[self.CALLER]]),
                "transform": "gate_30hz"}
        self._refused(r"code-hook site SYNTH_WIDEN: address 0x00001200 collides with site SYNTH_GATE",
                      self._timing(gate), self._hook())

    def test_a_hook_whose_delay_slot_the_timing_package_rewrites_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        slot = self._count_site("SYNTH_SLOT", "synth_scene_cache", 4)
        self._refused(r"code-hook site SYNTH_WIDEN at 0x00001200: its delay slot 0x00001204 is site "
                      r"SYNTH_SLOT of the enhancement package, inside a span it rewrites",
                      self._timing(slot), self._hook())

    def test_a_hook_at_the_delay_slot_of_a_rewritten_call_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        gate = {"id": "SYNTH_GATE", "function": "synth_pair", "insn_offset": 0,
                "stock_digest": compute_stock_words_digest([self.words[self.PAIR]]), "transform": "gate_30hz"}
        hook = self._hook()
        hook["sites"][0].update(function="synth_pair", insn_offset=4,
                                stock_digest=compute_stock_words_digest([self.words[self.PAIR + 4]]))
        self._refused(r"code-hook site SYNTH_WIDEN: address 0x00001404 collides with the delay slot of site "
                      r"SYNTH_GATE of the enhancement package", self._timing(gate), hook)

    def test_a_hook_on_a_call_an_alternate_gate_bypasses_is_refused(self) -> None:
        # The timing package gates jal synth_relay with the alternate synth_scene_build, so on an alternate
        # update it calls synth_scene_build itself and the hooked jal inside synth_relay never runs.
        self._elf_bytes_for_packages()
        alt = {"id": "SYNTH_ALT", "function": "synth_entry", "insn_offset": 16,
               "stock_digest": compute_stock_words_digest([self.words[self.ENTRY + 16]]),
               "transform": "gate_30hz_alt", "alternates": ["synth_scene_build"]}
        hook = self._hook()
        hook["sites"][0].update(function="synth_relay", insn_offset=0,
                                stock_digest=compute_stock_words_digest([self.words[self.RELAY]]))
        self._refused(r"code-hook site SYNTH_WIDEN at 0x00001500: its call to 0x00001100 is an alternate that "
                      r"site SYNTH_ALT of the enhancement package calls itself on alternate updates",
                      self._timing(alt), hook)

    def test_a_hook_package_with_no_sites_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        hook = self._hook()
        for key in ("sites", "game_version", "executable_sha256"):
            del hook[key]
        validate_package(copy.deepcopy(hook))
        self._refused(r"code-hook package 'synth.widen.clip' has no sites", self._timing(), hook)

    def test_a_hook_whose_delay_slot_is_the_update_boundary_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        timing = self._timing()
        timing["update_boundary"] = {"function": "synth_scene_cache", "insn_offset": 4,
                                     "stock_digest": compute_stock_words_digest([self.COUNT])}
        self._refused(r"code-hook site SYNTH_WIDEN at 0x00001200: its delay slot 0x00001204 is the "
                      r"update_boundary of the enhancement package", timing, self._hook())

    def test_a_hook_rewrite_that_keeps_run_time_state_is_refused(self) -> None:
        # No code-hook transform keeps state today; this pins the guard for the first one that does.
        self._elf_bytes_for_packages()
        real = enhancement_package.verify_and_resolve_sites

        def with_state(pkg: dict[str, Any], **kwargs: Any) -> Any:
            sites = real(pkg, **kwargs)
            if pkg["category"] == "guest_memory_code_hook":
                for site in sites.values():
                    site["rewrite"]["requires"] = ["SYNTH_TIMER"]
            return sites

        with mock.patch.object(enhancement_package, "verify_and_resolve_sites", side_effect=with_state):
            self._refused(r"code-hook site SYNTH_WIDEN: transform 'clip_x_widen' keeps run-time state",
                          self._timing(), self._hook())

    def test_a_hook_for_another_title_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        self._refused(r"target_title_id 'another-title-v1' is not the enhancement package's",
                      self._timing(), self._hook(target_title_id="another-title-v1"))

    def test_a_site_id_both_packages_use_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        hook = self._hook()
        hook["sites"][0]["id"] = "SYNTH_TIMER"
        self._refused(r"code-hook site SYNTH_TIMER: the enhancement package has a site with that id",
                      self._timing(), hook)

    def test_a_hook_package_alone_is_refused(self) -> None:
        self._elf_bytes_for_packages()
        self._refused(r"--code-hook-package is applied beside --enhancement-package", None, self._hook(),
                      status=2)

    def _elf_bytes_for_packages(self) -> None:
        # The packages carry the executable's SHA-256: write the ELF once to learn it.
        with tempfile.TemporaryDirectory() as tmp_dir:
            self._elf(Path(tmp_dir))


class TestPublishedPackages(_SynthTimingElfFixture):
    """Published packages (ENHANCEMENT_CONTRACT.md section 5.1): site data with no game bytes.

    Every fixture is synthetic: the six-function ELF of _SynthTimingElfFixture.
    """

    DISC = "ULUS99999"
    VERSION = "1.01"

    def _readers(self, elf_bytes: bytes):
        from analyze import Elf

        return enhancement_package.elf_readers(Elf(elf_bytes, base=0))

    def _publish(self, elf_bytes: bytes, pkg: dict[str, Any] | None = None) -> dict[str, Any]:
        private, symbols = self._make_package_and_symbols(elf_bytes)
        read_words, read_data = self._readers(elf_bytes)
        published: dict[str, Any] = enhancement_package.publish_package(
            private if pkg is None else pkg, {k: int(v, 16) for k, v in symbols.items()},
            executable=elf_bytes, disc_id=self.DISC, disc_version=self.VERSION,
            read_words_fn=read_words, read_data_fn=read_data)
        return published

    def test_published_package_builds_the_same_code_as_the_private_package(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)
            private, symbols = self._make_package_and_symbols(elf_bytes)
            priv_path = tmp / "private" / "synth-60hz.json"
            priv_path.parent.mkdir()
            priv_path.write_text(json.dumps(private), encoding="utf-8")
            priv_path.with_suffix(".symbols.json").write_text(json.dumps(symbols), encoding="utf-8")

            # The command-line export, as a title's package is published.
            pub_path = tmp / "public" / "ulus99999-60hz.json"
            pub_path.parent.mkdir()
            with mock.patch("sys.stdout", new=StringIO()):
                rc = enhancement_package.main([
                    "--publish", str(priv_path), "--elf", str(elf_path), "--base", "0",
                    "--disc-id", self.DISC, "--disc-version", self.VERSION, "--out", str(pub_path)])
            self.assertEqual(rc, 0)
            published = json.loads(pub_path.read_text(encoding="utf-8"))
            self.assertEqual(enhancement_package.published_package_findings(published), [])
            self.assertEqual(published["site_map"], {
                name: symbols[name] for name in
                ("synth_vblank_wait", "synth_timer_duration", "synth_velocity_step", "synth_damping_decay",
                 "synth_rng_prob_check", "synth_subtick_gate")})
            # No published digest is the plain digest of the words it covers.
            plain = {compute_stock_words_digest([w]) for w in self.words.values()}
            for site in published["sites"]:
                self.assertNotIn(site["stock_digest"], plain)
            self.assertNotIn(published["update_boundary"]["stock_digest"], plain)

            runs = {}
            for label, path in (("private", priv_path), ("published", pub_path)):
                out_dir = tmp / f"out-{label}"
                out_dir.mkdir()
                rc_run, output, files = self._run_codegen(
                    elf_path, out_dir / "out.c", [f"--enhancement-package={path}"])
                self.assertEqual(rc_run, 0, output)
                runs[label] = files
            self.assertTrue(runs["private"])
            self.assertEqual(runs["published"], runs["private"])

            # A keyed digest that does not match its site refuses the whole package, by name.
            bad = copy.deepcopy(published)
            bad["sites"][1]["stock_digest"] = enhancement_package.keyed_stock_digest(b"other key", b"\0" * 4)
            bad_path = tmp / "public" / "bad.json"
            bad_path.write_text(json.dumps(bad), encoding="utf-8")
            bad_dir = tmp / "out-bad"
            bad_dir.mkdir()
            rc_bad, out_bad, files_bad = self._run_codegen(
                elf_path, bad_dir / "out.c", [f"--enhancement-package={bad_path}"])
            self.assertNotEqual(rc_bad, 0)
            self.assertIn("site_mismatch: sites[1](SYNTH_COUNT)", out_bad)
            self.assertEqual(files_bad, {})

    def test_resolve_refuses_another_executable_by_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            elf_bytes = _write_synthetic_mips_elf(Path(tmp_dir) / "synth.elf", self.words, self.entry)
            published = self._publish(elf_bytes)
            patched_words = dict(self.words)
            patched_words[0x00001200] = 0x24040028  # one stock word changed: a patched image
            patched = _write_synthetic_mips_elf(Path(tmp_dir) / "patched.elf", patched_words, self.entry)
            read_words, read_data = self._readers(patched)
            with self.assertRaises(enhancement_package.PublishedPackageRefusal) as ctx:
                enhancement_package.resolve_published_package(
                    published, executable=patched, read_words_fn=read_words, read_data_fn=read_data)
            self.assertEqual(ctx.exception.code, "modified_executable")
            # Even with the right executable digest, bytes that differ at a site are refused.
            read_words_ok, _ = self._readers(elf_bytes)

            def lying_reader(vaddr: int, count: int):
                words = read_words_ok(vaddr, count)
                return None if words is None else [w ^ 1 for w in words]

            with self.assertRaises(enhancement_package.PublishedPackageRefusal) as ctx:
                enhancement_package.resolve_published_package(
                    published, executable=elf_bytes, read_words_fn=lying_reader)
            self.assertEqual(ctx.exception.code, "site_mismatch")
            # The right executable resolves to the private declaration exactly.
            private, _ = self._make_package_and_symbols(elf_bytes)
            resolved, symbols = enhancement_package.resolve_published_package(
                published, executable=elf_bytes, read_words_fn=read_words_ok)
            self.assertEqual(resolved, validate_package(private))
            self.assertEqual(symbols["synth_timer_duration"], 0x00001200)

    def test_digest_key_is_not_derivable_from_the_published_digest(self) -> None:
        elf_bytes = b"synthetic executable bytes"
        key = enhancement_package.published_digest_key(elf_bytes)
        self.assertNotEqual(key, hashlib.sha256(elf_bytes).digest())
        self.assertNotEqual(enhancement_package.keyed_stock_digest(key, b"\x1e\x00\x04\x24"),
                            compute_stock_words_digest([0x2404001E]))

    def test_data_sites_and_files_round_trip(self) -> None:
        words = dict(self.words)
        words[0x00002000] = struct.unpack("<I", struct.pack("<f", 0.75))[0]
        pkg: dict[str, Any]
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_bytes = _write_synthetic_mips_elf(
                tmp / "synth.elf", words, self.entry,
                sections=((".text", 0x00001000, 0x700, 6), (".data", 0x00002000, 4, 3)))
            private, symbols = self._make_package_and_symbols(elf_bytes)
            table = b"name,frames\nidle,30\n"
            (tmp / "data" / "PSP_GAME").mkdir(parents=True)
            (tmp / "data" / "PSP_GAME" / "TUNE.CSV").write_bytes(table)
            pkg = dict(private, data_sites=[{
                "id": "TABLE", "symbol": "synth_table", "offset": 0, "type": "f32", "count": 1,
                "stock_digest": hashlib.sha256(struct.pack("<f", 0.75)).hexdigest(),
                "transform": "retention_m_to_sqrt(m)"}], data_files=[{
                    "id": "TUNE", "path": "PSP_GAME/TUNE.CSV", "sha256": hashlib.sha256(table).hexdigest(),
                    "format": "csv", "header_row": 0, "first_data_row": 1,
                    "cells": [{"column": "frames", "transform": "count_x2"}]}])
            all_symbols = {k: int(v, 16) for k, v in symbols.items()}
            all_symbols["synth_table"] = 0x00002000
            read_words, read_data = self._readers(elf_bytes)
            published = enhancement_package.publish_package(
                pkg, all_symbols, executable=elf_bytes, disc_id=self.DISC, disc_version=self.VERSION,
                read_words_fn=read_words, read_data_fn=read_data, data_root=tmp / "data")
            self.assertNotEqual(published["data_sites"][0]["stock_digest"], pkg["data_sites"][0]["stock_digest"])
            self.assertNotEqual(published["data_files"][0]["sha256"], pkg["data_files"][0]["sha256"])
            self.assertEqual(published["site_map"]["synth_table"], "0x00002000")
            resolved, _ = enhancement_package.resolve_published_package(
                published, executable=elf_bytes, read_words_fn=read_words, read_data_fn=read_data,
                data_root=tmp / "data")
            self.assertEqual(resolved, validate_package(copy.deepcopy(pkg)))
            # Another file at the data file's path is refused.
            (tmp / "data" / "PSP_GAME" / "TUNE.CSV").write_bytes(b"name,frames\nidle,31\n")
            with self.assertRaisesRegex(EnhancementPackageError, "keyed digest"):
                enhancement_package.resolve_published_package(
                    published, executable=elf_bytes, read_words_fn=read_words, read_data_fn=read_data,
                    data_root=tmp / "data")

    def test_publish_keeps_no_values_read_from_the_game_and_refuses_wrong_digests(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            words = dict(self.words)
            words[0x00001700] = 0xE4820000  # swc1 f2, 0(a0): a store site
            words[0x00001704] = 0x03E00008
            elf_bytes = _write_synthetic_mips_elf(Path(tmp_dir) / "synth.elf", words, self.entry)
            private, _ = self._make_package_and_symbols(elf_bytes)
            with_values = copy.deepcopy(private)
            with_values["sites"].append({
                "id": "SYNTH_STORE", "function": "synth_store", "insn_offset": 0,
                "stock_digest": compute_stock_words_digest([0xE4820000]), "transform": "count_x2",
                "stock_values": [30, 37.5]})
            from analyze import Elf

            read_words, read_data = enhancement_package.elf_readers(Elf(elf_bytes, base=0))
            symbols = dict({k: v for k, v in self.fn_addrs.items()}, synth_store=0x00001700)
            published = enhancement_package.publish_package(
                with_values, symbols, executable=elf_bytes, disc_id=self.DISC, disc_version=self.VERSION,
                read_words_fn=read_words, read_data_fn=read_data)
            store = published["sites"][-1]
            self.assertNotIn("stock_values", store)
            key = enhancement_package.published_digest_key(elf_bytes)
            self.assertEqual(store["stock_value_digests"], [
                enhancement_package.keyed_stock_value_digest(key, "SYNTH_STORE", _f32_bits_of(v)) for v in (30.0, 37.5)])
            self.assertEqual(enhancement_package.published_package_findings(published), [])
            wrong = copy.deepcopy(private)
            wrong["sites"][1]["stock_digest"] = compute_stock_words_digest([0x24040028])
            with self.assertRaisesRegex(EnhancementPackageError, "does not match the executable"):
                self._publish(elf_bytes, wrong)

    def test_shipped_hot_shots_tennis_package_holds_its_eased_fields(self) -> None:
        """Pins the four gate_30hz_store sites (section 11.3.2.2) that package 15.0.0-pre1 ships: the
        menu cursor ease (S0375) and the ticker's float ease (S0386), by id, function, instruction
        offset and transform. The validator checks each site's map name, word count and the update
        boundary (test_shipped_packages_validate_and_pass_the_publication_check); what the gate does
        is proven on a fixture by test_gate_30hz_store_holds_each_30hz_value_for_two_updates, and the
        game time of each ease by the private runs section 9.10.1 records."""
        package = json.loads((ROOT / "assets" / "enhancements" / "ucus98701-60hz.json").read_text("utf-8"))
        stores = sorted((site["id"], site["function"], site["insn_offset"], site["transform"])
                        for site in package["sites"] if site["transform"] == "gate_30hz_store")
        self.assertEqual(stores, [
            ("S0375_a", "hst_fn_0010bc70", 1160, "gate_30hz_store"),
            ("S0375_b", "hst_fn_0010bc70", 1244, "gate_30hz_store"),
            ("S0386_a", "hst_fn_00110bfc", 248, "gate_30hz_store"),
            ("S0386_b", "hst_fn_00110bfc", 264, "gate_30hz_store"),
        ])

    def test_shipped_packages_validate_and_pass_the_publication_check(self) -> None:
        """Every package in assets/enhancements/ is a valid published package with no findings.

        The validator also refuses a site_map name that no site, data site, hook or update
        boundary uses, so a republished package must drop the names it no longer needs.
        """
        import publish_audit

        shipped = sorted((ROOT / "assets" / "enhancements").glob("*.json"))
        self.assertTrue(shipped)
        for path in shipped:
            rel = path.relative_to(ROOT).as_posix()
            with self.subTest(package=rel):
                data = path.read_bytes()
                package = json.loads(data.decode("utf-8"))
                validate_package(copy.deepcopy(package))
                self.assertEqual(publish_audit.check_enhancement_packages({rel: (data, None)}), [])

    def test_shipped_packages_are_experimental_until_released(self) -> None:
        """Section 9.10: a package the project ships declares "experimental": false only when
        its release entry under section 9.10 names it. Hot Shots Tennis was released by the
        owner's decision of 2026-11-12 (section 9.10.1); a re-export from a declaration without
        the flag would put it back to Experimental.

        The entry also records the version the release covers: the last package version its
        Status names must be the shipped package's version, so a new version must update the
        entry (what was measured and the limits known) in the same change."""
        released = {"ucus98701-60hz.json": "#### 9.10.1."}
        contract = (ROOT / "docs" / "ENHANCEMENT_CONTRACT.md").read_text(encoding="utf-8")
        shipped = sorted((ROOT / "assets" / "enhancements").glob("*.json"))
        self.assertTrue(shipped)
        for path in shipped:
            with self.subTest(package=path.name):
                package = json.loads(path.read_text("utf-8"))
                self.assertEqual(package.get("experimental") is False, path.name in released)
                if path.name not in released:
                    continue
                entry = contract[contract.index(released[path.name]):]
                following = re.search(r"\n#{1,4} ", entry)
                entry = entry[:following.start()] if following else entry
                status = re.search(r"^- \*\*Status\.\*\* (.*?)(?=^- \*\*|\Z)", entry, re.M | re.S)
                self.assertIsNotNone(status, f"{released[path.name]} has a Status item")
                assert status is not None
                versions = re.findall(r"\bpackage (\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?)",
                                      " ".join(status.group(1).split()))
                self.assertTrue(versions, f"{released[path.name]} Status names a package version")
                self.assertEqual(versions[-1], package["version"])

    def test_validator_refuses_malformed_published_packages(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            elf_bytes = _write_synthetic_mips_elf(Path(tmp_dir) / "synth.elf", self.words, self.entry)
            published = self._publish(elf_bytes)
        self.assertIs(validate_package(copy.deepcopy(published))["published_package"], 1)

        def broken(**changes: Any) -> dict[str, Any]:
            data = copy.deepcopy(published)
            for key, value in changes.items():
                if value is None:
                    del data[key]
                else:
                    data[key] = value
            return data

        site_map = published["site_map"]
        cases = {
            "missing site map name": (broken(site_map={k: v for k, v in site_map.items()
                                                       if k != "synth_timer_duration"}), "does not name"),
            "extra address": (broken(site_map=dict(site_map, synth_other="0x00004000")), "no site uses"),
            "address-literal name": (broken(site_map=dict(site_map, sub_00001200="0x00001200")), "Address-literal"),
            "misaligned address": (broken(site_map=dict(site_map, synth_timer_duration="0x00001202")), "aligned"),
            "decimal address": (broken(site_map=dict(site_map, synth_timer_duration=4608)), "aligned"),
            "digest scheme": (broken(digest_scheme="sha256"), "digest_scheme"),
            "disc id": (broken(disc_id="ulus99999"), "disc_id"),
            "disc version": (broken(disc_version="1"), "disc_version"),
            "format version": (broken(published_package=2), "published_package"),
            "boolean format version": (broken(published_package=True), "published_package"),
            "missing disc version": (broken(disc_version=None), "disc_version"),
            "no executable": (broken(executable_sha256=None), "executable_sha256"),
            "unknown key": (broken(stock_words=[0x2404001E]), "Unknown properties"),
        }
        values = copy.deepcopy(published)
        values["sites"][1]["stock_values"] = [30]
        cases["stock values"] = (values, "values read from the game")
        not_store = copy.deepcopy(published)
        not_store["sites"][0]["stock_value_digests"] = ["d" * 64]   # vblank_wait_2_to_1 stores nothing
        cases["value digests on a site that stores nothing"] = (not_store, "belongs only to a store site")
        address = site_map["synth_timer_duration"]
        other_suffix = json.loads(json.dumps(published).replace(
            '"synth_timer_duration"', '"synth_fn_%08x"' % (int(address, 16) + 4)))
        cases["name ending in another address"] = (other_suffix, "ends in an address other than its own")
        cases["build metadata"] = (broken(version="1.0.0+build"), "no build metadata")
        cases["long pre-release"] = (broken(version="1.0.0-" + "p" * 17), "no build metadata")
        long_reason = copy.deepcopy(published)
        long_reason["rng_policy"]["reason"] = ("Same odds per second. " * 50).strip()
        cases["long reason"] = (long_reason, "at most 1024 characters")
        for label, (data, message) in cases.items():
            with self.subTest(label):
                with self.assertRaisesRegex(EnhancementPackageError, message):
                    validate_package(data)
                self.assertTrue(enhancement_package.published_package_findings(data))

    def test_publication_gate_refuses_anything_that_could_carry_game_bytes(self) -> None:
        import publish_audit

        with tempfile.TemporaryDirectory() as tmp_dir:
            elf_bytes = _write_synthetic_mips_elf(Path(tmp_dir) / "synth.elf", self.words, self.entry)
            published = self._publish(elf_bytes)
            private, symbols = self._make_package_and_symbols(elf_bytes)

        def encode(data: Any) -> tuple[bytes, None]:
            return json.dumps(data, indent=2).encode("utf-8") + b"\n", None

        good = "assets/enhancements/ulus99999-60hz.json"
        self.assertEqual(publish_audit.check_enhancement_packages({good: encode(published)}), [])
        hexed = copy.deepcopy(published)
        hexed["rng_policy"]["reason"] = "stock word 2404001e2404001e"
        addressed = copy.deepcopy(published)
        addressed["display_name"] = "Patch at 0x08804000"
        valued = copy.deepcopy(published)
        valued["sites"][1]["stock_values"] = [30]
        cases = {
            "private declaration in the package folder": ({good: encode(private)}, "ENHANCEMENT_PACKAGE_GAME_BYTES"),
            "symbol map anywhere": ({"docs/synth-60hz.symbols.json": encode(symbols)}, "ENHANCEMENT_SYMBOL_MAP"),
            "package outside the folder": ({"docs/synth-60hz.json": encode(published)},
                                           "ENHANCEMENT_PACKAGE_OUTSIDE_DIR"),
            "private package outside the folder": ({"tools/synth.json": encode(private)},
                                                   "ENHANCEMENT_PACKAGE_OUTSIDE_DIR"),
            "hex run in text": ({good: encode(hexed)}, "ENHANCEMENT_PACKAGE_GAME_BYTES"),
            "address in text": ({good: encode(addressed)}, "ENHANCEMENT_PACKAGE_GAME_BYTES"),
            "stock values": ({good: encode(valued)}, "ENHANCEMENT_PACKAGE_GAME_BYTES"),
            "file name": ({"assets/enhancements/synth.json": encode(published)}, "ENHANCEMENT_PACKAGE_NAME"),
            "other disc's name": ({"assets/enhancements/ucus12345-60hz.json": encode(published)},
                                  "ENHANCEMENT_PACKAGE_NAME"),
            "not JSON": ({good: (b"\x1e\x00\x04\x24", None)}, "ENHANCEMENT_PACKAGE_UNREADABLE"),
        }
        for label, (content, code) in cases.items():
            with self.subTest(label):
                codes = [f.code for f in publish_audit.check_enhancement_packages(content)]
                self.assertIn(code, codes)
        # The package schema is not a package.
        schema = {"assets/enhancement_package.schema.json": (SCHEMA_PATH.read_bytes(), None)}
        self.assertEqual(publish_audit.check_enhancement_packages(schema), [])

    def test_publication_gate_checks_every_string_and_every_file_in_the_folder(self) -> None:
        import publish_audit

        with tempfile.TemporaryDirectory() as tmp_dir:
            elf_bytes = _write_synthetic_mips_elf(Path(tmp_dir) / "synth.elf", self.words, self.entry)
            published = self._publish(elf_bytes)
        good = "assets/enhancements/ulus99999-60hz.json"

        def encode(data: Any) -> tuple[bytes, None]:
            return json.dumps(data, indent=2).encode("utf-8") + b"\n", None

        def with_field(path: tuple[str | int, ...], value: Any) -> dict[str, Any]:
            data = copy.deepcopy(published)
            node: Any = data
            for part in path[:-1]:
                node = node[part]
            node[path[-1]] = value
            return data

        def renamed(name: str) -> dict[str, Any]:
            # Every use of one function name, the site map's key included.
            text = json.dumps(published).replace('"synth_timer_duration"', json.dumps(name))
            data: dict[str, Any] = json.loads(text)
            return data

        hexes = "0123456789abcdef" * 8
        cases: dict[str, dict[str, Any]] = {
            "version build metadata": with_field(("version",), "1.0.0+" + hexes * 32),
            "version pre-release": with_field(("version",), "1.0.0-" + hexes[:64]),
            "package id": with_field(("id",), "p" + hexes[:63]),
            "game version": with_field(("game_version",), "v" + hexes[:31]),
            "site id": with_field(("sites", 0, "id"), "s" + hexes[:63]),
            "long function name": renamed("f" + hexes[:126]),
            "function name stem": renamed("fn_" + hexes[:64]),
            "nine hex digits in a name": renamed("synth_" + hexes[:9]),
            "base64 run": with_field(("display_name",), "QUJDREVGR0hJSktMTU5PUFFSU1RVVldY"),
            "escaped byte": with_field(("rng_policy", "reason"), "Same odds: \\x1e per update."),
            # Instruction words written as separated 8-hex tokens, as a disassembly paste would be.
            "separated words in a reason": with_field(
                ("rng_policy", "reason"), "Same odds: " + " ".join(["8c4200fe", "3c0100ff"] * 12)),
            "separated words in a display name": with_field(("display_name",), "Words 8c4200fe 3c0100ff"),
            "dotted words in an id": with_field(("id",), "a.8c4200fe.3c0100ff"),
            "words in a pre-release tag": with_field(("version",), "1.0.0-8c4200fe.3c0100ff"),
            "words in a site id": with_field(("sites", 0, "id"), "s_8c4200fe_3c0100ff"),
            "words before a name's own address": renamed(
                "f_8c4200fe_3c0100ff_" + published["site_map"]["synth_timer_duration"][2:]),
            # Fixed-shape slots hold exactly their value: no trailing newline.
            "newline after a digest": with_field(
                ("sites", 0, "stock_digest"), published["sites"][0]["stock_digest"] + "\n"),
            "newline after an address": with_field(
                ("site_map", "synth_timer_duration"), published["site_map"]["synth_timer_duration"] + "\n"),
            "control character in text": with_field(("rng_policy", "reason"), "Same odds\tper second."),
        }
        for label, data in cases.items():
            with self.subTest(label):
                codes = [f.code for f in publish_audit.check_enhancement_packages({good: encode(data)})]
                self.assertIn("ENHANCEMENT_PACKAGE_GAME_BYTES", codes)
        # Eight hex digits (one word or address, as the site map publishes) are not a finding.
        own = "synth_fn_" + published["site_map"]["synth_timer_duration"][2:]
        self.assertEqual(publish_audit.check_enhancement_packages({good: encode(renamed(own))}), [])
        # Five hex digits in a site id (a display-list offset) are not a word.
        self.assertEqual(publish_audit.check_enhancement_packages(
            {good: encode(with_field(("sites", 0, "id"), "P133_disp_ee8d0"))}), [])

        # The folder holds published packages only, whatever a file's extension.
        files = {
            "upper-case extension": ("assets/enhancements/ulus99999-60hz.JSON", encode(published)),
            "binary blob": ("assets/enhancements/blob.bin", (bytes(range(256)), None)),
            "second extension": ("assets/enhancements/ulus99999-60hz.json.txt", encode(published)),
            "subfolder": ("assets/enhancements/old/ulus99999-60hz.json", encode(published)),
            "unreadable": (good, (None, "permission denied")),
        }
        for label, (path, content) in files.items():
            with self.subTest(label):
                findings = publish_audit.check_enhancement_packages({path: content})
                self.assertTrue(findings)
                self.assertTrue(all(f.code in ("ENHANCEMENT_PACKAGE_NAME", "ENHANCEMENT_PACKAGE_UNREADABLE")
                                    for f in findings), findings)

    def test_publish_refuses_names_from_the_executables_symbol_table(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)
            private, symbols = self._make_package_and_symbols(elf_bytes)
            priv_path = tmp / "synth-60hz.json"
            priv_path.write_text(json.dumps(private), encoding="utf-8")
            priv_path.with_suffix(".symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            out = tmp / "ulus99999-60hz.json"
            args = ["--publish", str(priv_path), "--elf", str(elf_path), "--base", "0",
                    "--disc-id", self.DISC, "--disc-version", self.VERSION, "--out", str(out)]
            # A synthetic symbol table that holds one of the package's names: a string from the game.
            game_symbols = {"synth_timer_duration": int(symbols["synth_timer_duration"], 16), "other": 0x1000}
            err = StringIO()
            with mock.patch.object(enhancement_package, "extract_elf_func_symbols", return_value=game_symbols), \
                    mock.patch("sys.stderr", new=err), mock.patch("sys.stdout", new=StringIO()):
                rc = enhancement_package.main(args)
            self.assertEqual(rc, 1)
            self.assertIn("the executable's own symbols", err.getvalue())
            self.assertFalse(out.exists())
            # The names resolve only through the project's map: without it, nothing is published.
            priv_path.with_suffix(".symbols.json").unlink()
            with mock.patch.object(enhancement_package, "extract_elf_func_symbols", return_value=game_symbols), \
                    mock.patch("sys.stderr", new=StringIO()), mock.patch("sys.stdout", new=StringIO()):
                self.assertNotEqual(enhancement_package.main(args), 0)
            self.assertFalse(out.exists())


def _lui(rt: int, imm: int) -> int:
    return _i(0x0F, 0, rt, imm)


def _j(target: int) -> int:
    return (0x02 << 26) | ((target >> 2) & 0x3FFFFFF)


def _lvs(vt: int, off: int, base: int) -> int:
    return _i(0x32, base, vt & 31, (off & 0xFFFC) | (vt >> 5))


def _lvq(vt: int, off: int, base: int) -> int:
    return _i(0x36, base, vt & 31, (off & 0xFFFC) | (vt >> 5))


def _fpu_s(fn: int, fd: int, fs: int, ft: int) -> int:
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn


def _bc1f(pc: int, to: int) -> int:
    return _i(0x11, 0x08, 0, (to - pc - 4) >> 2)


def _f32_round(value: float) -> float:
    rounded: float = struct.unpack("<f", struct.pack("<f", value))[0]
    return rounded


class TestTwoUpdateTransforms(unittest.TestCase):
    """gate_30hz_phase, velocity_source_x_kappa, launch_from_rest_60hz, value_reference and
    retention_m_to_sqrt(m) on an arithmetic result (contract section 9.2), on a synthetic guest.

    WORLD loads an acceleration g and a damping coefficient c from read-only data. INTEGRATE loads
    the step h, a gravity column, and computes the retention 1 - c*L. STATE compares its own time,
    the float at 0x24 from its state pointer in $s0, with a threshold, loads the size of an
    impulse and calls IMPULSE, which adds it to the velocity of the body in $a0, the two floats at
    0x30. LAUNCH calls IMPULSE too. TUNE stores an impulse size. SURGE loads a size into a VFPU
    register, with its state pointer in $s1. Every value is made up for this fixture."""

    ENTRY, WORLD, INTEGRATE, STATE, IMPULSE, LAUNCH, TUNE, SURGE, END = (
        0x1000, 0x1100, 0x1200, 0x1300, 0x1400, 0x1500, 0x1600, 0x1700, 0x1800)
    DATA = 0x2000
    GRAVITY, DAMPING, DAMPING_STEP, STEP = -4.0, 0.25, 0.0625, _f32_round(1.0 / 30.0)
    BOOST = 0.125
    COMPARE_AT, MAGNITUDE_AT, CALL_AT, LAUNCH_AT, SURGE_AT = 0x18, 0x1C, 0x28, 0x08, 0x0C
    F0, F2, F3, F4, F6, F8, F12, F13, F14, F16, F17 = 0, 2, 3, 4, 6, 8, 12, 13, 14, 16, 17
    TIME_AT, SIZE_AT, VELOCITY_AT = 0x24, 0x2C, 0x30   # the state's time and impulse size; the body's velocity
    # VFPU registers whose number needs the bit an lv.s or lv.q keeps beside its offset.
    G_VREG, GRAVITY_VREG, SURGE_VREG = 39, 36, 37
    CAPABILITIES = ("two_update_motion", "launch_velocity_write")

    def _world(self) -> list[int]:
        return [_lui(V0, 0), _lvs(self.G_VREG, self.DATA, V0), _lui(V0, 0), _lwc1(self.F0, self.DATA + 4, V0),
                JR_RA, NOP]

    def _integrate(self) -> list[int]:
        return [_lui(V0, 0), _lwc1(self.F13, self.DATA + 0xC, V0), _lvq(self.GRAVITY_VREG, 0x40, A0),
                _lwc1(self.F2, 0x50, A0), _lwc1(self.F4, self.DATA + 8, V0), _fpu_s(0x02, self.F2, self.F2, self.F4),
                _lwc1(self.F0, self.DATA + 0x10, V0), _fpu_s(0x01, self.F3, self.F0, self.F2), JR_RA, NOP]

    def _state(self) -> list[int]:
        s = self.STATE
        return [_addiu(SP, SP, -16), _sw(RA, 12, SP), _sw(S0, 8, SP), _move(S0, A0),
                _lwc1(self.F6, self.TIME_AT, S0), _lwc1(self.F8, 0x28, S0), _fpu_s(0x3E, 0, self.F8, self.F6),
                _lwc1(self.F12, self.SIZE_AT, S0), _bc1f(s + 0x20, s + 0x30), NOP, _jal(self.IMPULSE),
                _move(A0, S0), _lw(V0, 0x40, S0), _lw(V1, 0x44, S0), _lwc1(self.F0, 0x48, S0), _lw(S0, 8, SP),
                _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)]

    def _impulse(self, base: int = A0) -> list[int]:
        """Adds (f14, f12) to the velocity of the body in $a0, and stores both floats."""
        v = self.VELOCITY_AT
        return [_lwc1(self.F16, v + 4, base), _fpu_s(0x00, self.F16, self.F16, self.F12), _swc1(self.F16, v + 4, base),
                _lwc1(self.F17, v, base), _fpu_s(0x00, self.F17, self.F17, self.F14), JR_RA, _swc1(self.F17, v, base)]

    def _launch(self) -> list[int]:
        return [_addiu(SP, SP, -8), _sw(RA, 4, SP), _jal(self.IMPULSE), NOP, _lw(RA, 4, SP), JR_RA,
                _addiu(SP, SP, 8)]

    def _tune(self) -> list[int]:
        return [_swc1(self.F0, 0x58, A0), JR_RA, NOP]

    def _surge(self) -> list[int]:
        return [_addiu(SP, SP, -8), _sw(S1, 0, SP), _move(S1, A0), _lvs(self.SURGE_VREG, self.SIZE_AT, S1),
                _lw(S1, 0, SP), JR_RA, _addiu(SP, SP, 8)]

    def _entry(self) -> list[int]:
        calls: list[int] = []
        for target in (self.WORLD, self.INTEGRATE, self.STATE, self.LAUNCH, self.TUNE, self.SURGE):
            calls += [_jal(target), NOP]
        return calls + [JR_RA, NOP]

    def _words(self, **parts: list[int]) -> dict[int, int]:
        layout = {"entry": self.ENTRY, "world": self.WORLD, "integrate": self.INTEGRATE, "state": self.STATE,
                  "impulse": self.IMPULSE, "launch": self.LAUNCH, "tune": self.TUNE, "surge": self.SURGE}
        words: dict[int, int] = {}
        for name, base in layout.items():
            code = parts.get(name) or getattr(self, f"_{name}")()
            words.update({base + 4 * i: w for i, w in enumerate(code)})
        for addr in range(self.ENTRY, self.END, 4):
            words.setdefault(addr, NOP)
        return words

    def _data(self, **values: float) -> bytes:
        floats = {"gravity": self.GRAVITY, "damping": self.DAMPING, "damping_step": self.DAMPING_STEP,
                  "step": self.STEP, "one": 1.0}
        floats.update(values)
        return b"".join(struct.pack("<f", floats[k]) for k in ("gravity", "damping", "damping_step", "step", "one"))

    def _symbols(self) -> dict[str, int]:
        return {"synth_entry": self.ENTRY, "synth_world": self.WORLD, "synth_integrate": self.INTEGRATE,
                "synth_state": self.STATE, "synth_impulse": self.IMPULSE, "synth_launch": self.LAUNCH,
                "synth_tune": self.TUNE, "synth_surge": self.SURGE}

    def _known(self) -> list[int]:
        return [*self._symbols().values(), self.END]

    def _sites(self, words: dict[int, int]) -> dict[str, dict[str, Any]]:
        def site(name: str, function: str, offset: int, transform: str, **extra: Any) -> dict[str, Any]:
            addr = self._symbols()[function] + offset
            return {"id": name, "function": function, "insn_offset": offset,
                    "stock_digest": compute_stock_words_digest([words[addr]]), "transform": transform, **extra}

        damping = {"damping_site": "C", "damping_step_site": "DSTEP"}
        phase = {"phase_time": {"register": "s0", "offset": self.TIME_AT}, "step_site": "STEP"}
        sites = [
            site("G", "synth_world", 4, "value_reference"),
            site("C", "synth_world", 0xC, "value_reference"),
            site("STEP", "synth_integrate", 4, "step_x0.5"),
            site("DSTEP", "synth_integrate", 0x10, "value_reference"),
            site("GRAVITY", "synth_integrate", 8, "velocity_source_x_kappa", **damping),
            site("RETAIN", "synth_integrate", 0x1C, "retention_m_to_sqrt(m)"),
            site("COMPARE", "synth_state", self.COMPARE_AT, "gate_30hz_phase", **phase, odd_phase_condition=False),
            site("CALL", "synth_state", self.CALL_AT, "gate_30hz_phase", **phase),
            site("MAGNITUDE", "synth_state", self.MAGNITUDE_AT, "gate_30hz_phase", **phase),
            site("SURGE", "synth_surge", self.SURGE_AT, "gate_30hz_phase", step_site="STEP",
                 phase_time={"register": "s1", "offset": self.TIME_AT}),
            site("LAUNCH", "synth_launch", self.LAUNCH_AT, "launch_from_rest_60hz",
                 velocity_offset=self.VELOCITY_AT, gravity_axis=1, gravity_site="G", step_site="STEP", **damping),
            site("BOOST", "synth_tune", 0, "velocity_source_x_kappa", stock_values=[self.BOOST], **damping),
        ]
        return {s["id"]: s for s in sites}

    def _package(self, words: dict[int, int], sha256: str, *, capabilities: tuple[str, ...] | None = None,
                 data_sites: list[dict[str, Any]] | None = None, **changes: Any) -> dict[str, Any]:
        """The fixture's package. ``changes`` maps a site id to the fields to set on it (None removes a
        field), or to None to leave the site out."""
        sites = self._sites(words)
        for name, fields in changes.items():
            if fields is None:
                del sites[name]
                continue
            site = sites.setdefault(name, {"id": name})
            site.update(fields)
            for key in [k for k, v in site.items() if v is None]:
                del site[key]
            if "stock_digest" not in fields:
                at = self._symbols()[site["function"]] + site["insn_offset"]
                site["stock_digest"] = compute_stock_words_digest(
                    [words[at + 4 * i] for i in range(site.get("word_count", 1))])
        package = {
            "schema_version": 1, "id": "synth.two-update", "display_name": "Synthetic two-update motion",
            "version": "1.0.0", "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
            "executable_sha256": sha256,
            "capabilities": list(self.CAPABILITIES if capabilities is None else capabilities),
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "sites": list(sites.values()),
        }
        if data_sites is not None:
            package["data_sites"] = data_sites
        return package

    def _resolve(self, words: dict[int, int] | None = None, *, data: bytes | None = None,
                 with_data: bool = True, known: bool = True, reader: Any = None,
                 **changes: Any) -> dict[str, Any]:
        """Resolve the package over dictionary readers; returns the rewrites by site id."""
        words = self._words() if words is None else words
        blob = self._data() if data is None else data

        def read_words(addr: int, count: int) -> list[int] | None:
            if all(addr + 4 * i in words for i in range(count)):
                return [words[addr + 4 * i] for i in range(count)]
            return None

        def read_const(addr: int, count: int) -> bytes | None:
            lo = addr - self.DATA
            return blob[lo:lo + count] if 0 <= lo and lo + count <= len(blob) else None

        resolved = verify_and_resolve_sites(
            self._package(words, "a" * 64, **changes), executable_sha256="a" * 64,
            symbol_to_addr={**self._symbols(), "synth_constants": self.DATA}, read_words_fn=read_words,
            read_const_fn=(reader or read_const) if with_data else None,
            known_functions=self._known() if known else [])
        return {entry["id"]: entry["rewrite"] for entry in resolved.values()}

    def _factors(self) -> tuple[float, float]:
        kappa, q = enhancement_package.two_update_factors(self.DAMPING, self.DAMPING_STEP, self.STEP)
        assert q is not None
        return kappa, q

    def test_resolved_rewrites_come_from_the_referenced_stock_values(self) -> None:
        """kappa, q*g and h/2 are computed from the floats the referenced sites load: nothing in the
        package holds them."""
        rewrites = self._resolve()
        kappa, q = self._factors()
        mu = math.sqrt(1.0 - self.DAMPING * self.DAMPING_STEP)
        self.assertEqual(kappa, 2.0 * mu / (1.0 + mu))
        self.assertEqual(q, self.STEP * mu * mu / (1.0 + mu) ** 2)
        phase = {"reg": 16, "offset": self.TIME_AT, "half_step": self.STEP / 2}
        self.assertEqual(rewrites, {
            "G": {"kind": "reference"}, "C": {"kind": "reference"}, "DSTEP": {"kind": "reference"},
            "STEP": {"kind": "scale_f_reg", "reg": 13, "factor": 0.5},
            "GRAVITY": {"kind": "scale_vq_reg", "vreg": self.GRAVITY_VREG, "factor": kappa},
            "RETAIN": {"kind": "map_f_reg", "reg": 3, "map": "retention"},
            "COMPARE": {"kind": "gate_phase_cond", **phase, "condition": False},
            "CALL": {"kind": "gate_phase", **phase},
            "MAGNITUDE": {"kind": "gate_phase_zero_f", **phase, "value_reg": 12},
            "SURGE": {"kind": "gate_phase_zero_v", **phase, "reg": 17, "value_vreg": self.SURGE_VREG},
            "LAUNCH": {"kind": "launch_from_rest", "offset": self.VELOCITY_AT, "axis": 1, "kappa": kappa,
                       "gravity_offset": q * self.GRAVITY},
            "BOOST": {"kind": "scale_f_store", "reg": 0, "factor": kappa,
                      "stock_values": [[_f32_bits_of(self.BOOST), _f32_bits_of(self.BOOST * kappa)]]},
        })
        # Other stock values give other factors: the rule is published, never its result.
        other = self._resolve(data=self._data(damping=0.5, gravity=-8.0))
        kappa2, q2 = enhancement_package.two_update_factors(0.5, self.DAMPING_STEP, self.STEP)
        assert q2 is not None
        self.assertNotEqual(kappa2, kappa)
        self.assertEqual(other["GRAVITY"]["factor"], kappa2)
        self.assertEqual(other["LAUNCH"]["gravity_offset"], q2 * -8.0)
        # A float built by a lui/ori pair is a reference too (word_count 2), and so is a lone lui
        # with its pinned low word.
        bits = _f32_bits_of(0.5)
        pair = [_lui(V1, bits >> 16), _i(0x0D, V1, V1, bits & 0xFFFF)]
        words = self._words(world=self._world()[:2] + pair + [JR_RA, NOP])
        as_pair = self._resolve(words, C={"insn_offset": 8, "word_count": 2})
        self.assertEqual(as_pair["GRAVITY"]["factor"], kappa2)
        self.assertEqual(as_pair["C"], {"kind": "reference"})
        as_low = self._resolve(words, C={"insn_offset": 8, "low_insn_offset": 0xC,
                                         "low_stock_digest": compute_stock_words_digest(pair[1:])})
        self.assertEqual(as_low["GRAVITY"]["factor"], kappa2)
        # The step may be pinned by a site that rewrites nothing, when another site halves it.
        unscaled = self._resolve(STEP={"transform": "value_reference"})
        self.assertEqual(unscaled["CALL"], rewrites["CALL"])
        self.assertEqual(unscaled["STEP"], {"kind": "reference"})

    def test_plans_without_resolved_values_are_refused(self) -> None:
        words = self._words()
        sites = self._sites(words)
        for name in ("GRAVITY", "COMPARE", "CALL", "MAGNITUDE", "SURGE", "LAUNCH"):
            site = dict(sites[name], word_count=1)
            addr = self._symbols()[site["function"]] + site["insn_offset"]
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, "resolve the package"):
                plan_site_rewrite(site, words[addr])
        retain = {"id": "R", "transform": "retention_m_to_sqrt(m)"}
        for fn in (0x00, 0x01, 0x02, 0x03):
            self.assertEqual(plan_site_rewrite(retain, _fpu_s(fn, 3, 0, 2)),
                             {"kind": "map_f_reg", "reg": 3, "map": "retention"})
        # A move copies the factor: its destination is mapped (section 11.3.2.7).
        self.assertEqual(plan_site_rewrite(retain, _fpu_s(0x06, 3, 0, 0)),
                         {"kind": "map_f_reg", "reg": 3, "map": "retention"})
        # A compare, and the decay transform on arithmetic, keep their refusals.
        with self.assertRaisesRegex(EnhancementPackageError, "or single-precision arithmetic"):
            plan_site_rewrite(retain, _fpu_s(0x3E, 0, 0, 1))
        with self.assertRaisesRegex(EnhancementPackageError, r"an lv\.s or a mov\.s\) \(fail-closed"):
            plan_site_rewrite({"id": "D", "transform": "decay_k_to_1-sqrt(1-k)"}, _fpu_s(0x01, 3, 0, 2))
        # An lv.q names its register with the low bit of its offset field; the other low bit is
        # not part of a plain lv.q, and such a word is refused.
        kappa = self._factors()[0]
        scale = dict(sites["GRAVITY"], word_count=1)
        for vreg in (0, 5, 36, 63):
            self.assertEqual(plan_site_rewrite(scale, _lvq(vreg, 0x40, A0), {"kappa": kappa}),
                             {"kind": "scale_vq_reg", "vreg": vreg, "factor": kappa})
        with self.assertRaisesRegex(EnhancementPackageError, "takes an lwc1, an lv.s, an lv.q or a swc1"):
            plan_site_rewrite(scale, _lvq(36, 0x40, A0) | 2, {"kappa": kappa})

    def test_schema_refusals(self) -> None:
        words = self._words()
        time = self.TIME_AT
        cases: tuple[tuple[str, dict[str, Any], str], ...] = (
            ("no step site", {"CALL": {"step_site": None}}, "gate_30hz_phase needs step_site"),
            ("unknown site", {"CALL": {"step_site": "NOWHERE"}}, "must name another site of this package"),
            ("its own id", {"GRAVITY": {"damping_site": "GRAVITY"}}, "must name another site of this package"),
            ("a foreign reference", {"STEP": {"step_site": "C"}}, "step_site belongs only to gate_30hz_phase, "),
            ("gravity on a gate", {"CALL": {"gravity_site": "G"}}, "gravity_site belongs only to launch_from_rest"),
            ("no phase time", {"CALL": {"phase_time": None}}, "needs phase_time"),
            ("a caller-saved register", {"CALL": {"phase_time": {"register": "a0", "offset": time}}},
             "needs phase_time"),
            ("an unaligned time", {"CALL": {"phase_time": {"register": "s0", "offset": time + 2}}},
             "needs phase_time"),
            ("a register that is no name", {"CALL": {"phase_time": {"register": ["s0"], "offset": time}}},
             "needs phase_time"),
            ("an extra phase field", {"CALL": {"phase_time": {"register": "s0", "offset": time, "scale": 2}}},
             "needs phase_time"),
            ("a condition that is no boolean", {"COMPARE": {"odd_phase_condition": 0}}, "must be true or false"),
            ("a phase time elsewhere", {"STEP": {"phase_time": {"register": "s0", "offset": time}}},
             "phase_time belongs only to gate_30hz_phase"),
            ("an unaligned velocity", {"LAUNCH": {"velocity_offset": self.VELOCITY_AT + 2}},
             "needs velocity_offset"),
            ("a third axis", {"LAUNCH": {"gravity_axis": 2}}, "needs gravity_axis"),
            ("a boolean axis", {"LAUNCH": {"gravity_axis": True}}, "needs gravity_axis"),
            ("an axis elsewhere", {"STEP": {"gravity_axis": 1}}, "gravity_axis belongs only to launch_from_rest"),
            ("two words", {"CALL": {"word_count": 2}}, "names one instruction"),
            ("a reference held back", {"G": {"requires": ["BOOST"]}}, "no rewrite for requires to hold back"),
            ("values on a reference", {"G": {"stock_values": [1.0]}}, "belongs only to a store site"),
            # A reference names a site whose value no transform changes; the step is the exception.
            ("a halved step as the damping step", {"GRAVITY": {"damping_step_site": "STEP"}},
             "damping_step_site 'STEP' names a step_x0.5 site; it must name a value_reference site"),
            ("a gate as the damping", {"GRAVITY": {"damping_site": "CALL"}},
             "names a gate_30hz_phase site; it must name a value_reference site"),
            ("a scaled load as the step", {"CALL": {"step_site": "GRAVITY"}},
             "it must name a value_reference or step_x0.5 site"),
            # One site pins one quantity, within a site and across the package.
            ("one site for c and L", {"GRAVITY": {"damping_site": "DSTEP"}},
             "site 'DSTEP' is named as damping_site and as damping_step_site"),
            ("g and c swapped in one site", {"LAUNCH": {"gravity_site": "C", "damping_site": "G"}},
             "site 'C' is named as damping_site and as gravity_site"),
            # A site that writes guest memory needs its capability named.
            ("no write capability", {"capabilities": ("two_update_motion",)},
             "launch_from_rest_60hz writes guest memory: the package must name the capability "
             "'launch_velocity_write'"),
        )
        for name, changes, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                validate_package(self._package(words, "a" * 64, **changes))
        validate_package(self._package(words, "a" * 64))
        # Without the site that writes, the capability is not asked for.
        validate_package(self._package(words, "a" * 64, capabilities=("two_update_motion",), LAUNCH=None))

    def test_instruction_refusals(self) -> None:
        integrate = self._integrate()
        integrate[2] |= 2
        cases: tuple[tuple[str, dict[str, Any], str], ...] = (
            ("a gate on a move", {"CALL": {"insn_offset": 0x0C}},
             r"gate_30hz_phase takes a jal, a single-precision compare \(c.cond.s\), or an lwc1 or lv.s"),
            ("a compare with no condition", {"COMPARE": {"odd_phase_condition": None}},
             "compare needs odd_phase_condition"),
            ("a call with a condition", {"CALL": {"odd_phase_condition": True}},
             "odd_phase_condition belongs to a compare site; a call is skipped"),
            ("a load with a condition", {"MAGNITUDE": {"odd_phase_condition": False}},
             "odd_phase_condition belongs to a compare site; a load reads"),
            ("a launch that is no call", {"LAUNCH": {"insn_offset": 0}}, "launch_from_rest_60hz takes a jal"),
            ("kappa on arithmetic", {"GRAVITY": {"insn_offset": 0x14}},
             "velocity_source_x_kappa takes an lwc1, an lv.s, an lv.q or a swc1"),
            ("an lv.q with its other low bit set", {"words": self._words(integrate=integrate)},
             "velocity_source_x_kappa takes an lwc1, an lv.s, an lv.q or a swc1"),
            # A value_reference that pins nothing is refused, referenced or not.
            ("a reference to no load", {"X": {"function": "synth_launch", "insn_offset": 0xC,
                                              "transform": "value_reference"}},
             "site 'X' does not pin a float"),
            ("a lone lui", {"X": {"function": "synth_world", "insn_offset": 8, "transform": "value_reference"}},
             "site 'X' does not pin a float"),
            ("a load from a pointer", {"X": {"function": "synth_integrate", "insn_offset": 0xC,
                                             "transform": "value_reference"}},
             "site 'X' does not load from a constant address"),
        )
        for name, changes, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(**changes)
        # Planned alone, a value_reference on anything else is refused by name.
        with self.assertRaisesRegex(EnhancementPackageError, "value_reference pins the float"):
            plan_site_rewrite({"id": "X", "transform": "value_reference"}, NOP)

    def _world_with_c(self, before: list[int]) -> tuple[dict[int, int], dict[str, Any]]:
        """WORLD with ``before`` placed between g's load and c's load; returns the words and c's site."""
        code = self._world()[:2] + before + [_lwc1(self.F0, self.DATA + 4, V0), JR_RA, NOP]
        return self._words(world=code), {"insn_offset": 4 * (2 + len(before))}

    def test_referenced_values_are_read_and_checked(self) -> None:
        kappa = self._factors()[0]
        cases: tuple[tuple[str, dict[str, Any], str], ...] = (
            ("damping of one or more", {"data": self._data(damping=16.0)}, r"c\*L = 1\.0, is outside \(0, 1\)"),
            ("no damping", {"data": self._data(damping=0.0)}, r"is outside \(0, 1\)"),
            ("negative damping", {"data": self._data(damping_step=-0.0625)}, r"is outside \(0, 1\)"),
            ("no step", {"data": self._data(step=0.0)}, "is not a positive float32 a state's time can count"),
            ("a backward step", {"data": self._data(step=-0.03125)}, "is not a positive"),
            ("a step that is no number", {"data": self._data(step=float("nan"))}, "pins a float that is not finite"),
            ("no data reader", {"with_data": False}, "which this resolver was not given"),
            ("data that ends early", {"data": self._data()[:6]}, "is not read-only initialised data"),
            ("a reader that returns half a word", {"reader": lambda addr, count: self._data()[:2]},
             "is not read-only initialised data"),
        )
        for name, kwargs, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(**kwargs)
        w = self.WORLD
        beql = _i(0x14, A1, ZERO, 2)     # beql a1, zero, +2: its slot runs only when it is taken
        refused: tuple[tuple[str, list[int], str], ...] = (
            ("a computed base", [_move(V0, A0)], "the base register of its load is computed at 0x00001108"),
            ("a base from before a call", [_lui(V0, 0), _jal(self.IMPULSE), NOP],
             "does not load from a constant address"),
            ("a lui in the slot of a branch that is always taken", [_beq(ZERO, ZERO, w + 8, w + 0x10), _lui(V0, 0)],
             "does not load from a constant address"),
            ("a lui in the slot of a branch-likely", [beql, _lui(V0, 0)], "does not load from a constant address"),
            ("a lui in the slot of a jump", [_j(w + 0x10), _lui(V0, 0)], "does not load from a constant address"),
            ("a lui in the slot of a call", [_jal(self.IMPULSE), _lui(V0, 0)], "does not load from a constant address"),
            ("a lui before a branch-likely", [_lui(V0, 0), beql, NOP], "does not load from a constant address"),
            ("a lui seventeen instructions back", [_lui(V0, 0)] + [NOP] * 16, "does not load from a constant address"),
            ("a load in the slot of a call that links into its base",
             [_lui(V0, 0), _r(A1, 0, V0, 0, 0x09)], "the base register of its load is computed at 0x0000110c"),
            # A second path to the load: the branch is taken around part of the code read.
            ("a branch over the addiu, landing on the load",
             [_beq(A1, ZERO, w + 8, w + 0x14), _lui(V0, 0), _addiu(V0, V0, 8)],
             "the transfer at 0x00001108 can land at 0x00001114, inside the code that builds its load's address"),
            ("a branch over a second lui, landing on the load",
             [_lui(V0, 0), _beq(A1, ZERO, w + 0xC, w + 0x18), NOP, _lui(V0, 0)],
             "the transfer at 0x0000110c can land at 0x00001118, inside the code that builds"),
            ("a branch from before the lui to the addiu after it",
             [_beq(A1, ZERO, w + 8, w + 0x18), NOP, NOP, _lui(V0, 0), _addiu(V0, V0, 0)],
             "the transfer at 0x00001108 can land at 0x00001118, inside the code that builds"),
            ("a jump over the lui, landing on the load", [_j(w + 0x14), NOP, _lui(V0, 0)],
             "the transfer at 0x00001108 can land at 0x00001114, inside the code that builds"),
        )
        for name, before, why in refused:
            words, c_site = self._world_with_c(before)
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(words, C=c_site)
        # A delay slot is straight-line code where it runs on the way to the load: the load's own
        # jump, call or branch comes after everything before it, and a conditional branch that is
        # not a likely form runs its slot on the path that falls through.
        accepted: tuple[tuple[str, list[int]], ...] = (
            ("a lui then an addiu", [_lui(V0, 0), _addiu(V0, V0, 0)]),
            ("a load in the slot of a jump", [_lui(V0, 0), _j(w + 0x14)]),
            ("a load in the slot of a call", [_lui(V0, 0), _jal(self.IMPULSE)]),
            ("a load in the slot of a branch-likely", [_lui(V0, 0), beql]),
            ("a lui in the slot of a conditional branch", [_beq(A1, ZERO, w + 8, w + 0x14), _lui(V0, 0)]),
            ("a lui before a conditional branch", [_lui(V0, 0), _beq(A1, ZERO, w + 0xC, w + 0x18), NOP]),
            # Taken to the load, but around nothing that builds the address: both paths build it.
            ("a lui in the slot of a branch taken to the load",
             [_beq(A1, ZERO, w + 8, w + 0x14), _lui(V0, 0), NOP]),
            ("a lui and an addiu, then a branch taken to the load",
             [_lui(V0, 0), _addiu(V0, V0, 0), _beq(A1, ZERO, w + 0x10, w + 0x1C), NOP, NOP]),
            ("both: a lui in a slot and the load in the next slot",
             [_beq(A1, ZERO, w + 8, w + 0x18), _lui(V0, 0), _j(w + 0x18)]),
            ("a lui sixteen instructions back", [_lui(V0, 0)] + [NOP] * 15),
            ("a branch that lands just after its own slot",
             [_beq(A1, ZERO, w + 8, w + 0x10), _lui(V0, 0), _addiu(V0, V0, 0)]),
            ("a branch from before the lui to the lui itself", [_beq(A1, ZERO, w + 8, w + 0x10), NOP, _lui(V0, 0)]),
        )
        for name, before in accepted:
            words, c_site = self._world_with_c(before)
            with self.subTest(name):
                self.assertEqual(self._resolve(words, C=c_site)["GRAVITY"]["factor"], kappa)
        # A lui then an addiu that carries the low half of the address.
        split = [_lui(V0, 0), _lvs(self.G_VREG, self.DATA, V0), _lui(V0, 0), _addiu(V0, V0, self.DATA),
                 _lwc1(0, 4, V0), JR_RA, NOP]
        self.assertEqual(self._resolve(self._words(world=split), C={"insn_offset": 0x10})["GRAVITY"]["factor"], kappa)
        # The gate reads only the step: it needs no damping, and refuses a step it cannot count.
        with self.assertRaisesRegex(EnhancementPackageError, r"sites\[\d\]\(COMPARE\).*not a positive float32"):
            self._resolve(data=self._data(step=0.0), GRAVITY=None, LAUNCH=None, BOOST=None)

    def _state_with(self, at: int, new: list[int]) -> dict[int, int]:
        body = self._state()
        body[at // 4:at // 4 + len(new)] = new
        return self._words(state=body)

    def test_phase_register_is_written_once_before_the_site(self) -> None:
        s = self.STATE
        # The epilogue restores $s0 after the sites: that write comes before none, and is accepted.
        self.assertEqual(self._resolve()["CALL"]["kind"], "gate_phase")
        skip_the_move = self._state()
        skip_the_move[1:3] = [_beq(A1, ZERO, s + 4, s + 0x10), NOP]     # one path keeps the caller's $s0
        second_move = self._state_with(0x1C, [_move(S0, A1)])           # between the compare and the call
        loop = self._state()
        loop[12:14] = [_beq(V0, ZERO, s + 0x30, s + 0x10), _addiu(S0, S0, 4)]   # steps $s0, then back
        cases: tuple[tuple[str, dict[int, int], str], ...] = (
            ("a path without the write", self._words(state=skip_the_move),
             r"on a path that does not write \$s0 first, at 0x0000130c"),
            ("no write at all", self._state_with(0x0C, [NOP]), r"writes \$s0 0 times on its paths to the site"),
            ("a second write before the call", second_move,
             r"\(CALL\).*writes \$s0 2 times on its paths to the site \(0x0000130c, 0x0000131c\)"),
            ("a write in the call's delay slot", self._state_with(0x2C, [_move(S0, A0)]),
             r"delay slot of the gated call writes \$s0"),
            ("a write on a loop back", self._words(state=loop),
             r"writes \$s0 2 times on its paths to the site \(0x0000130c, 0x00001334\)"),
            ("an unreadable word before the site", self._state_with(0x1C, [0x70000000]),
             r"has a word this check cannot read at 0x0000131c"),
        )
        for name, words, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(words)
        # The second move follows the compare, so the compare alone is accepted.
        self.assertEqual(self._resolve(second_move, CALL=None, MAGNITUDE=None)["COMPARE"]["reg"], 16)
        # A gated load answers to the same check, in its own function and register.
        surge = self._surge()
        surge[2] = NOP
        with self.assertRaisesRegex(EnhancementPackageError, r"\(SURGE\).*writes \$s1 0 times on its paths"):
            self._resolve(self._words(surge=surge))
        with self.assertRaisesRegex(EnhancementPackageError, "is not a known function start"):
            self._resolve(known=False)

    def test_gated_call_and_compare_refusals(self) -> None:
        s = self.STATE
        compare_in_slot = self._state()
        compare_in_slot[5:8] = [_beq(A1, ZERO, s + 0x14, s + 0x20), _fpu_s(0x3E, 0, self.F8, self.F6), NOP]
        ctc1 = (0x11 << 26) | (0x06 << 21) | (V0 << 16) | (31 << 11)    # ctc1 v0, $31
        cases: tuple[tuple[str, dict[int, int], dict[str, Any], str], ...] = (
            ("the caller reads the result", self._state_with(0x30, [_addiu(V0, V0, 1)]), {},
             r"gate_30hz_phase caller 0x00001300 reads \$v0 at 0x00001330 after the gated call"),
            ("the caller returns the result", self._state_with(0x30, [NOP]), {},
             r"gate_30hz_phase caller 0x00001300 returns after the gated call without writing \$v0"),
            ("no branch reads the flag", self._state_with(0x20, [NOP] * 4), {"CALL": None},
             "no bc1f or bc1t reads the compare's flag in the 4 instructions"),
            ("a call before the branch", self._state_with(0x1C, [_jal(self.IMPULSE), NOP, NOP]), {"CALL": None},
             "a call, jump or branch at 0x0000131c follows the compare"),
            ("another compare first", self._state_with(0x1C, [_fpu_s(0x3C, 0, self.F8, self.F6)]), {"CALL": None},
             "0x0000131c writes the FPU condition flag before"),
            ("a control-register write first", self._state_with(0x1C, [ctc1]), {"CALL": None},
             "0x0000131c writes the FPU condition flag before"),
            ("a compare in a delay slot", self._words(state=compare_in_slot), {"CALL": None},
             "does not reach the site at 0x00001318 as an instruction of its own"),
        )
        for name, words, changes, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(words, **changes)
        # A gated load needs neither check: the call it feeds always runs, and no flag is forced.
        # With the caller reading the call's result, the gated call is refused and the load is not.
        reads_result = self._state_with(0x30, [_addiu(V0, V0, 1)])
        self.assertEqual(self._resolve(reads_result, CALL=None)["MAGNITUDE"]["kind"], "gate_phase_zero_f")
        # An lv.q in a delay slot is refused: the column is scaled where straight-line code loads it.
        base = self.INTEGRATE
        slot = [_lui(V0, 0), _lwc1(self.F13, self.DATA + 0xC, V0), _beq(ZERO, ZERO, base + 8, base + 0x10),
                _lvq(self.GRAVITY_VREG, 0x40, A0), _lui(V0, 0), _lwc1(self.F4, self.DATA + 8, V0), JR_RA, NOP]
        with self.assertRaisesRegex(EnhancementPackageError, "an lv.q site is in the delay slot of"):
            self._resolve(self._words(integrate=slot), GRAVITY={"insn_offset": 0xC}, DSTEP={"insn_offset": 0x14},
                          RETAIN=None)

    def test_a_data_site_may_not_rescale_a_pinned_word(self) -> None:
        """A word a site reference pins is the stock value the title uses. A data site that
        rescales it at load is refused; the step, which the package halves by design, is not."""
        def table(offset: int, count: int = 1) -> dict[str, Any]:
            raw = self._data()[offset:offset + 4 * count]
            return {"id": "TABLE", "symbol": "synth_constants", "offset": offset, "type": "f32", "count": count,
                    "stock_digest": hashlib.sha256(raw).hexdigest(), "transform": "step_x0.5"}

        for name, site, pinned in (("the damping step", table(8), "DSTEP"), ("the damping", table(4), "C"),
                                   ("gravity, in a longer table", table(0, 2), "C"),
                                   ("the damping step, at the end of a table", table(4, 2), "C")):
            with self.subTest(name), self.assertRaisesRegex(
                    EnhancementPackageError, rf"data_sites\[0\]\(TABLE\): rescales the word at 0x0000200\w that site "
                                             rf"'{pinned}' pins"):
                self._resolve(data_sites=[site])
        # The step's word, and a word no site pins, may be rescaled.
        for offset in (0xC, 0x10):
            self.assertEqual(self._resolve(data_sites=[table(offset)])["GRAVITY"]["factor"], self._factors()[0])
        # A word pinned without a data address (a lui/ori pair) has nothing a data site can reach.
        bits = _f32_bits_of(self.DAMPING)
        pair = [_lui(V1, bits >> 16), _i(0x0D, V1, V1, bits & 0xFFFF)]
        words = self._words(world=self._world()[:2] + pair + [JR_RA, NOP])
        self.assertEqual(self._resolve(words, C={"insn_offset": 8, "word_count": 2}, data_sites=[table(4)])
                         ["GRAVITY"]["factor"], self._factors()[0])

    def test_a_gated_load_is_not_the_time_and_is_not_stored(self) -> None:
        """The zero a gated load reads is for an effect the value scales. The resolver refuses the
        load of the state's own time, and a load whose register the title then stores: that store
        would write a zero the stock code never stores."""
        with self.assertRaisesRegex(EnhancementPackageError, r"\(MAGNITUDE\): the gated load is the load of the "
                                                             r"state's own time"):
            self._resolve(MAGNITUDE={"insn_offset": 0x10})
        # In STATE the load's register is f12, and the branch after it has one delay slot.
        with self.assertRaisesRegex(EnhancementPackageError, r"\(MAGNITUDE\): the instruction at 0x00001324 stores "
                                                             r"the gated register"):
            self._resolve(self._state_with(0x24, [_swc1(self.F12, 0x60, S0)]))
        self.assertEqual(self._resolve(self._state_with(0x24, [_swc1(self.F8, 0x60, S0)]))["MAGNITUDE"]["value_reg"], 12)

        def surge(load: int, after: list[int]) -> dict[int, int]:
            code = self._surge()
            return self._words(surge=code[:3] + [load] + after + code[4:])

        vreg, size = self.SURGE_VREG, self.SIZE_AT
        lv_s = _lvs(vreg, size, S1)
        lwc1 = _lwc1(self.F12, size, S1)
        sv_s = _i(0x3A, S1, vreg & 31, 0x60 | (vreg >> 5))
        mfv = (0x12 << 26) | (0x03 << 21) | (V0 << 16) | vreg            # mfv v0, the gated register
        mfc1 = (0x11 << 26) | (V0 << 16) | (self.F12 << 11)              # mfc1 v0, f12
        refused: tuple[tuple[str, int, list[int], str], ...] = (
            ("an sv.s of the gated VFPU register", lv_s, [NOP, sv_s], "0x00001714 stores the gated register"),
            ("a copy to a general register, then stored", lv_s, [mfv, _sw(V0, 0x60, S1)],
             "0x00001714 stores the gated register"),
            ("a swc1 of the gated FPU register", lwc1, [_swc1(self.F12, 0x60, S1)],
             "0x00001710 stores the gated register"),
            ("an FPU copy to a general register, then stored", lwc1, [mfc1, NOP, _sw(V0, 0x60, S1)],
             "0x00001718 stores the gated register"),
            ("a store in the delay slot of the first branch", lwc1, [_beq(A1, ZERO, self.SURGE + 0x10,
                                                                         self.SURGE + 0x18),
                                                                    _swc1(self.F12, 0x60, S1)],
             "0x00001714 stores the gated register"),
        )
        for name, load, after, why in refused:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, rf"\(SURGE\).*{why}"):
                self._resolve(surge(load, after))
        accepted: tuple[tuple[str, int, list[int]], ...] = (
            ("overwritten by another load first", lv_s, [lv_s, sv_s]),
            ("an FPU register overwritten by a result first", lwc1, [_fpu_s(0x00, self.F12, self.F0, self.F0),
                                                                      _swc1(self.F12, 0x60, S1)]),
            ("the copy's register reused before the store", lwc1, [mfc1, _addiu(V0, ZERO, 1), _sw(V0, 0x60, S1)]),
            ("another register stored", lwc1, [_swc1(self.F0, 0x60, S1), _sw(V0, 0x64, S1)]),
            ("a store after the first branch and its slot", lwc1, [_beq(A1, ZERO, self.SURGE + 0x10,
                                                                       self.SURGE + 0x18), NOP,
                                                                  _swc1(self.F12, 0x60, S1)]),
        )
        for name, load, after in accepted:
            with self.subTest(name):
                self.assertIn(self._resolve(surge(load, after))["SURGE"]["kind"],
                              ("gate_phase_zero_f", "gate_phase_zero_v"))

    def test_launch_callee_must_store_the_velocity_itself(self) -> None:
        """A launch map may only replace a value the stock code stores at the same place: the
        resolver reads the callee and refuses unless it stores both floats, from its entry $a0,
        on every path that returns."""
        v = self.VELOCITY_AT
        stock = self._impulse()
        sv_q = _i(0x3E, A0, 4, v)                                  # sv.q: 16 bytes at the velocity
        both_at_once = [_lwc1(self.F16, v, A0), JR_RA, sv_q]
        early_out = [_beq(A1, ZERO, self.IMPULSE, self.IMPULSE + 4 * 9), NOP] + stock + [JR_RA, NOP]
        one_float = stock[:6] + [NOP]
        moved_base = [_addiu(A0, A0, 4)] + stock
        delegated = [_addiu(SP, SP, -8), _sw(RA, 4, SP), _jal(self.TUNE), NOP, _lw(RA, 4, SP), JR_RA,
                     _addiu(SP, SP, 8)]
        elsewhere = [w if (w >> 26) != 0x39 else w + 8 for w in stock]   # the stores land beside the velocity
        for name, callee in (("the fixture's", stock), ("through a copy of $a0", [_move(T0, A0)] + self._impulse(T0)),
                             ("one 16-byte store", both_at_once)):
            with self.subTest(name):
                self.assertEqual(self._resolve(self._words(impulse=callee))["LAUNCH"]["kind"], "launch_from_rest")
        cases: tuple[tuple[str, list[int], str], ...] = (
            ("a path that returns without storing", early_out,
             rf"callee 0x00001400 can return without storing the float at {v} from its entry \$a0"),
            ("only one float stored", one_float, f"can return without storing the float at {v} from"),
            ("a base that is no longer the entry $a0", moved_base, "can return without storing the float at"),
            ("stores made by a function it calls", delegated, "can return without storing the float at"),
            ("stores beside the velocity", elsewhere, f"can return without storing the float at {v} from"),
        )
        for name, callee, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(impulse=callee))

    def _elf(self, path: Path, *, data: bytes | None = None, data_flags: int = 2) -> bytes:
        """Write the fixture as an ELF: code, and the constants in a read-only data section."""
        image = dict(self._words())
        image.update({self.DATA + 4 * i: w
                      for i, w in enumerate(struct.unpack("<5I", self._data() if data is None else data))})
        return _write_synthetic_mips_elf(
            path, image, self.ENTRY,
            sections=((".text", self.ENTRY, self.END - self.ENTRY, 6), (".rodata", self.DATA, 20, data_flags)))

    def _codegen(self, tmp: Path, name: str, elf_path: Path, pkg: dict[str, Any] | None,
                 extra: list[str] | None = None) -> tuple[int, str, str]:
        args = list(extra or [])
        if pkg is not None:
            pkg_path = tmp / f"{name}.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            (tmp / f"{name}.symbols.json").write_text(
                json.dumps({k: f"0x{v:08x}" for k, v in self._symbols().items()}), encoding="utf-8")
            args.append(f"--enhancement-package={pkg_path}")
        out_dir = tmp / name
        out_dir.mkdir()
        rc, output, files = TestEnhancementAotCodegenAndCache._run_codegen(elf_path, out_dir / "out.c", args)
        return rc, output, b"\n".join(files.values()).decode("utf-8")

    def _build(self, tmp: Path, name: str, *, with_package: bool, extra: list[str] | None = None,
               **changes: Any) -> str:
        """Generate the fixture with the package (or without, the Authentic build); returns the C."""
        elf_path = tmp / "two_update.elf"
        self.elf_bytes = self._elf(elf_path)
        pkg = self._package(self._words(), hashlib.sha256(self.elf_bytes).hexdigest(), **changes) \
            if with_package else None
        rc, output, text = self._codegen(tmp, name, elf_path, pkg, extra)
        self.assertEqual(rc, 0, output)
        return text

    #: The sites whose statements the tests take from the generated C, with their addresses.
    def _site_addresses(self) -> dict[str, int]:
        return {"step": self.INTEGRATE + 4, "gravity": self.INTEGRATE + 8, "retain": self.INTEGRATE + 0x1C,
                "compare": self.STATE + self.COMPARE_AT, "magnitude": self.STATE + self.MAGNITUDE_AT,
                "call": self.STATE + self.CALL_AT, "surge": self.SURGE + self.SURGE_AT,
                "launch": self.LAUNCH + self.LAUNCH_AT}

    def _statements(self, text: str, with_package: bool) -> dict[str, str]:
        """The generated statement of each site: a rewritten line is its two forms, a call runs
        from its trace hook to the end of its statement."""
        lines = text.splitlines()

        def at(addr: int) -> int:
            return next(i for i, line in enumerate(lines) if f"sr_begin(s, 0x{addr:08x}u" in line)

        out: dict[str, str] = {}
        for name, addr in self._site_addresses().items():
            start = at(addr)
            rewritten = with_package and lines[start].lstrip().startswith("if (nk_enhancement_on())")
            if name in ("call", "launch"):
                end = next(i for i in range(start, len(lines)) if "sr_aot_return_boundary_begin" in lines[i])
                if any("_nk_" in line for line in lines[start:end]):
                    closer = "      } }" if name == "call" else "    }"
                    end = next(i for i in range(end, len(lines)) if lines[i] == closer)
                out[name] = "\n".join(lines[start:end + 1])
            else:
                out[name] = "\n".join(lines[start:start + 2]) if rewritten else lines[start]
        return out

    @staticmethod
    def _runtime(text: str) -> str:
        """The generated run-time half of the phase gates and the launch map, as emitted."""
        blocks = []
        for opening, closing in (("/* Enhancement phase tables", "/* End of enhancement phase tables. */"),
                                 ("/* Enhancement launch sites", "/* End of enhancement launch sites. */")):
            if opening in text:
                start = text.index(opening)
                blocks.append(text[start:text.index(closing, start) + len(closing)])
        return "\n".join(blocks)

    @staticmethod
    def _rewrite_part(statement: str) -> str:
        """What a rewritten line adds after its stock statement: the text between the stock form
        and the end of the enhanced form."""
        enhanced, stock = statement.split("\n")
        stock = stock.strip()[len("else { "):-len(" }")]
        head = f"if (nk_enhancement_on()) {{ {stock} "
        assert enhanced.strip().startswith(head), statement
        return enhanced.strip()[len(head):-len(" }")]

    def test_emitted_forms(self) -> None:
        """Each site is emitted in both forms, a build without the package is untouched, and a
        package without these sites carries none of their run-time code."""
        kappa, q = self._factors()
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            authentic = self._build(tmp, "authentic", with_package=False)
            text = self._build(tmp, "pkg", with_package=True)
            lle = self._build(tmp, "lle", with_package=True, extra=["--lle-cpu"])
            requires = {"requires": ["BOOST"]}
            waits = self._build(tmp, "waits", with_package=True, CALL=requires, LAUNCH=requires, COMPARE=requires)
            plain = self._build(tmp, "plain", with_package=True, COMPARE=None, CALL=None, MAGNITUDE=None,
                                SURGE=None, LAUNCH=None)
        self.assertNotIn("nk_enhancement", authentic)
        self.assertNotIn("_nk_", authentic)
        for name in ("nk_enhancement_phase_odd", "NkEnhPhaseRun", "nk_enhancement_launch", "nk_enh_launch"):
            self.assertNotIn(name, plain)
        stock = self._statements(authentic, False)
        both = self._statements(text, True)
        # Toggle off, every site runs the Authentic statement.
        for name in ("step", "gravity", "retain", "compare", "magnitude", "surge"):
            self.assertIn(f"else {{ {stock[name].strip()} }}", both[name], name)
        self.assertIn(stock["call"].splitlines()[-1], both["call"])
        self.assertIn(stock["launch"].splitlines()[-1], both["launch"])
        # The gates read the state's own time through the guest accessor and look it up in the
        # one table of the package's step; no update count is used.
        for name, reg, site_id in (("compare", 16, "COMPARE"), ("call", 16, "CALL"), ("magnitude", 16, "MAGNITUDE"),
                                ("surge", 17, "SURGE")):
            self.assertIn(f"static int nk_kept; uint32_t _nk_time = MEM_R32(s->r[{reg}] + {self.TIME_AT}u); "
                          f'int _nk_odd = nk_enhancement_phase_odd(&nk_kept, "{site_id}", '
                          f"0x{self._site_addresses()[name]:08x}u, _nk_time, 0u);", both[name])
        self.assertEqual(text.count("static const NkEnhPhaseRun nk_enh_phase_runs_"), 1)
        rows = ", ".join(f"{{{n0}u, {count}u, {a0!r}, {a1!r}, {delta!r}}}"
                         for n0, count, a0, a1, delta in enhancement_package.phase_runs(self.STEP / 2))
        self.assertIn(f"nk_enh_phase_runs_0[] = {{{rows}}};", text)
        # The lookup matches the time exactly: inside a row, and a whole number of its steps from its start.
        self.assertIn("    for (i = 0; i < nk_enh_phase_tables[table].count; i++) {\n"
                      "        if (x >= runs[i].a0 && x <= runs[i].a1) {\n"
                      "            double k = (x - runs[i].a0) / runs[i].delta;\n"
                      "            if (k == floor(k)) return (int)((runs[i].n0 + (uint32_t)k) & 1u);\n"
                      "            break;\n", text)
        self.assertIn("if (_nk_odd) { s->fpcond = 0u; s->fcr31 = (s->fcr31 & ~0x00800000u) | (s->fpcond << 23); }",
                      both["compare"])
        self.assertIn("_nk_run = !_nk_odd; }", both["call"])
        self.assertIn("if (_nk_odd) { s->fi[12] = 0u; }", both["magnitude"])
        self.assertIn(f"if (_nk_odd) {{ s->vi[{codegen.vreg_indices(self.SURGE_VREG, 1)[0]}] = 0u; }}", both["surge"])
        self.assertNotIn("nk_enhancement_gate_30hz()) {", text)
        self.assertNotIn("nk_enhancement_updates++", text)
        # The launch statement: $a0 and the other axis's float are taken before the call; after it
        # the gravity axis is mapped whole and the other axis only by what the call changed.
        launch = both["launch"]
        site = self.LAUNCH + self.LAUNCH_AT
        x, y = self.VELOCITY_AT, self.VELOCITY_AT + 4
        before = (f"    {{ uint32_t _nk_a0 = s->r[4]; uint32_t _nk_before = 0u; int _nk_map = nk_enhancement_on();\n"
                  f"      if (_nk_map) {{ uint32_t _nk_b = MEM_R32(_nk_a0 + {x}u); _nk_before = _nk_b; "
                  f"nk_enhancement_launch_begin(0u); }}\n")
        self.assertIn(before, launch)
        self.assertLess(launch.index(before), launch.index("sr_aot_return_boundary_begin"))
        after = launch[launch.index("sr_aot_return_boundary_end"):]
        self.assertIn(
            f"        {{ uint32_t _nk_v0 = MEM_R32(_nk_a0 + {x}u); if (_nk_v0 != _nk_before) {{ float _nk_f, _nk_g; "
            f"memcpy(&_nk_f, &_nk_v0, sizeof _nk_f); memcpy(&_nk_g, &_nk_before, sizeof _nk_g); "
            f"_nk_f = (float)(fma((double)_nk_f - (double)_nk_g, {kappa!r}, (double)_nk_g)); "
            f"memcpy(&_nk_v0, &_nk_f, sizeof _nk_v0); MEM_W32_PC(_nk_a0 + {x}u, _nk_v0, 0x{site:08x}u); }} }}\n"
            f"        {{ uint32_t _nk_v1 = MEM_R32(_nk_a0 + {y}u); float _nk_f; memcpy(&_nk_f, &_nk_v1, sizeof _nk_f); "
            f"_nk_f = (float)(fma((double)_nk_f, {kappa!r}, {q * self.GRAVITY!r})); "
            f"memcpy(&_nk_v1, &_nk_f, sizeof _nk_v1); MEM_W32_PC(_nk_a0 + {y}u, _nk_v1, 0x{site:08x}u); }}\n"
            f"        nk_enhancement_launch_done(0u);\n", after)
        self.assertIn(f'    {{"LAUNCH", 0x{site:08x}u, 0u, 0u}},', text)
        self.assertIn("ENHANCEMENT_LAUNCH_NOT_MAPPED: site %s at 0x%08x", text)
        # The loaded gravity column is scaled after the load, lane by lane; the computed retention
        # is mapped after the subtraction; the stored impulse goes through the stock-value check.
        lanes = " ".join(f"s->v[{v}] = (float)((double)s->v[{v}] * {kappa!r});"
                         for v in codegen.vreg_indices(self.GRAVITY_VREG, 4))
        self.assertEqual(self._rewrite_part(both["gravity"]), lanes)
        self.assertEqual(self._rewrite_part(both["step"]), "s->f[13] *= 0.5f;")
        self.assertEqual(self._rewrite_part(both["retain"]),
                         "if (s->f[3] > 0.0f && s->f[3] < 1.0f) { s->f[3] = sqrtf(s->f[3]); } else { static int "
                         f'nk_kept; nk_enhancement_factor_kept(&nk_kept, "RETAIN", 0x{self.INTEGRATE + 0x1C:08x}u, '
                         "s->f[3]); }")
        self.assertIn("s->f[3] = sr_fpu_sub_s(_a,_b,s->fcr31); } sr_end(s, 0u, 0); if (s->f[3] > 0.0f", both["retain"])
        self.assertIn("uint32_t _nk_bits = nk_enh_store(0u, s->fi[0]);", text)
        self.assertIn(f"0x{_f32_bits_of(self.BOOST * kappa):08x}u", text)
        # A value_reference site is checked and emits nothing: its instruction has one form.
        world = [line for line in text.splitlines() if f"sr_begin(s, 0x{self.WORLD + 4:08x}u" in line]
        self.assertEqual(len(world), 1)
        self.assertNotIn("nk_enhancement_on()", world[0])
        # A site that requires a store site runs stock until that site has applied (section 9.6).
        held = self._statements(waits, True)
        self.assertIn("if (nk_enhancement_on() && nk_enh_ready(1u)) { static int nk_kept;", held["call"])
        self.assertIn("int _nk_map = nk_enhancement_on() && nk_enh_ready(2u);\n", held["launch"])
        self.assertIn("if (nk_enh_ready(0u)) { sr_begin(s, 0x00001318u", held["compare"])
        # Under the LLE CPU each access is checked first, as a guest load or store at the site is.
        guarded = self._statements(lle, True)
        site = self.STATE + self.CALL_AT
        self.assertIn(f"uint32_t _nk_time_ea = s->r[16] + {self.TIME_AT}u; if (sr_cpu_guard_access(s, _nk_time_ea, "
                      f"4u, 0, 0x{site:08x}u, 0u, 0u)) return; uint32_t _nk_time = MEM_R32(_nk_time_ea);",
                      guarded["call"])
        site = self.LAUNCH + self.LAUNCH_AT
        self.assertIn(f"uint32_t _nk_b_ea = _nk_a0 + {x}u; if (sr_cpu_guard_access(s, _nk_b_ea, 4u, 0, "
                      f"0x{site:08x}u, 0u, 0u)) return; uint32_t _nk_b = MEM_R32(_nk_b_ea);", guarded["launch"])
        self.assertIn(f"if (sr_cpu_guard_access(s, _nk_v1_ea, 4u, 1, 0x{site:08x}u, 0u, 0u)) return; "
                      f"MEM_W32_PC(_nk_v1_ea, _nk_v1, 0x{site:08x}u);", guarded["launch"])

    def test_a_reference_the_executable_cannot_supply_refuses_the_build(self) -> None:
        """Codegen reads the referenced values from the executable's read-only data: stock values
        the rule does not cover, or values in data the title can write, refuse the package, and
        nothing is written."""
        cases: tuple[tuple[str, dict[str, Any], str], ...] = (
            ("stiff", {"data": self._data(damping=16.0)}, "is outside (0, 1)"),
            ("still", {"data": self._data(step=0.0)}, "is not a positive float32"),
            ("writable", {"data_flags": 3}, "is not read-only initialised data of the executable: a value the "
                                            "title can change while it runs cannot be pinned"),
        )
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            for name, elf_kwargs, why in cases:
                with self.subTest(name):
                    elf_path = tmp / f"{name}.elf"
                    elf = self._elf(elf_path, **elf_kwargs)
                    pkg = self._package(self._words(), hashlib.sha256(elf).hexdigest())
                    rc, output, text = self._codegen(tmp, name, elf_path, pkg)
                    self.assertNotEqual(rc, 0)
                    self.assertIn("enhancement package refused (fail-closed)", output)
                    self.assertIn(why, output)
                    self.assertEqual(text, "")

    def test_phase_follows_the_states_float32_time_for_hours(self) -> None:
        """The phase is the parity of the number of updates since the state's time was zero, read
        off the time by matching it exactly against the state's own float32 sum.

        A rounded quotient t / (h/2) leaves the half-step grid after about 87 s of state time and
        returns the wrong parity after about 126 s. The exact rows have no such horizon: checked
        here update by update for several steps over more than an hour of state time each, and at
        the two updates where the quotient failed."""
        steps = (self.STEP, _f32_round(0.033), _f32_round(1001 / 30000), 0.03125, _f32_round(1 / 60))
        updates = 250_000                                           # 69 minutes at 60 updates a second
        for step in steps:
            half = step / 2
            runs = enhancement_package.phase_runs(half)
            with self.subTest(step=step):
                # The rows are the float32 sum, value for value.
                t, row = 0.0, 0
                for n in range(updates):
                    while n > runs[row][0] + runs[row][1]:
                        row += 1
                    n0, _count, a0, _a1, delta = runs[row]
                    if a0 + (n - n0) * delta != t:
                        self.fail(f"update {n}: the rows give {a0 + (n - n0) * delta!r}, the sum is {t!r}")
                    if (n % 97 == 0 or n in (5239, 7580)) and enhancement_package.phase_of_time(t, half) != n & 1:
                        self.fail(f"update {n}: wrong phase at {t!r}")
                    t = _f32_round(t + half)
                # They cover the life of a state: to one update before the sum stops advancing. The
                # value it then repeats is on no row, so a stalled state is reported, not guessed at.
                last = runs[-1]
                stall = _f32_round(last[3] + half)
                self.assertEqual(_f32_round(stall + half), stall)
                self.assertEqual(enhancement_package.phase_of_time(last[3], half), (last[0] + last[1]) & 1)
                self.assertIsNone(enhancement_package.phase_of_time(stall, half))
                self.assertGreater(last[0] + last[1], 10_000_000)
                self.assertLess(len(runs), 64)
                self.assertEqual([r[0] for r in runs[1:]], [r[0] + r[1] for r in runs[:-1]])
        # At the updates where a rounded quotient goes wrong, the quotient really is off the grid.
        half = self.STEP / 2
        t = 0.0
        for _ in range(7580):
            t = _f32_round(t + half)
        self.assertGreater(abs(t / half - 7580), 0.5)
        self.assertEqual(enhancement_package.phase_of_time(t, half), 0)
        # A time the state does not reach is no phase: one float32 step beside a reachable time,
        # a time between two updates, a negative time, a time past the end, and what is no number.
        reachable = _f32_round(_f32_round(half + half) + half)
        beside = _f32_of(_f32_bits_of(reachable) + 1)
        for time in (beside, _f32_round(2.5 * half), -half, 1.0e30, float("inf"), float("-inf"), float("nan")):
            self.assertIsNone(enhancement_package.phase_of_time(time, half), time)
        self.assertEqual(enhancement_package.phase_of_time(reachable, half), 1)
        self.assertEqual(enhancement_package.phase_of_time(-0.0, half), 0)
        # A step so large that the second sum overflows has the two times it reaches, and no more.
        huge = _f32_round(3.0e38)
        self.assertEqual(enhancement_package.phase_runs(huge), ((0, 1, 0.0, huge, huge),))
        self.assertEqual(enhancement_package.phase_of_time(huge, huge), 1)
        self.assertIsNone(enhancement_package.phase_of_time(float("inf"), huge))
        for bad in (0.0, -half, float("inf"), float("nan"), 1.0e-46, half * (1 + 2.0 ** -30)):
            with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, "is not a positive float32"):
                enhancement_package.phase_runs(bad)

    #: Guest addresses of the harness's state and body, inside its one page of memory.
    STATE_AT, BODY_AT = 0x4100, 0x4200
    SIZE = 0.75

    def _phase_times(self) -> list[float]:
        """Times a state may read: its float32 sum over 0 to 120 updates and at the two updates
        where a rounded quotient fails, then times it does not reach."""
        half = self.STEP / 2
        times, t = [], 0.0
        for n in range(7581):
            if n <= 120 or n in (5239, 7580):
                times.append(t)
            t = _f32_round(t + half)
        times += [_f32_of(_f32_bits_of(times[3]) + 1), _f32_round(2.5 * half), -half, 1.0e30, float("inf"),
                  float("nan"), -0.0]
        # The last time on the lookup's last row, and the next: the value the sum then repeats on
        # every update, which is no update's time.
        last = enhancement_package.phase_runs(half)[-1][3]
        times += [last, _f32_round(last + half)]
        return times

    def _harness(self, tmp: Path, name: str, statements: dict[str, str], runtime: str,
                 velocities: list[tuple[float, float, float, float]]) -> tuple[list[str], list[str]]:
        """Compile the generated statements with the generated run-time code and run them three
        ways: toggle off, toggle on with a required site not yet applied, and toggle on."""
        callee_match = re.search(r"(\w+)\(s\); SR_TAIL_DRAIN", statements["call"])
        assert callee_match is not None, statements["call"]
        times = self._phase_times()
        surge = codegen.vreg_indices(self.SURGE_VREG, 1)[0]
        long_run = (
            "    {   /* The state's own sum, update by update, for hours of state time. */\n"
            "        volatile float t = 0.0f; float d = (float)nk_enh_phase_tables[0].step;\n"
            "        int reported = 0; unsigned wrong = 0u;\n"
            "        for (uint32_t n = 0; n < 2000000u; n++) {\n"
            "            float now = t; uint32_t bits; memcpy(&bits, &now, 4);\n"
            "            if (nk_enhancement_phase_odd(&reported, \"LONG\", 0u, bits, 0u) != (int)(n & 1u)) wrong++;\n"
            "            t = now + d;\n"
            "        }\n"
            "        printf(\"long wrong %u reported %d\\n\", wrong, reported);\n"
            "    }\n") if "nk_enh_phase_tables" in runtime else ""
        harness = tmp / f"{name}.c"
        harness.write_text(
            "#include \"recomp.h\"\n#include <math.h>\n"
            "/* The real CpuState and trace hooks. The return boundary is stubbed, and guest memory is one\n"
            " * page of this harness, reached through the accessors the statements use. The callee counts\n"
            " * its calls, adds an impulse to the velocity of the body in $a0 and clobbers $a0, as a real\n"
            " * callee may. */\n"
            "#undef SR_TAIL_DRAIN\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
            "static int leave;\n"
            "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
            "#define sr_aot_return_boundary_end(s) (leave)\n"
            "static uint32_t page[1024];\n"
            "#undef MEM_R32\n#undef MEM_W32_PC\n"
            "#define MEM_R32(a) (page[((a) - 0x4000u) / 4u])\n"
            "#define MEM_W32_PC(a, v, pc) (page[((a) - 0x4000u) / 4u] = (v))\n"
            "static unsigned guards;\n"
            "#define sr_cpu_guard_access(s, ea, width, store, pc, branch, slot) (guards++, 0)\n"
            "static int on, ready;\n"
            "int nk_enhancement_on(void) { return on; }\n"
            "int nk_enh_ready(unsigned site) { (void)site; return ready; }\n"
            "void sr_at_process_end(void (*report)(void)) { (void)report; }\n"
            f"{runtime}\n"
            "static int calls; static float impulse[2];\n"
            f"static void {callee_match.group(1)}(CpuState *s) {{\n"
            "    calls++;\n"
            "    for (int i = 0; i < 2; i++) {\n"
            f"        float v; uint32_t at = (s->r[4] + {self.VELOCITY_AT}u + 4u * (uint32_t)i - 0x4000u) / 4u;\n"
            "        memcpy(&v, &page[at], 4); v += impulse[i]; memcpy(&page[at], &v, 4);\n"
            "    }\n"
            "    s->r[4] = 0x4300u;\n"
            "}\n"
            + "".join(f"static void {site}(CpuState *s) {{\n{statements[site]}\n}}\n"
                      for site in ("compare", "magnitude", "surge", "call", "launch"))
            + f"static const uint32_t times[] = {{{', '.join(f'0x{_f32_bits_of(t):08x}u' for t in times)}}};\n"
            "static const float velocities[][4] = {"
            + ", ".join("{" + ", ".join(f"{v!r}f" for v in row) + "}" for row in velocities) + "};\n"
            "static const char *const modes[] = {\"off\", \"held\", \"on\"};\n"
            "int main(void) {\n"
            "    CpuState st; CpuState *s = &st; memset(&st, 0, sizeof st);\n"
            f"    const uint32_t state = 0x{self.STATE_AT:x}u, body = 0x{self.BODY_AT:x}u;\n"
            f"    const uint32_t at = (body + {self.VELOCITY_AT}u - 0x4000u) / 4u;\n"
            f"    const float size = {self.SIZE!r}f;\n"
            "    for (int mode = 0; mode < 3; mode++) {\n"
            "        on = mode > 0; ready = mode > 1;\n"
            "        for (unsigned i = 0; i < sizeof times / sizeof times[0]; i++) {\n"
            "            s->r[16] = state; s->r[17] = state;\n"
            f"            page[(state + {self.TIME_AT}u - 0x4000u) / 4u] = times[i];\n"
            f"            memcpy(&page[(state + {self.SIZE_AT}u - 0x4000u) / 4u], &size, 4);\n"
            "            s->fi[6] = times[i]; s->f[8] = 0.0f; s->fpcond = 2u; s->fcr31 = 0x01000000u;\n"
            f"            s->f[12] = -1.0f; s->v[{surge}] = -1.0f; impulse[0] = 0.0f; impulse[1] = 0.0f;\n"
            "            compare(s); magnitude(s); surge(s);\n"
            "            calls = 0; s->r[4] = s->r[16]; call(s);\n"
            "            printf(\"%s time %u flag %u fcr31 %08x size %08x surge %08x calls %d\\n\", modes[mode], i,\n"
            f"                   (unsigned)s->fpcond, (unsigned)s->fcr31, (unsigned)s->fi[12], (unsigned)s->vi[{surge}],\n"
            "                   calls);\n"
            "        }\n"
            "        for (unsigned i = 0; i < sizeof velocities / sizeof velocities[0]; i++) {\n"
            "            page[at - 1u] = 0x11111111u; page[at + 2u] = 0x22222222u;\n"
            "            memcpy(&page[at], &velocities[i][0], 4); memcpy(&page[at + 1u], &velocities[i][1], 4);\n"
            "            impulse[0] = velocities[i][2]; impulse[1] = velocities[i][3];\n"
            "            calls = 0; s->r[4] = body; launch(s);\n"
            "            printf(\"%s launch %u calls %d %08x %08x guard %08x %08x a0 %x\\n\", modes[mode], i, calls,\n"
            "                   (unsigned)page[at], (unsigned)page[at + 1u], (unsigned)page[at - 1u],\n"
            "                   (unsigned)page[at + 2u], (unsigned)s->r[4]);\n"
            "        }\n"
            "    }\n"
            "    {   /* What a zero size adds: v + 0*k, for the forced +0.0. */\n"
            "        static const uint32_t probes[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x3f800000u,\n"
            "                                          0xbf800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u};\n"
            "        volatile float zero = 0.0f, mass = 0.75f;\n"
            "        for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {\n"
            "            float v, sum; uint32_t bits; memcpy(&v, &probes[i], 4);\n"
            "            sum = v + zero * mass; memcpy(&bits, &sum, 4);\n"
            "            printf(\"zero %08x %08x\\n\", (unsigned)probes[i], (unsigned)bits);\n"
            "        }\n"
            "    }\n"
            "    printf(\"guards %u\\n\", guards);\n"
            f"{long_run}"
            "    /* Two calls that do not come back to the statement: the run's last lines name the site. */\n"
            "    on = 1; ready = 1; leave = 1; impulse[0] = 0.0f; impulse[1] = 1.0f;\n"
            "    s->r[4] = body; launch(s); s->r[4] = body; launch(s);\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        exe = tmp / f"{name}.exe"
        assert GCC is not None, "skipUnless(GCC) runs this test only with a compiler"
        built = subprocess.run([GCC, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                "-Wno-misleading-indentation", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                                f"-I{ROOT / 'src' / 'rt'}", "-o", str(exe), str(harness), "-lm"],
                               capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
        return run.stdout.splitlines(), run.stderr.splitlines()

    @staticmethod
    def _mode(lines: list[str], mode: str) -> list[str]:
        return [line[len(mode) + 1:] for line in lines if line.startswith(mode + " ")]

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_generated_statements_match_the_rules_when_run(self) -> None:
        """Compile the generated statements with the generated run-time code and run them.

        Toggle on: after an even number of updates of the state's time the gated call is made, the
        compare keeps its result and the gated loads read their stock value; after an odd number
        the call is skipped, the compare reads as declared and the loads read +0.0; at a time the
        state does not reach every site runs stock and reports once. The generated lookup follows
        the state's float32 sum for two million updates. The launch statement maps the gravity
        axis to kappa*v + q*g and scales only the change on the other axis, bit for bit, on the
        body the site passed. A build for the other flag value and the other axis, a build for
        the LLE CPU and a build whose sites wait for a store site do the same. Toggle off, every
        build prints what the Authentic build prints."""
        kappa, q = self._factors()
        offset = q * self.GRAVITY
        times = self._phase_times()
        # (x before, y before, x impulse, y impulse): from rest, with a carried velocity on either
        # axis, with an impulse on both, and with a call that changes nothing.
        velocities = [(0.0, 0.0, 0.0, 1.5), (0.0, 0.0, 0.25, -0.75), (2.0, 0.0, 0.0, 1.5), (-1.25, 0.5, 0.5, 1.0),
                      (0.001, -0.002, 0.0, 0.0), (3.0, 0.0, -3.0, 123.456), (-0.0, 0.0, 0.0, 2.5)]
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            authentic = self._statements(self._build(tmp, "authentic", with_package=False), False)
            stock_out, stock_err = self._harness(tmp, "authentic", authentic, "", velocities)
            requires = {"requires": ["BOOST"]}
            builds: dict[str, tuple[dict[str, Any], list[str]]] = {
                "plain": ({}, []),
                "flipped": ({"COMPARE": {"odd_phase_condition": True}, "LAUNCH": {"gravity_axis": 0}}, []),
                "lle": ({}, ["--lle-cpu"]),
                "waiting": ({name: requires for name in ("COMPARE", "CALL", "MAGNITUDE", "SURGE", "LAUNCH")}, []),
            }
            runs: dict[str, tuple[list[str], list[str]]] = {}
            for name, (changes, extra) in builds.items():
                text = self._build(tmp, name, with_package=True, extra=extra, **changes)
                runs[name] = self._harness(tmp, name, self._statements(text, True), self._runtime(text), velocities)

        half = self.STEP / 2
        phases = [enhancement_package.phase_of_time(t, half) for t in times]
        self.assertEqual(phases[:121], [n & 1 for n in range(121)])
        last_row = enhancement_package.phase_runs(half)[-1]
        self.assertEqual(phases[121:], [1, 0, None, None, None, None, None, None, 0,
                                        (last_row[0] + last_row[1]) & 1, None])
        size = _f32_bits_of(self.SIZE)

        def time_lines(forced_flag: int, gated: bool) -> list[str]:
            lines = []
            for i, (t, phase) in enumerate(zip(times, phases, strict=True)):
                flag = 1 if 0.0 <= t else 0                    # c.le.s 0.0, t: false for what is no number
                value, calls = size, 1
                if gated and phase == 1:
                    flag, value, calls = forced_flag, 0, 0
                lines.append(f"time {i} flag {flag} fcr31 {0x01000000 | flag << 23:08x} size {value:08x} "
                             f"surge {value:08x} calls {calls}")
            return lines

        def launch_lines(axis: int | None) -> list[str]:
            lines = []
            for i, (bx, by, ix, iy) in enumerate(velocities):
                before = [_f32_round(bx), _f32_round(by)]
                left = [_f32_round(before[0] + _f32_round(ix)), _f32_round(before[1] + _f32_round(iy))]
                want = [_f32_bits_of(v) for v in left]
                if axis is not None:
                    other = 1 - axis
                    want[axis] = _f32_bits_of(math.fma(left[axis], kappa, offset))
                    if _f32_bits_of(left[other]) != _f32_bits_of(before[other]):
                        want[other] = _f32_bits_of(math.fma(left[other] - before[other], kappa, before[other]))
                lines.append(f"launch {i} calls 1 {want[0]:08x} {want[1]:08x} guard 11111111 22222222 a0 4300")
            return lines

        # Section 11.2, item 1: toggle off is the Authentic statements' output, byte for byte.
        off = self._mode(stock_out, "off")
        self.assertEqual(off, time_lines(0, False) + launch_lines(None))
        self.assertEqual(self._mode(stock_out, "held"), off)
        self.assertEqual(self._mode(stock_out, "on"), off)
        self.assertEqual(stock_err, [])
        first = times[phases.index(None)]
        site_at = self._site_addresses()
        kept = [f"ENHANCEMENT_PHASE_KEPT: site {site} at 0x{site_at[site.lower()]:08x} read the time {first:.9g}, "
                f"which its state does not reach by adding {half:.9g} from zero; it runs its stock instruction"
                for site in ("COMPARE", "MAGNITUDE", "SURGE", "CALL")]
        not_mapped = (f"ENHANCEMENT_LAUNCH_NOT_MAPPED: site LAUNCH at 0x{site_at['launch']:08x}: 2 of "
                      f"{2 * len(velocities) + 2} call(s) were not followed by the launch map; the velocity they "
                      f"left kept its stock value")
        for name, (out, err) in runs.items():
            with self.subTest(name):
                self.assertEqual(self._mode(out, "off"), off)
                flag, axis = (1, 0) if name == "flipped" else (0, 1)
                enhanced = time_lines(flag, True) + launch_lines(axis)
                self.assertEqual(self._mode(out, "on"), enhanced)
                # A site that waits for a store site runs stock until that site has applied.
                self.assertEqual(self._mode(out, "held"), off if name == "waiting" else enhanced)
                # The generated lookup against the state's own float32 sum, 9 hours of updates.
                self.assertIn("long wrong 0 reported 0", out)
                made = (2 if name == "waiting" else 3) * len(velocities) - len(velocities) + 2
                self.assertEqual(err, kept + [not_mapped.replace(f"of {2 * len(velocities) + 2} ", f"of {made} ")])
                guards = int(next(line for line in out if line.startswith("guards ")).split()[1])
                self.assertEqual(guards > 0, name == "lle")
        # A gated load reads +0.0, and what a zero size adds to a velocity is nothing: v + 0*k is
        # v bit for bit, for every v but -0.0, which becomes +0.0 (equal in value).
        zero = [line.split()[1:] for line in runs["plain"][0] if line.startswith("zero ")]
        self.assertEqual(len(zero), 9)
        for before, after in zero:
            self.assertEqual(after, "00000000" if before == "80000000" else before)
        # The fixture's carried x velocity is never scaled: with no x impulse it keeps its bits.
        self.assertEqual(launch_lines(1)[2].split()[4], f"{_f32_bits_of(2.0):08x}")

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_kappa_scale_is_one_rounding_in_every_form(self) -> None:
        """Compile the emitted scale of each velocity_source_x_kappa form and run it.

        A loaded FPU register, a loaded VFPU register, each of the four lanes an lv.q loads and the
        copy an unchecked swc1 stores are all the value times kappa in double, rounded to single
        precision once: the bits a checked store's stock-value table holds for the same value. The
        constant scales keep their single-precision multiply."""
        kappa = self._factors()[0]
        rng = random.Random(1931)
        values = [0.0, -0.0, 1.0, -4.0, 0.125, 1.0e-3, 3.0e38, _f32_of(0x00000001), _f32_of(0x007FFFFF),
                  _f32_of(0x00800000), float("inf")]
        values += [rng.uniform(-10.0, 10.0) for _ in range(2000)]
        values += [2.0 ** rng.uniform(-149, 127) for _ in range(500)]
        values = [_f32_round(v) for v in values]
        lanes = codegen.vreg_indices(self.GRAVITY_VREG, 4)
        single = codegen.vreg_indices(self.G_VREG, 1)[0]
        forms: tuple[tuple[str, int, dict[str, Any], list[str]], ...] = (
            ("lwc1", _lwc1(3, 0x10, V0), {"kind": "scale_f_reg", "reg": 3}, ["s->f[3]"]),
            ("lv.s", _lvs(self.G_VREG, 0x10, V0), {"kind": "scale_v_reg", "vreg": self.G_VREG}, [f"s->v[{single}]"]),
            ("lv.q", _lvq(self.GRAVITY_VREG, 0x40, A0), {"kind": "scale_vq_reg", "vreg": self.GRAVITY_VREG},
             [f"s->v[{v}]" for v in lanes]),
            ("swc1", _swc1(4, 0x10, A1), {"kind": "scale_f_store", "reg": 4}, ["_nk_f"]),
        )
        scale = re.compile(r"(?:s->[fv]\[\d+\] = |float _nk_f = )\(float\)\(\(double\)s->[fv]\[\d+\] \* [^;]+\);")
        blocks: list[str] = []
        saved = codegen.ENHANCEMENT_SITES
        try:
            for name, word, rewrite, cells in forms:
                codegen.ENHANCEMENT_SITES = {0x1300: {"id": "K", "rewrite": dict(rewrite, factor=kappa)}}
                codegen.ENHANCEMENT_APPLIED.clear()
                enhanced = codegen.normal_line(0x1300, word, lle_cpu=False).rsplit("else", 1)[0]
                statements = scale.findall(enhanced)
                self.assertEqual(len(statements), len(cells), f"{name}: {enhanced}")
                self.assertTrue(all(f"* {kappa!r});" in stmt for stmt in statements), statements)
                prints = " ".join(f"show({cell});" for cell in cells)
                blocks.append(f"        {{ {' '.join(statements)} {prints} }}")
                # The same forms under a constant scale are the single-precision multiply they were.
                codegen.ENHANCEMENT_SITES = {0x1300: {"id": "K", "rewrite": dict(rewrite, factor=0.5)}}
                codegen.ENHANCEMENT_APPLIED.clear()
                halved = codegen.normal_line(0x1300, word, lle_cpu=False).rsplit("else", 1)[0]
                self.assertEqual(scale.findall(halved), [])
                self.assertEqual(halved.count("0.5f"), len(cells), halved)
        finally:
            codegen.ENHANCEMENT_SITES = saved
            codegen.ENHANCEMENT_APPLIED.clear()
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            (tmp / "v.txt").write_text("\n".join(struct.pack("<f", v).hex() for v in values), encoding="utf-8")
            harness = tmp / "scale.c"
            harness.write_text(
                "#include <math.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
                "typedef struct { float f[32]; float v[128]; } S;\n"
                "static void show(float x) { uint32_t b; memcpy(&b, &x, 4); printf(\"%08x\\n\", (unsigned)b); }\n"
                "int main(int argc, char **argv) {\n"
                "    FILE *in = fopen(argv[1], \"r\"); char hex[16]; S st, *s = &st;\n"
                "    if (argc < 2 || !in) return 1;\n"
                "    while (fscanf(in, \"%15s\", hex) == 1) {\n"
                "        uint8_t b[4]; float x;\n"
                "        for (int i = 0; i < 4; i++) sscanf(hex + 2 * i, \"%2hhx\", &b[i]);\n"
                "        memcpy(&x, b, 4);\n"
                "        for (int i = 0; i < 32; i++) s->f[i] = x;\n"
                "        for (int i = 0; i < 128; i++) s->v[i] = x;\n"
                + "\n".join(blocks) + "\n"
                "    }\n"
                "    fclose(in); return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            exe = tmp / "scale.exe"
            subprocess.run([str(GCC), "-std=c11", "-O2", "-Wall", "-Werror", "-o", str(exe), str(harness), "-lm"],
                           check=True, capture_output=True, text=True)
            out = subprocess.run([str(exe), str(tmp / "v.txt")], check=True, capture_output=True,
                                 text=True).stdout.split()
        per_value = sum(len(cells) for _name, _word, _rewrite, cells in forms)
        self.assertEqual(len(out), per_value * len(values))
        finite = list({_f32_bits_of(v): v for v in values if math.isfinite(v)}.values())
        table = dict(plan_site_rewrite({"id": "K", "transform": "velocity_source_x_kappa", "stock_values": finite},
                                       _swc1(4, 0x10, A1), {"kappa": kappa})["stock_values"])
        for i, value in enumerate(values):
            want = _f32_bits_of(value * kappa)
            self.assertEqual({int(got, 16) for got in out[per_value * i:per_value * (i + 1)]}, {want}, f"{value!r}")
            if math.isfinite(value):
                self.assertEqual(table[_f32_bits_of(value)], want)

    #: The made-up flight the arc tests run: a launch from rest, then timed impulses.
    LAUNCH_X, LAUNCH_Y, THRESHOLD, WINDOW = 0.5, 1.5, 0.17, 0.11

    def _arc32(self, updates: int, *, package: bool, boost: float,
               launch_offset: bool = True) -> list[tuple[float, float]]:
        """The fixture's guest in float32, operation by operation as the guest's FPU and the
        emitted statements round: positions (x, y) after each update. ``package`` runs the 60 Hz
        forms: the halved step, gravity times kappa, the square-root retention, the launch map
        and the phase gate on the compare and the impulse."""
        f = _f32_round
        kappa, q = self._factors()
        half = self.STEP / 2
        dt = half if package else self.STEP
        m = f(1.0 - f(self.DAMPING * self.DAMPING_STEP))
        retention = f(math.sqrt(m)) if package else m
        gravity = [0.0, f(self.GRAVITY * kappa) if package else self.GRAVITY]
        threshold, window = f(self.THRESHOLD), f(self.WINDOW)
        v, x = [0.0, 0.0], [0.0, 0.0]
        time, remaining, opened = 0.0, 0.0, False
        positions = []
        for update in range(updates):
            if update == 0:
                v = [f(v[0] + f(self.LAUNCH_X)), f(v[1] + f(self.LAUNCH_Y))]
                if package:
                    v = [f(math.fma(v[0] - 0.0, kappa, 0.0)),
                         f(math.fma(v[1], kappa, q * self.GRAVITY if launch_offset else 0.0))]
            odd = package and enhancement_package.phase_of_time(time, half) == 1
            if threshold <= time and not odd and not opened:
                opened, remaining = True, window
            if remaining > 0.0:
                remaining = f(remaining - dt)
                if not odd:
                    v[1] = f(v[1] + boost)
            for i in (0, 1):
                v[i] = f(f(v[i] + f(gravity[i] * dt)) * retention)
                x[i] = f(x[i] + f(v[i] * dt))
            time = f(time + dt)
            positions.append((x[0], x[1]))
        return positions

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_emitted_statements_follow_the_stock_arc_in_float32(self) -> None:
        """Run the emitted statements as a guest runs them, in float32, through a whole flight.

        A small guest in C launches a body from rest, adds timed impulses and integrates it, using
        the statements codegen emitted at the fixture's sites wherever a site is. Toggle off it
        takes stock steps; toggle on, twice as many half steps. The positions it prints are those
        of a float32 model of the rule, bit for bit, so the bound that model meets
        (test_the_float32_model_stays_within_a_millionth_of_the_stock_arc) is what the emitted
        code gives: float32 rounds each operation, so the pair ends agree with stock's in most
        bits, not all. The impulse gated as a call and gated as a zero load give the same arc,
        bit for bit."""
        stock_updates = 90
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            arcs: dict[str, list[str]] = {}
            for name, changes in (("gated_call", {"MAGNITUDE": None, "SURGE": None}),
                                  ("gated_load", {"CALL": None, "SURGE": None})):
                text = self._build(tmp, name, with_package=True, **changes)
                statements = self._statements(text, True)
                callee_match = re.search(r"(\w+)\(s\); SR_TAIL_DRAIN", statements["call"])
                assert callee_match is not None, statements["call"]
                lane = codegen.vreg_indices(self.GRAVITY_VREG, 4)
                boost_bits, boost_scaled_bits = self._resolve()["BOOST"]["stock_values"][0]
                velocity = f"(body + {self.VELOCITY_AT}u - 0x4000u) / 4u"
                harness = tmp / f"{name}.c"
                harness.write_text(
                    "#include \"recomp.h\"\n#include <math.h>\n"
                    "/* The fixture's guest, with the generated statements at its sites. Guest memory is one\n"
                    " * page of this harness; the return boundary is stubbed. */\n"
                    "#undef SR_TAIL_DRAIN\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
                    "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
                    "#define sr_aot_return_boundary_end(s) 0\n"
                    "static uint32_t page[1024];\n"
                    "#undef MEM_R32\n#undef MEM_W32_PC\n"
                    "#define MEM_R32(a) (page[((a) - 0x4000u) / 4u])\n"
                    "#define MEM_W32_PC(a, v, pc) (page[((a) - 0x4000u) / 4u] = (v))\n"
                    "static int on, kept;\n"
                    "int nk_enhancement_on(void) { return on; }\n"
                    "void sr_at_process_end(void (*report)(void)) { (void)report; }\n"
                    "void nk_enhancement_factor_kept(int *reported, const char *site, uint32_t pc, float value) {\n"
                    "    (void)reported; (void)site; (void)pc; (void)value; kept++;\n"
                    "}\n"
                    f"{self._runtime(text)}\n"
                    "/* One object is the state and the body: STATE passes its own pointer to IMPULSE. */\n"
                    f"static const uint32_t state = 0x{self.STATE_AT:x}u, body = 0x{self.STATE_AT:x}u;\n"
                    "/* IMPULSE: adds (f14, f12) to the velocity of the body in $a0, and clobbers $a0. */\n"
                    f"static void {callee_match.group(1)}(CpuState *s) {{\n"
                    f"    float v; uint32_t at = (s->r[4] + {self.VELOCITY_AT}u - 0x4000u) / 4u;\n"
                    "    memcpy(&v, &page[at + 1u], 4); v = v + s->f[12]; memcpy(&page[at + 1u], &v, 4);\n"
                    "    memcpy(&v, &page[at], 4); v = v + s->f[14]; memcpy(&page[at], &v, 4);\n"
                    "    s->r[4] = 0x4300u;\n"
                    "}\n"
                    + "".join(f"static void {site}(CpuState *s) {{\n{statements[site]}\n}}\n"
                              for site in ("compare", "magnitude", "call", "launch"))
                    + "/* INTEGRATE, then the move: v = (v + g*dt) * m and x += v*dt, each operation in float32. */\n"
                    "static void integrate(CpuState *s, float *x) {\n"
                    f"    static const int lane[2] = {{{lane[0]}, {lane[1]}}};\n"
                    f"    const uint32_t at = {velocity};\n"
                    f"    s->f[13] = {self.STEP!r}f; if (on) {{ {self._rewrite_part(statements['step'])} }}\n"
                    f"    s->v[{lane[0]}] = 0.0f; s->v[{lane[1]}] = {self.GRAVITY!r}f; s->v[{lane[2]}] = 0.0f; "
                    f"s->v[{lane[3]}] = 0.0f;\n"
                    f"    if (on) {{ {self._rewrite_part(statements['gravity'])} }}\n"
                    f"    s->f[2] = {self.DAMPING!r}f; s->f[4] = {self.DAMPING_STEP!r}f; s->f[2] = s->f[2] * s->f[4];\n"
                    "    s->f[0] = 1.0f; s->f[3] = s->f[0] - s->f[2];\n"
                    f"    if (on) {{ {self._rewrite_part(statements['retain'])} }}\n"
                    "    for (int i = 0; i < 2; i++) {\n"
                    "        float v, step, sum, moved;\n"
                    "        memcpy(&v, &page[at + (uint32_t)i], 4);\n"
                    "        step = s->v[lane[i]] * s->f[13]; sum = v + step; v = sum * s->f[3];\n"
                    "        memcpy(&page[at + (uint32_t)i], &v, 4);\n"
                    "        moved = v * s->f[13]; x[i] = x[i] + moved;\n"
                    "    }\n"
                    "}\n"
                    "static void fly(CpuState *s, int updates, const char *tag) {\n"
                    "    float x[2] = {0.0f, 0.0f}, remaining = 0.0f;\n"
                    "    int opened = 0;\n"
                    f"    const uint32_t time_at = (state + {self.TIME_AT}u - 0x4000u) / 4u;\n"
                    f"    const uint32_t size_at = (state + {self.SIZE_AT}u - 0x4000u) / 4u;\n"
                    f"    const uint32_t at = {velocity};\n"
                    "    page[time_at] = 0u; page[at] = 0u; page[at + 1u] = 0u;\n"
                    "    for (int u = 0; u < updates; u++) {\n"
                    "        float dt, t;\n"
                    f"        s->f[13] = {self.STEP!r}f; if (on) {{ {self._rewrite_part(statements['step'])} }}\n"
                    "        dt = s->f[13];\n"
                    "        s->r[16] = state;\n"
                    "        if (u == 0) {\n"
                    f"            s->f[12] = {self.LAUNCH_Y!r}f; s->f[14] = {self.LAUNCH_X!r}f; s->r[4] = body; launch(s);\n"
                    "        }\n"
                    "        memcpy(&t, &page[time_at], 4);\n"
                    f"        s->f[6] = t; s->f[8] = {self.THRESHOLD!r}f; compare(s);\n"
                    f"        if (s->fpcond && !opened) {{ opened = 1; remaining = {self.WINDOW!r}f; }}\n"
                    "        if (remaining > 0.0f) {\n"
                    "            remaining = remaining - dt;\n"
                    "            /* The size TUNE's checked store left in the state: stock, or its 60 Hz value. */\n"
                    f"            page[size_at] = on ? 0x{boost_scaled_bits:08x}u : 0x{boost_bits:08x}u;\n"
                    "            magnitude(s);\n"
                    "            s->f[14] = 0.0f; s->r[4] = body; call(s);\n"
                    "        }\n"
                    "        integrate(s, x);\n"
                    "        t = t + dt; memcpy(&page[time_at], &t, 4);\n"
                    "        { uint32_t b[2]; memcpy(b, x, 8); printf(\"%s %08x %08x\\n\", tag, (unsigned)b[0], (unsigned)b[1]); }\n"
                    "    }\n"
                    "}\n"
                    "int main(void) {\n"
                    "    CpuState st; CpuState *s = &st; memset(&st, 0, sizeof st);\n"
                    f"    on = 0; fly(s, {stock_updates}, \"stock\");\n"
                    f"    on = 1; fly(s, {2 * stock_updates}, \"package\");\n"
                    "    printf(\"kept %d\\n\", kept);\n"
                    "    return 0;\n"
                    "}\n",
                    encoding="utf-8",
                )
                exe = tmp / f"{name}.exe"
                assert GCC is not None, "skipUnless(GCC) runs this test only with a compiler"
                built = subprocess.run([GCC, "-std=gnu11", "-ffp-contract=off", "-Wall", "-Werror",
                                        "-Wno-unused-function", "-Wno-misleading-indentation", "-Wno-unused-variable",
                                        f"-I{ROOT / 'src' / 'rt'}", "-o", str(exe), str(harness), "-lm"],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
                self.assertEqual(run.stderr, "")
                arcs[name] = run.stdout.splitlines()

        def bits(positions: list[tuple[float, float]], tag: str) -> list[str]:
            return [f"{tag} {_f32_bits_of(x):08x} {_f32_bits_of(y):08x}" for x, y in positions]

        stock = self._arc32(stock_updates, package=False, boost=self.BOOST)
        package = self._arc32(2 * stock_updates, package=True, boost=_f32_of(boost_scaled_bits))
        self.assertEqual(arcs["gated_call"], bits(stock, "stock") + bits(package, "package") + ["kept 0"])
        # A zero impulse adds nothing: the call that always runs and the call that is skipped leave
        # the same velocity, so the same arc, in every bit.
        self.assertEqual(arcs["gated_load"], arcs["gated_call"])
        # The bound itself is test_the_float32_model_stays_within_a_millionth_of_the_stock_arc's,
        # on the model these statements have just been shown to equal.

    def test_the_float32_model_stays_within_a_millionth_of_the_stock_arc(self) -> None:
        """What "exact" means for these transforms (contract section 9.2), on the float32 model of
        the rule: at the end of every pair of updates the 60 Hz position is within 1e-6 of the
        arc's travel of the stock position, on both axes. Measured here: 3.2e-7 of the height on
        the gravity axis (5 of 90 pair ends bit-identical) and 4.6e-7 of the distance on the other
        (6 of 90). It needs no compiler; the compiled test shows that the emitted statements
        compute this model bit for bit."""
        stock_updates = 90
        scaled_boost = _f32_of(self._resolve()["BOOST"]["stock_values"][0][1])
        stock = self._arc32(stock_updates, package=False, boost=self.BOOST)
        package = self._arc32(2 * stock_updates, package=True, boost=scaled_boost)
        measured = {}
        for axis, name in ((0, "x"), (1, "y")):
            travelled = [p[axis] for p in stock]
            height = max(travelled) - min(travelled)
            self.assertGreater(height, 0.25)
            worst = max(abs(package[2 * k + 1][axis] - stock[k][axis]) for k in range(stock_updates))
            same = sum(_f32_bits_of(package[2 * k + 1][axis]) == _f32_bits_of(stock[k][axis])
                       for k in range(stock_updates))
            measured[name] = (worst / height, same)
            largest = max(abs(position) for position in travelled)
            one_step = _f32_of(_f32_bits_of(largest) + 1) - largest
            with self.subTest(axis=name):
                self.assertLess(worst, 1e-6 * height, f"worst {worst / height:.2e} of the travel; {same} identical")
                self.assertGreater(worst, 0.0)      # float32 rounds: agreement in most bits, not in all
                # It is accumulated rounding: a few float32 steps of the largest position reached,
                # so a larger share of a small arc than of a large one.
                self.assertLessEqual(worst, 6 * one_step)
        # The figures the contract quotes.
        self.assertAlmostEqual(measured["y"][0], 3.2e-7, delta=0.1e-7)
        self.assertAlmostEqual(measured["x"][0], 4.6e-7, delta=0.1e-7)
        self.assertEqual((measured["y"][1], measured["x"][1]), (5, 6))
        # Without the launch offset the same float32 flight is three orders of magnitude outside it.
        plain = self._arc32(2 * stock_updates, package=True, boost=scaled_boost, launch_offset=False)
        self.assertGreater(max(abs(plain[2 * k + 1][1] - stock[k][1]) for k in range(stock_updates)), 1e-3)

    def _arc(self, updates: int, step: float, *, half_steps: bool, kappa: float = 1.0, launch_offset: float = 0.0,
             retention: float, gate_compare: bool = False, gate_call: bool = False,
             impulse_scale: float = 1.0) -> list[float]:
        """A model of the fixture's guest in double precision: positions after each of ``updates``
        updates. It shows the algebra of the rule, not what float32 code gives (_arc32 does that).

        Each update the state logic runs, then the integrator. The state's time is the f32 sum of
        the steps so far. Update 0 launches from rest. Once the time passes a threshold a window
        opens, and while it is open every update counts it down by the step and adds an impulse.
        The integrator adds gravity over the step, applies the retention, then moves by the
        velocity over the step. ``half_steps`` runs the package's form: the phase of the state's
        time gates the compare and the impulse as gate_30hz_phase does (a skipped call and a zero
        load add the same nothing)."""
        half_step = self.STEP / 2
        velocity, position, positions = 0.0, 0.0, []
        time, remaining, opened = 0.0, 0.0, False
        for update in range(updates):
            phase = enhancement_package.phase_of_time(time, half_step) if half_steps else 0
            if update == 0:
                velocity = (velocity + self.LAUNCH_Y) * (kappa if half_steps else 1.0) + launch_offset
            passed = self.THRESHOLD <= time and not (gate_compare and phase == 1)
            if passed and not opened:
                opened, remaining = True, self.WINDOW
            if remaining > 0.0:
                remaining -= step
                if not (gate_call and phase == 1):
                    velocity += self.BOOST * impulse_scale
            velocity = (velocity + self.GRAVITY * (kappa if half_steps else 1.0) * step) * retention
            position += velocity * step
            positions.append(position)
            time = _f32_round(time + _f32_round(step))
        return positions

    def test_two_updates_reproduce_one_stock_update_at_the_end_of_every_pair(self) -> None:
        """The algebra behind the transforms, in a double-precision model, on the values the
        resolver planned.

        With the gravity and the impulses scaled by kappa, the launch mapped to kappa*v + q*g, the
        retention mapped to its square root and the state's compare and impulse gated on its own
        phase, two updates of half a step reach the position of one stock update at the end of
        every pair, through the launch, the timed impulses and the fall, to the rounding of the
        model's doubles. Without any one of them they do not. What the emitted float32 statements
        give is test_emitted_statements_follow_the_stock_arc_in_float32."""
        rewrites = self._resolve()
        kappa = rewrites["GRAVITY"]["factor"]
        self.assertEqual(rewrites["LAUNCH"]["kappa"], kappa)
        self.assertEqual(rewrites["BOOST"]["factor"], kappa)
        self.assertEqual(rewrites["CALL"]["half_step"], self.STEP / 2)
        m = 1.0 - self.DAMPING * self.DAMPING_STEP
        stock_updates = 90
        stock = self._arc(stock_updates, self.STEP, half_steps=False, retention=m)
        full: dict[str, Any] = {
            "half_steps": True, "kappa": kappa, "launch_offset": rewrites["LAUNCH"]["gravity_offset"],
            "retention": _scale_value("retention_m_to_sqrt(m)", m), "gate_compare": True, "gate_call": True,
            "impulse_scale": kappa}

        def worst(**changes: Any) -> float:
            package = self._arc(2 * stock_updates, self.STEP / 2, **{**full, **changes})
            return max(abs(package[2 * k + 1] - stock[k]) for k in range(stock_updates))

        height = max(stock) - min(stock)
        self.assertGreater(height, 1.0)
        self.assertLess(worst(), 1e-12 * height)
        # Each part is needed: without it the arc leaves the stock one by far more than rounding.
        for name, changes, least in (
            ("no launch offset", {"launch_offset": 0.0}, 1e-3),
            ("no kappa on the impulses", {"impulse_scale": 1.0}, 1e-5),
            ("the plain half-step retention", {"retention": 1.0 - self.DAMPING * self.DAMPING_STEP / 2}, 1e-6),
            ("the compare not gated", {"gate_compare": False}, 1e-3),
            ("the impulse not gated", {"gate_call": False}, 1e-2),
        ):
            with self.subTest(name):
                self.assertGreater(worst(**changes), least * height)
        # The gates put the impulses on the stock instants: the window opens on an odd update
        # without the compare's gate, and one update early is one impulse short.
        eleventh = 0.0
        for _ in range(11):
            eleventh = _f32_round(eleventh + self.STEP / 2)
        self.assertGreaterEqual(eleventh, self.THRESHOLD)
        self.assertEqual(enhancement_package.phase_of_time(eleventh, self.STEP / 2), 1)

    def _plain_flight(self, form: str, updates: int, *, half_steps: bool, launch: float = 1.5,
                      impulse_at: int | None = None, clamp: float | None = None,
                      ground: bool = False) -> list[float]:
        """Positions of a body launched from rest under one integrator form, in double precision.

        ``force_damp_move`` is the form the rule is derived for: v = (v + g*h) * m, then x += v*h.
        ``damp_force_move`` damps first: v = v*m + g*h. ``move_force_damp`` moves by the old
        velocity first (explicit Euler). ``half_steps`` runs the package's two updates per step
        with the rule's constants. ``impulse_at`` adds four impulses, one per stock step, in the
        first (0) or the second (1) update of their pairs. ``clamp`` limits the fall speed after
        each update. ``ground`` stops at the first position at or below the start."""
        kappa, q = self._factors()
        m = 1.0 - self.DAMPING * self.DAMPING_STEP
        if half_steps:
            v, gh, r, dt, per = kappa * launch + q * self.GRAVITY, kappa * self.GRAVITY * self.STEP / 2, \
                math.sqrt(m), self.STEP / 2, 2
        else:
            v, gh, r, dt, per = launch, self.GRAVITY * self.STEP, m, self.STEP, 1
        x, positions = 0.0, []
        for update in range(per * updates):
            k, part = divmod(update, per)
            if impulse_at is not None and 5 <= k < 9 and part == (impulse_at if half_steps else 0):
                v += self.BOOST * (kappa if half_steps else 1.0)
            if form == "force_damp_move":
                v = (v + gh) * r
                x += v * dt
            elif form == "damp_force_move":
                v = v * r + gh
                x += v * dt
            else:
                x += v * dt
                v = (v + gh) * r
            if clamp is not None and v < -clamp:
                v = -clamp
            positions.append(x)
            if ground and x <= 0.0:
                break
        return positions

    def test_the_rule_is_exact_for_one_integrator_and_first_order_elsewhere(self) -> None:
        """The conditions the contract states (section 9.2), in a double-precision model.

        The constants are exact when each update applies the force, then the damping, then moves
        by the new velocity, with every impulse in the first update of its pair and nothing
        clamping in between. Under another order, an impulse in the second update or a clamp, the
        same constants are a first-order approximation: the arc is off by a fraction of the
        damping per update, here 1.6 %. With a ground, the 60 Hz run can land on the odd update of
        a pair, half a stock step before stock's update, and its samples between pair ends can
        rise above stock's highest."""
        n = 90

        def worst(form: str, **kwargs: Any) -> float:
            stock = self._plain_flight(form, n, half_steps=False, **kwargs)
            package = self._plain_flight(form, n, half_steps=True, **kwargs)
            height = max(stock) - min(stock)
            return max(abs(package[2 * k + 1] - stock[k]) for k in range(n)) / height

        self.assertLess(worst("force_damp_move"), 1e-12)
        self.assertLess(worst("force_damp_move", impulse_at=0), 1e-12)
        for name, error, low, high in (
            ("damping before the force", worst("damp_force_move"), 5e-3, 2e-2),
            ("moving by the old velocity", worst("move_force_damp"), 5e-3, 3e-2),
            ("an impulse in the second update of its pair", worst("force_damp_move", impulse_at=1), 3e-4, 3e-3),
            ("a clamp on the fall speed", worst("force_damp_move", clamp=1.0), 1e-2, 1e-1),
        ):
            with self.subTest(name):
                self.assertGreater(error, low)
                self.assertLess(error, high)
        # A ground: stock's landing update is 2L in half updates; the package lands there or one
        # half update earlier, and its highest sample is at or just above stock's.
        early = 0
        for launch in (1.5, 1.4, 1.3, 1.2, 1.1, 1.0, 0.9, 0.8):
            stock = self._plain_flight("force_damp_move", 400, half_steps=False, launch=launch, ground=True)
            package = self._plain_flight("force_damp_move", 400, half_steps=True, launch=launch, ground=True)
            self.assertIn(len(package), (2 * len(stock), 2 * len(stock) - 1), launch)
            early += len(package) == 2 * len(stock) - 1
            self.assertGreaterEqual(max(package), max(stock))
            self.assertLess(max(package), 1.003 * max(stock))
        self.assertGreater(early, 0)
        self.assertLess(early, 8)

    def test_published_form_keeps_rules_and_site_names_only(self) -> None:
        """Published, the package holds the rules, the site references and keyed digests: no
        factor, no stock value. Resolved against the executable it plans the same rewrites."""
        from analyze import Elf

        with tempfile.TemporaryDirectory() as tmp_dir:
            elf_bytes = self._elf(Path(tmp_dir) / "two_update.elf")
        sha256 = hashlib.sha256(elf_bytes).hexdigest()
        private = self._package(self._words(), sha256)
        elf = Elf(elf_bytes, base=0)
        read_words, read_data = enhancement_package.elf_readers(elf)
        read_const = enhancement_package.elf_constant_reader(elf)
        notes: list[str] = []
        published = enhancement_package.publish_package(
            private, self._symbols(), executable=elf_bytes, disc_id="ULUS99999", disc_version="1.01",
            read_words_fn=read_words, read_data_fn=read_data, read_const_fn=read_const, notes=notes)
        self.assertEqual(notes, [])
        self.assertEqual(enhancement_package.published_package_findings(published), [])
        self.assertIn("launch_velocity_write", published["capabilities"])
        by_id = {site["id"]: site for site in published["sites"]}
        self.assertNotIn("stock_values", by_id["BOOST"])
        key = enhancement_package.published_digest_key(elf_bytes)
        self.assertEqual(by_id["BOOST"]["stock_value_digests"],
                         [enhancement_package.keyed_stock_value_digest(key, "BOOST", _f32_bits_of(self.BOOST))])
        self.assertEqual({k: v for k, v in by_id["LAUNCH"].items() if k not in ("stock_digest",)}, {
            "id": "LAUNCH", "function": "synth_launch", "insn_offset": self.LAUNCH_AT, "word_count": 1,
            "transform": "launch_from_rest_60hz", "velocity_offset": self.VELOCITY_AT, "gravity_axis": 1,
            "gravity_site": "G", "step_site": "STEP", "damping_site": "C", "damping_step_site": "DSTEP"})
        self.assertEqual(by_id["CALL"]["phase_time"], {"register": "s0", "offset": self.TIME_AT})
        # No number in the file is a stock value or a factor computed from one.
        kappa, q = self._factors()

        def numbers(value: Any) -> list[Any]:
            if isinstance(value, dict):
                return [n for item in value.values() for n in numbers(item)]
            if isinstance(value, list):
                return [n for item in value for n in numbers(item)]
            return [value] if isinstance(value, (int, float)) and not isinstance(value, bool) else []

        self.assertEqual(sorted(set(numbers(published))), [0, 1, 4, 8, 12, 16, 24, 28, 0x24, 40, 0x30])
        self.assertTrue(all(isinstance(n, int) for n in numbers(published)))
        declaration, symbols = enhancement_package.resolve_published_package(
            published, executable=elf_bytes, read_words_fn=read_words, read_data_fn=read_data)
        resolved = verify_and_resolve_sites(
            declaration, executable_sha256=sha256, symbol_to_addr=symbols, read_words_fn=read_words,
            read_const_fn=read_const, known_functions=self._known())
        rewrites = {entry["id"]: entry["rewrite"] for entry in resolved.values()}
        expected = self._resolve()
        expected["BOOST"] = {"kind": "scale_f_store", "reg": 0, "factor": kappa,
                             "stock_value_digests": by_id["BOOST"]["stock_value_digests"]}
        self.assertEqual(rewrites, expected)
        self.assertEqual(rewrites["LAUNCH"]["gravity_offset"], q * self.GRAVITY)
        # A store scaled by a computed factor needs the executable's constants to publish.
        with self.assertRaisesRegex(EnhancementPackageError, "which this resolver was not given"):
            enhancement_package.publish_package(
                private, self._symbols(), executable=elf_bytes, disc_id="ULUS99999", disc_version="1.01",
                read_words_fn=read_words, read_data_fn=read_data)
        # A published file that carried a factor would not be a package: the format has no slot for one.
        with_factor = copy.deepcopy(published)
        with_factor["sites"][4]["factor"] = kappa
        self.assertTrue(enhancement_package.published_package_findings(with_factor))


_ACE_BASE = 0x08804000
_ACE_WAIT_NID = 0x984C27E7    # sceDisplayWaitVblankStart
_ACE_OTHER_NID = 0x7A000001   # a synthetic NID no transform knows
_ACE_SLOT_WORD = 0x24840007   # addiu a0, a0, 7: a delay slot whose every run shows in $a0


class _AcePartsFixture(unittest.TestCase):
    """The fixture of the ace_parts tests (ENHANCEMENT_CONTRACT.md section 9.8). Every fixture is
    synthetic: tools/import_fixtures.build_text_stub_run_elf with two import stubs
    (sceDisplayWaitVblankStart and a synthetic NID) and a plain callee.
    """

    def setUp(self) -> None:
        import import_fixtures

        self.data_off = int(import_fixtures.DATA_FILE_OFF)
        raw, stubs = import_fixtures.build_text_stub_run_elf(
            nids=[_ACE_WAIT_NID, _ACE_OTHER_NID], library="sceDisplay", late_callee=True)
        self.data = bytearray(raw)
        self.stub_wait, self.stub_other = stubs
        # The fixture's orphan caller: addiu sp; sw ra; jal stub_wait; <slot>; jal stub_other; nop;
        # jal callee; nop; ... The callee (addiu v0, zero, 1) follows the stub run.
        self.loader = _ACE_BASE + 8
        self.callee = stubs[-1] + 8
        self._put(self.loader + 12, _ACE_SLOT_WORD)
        self.elf = bytes(self.data)

    def _off(self, addr: int) -> int:
        return self.data_off + (addr - _ACE_BASE)

    def _put(self, addr: int, word: int) -> None:
        struct.pack_into("<I", self.data, self._off(addr), word)

    def word(self, addr: int) -> int:
        return int(struct.unpack_from("<I", self.elf, self._off(addr))[0])

    def _site(self, offset: int, site_id: str = "SYNTH_LOAD_WAIT", function: str = "synth_loader") -> dict[str, Any]:
        return {"id": site_id, "function": function, "insn_offset": offset,
                "stock_digest": self._digest(function, offset), "transform": "vblank_wait_skip"}

    def _digest(self, function: str, offset: int, words: int = 1) -> str:
        fn = {"synth_loader": self.loader, "synth_loader_alias": self.loader, "synth_callee": self.callee}[function]
        return str(compute_stock_words_digest([self.word(fn + offset + 4 * i) for i in range(words)]))

    def package(self, *, ace: bool = True, offset: int = 8) -> dict[str, Any]:
        pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.load",
            "display_name": "Synthetic Load Waits",
            "version": "1.0.0",
            "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00",
            "executable_sha256": hashlib.sha256(self.elf).hexdigest(),
            "capabilities": ["count_x2"],
            "rng_policy": {"mode": "rescale_probability", "reason": "Synthetic: no roll sites."},
            "sites": [{"id": "SYNTH_COUNT", "function": "synth_callee", "insn_offset": 0,
                       "stock_digest": compute_stock_words_digest([self.word(self.callee)]),
                       "transform": "count_x2"}],
            # A load part needs the boundary (section 9.8); the callee's last word is the one
            # non-control word the fixture has left outside the loader.
            "update_boundary": {"function": "synth_callee", "insn_offset": 8,
                                "stock_digest": self._digest("synth_callee", 8)},
        }
        if ace:
            pkg["ace_parts"] = {"load": {"sites": [self._site(offset)]}}
        return pkg

    def symbols(self, alias: bool = False) -> dict[str, str]:
        names = {"synth_loader": f"0x{self.loader:08x}", "synth_callee": f"0x{self.callee:08x}"}
        if alias:
            # A second name for the loader, as a symbol map or a site map may give one address.
            names["synth_loader_alias"] = f"0x{self.loader:08x}"
        return names

    def _codegen(self, tmp: Path, name: str, pkg: dict[str, Any] | None,
                 symbols: dict[str, str] | None = None, extra: tuple[str, ...] = ()) -> tuple[int, str, dict[str, str]]:
        elf_path = tmp / "load.elf"
        elf_path.write_bytes(self.elf)
        out_dir = tmp / name
        out_dir.mkdir()
        argv = ["codegen.py", str(elf_path), str(out_dir / "out.c"), f"--base={_ACE_BASE:08x}",
                "--profile=none", "--funcs-per-chunk=4000"]
        if pkg is not None:
            pkg_path = tmp / f"{name}.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            if symbols is not None:
                pkg_path.with_suffix(".symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            argv.append(f"--enhancement-package={pkg_path}")
        argv.extend(extra)
        out, err = StringIO(), StringIO()
        with mock.patch("sys.stdout", new=out), mock.patch("sys.stderr", new=err):
            rc = codegen.main(argv)
        files = {p.name: p.read_text(encoding="utf-8") for p in sorted(out_dir.glob("out*")) if p.is_file()}
        return rc, out.getvalue() + err.getvalue(), files if rc == 0 else {}

    def _readers(self):
        from analyze import Elf

        return enhancement_package.elf_readers(Elf(self.elf, base=_ACE_BASE))

    def _publish(self, pkg: dict[str, Any]) -> dict[str, Any]:
        read_words, read_data = self._readers()
        published: dict[str, Any] = enhancement_package.publish_package(
            pkg, {k: int(v, 16) for k, v in self.symbols().items()}, executable=self.elf,
            disc_id="ULUS99998", disc_version="1.00", read_words_fn=read_words, read_data_fn=read_data)
        return published

    def _codegen_with_hook(self, tmp: Path, offset: int, site_id: str,
                           pkg: dict[str, Any] | None = None) -> tuple[int, str, dict[str, str]]:
        """Codegen with the load package (or `pkg`) and a code-hook package whose clip_x_widen site is
        the jal at synth_loader+offset (contract section 9.9)."""
        hook = {
            "schema_version": 1, "id": "synth.widen.clip", "display_name": "Synthetic clip widen",
            "version": "1.0.0", "category": "guest_memory_code_hook", "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00", "executable_sha256": hashlib.sha256(self.elf).hexdigest(),
            "capabilities": ["widen_visibility"],
            "sites": [{"id": site_id, "function": "synth_loader", "insn_offset": offset,
                       "stock_digest": self._digest("synth_loader", offset), "transform": "clip_x_widen",
                       "clip_register": "v0", "clip_offsets": [640]}],
        }
        hook_path = tmp / "hook.json"
        hook_path.write_text(json.dumps(hook), encoding="utf-8")
        hook_path.with_suffix(".symbols.json").write_text(json.dumps(self.symbols()), encoding="utf-8")
        return self._codegen(tmp, "stacked", pkg if pkg is not None else self.package(), self.symbols(),
                             extra=(f"--code-hook-package={hook_path}",))


class TestAcePartsLoad(_AcePartsFixture):
    """ace_parts.load and its vblank_wait_skip transform (ENHANCEMENT_CONTRACT.md sections 9.8 and
    11.3.10)."""

    def test_a_code_hook_site_on_an_ace_part_site_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            rc, out, files = self._codegen_with_hook(Path(tmp_dir), 8, "SYNTH_WIDEN")
        self.assertEqual(rc, 1, out)
        self.assertRegex(out, rf"code-hook site SYNTH_WIDEN: address 0x{self.loader + 8:08x} collides with "
                              r"site SYNTH_LOAD_WAIT of the enhancement package")
        self.assertEqual(files, {})

    def test_a_code_hook_site_id_an_ace_part_uses_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            rc, out, files = self._codegen_with_hook(Path(tmp_dir), 24, "SYNTH_LOAD_WAIT")
        self.assertEqual(rc, 1, out)
        self.assertIn("code-hook site SYNTH_LOAD_WAIT: the enhancement package has a site with that id", out)
        self.assertEqual(files, {})

    def test_a_code_hook_site_beside_an_ace_part_builds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            rc, out, files = self._codegen_with_hook(Path(tmp_dir), 24, "SYNTH_WIDEN")
        self.assertEqual(rc, 0, out)
        text = "\n".join(files.values())
        self.assertEqual(text.count("sr_enhancement_widen_clip_x(s->r[2], _nk_clip_off, 1u);"), 1)
        self.assertIn('nk_ace_load_skip_wait(s, "SYNTH_LOAD_WAIT"', text)

    # ---- the validator ------------------------------------------------------------------------

    def _boundary_pkg(self) -> dict[str, Any]:
        pkg = self.package()
        pkg["sites"][0]["transform"] = "gate_30hz"
        pkg["sites"][0]["insn_offset"] = 24
        pkg["sites"][0]["function"] = "synth_loader"
        pkg["update_boundary"] = {"function": "synth_callee", "insn_offset": 0,
                                  "stock_digest": compute_stock_words_digest([self.word(self.callee)])}
        return pkg

    def test_validator_accepts_a_load_part(self) -> None:
        validated = validate_package(self.package())
        site = validated["ace_parts"]["load"]["sites"][0]
        self.assertEqual(site["word_count"], 1)
        self.assertEqual(site["transform"], "vblank_wait_skip")
        validate_package(self._boundary_pkg())
        explicit = self.package()
        explicit["ace_parts"]["load"]["sites"][0]["word_count"] = 1
        validate_package(explicit)
        # The two transform sets are disjoint, so neither kind of site takes the other's transforms.
        self.assertFalse(enhancement_package.ACE_PART_TRANSFORMS & SITE_TRANSFORMS)
        self.assertEqual(enhancement_package.ACE_PART_TRANSFORMS, {"vblank_wait_skip", "alpha_edge_soften"})
        self.assertEqual(enhancement_package.ACE_PART_NAMES, {"load", "soft_shadows"})

    def test_validator_refusals(self) -> None:
        def ace(pkg: dict[str, Any]) -> dict[str, Any]:
            site: dict[str, Any] = pkg["ace_parts"]["load"]["sites"][0]
            return site

        cases: list[tuple[str, Any, str]] = []

        def case(name: str, why: str):
            def wrap(fn):
                cases.append((name, fn, why))
                return fn
            return wrap

        @case("duplicate id with a 60 Hz site", "duplicate site id 'SYNTH_COUNT'")
        def _(p):
            ace(p)["id"] = "SYNTH_COUNT"

        @case("duplicate id inside ace_parts", "duplicate site id 'SYNTH_LOAD_WAIT'")
        def _(p):
            p["ace_parts"]["load"]["sites"].append(dict(ace(p), insn_offset=16))

        @case("a word a 60 Hz site covers", "may not cover a word site 'SYNTH_COUNT' already covers "
                                            "('synth_callee', insn_offset=0, compared by name)")
        def _(p):
            ace(p).update(function="synth_callee", insn_offset=0)

        @case("the second word of a two-word 60 Hz site", "may not cover a word site 'SYNTH_COUNT' (its word 2) "
                                                         "already covers ('synth_loader', insn_offset=8")
        def _(p):
            p["sites"][0].update(function="synth_loader", insn_offset=4, word_count=2,
                                 stock_digest=self._digest("synth_loader", 4, 2))

        @case("the update_boundary's word", "may not cover a word the update_boundary already covers")
        def _(p):
            p.update(self._boundary_pkg())
            p["update_boundary"].update(function="synth_loader", insn_offset=8)

        @case("a load part without update_boundary", "A package with ace_parts.load must declare update_boundary")
        def _(p):
            del p["update_boundary"]

        @case("requires on an ace_parts site", "an ace_parts site cannot declare requires")
        def _(p):
            ace(p)["requires"] = ["SYNTH_COUNT"]

        @case("requires naming an ace_parts site", "requires names 'SYNTH_LOAD_WAIT', an ace_parts site")
        def _(p):
            p["sites"][0]["requires"] = ["SYNTH_LOAD_WAIT"]

        @case("vblank_wait_skip in the boundary's function", "may not be in the update_boundary's function")
        def _(p):
            p.update(self._boundary_pkg())
            p["update_boundary"]["function"] = "synth_loader"
            p["update_boundary"]["insn_offset"] = 4

        @case("an unknown part", "Unknown ace part ['save']")
        def _(p):
            p["ace_parts"]["save"] = p["ace_parts"]["load"]

        @case("a 60 Hz transform in ace_parts", "Unknown ace part transform: 'gate_30hz'")
        def _(p):
            ace(p)["transform"] = "gate_30hz"

        @case("vblank_wait_skip in sites", "Unknown site transform: 'vblank_wait_skip'")
        def _(p):
            p["sites"][0]["transform"] = "vblank_wait_skip"

        @case("two words", "word_count must be 1")
        def _(p):
            ace(p)["word_count"] = 2

        @case("a 60 Hz site field", "Unknown properties forbidden in ace part site: ['operand']")
        def _(p):
            ace(p)["operand"] = "f64_high"

        @case("an address for a function", "Address-literal site function rejected")
        def _(p):
            ace(p)["function"] = "sub_08804008"

        @case("an empty section", "ace_parts must be a non-empty object")
        def _(p):
            p["ace_parts"] = {}

        @case("an empty part", "an ace part's sites must be a non-empty array")
        def _(p):
            p["ace_parts"]["load"]["sites"] = []

        @case("a code-hook package", "not valid for category 'guest_memory_code_hook'")
        def _(p):
            p["category"] = "guest_memory_code_hook"
            del p["rng_policy"]
            del p["update_boundary"]
            p["sites"] = [{"id": "SYNTH_CLIP", "function": "synth_loader", "insn_offset": 24,
                           "stock_digest": compute_stock_words_digest([self.word(self.loader + 24)]),
                           "transform": "clip_x_widen", "clip_register": "v0", "clip_offsets": [0]}]

        @case("a presentation package", "only valid for category 'timing_frame_presentation'")
        def _(p):
            for key in ("game_version", "executable_sha256", "rng_policy", "sites", "update_boundary"):
                del p[key]
            p["category"] = "presentation_only"

        for name, change, why in cases:
            with self.subTest(name):
                pkg = self.package()
                change(pkg)
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn(why, str(ctx.exception))

    def test_schema_describes_ace_parts(self) -> None:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        parts = schema["properties"]["ace_parts"]
        self.assertFalse(parts["additionalProperties"])
        self.assertEqual(set(parts["properties"]), enhancement_package.ACE_PART_NAMES)
        site = parts["properties"]["load"]["properties"]["sites"]["items"]
        self.assertFalse(site["additionalProperties"])
        self.assertEqual(set(site["properties"]), enhancement_package.ACE_PART_SITE_KEYS)
        self.assertEqual(set(site["properties"]["transform"]["enum"]), {"vblank_wait_skip"})
        self.assertEqual(site["properties"]["word_count"]["const"], 1)

    def test_digest_and_cache_key_cover_ace_parts(self) -> None:
        without = self.package(ace=False)
        with_ace = self.package()
        moved = self.package(offset=16)
        digests = {enhancement_package.compute_package_digest(validate_package(copy.deepcopy(p)))
                   for p in (without, with_ace, moved)}
        self.assertEqual(len(digests), 3)
        with tempfile.TemporaryDirectory() as tmp_dir:
            keys = []
            for idx, pkg in enumerate((without, with_ace, moved)):
                path = Path(tmp_dir) / f"p{idx}.json"
                path.write_text(json.dumps(pkg), encoding="utf-8")
                keys.append(package_cache.enhancement_package_options({"ENHANCEMENT_PACKAGE": str(path)})
                            ["enhancement_package_sha256"])
            self.assertEqual(len(set(keys)), 3)

    # ---- the published form ---------------------------------------------------------------------

    def test_published_form_covers_ace_parts_sites(self) -> None:
        published = self._publish(self.package())
        self.assertEqual(enhancement_package.published_package_findings(published), [])
        self.assertEqual(published["site_map"], self.symbols())
        site = published["ace_parts"]["load"]["sites"][0]
        self.assertNotEqual(site["stock_digest"], compute_stock_words_digest([self.word(self.loader + 8)]))

        # The site map must name the ace site's function, and nothing else.
        missing = copy.deepcopy(published)
        del missing["site_map"]["synth_loader"]
        self.assertTrue(any("does not name ['synth_loader']" in f
                            for f in enhancement_package.published_package_findings(missing)))
        extra = copy.deepcopy(published)
        del extra["ace_parts"]
        self.assertTrue(any("which no site uses" in f
                            for f in enhancement_package.published_package_findings(extra)))

        read_words, read_data = self._readers()
        declaration, symbols = enhancement_package.resolve_published_package(
            published, executable=self.elf, read_words_fn=read_words, read_data_fn=read_data)
        self.assertEqual(declaration["ace_parts"], {"load": {"sites": [dict(self._site(8), word_count=1)]}})
        self.assertEqual(symbols["synth_loader"], self.loader)

        # A keyed digest that does not match the ace site's word refuses by name.
        bad = copy.deepcopy(published)
        bad["ace_parts"]["load"]["sites"][0]["stock_digest"] = enhancement_package.keyed_stock_digest(
            b"other key", b"\0" * 4)
        with self.assertRaises(enhancement_package.PublishedPackageRefusal) as ctx:
            enhancement_package.resolve_published_package(
                bad, executable=self.elf, read_words_fn=read_words, read_data_fn=read_data)
        self.assertEqual(ctx.exception.code, "site_mismatch")
        self.assertIn("ace_parts.load.sites[0](SYNTH_LOAD_WAIT)", str(ctx.exception))

        # The published package builds the same code as the private one.
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            rc_priv, out_priv, files_priv = self._codegen(tmp, "private", self.package(), self.symbols())
            rc_pub, out_pub, files_pub = self._codegen(tmp, "published", published)
            self.assertEqual(rc_priv, 0, out_priv)
            self.assertEqual(rc_pub, 0, out_pub)
            self.assertEqual(files_pub, files_priv)

    def test_published_form_compares_aliases_by_address(self) -> None:
        """A site map may give one address two names, so the namespace of section 9.8 is checked
        by address there: the publish gate refuses what codegen would."""
        published = self._publish(self.package())
        loader = published["site_map"]["synth_loader"]
        digest = published["ace_parts"]["load"]["sites"][0]["stock_digest"]

        def findings(change) -> list[str]:
            pkg = copy.deepcopy(published)
            pkg["site_map"]["synth_loader_alias"] = loader
            change(pkg)
            return list(enhancement_package.published_package_findings(pkg))

        def boundary_in_alias(pkg: dict[str, Any]) -> None:
            pkg["update_boundary"] = {"function": "synth_loader_alias", "insn_offset": 4, "stock_digest": digest}

        def gate_on_the_jal(pkg: dict[str, Any]) -> None:
            pkg["sites"].append({"id": "SYNTH_GATE", "function": "synth_loader_alias", "insn_offset": 8,
                                 "stock_digest": digest, "transform": "gate_30hz"})

        got = findings(boundary_in_alias)
        self.assertTrue(any("may not be in the update_boundary's function 'synth_loader_alias' "
                            "(compared by site-map address)" in f for f in got), got)
        got = findings(gate_on_the_jal)
        self.assertTrue(any("may not cover a word site 'SYNTH_GATE' already covers ('synth_loader', "
                            "insn_offset=8, compared by site-map address)" in f for f in got), got)

    # ---- codegen ---------------------------------------------------------------------------------

    def test_codegen_gates_the_wait_with_the_load_part_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            rc, output, files = self._codegen(tmp, "ace", self.package(), self.symbols())
            self.assertEqual(rc, 0, output)
            rc0, output0, files0 = self._codegen(tmp, "plain", self.package(ace=False), self.symbols())
            self.assertEqual(rc0, 0, output0)
        body = "\n".join(files.values())
        stub = f"f_{self.stub_wait:08x}(s)"
        slot = f"sr_begin(s, 0x{self.loader + 12:08x}u, 0x{_ACE_SLOT_WORD:08x}u); s->r[4] = (s->r[4] + 0x00000007u);"
        gate = f'    if (!nk_ace_load_skip_wait(s, "SYNTH_LOAD_WAIT", 0x{self.loader + 8:08x}u)) {{'
        skipped = "    }"
        hook = "    nk_ace_load_update();"
        self.assertEqual(body.count("nk_ace_load_skip_wait"), 1)
        self.assertEqual(body.count("nk_ace_load_update"), 1)
        lines = body.split("\n")
        at = lines.index(gate)
        self.assertIn(slot, lines[at - 1], "the delay slot runs as stock, before the gate")
        self.assertIn(stub, lines[at + 1], "the stock call to the wait's import stub")
        self.assertEqual(lines[at + 2], skipped, "the runtime, not the gate, sets $v0 for a skip")
        # Gated only by the Ace load part: never by the 60 Hz toggle or its update count.
        self.assertNotIn("nk_enhancement", "\n".join(lines[at - 1:at + 3]))

        # The pacing guard's hook (section 11.3.10) sits at the update boundary, ungated, after the
        # boundary's own count and before its stock statement.
        at = lines.index(hook)
        self.assertEqual(lines[at - 1], "    if (nk_enhancement_on()) { nk_enhancement_updates++; }")
        self.assertIn(f"sr_begin(s, 0x{self.callee + 8:08x}u", lines[at + 1])

        # The generated C differs from the build without the section only at that statement, at
        # the boundary's hook and in the declaration digest of the epoch stamp.
        self.assertEqual(set(files), set(files0))
        changed: dict[str, list[str]] = {}
        for name in files:
            delta = [d for d in difflib.ndiff(files0[name].split("\n"), files[name].split("\n"))
                     if d[:1] in "+-" and d[:2] != "? "]
            if delta:
                changed[name] = delta
        self.assertEqual(set(changed), {"out.c", "out_0.c"}, changed)
        self.assertEqual(changed["out_0.c"], ["+ " + gate, "+ " + skipped, "+ " + hook])
        self.assertEqual(len(changed["out.c"]), 2)
        self.assertTrue(all("AOT Enhancement Epoch" in d and "sites=1 " in d for d in changed["out.c"]))

    def test_a_wait_in_a_caller_of_the_boundary_is_accepted_and_counted_at_run_time(self) -> None:
        """The static rule covers only the boundary's own function (section 11.3.10, "What the
        tools check"). The fixture's loader calls the boundary's function, as a main loop calls
        its update, and its wait is accepted by the validator and by codegen; what catches a site
        that does pace updates is the run-time guard, fed by the boundary's ungated hook."""
        pkg = self.package()
        self.assertNotEqual(pkg["ace_parts"]["load"]["sites"][0]["function"], pkg["update_boundary"]["function"])
        validate_package(copy.deepcopy(pkg))
        with tempfile.TemporaryDirectory() as tmp_dir:
            rc, output, files = self._codegen(Path(tmp_dir), "caller", pkg, self.symbols())
        self.assertEqual(rc, 0, output)
        lines = "\n".join(files.values()).split("\n")
        loader = next(i for i, line in enumerate(lines) if f"sr_begin(s, 0x{self.loader + 8:08x}u" in line)
        self.assertTrue(any(f"f_{self.callee:08x}(s)" in line for line in lines[loader:]),
                        "the loader calls the boundary's function")
        self.assertEqual(sum(line == "    nk_ace_load_update();" for line in lines), 1)
        # A package without ace_parts gets no hook: its build is unchanged.
        with tempfile.TemporaryDirectory() as tmp_dir:
            rc0, output0, files0 = self._codegen(Path(tmp_dir), "plain", self.package(ace=False), self.symbols())
        self.assertEqual(rc0, 0, output0)
        self.assertNotIn("nk_ace_load", "\n".join(files0.values()))

    def test_contract_names_the_pacing_threshold_the_runtime_uses(self) -> None:
        """Section 11.3.10 states the guard's threshold once, as src/rt/ace.h defines it, and no
        longer claims the update counter is unaffected because of the static rule."""
        header = (ROOT / "src" / "rt" / "ace.h").read_text(encoding="utf-8")
        defined = re.search(r"^#define ACE_LOAD_PACING_UPDATES (\d+)u\b", header, re.M)
        self.assertIsNotNone(defined, "ace.h defines ACE_LOAD_PACING_UPDATES")
        assert defined is not None
        contract = (ROOT / "docs" / "ENHANCEMENT_CONTRACT.md").read_text(encoding="utf-8")
        section = contract[contract.index("#### 11.3.10."):]
        following = re.search(r"\n#{1,4} ", section)
        section = section[:following.start()] if following else section
        stated = re.findall(r"`ACE_LOAD_PACING_UPDATES` \(`src/rt/ace\.h`, (\d+) updates", section)
        self.assertEqual(stated, [defined.group(1)])
        self.assertNotIn("is unaffected, because", " ".join(section.split()))

    def test_codegen_refuses_by_resolved_address(self) -> None:
        """The private form names functions through a symbol map, which may give one address two
        names; codegen compares the resolved addresses (section 9.8, "One namespace")."""
        def boundary_in_alias(pkg: dict[str, Any]) -> None:
            pkg["update_boundary"] = {"function": "synth_loader", "insn_offset": 4,
                                      "stock_digest": self._digest("synth_loader", 4)}
            pkg["ace_parts"]["load"]["sites"][0] = self._site(8, function="synth_loader_alias")

        def gate_on_the_jal(pkg: dict[str, Any]) -> None:
            pkg["sites"].append({"id": "SYNTH_GATE", "function": "synth_loader_alias", "insn_offset": 8,
                                 "stock_digest": self._digest("synth_loader_alias", 8),
                                 "transform": "gate_30hz"})

        cases = (
            ("boundary function alias", boundary_in_alias,
             f"its function 'synth_loader_alias' resolves to 0x{self.loader:08x}, the update_boundary's function"),
            ("60 Hz site alias on the jal", gate_on_the_jal,
             "its word is also a 60 Hz site or the update_boundary"),
        )
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            for name, change, why in cases:
                with self.subTest(name):
                    pkg = self.package()
                    change(pkg)
                    validate_package(copy.deepcopy(pkg))   # the names differ, so only codegen can tell
                    rc, output, files = self._codegen(tmp, name.replace(" ", "-"), pkg, self.symbols(alias=True))
                    self.assertEqual(rc, 1, output)
                    self.assertIn("enhancement package refused (fail-closed)", output)
                    self.assertIn(why, output)
                    self.assertEqual(files, {})

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_codegen_gate_runs_the_stock_call_or_skips_it(self) -> None:
        """Compile the emitted statement and run it with the runtime's skip answering 0, then 1.

        The harness stubs nk_ace_load_skip_wait itself, so this covers only the emitted statement.
        The real one (its context check, 1 us delay wait, $v0 and the pacing guard) is covered in
        tests/native/hle_thread_selftest.c (test_ace_load_skip_wait_only_where_the_stock_call_blocks,
        test_ace_load_skip_wait_lets_a_lower_priority_thread_run and
        test_ace_load_skip_wait_stops_once_it_paces_updates), and the guard's counting in
        tests/native/ace_selftest.c."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            rc, output, files = self._codegen(tmp, "ace", self.package(), self.symbols())
            self.assertEqual(rc, 0, output)
            lines = "\n".join(files.values()).split("\n")
            at = next(i for i, line in enumerate(lines) if f"sr_begin(s, 0x{self.loader + 8:08x}u" in line)
            gate = lines.index(f'    if (!nk_ace_load_skip_wait(s, "SYNTH_LOAD_WAIT", 0x{self.loader + 8:08x}u)) {{')
            self.assertEqual(lines[gate + 2], "    }")
            statement = "\n".join(lines[at:gate + 3])
            stub = f"f_{self.stub_wait:08x}"
            harness = tmp / "gate.c"
            harness.write_text(
                "#include \"recomp.h\"\n"
                "/* The real CpuState and trace hooks; the statement runs alone, with no caller frame to\n"
                " * return through, so only the return boundary is stubbed. */\n"
                "#undef SR_TAIL_DRAIN\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
                "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
                "#define sr_aot_return_boundary_end(s) 0\n"
                "static int skip; static unsigned asked, waited;\n"
                "/* As src/rt/hle_display.c: a skip leaves $v0 = 0. */\n"
                "int nk_ace_load_skip_wait(CpuState *s, const char *site, uint32_t pc) {\n"
                "    (void)site; (void)pc; asked++; if (skip) s->r[2] = 0u; return skip;\n"
                "}\n"
                f"static void {stub}(CpuState *s) {{ waited++; s->r[2] = 0x5au; }}\n"
                f"static void site(CpuState *s) {{\n{statement}\n}}\n"
                "int main(void) {\n"
                "    for (skip = 0; skip < 2; skip++) {\n"
                "        CpuState st; CpuState *s = &st; memset(&st, 0, sizeof st);\n"
                "        s->r[2] = 0xdeadu; asked = waited = 0; site(s);\n"
                "        printf(\"%d %u %u %u 0x%x 0x%08x\\n\", skip, asked, waited, s->r[4], s->r[2], s->r[31]);\n"
                "    }\n"
                "    return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            exe = tmp / "gate.exe"
            assert GCC is not None, "skipUnless(GCC) runs this test only with a compiler"
            built = subprocess.run([GCC, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                    "-Wno-misleading-indentation", "-Wno-unused-variable",
                                    f"-I{ROOT / 'src' / 'rt'}", "-o", str(exe), str(harness)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
        ra = f"0x{self.loader + 16:08x}"
        # Off: the stock call runs once and its $v0 stands. On: it never runs and $v0 is 0. Either
        # way the runtime is asked once, the delay slot ran once (a0 += 7) and $ra = site + 8.
        self.assertEqual(run.stdout.split("\n")[:2], [f"0 1 1 7 0x5a {ra}", f"1 1 0 7 0x0 {ra}"])

    def test_codegen_refuses_a_jal_to_anything_but_the_wait(self) -> None:
        cases = (
            ("another import", 16, "vblank_wait_skip needs a jal to the sceDisplayWaitVblankStart import stub"),
            ("a plain function", 24, "vblank_wait_skip needs a jal to the sceDisplayWaitVblankStart import stub"),
            ("not a jal", 0, "a vblank_wait_skip site must be a jal to sceDisplayWaitVblankStart"),
        )
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            for name, offset, why in cases:
                with self.subTest(name):
                    rc, output, files = self._codegen(tmp, name.replace(" ", "-"), self.package(offset=offset),
                                                      self.symbols())
                    self.assertEqual(rc, 1, output)
                    self.assertIn("enhancement package refused (fail-closed)", output)
                    self.assertIn(why, output)
                    self.assertEqual(files, {})
            self.assertIn(f"NID 0x{_ACE_OTHER_NID:08x}", self._codegen(tmp, "nid", self.package(offset=16),
                                                                         self.symbols())[1])


def _synthetic_texture_digest(label: str) -> str:
    """A stand-in for a keyed texture digest: 64 lowercase hex digits no texture produced."""
    return hashlib.sha256(b"synthetic soft-shadow texture " + label.encode("ascii")).hexdigest()


class TestAcePartsSoftShadows(_AcePartsFixture):
    """ace_parts.soft_shadows and its alpha_edge_soften transform (ENHANCEMENT_CONTRACT.md sections
    9.8, 10.23 and 11.3.13): a list of keyed texture digests that generated code declares to the
    runtime once at start. The digests are synthetic."""

    def _texture(self, idx: int = 0, *, edge: int = 2, label: str | None = None) -> dict[str, Any]:
        return {"id": f"SYNTH_SHADOW_{idx}", "texture_digest": _synthetic_texture_digest(label or str(idx)),
                "transform": "alpha_edge_soften", "edge_texels": edge}

    def shadow_package(self, textures: list[dict[str, Any]] | None = None, *, load: bool = True) -> dict[str, Any]:
        pkg = self.package(ace=load)
        pkg.setdefault("ace_parts", {})["soft_shadows"] = {
            "textures": textures if textures is not None else [self._texture(0), self._texture(1, edge=4)]}
        return pkg

    # ---- the validator ------------------------------------------------------------------------

    def test_validator_accepts_soft_shadows(self) -> None:
        validated = validate_package(self.shadow_package())
        textures = enhancement_package.soft_shadow_textures(validated)
        self.assertEqual(textures, [self._texture(0), self._texture(1, edge=4)])
        self.assertEqual(validated["ace_parts"]["load"]["sites"][0]["transform"], "vblank_wait_skip")
        # The part stands on its own, without a load part, and every edge width 1 to 4 is taken.
        alone = validate_package(self.shadow_package([self._texture(i, edge=i) for i in range(1, 5)], load=False))
        self.assertEqual(set(alone["ace_parts"]), {"soft_shadows"})
        self.assertEqual(enhancement_package.ace_part_sites(alone), [])
        self.assertEqual(enhancement_package.soft_shadow_textures(validate_package(self.package())), [])
        validate_package(self.shadow_package([self._texture(i) for i in range(256)]))
        self.assertEqual(enhancement_package.ACE_PART_TRANSFORM_PLACE,
                         {"vblank_wait_skip": "load.sites", "alpha_edge_soften": "soft_shadows.textures"})
        self.assertIn("texture_digest", enhancement_package._PUBLISHED_DIGEST_SLOTS)

    def test_validator_refusals(self) -> None:
        def tex(pkg: dict[str, Any]) -> dict[str, Any]:
            texture: dict[str, Any] = pkg["ace_parts"]["soft_shadows"]["textures"][0]
            return texture

        cases: list[tuple[str, Any, str]] = []

        def case(name: str, why: str):
            def wrap(fn):
                cases.append((name, fn, why))
                return fn
            return wrap

        @case("alpha_edge_soften in load.sites",
              "ace part transform 'alpha_edge_soften' is valid only in ace_parts.soft_shadows.textures")
        def _(p):
            p["ace_parts"]["load"]["sites"][0]["transform"] = "alpha_edge_soften"

        @case("vblank_wait_skip in soft_shadows.textures",
              "ace part transform 'vblank_wait_skip' is valid only in ace_parts.load.sites")
        def _(p):
            tex(p)["transform"] = "vblank_wait_skip"

        @case("alpha_edge_soften in sites", "Unknown site transform: 'alpha_edge_soften'")
        def _(p):
            p["sites"][0]["transform"] = "alpha_edge_soften"

        @case("an unknown transform", "Unknown ace part transform: 'blur'")
        def _(p):
            tex(p)["transform"] = "blur"

        for name, digest in (("uppercase", _synthetic_texture_digest("0").upper()),
                             ("63 digits", _synthetic_texture_digest("0")[:63]),
                             ("65 digits", _synthetic_texture_digest("0") + "0"),
                             ("not hex", "g" * 64),
                             ("a number", 7)):
            @case(f"a digest: {name}", "texture_digest must be 64 lowercase hex digits")
            def _(p, digest=digest):
                tex(p)["texture_digest"] = digest

        for edge in (0, 5, -1, True, 2.0, "2"):
            @case(f"edge_texels {edge!r}", "edge_texels must be an integer from 1 to 4")
            def _(p, edge=edge):
                tex(p)["edge_texels"] = edge

        @case("no edge_texels", "Missing required property 'edge_texels' in soft shadow texture")
        def _(p):
            del tex(p)["edge_texels"]

        @case("no texture_digest", "Missing required property 'texture_digest' in soft shadow texture")
        def _(p):
            del tex(p)["texture_digest"]

        @case("a site field", "Unknown properties forbidden in soft shadow texture: ['stock_digest']")
        def _(p):
            tex(p)["stock_digest"] = p["sites"][0]["stock_digest"]

        @case("sites in soft_shadows", "Unknown properties forbidden in ace part: ['sites']")
        def _(p):
            p["ace_parts"]["soft_shadows"]["sites"] = p["ace_parts"]["load"]["sites"]

        @case("textures in load", "Unknown properties forbidden in ace part: ['textures']")
        def _(p):
            p["ace_parts"]["load"]["textures"] = p["ace_parts"]["soft_shadows"]["textures"]

        @case("an empty list", "soft_shadows.textures must be a non-empty array")
        def _(p):
            p["ace_parts"]["soft_shadows"]["textures"] = []

        @case("257 textures", "soft_shadows declares 257 textures; the runtime takes at most 256")
        def _(p):
            p["ace_parts"]["soft_shadows"]["textures"] = [self._texture(i) for i in range(257)]

        @case("the same digest twice", "a texture_digest is declared twice")
        def _(p):
            p["ace_parts"]["soft_shadows"]["textures"][1]["texture_digest"] = tex(p)["texture_digest"]

        @case("an id a 60 Hz site uses", "duplicate site id 'SYNTH_COUNT'")
        def _(p):
            tex(p)["id"] = "SYNTH_COUNT"

        @case("an id a load site uses", "duplicate site id 'SYNTH_LOAD_WAIT'")
        def _(p):
            tex(p)["id"] = "SYNTH_LOAD_WAIT"

        @case("an id used twice", "duplicate site id 'SYNTH_SHADOW_0'")
        def _(p):
            p["ace_parts"]["soft_shadows"]["textures"][1]["id"] = "SYNTH_SHADOW_0"

        @case("an address for an id", "Address-literal site id rejected")
        def _(p):
            tex(p)["id"] = "0x08804000"

        @case("requires naming a texture", "requires names 'SYNTH_SHADOW_0', an ace_parts site")
        def _(p):
            p["sites"][0]["requires"] = ["SYNTH_SHADOW_0"]

        for name, change, why in cases:
            with self.subTest(name):
                pkg = self.shadow_package()
                change(pkg)
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn(why, str(ctx.exception))

    def test_schema_describes_soft_shadows(self) -> None:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        part = schema["properties"]["ace_parts"]["properties"]["soft_shadows"]
        self.assertFalse(part["additionalProperties"])
        self.assertEqual(set(part["properties"]), enhancement_package.ACE_SOFT_SHADOW_PART_KEYS)
        textures = part["properties"]["textures"]
        self.assertEqual((textures["minItems"], textures["maxItems"]),
                         (1, enhancement_package.ACE_SOFT_SHADOW_TEXTURES_MAX))
        item = textures["items"]
        self.assertFalse(item["additionalProperties"])
        self.assertEqual(set(item["properties"]), enhancement_package.ACE_SOFT_SHADOW_TEXTURE_KEYS)
        self.assertEqual(set(item["required"]), enhancement_package.ACE_SOFT_SHADOW_TEXTURE_KEYS)
        self.assertEqual(item["properties"]["transform"]["enum"], ["alpha_edge_soften"])
        self.assertEqual(item["properties"]["texture_digest"]["pattern"], "^[0-9a-f]{64}$")
        self.assertEqual((item["properties"]["edge_texels"]["minimum"], item["properties"]["edge_texels"]["maximum"]),
                         (enhancement_package.ACE_SOFT_SHADOW_EDGE_MIN, enhancement_package.ACE_SOFT_SHADOW_EDGE_MAX))

    def test_digest_and_cache_key_cover_soft_shadows(self) -> None:
        variants = (
            self.package(),
            self.shadow_package([self._texture(0)]),
            self.shadow_package([self._texture(0, edge=3)]),
            self.shadow_package([self._texture(0, label="other")]),
            self.shadow_package([self._texture(0), self._texture(1)]),
        )
        digests = {enhancement_package.compute_package_digest(validate_package(copy.deepcopy(p))) for p in variants}
        self.assertEqual(len(digests), len(variants))
        with tempfile.TemporaryDirectory() as tmp_dir:
            keys = []
            for idx, pkg in enumerate(variants):
                path = Path(tmp_dir) / f"p{idx}.json"
                path.write_text(json.dumps(pkg), encoding="utf-8")
                keys.append(package_cache.enhancement_package_options({"ENHANCEMENT_PACKAGE": str(path)})
                            ["enhancement_package_sha256"])
            self.assertEqual(len(set(keys)), len(variants))

    # ---- the published form ---------------------------------------------------------------------

    def test_published_form_passes_the_texture_digest(self) -> None:
        pkg = self.shadow_package()
        published = self._publish(pkg)
        self.assertEqual(enhancement_package.published_package_findings(published), [])
        # Already keyed: the published form carries the declared digest as it is.
        self.assertEqual(published["ace_parts"]["soft_shadows"], pkg["ace_parts"]["soft_shadows"])
        # Only the digest slot is passed: every other string of a texture is still checked.
        bad = copy.deepcopy(published)
        bad["ace_parts"]["soft_shadows"]["textures"][0]["id"] = "shadow_0123456789"
        self.assertIn("$.ace_parts.soft_shadows.textures[0].id contains a run of more than 8 hex digits",
                      enhancement_package.published_package_findings(bad))
        read_words, read_data = self._readers()
        declaration, _symbols = enhancement_package.resolve_published_package(
            published, executable=self.elf, read_words_fn=read_words, read_data_fn=read_data)
        self.assertEqual(declaration["ace_parts"]["soft_shadows"], pkg["ace_parts"]["soft_shadows"])

    # ---- codegen ---------------------------------------------------------------------------------

    def test_codegen_declares_the_textures_once_at_start(self) -> None:
        textures = [self._texture(0), self._texture(1, edge=4)]
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            rc, output, files = self._codegen(tmp, "shadow", self.shadow_package(textures), self.symbols())
            self.assertEqual(rc, 0, output)
            rc0, output0, files0 = self._codegen(tmp, "plain", self.package(), self.symbols())
            self.assertEqual(rc0, 0, output0)
        call = ("    (void)nk_ace_soft_shadows_declare(nk_ace_shadow_key, nk_ace_shadow_digest, 2u, "
                "nk_ace_shadow_edge);")
        main = files["out.c"]
        self.assertEqual(main.count("nk_ace_soft_shadows_declare("), 2, "the prototype and the one call")
        lines = main.split("\n")
        at = lines.index(call)
        start = lines.index("void sr_register_all(void) {")
        guest = next(i for i, line in enumerate(lines) if "sr_register_all: starting" in line)
        self.assertTrue(start < at < guest, "declared in sr_register_all, before the guest's code registers")
        self.assertEqual(lines[at - 1], "    if (nk_enhancement_on()) nk_enhancement_apply_data();")
        # The key is the executable's stock-digest key, the digests and widths are the package's.
        key = enhancement_package.published_digest_key(self.elf)
        self.assertIn("static const uint8_t nk_ace_shadow_key[32] = {"
                      + ", ".join(f"0x{b:02x}" for b in key) + "};", lines)
        for texture in textures:
            row = ("    {" + ", ".join(f"0x{b:02x}" for b in bytes.fromhex(texture["texture_digest"]))
                   + f"}},  /* {texture['id']} */")
            self.assertIn(row, lines)
        self.assertIn("static const uint8_t nk_ace_shadow_edge[] = {2u, 4u};", lines)
        # Nothing else changes: the build without the part differs only by these lines and the
        # declaration digest of the epoch stamp, and has no trace of the part.
        self.assertNotIn("nk_ace_sh", "\n".join(files0.values()))
        self.assertEqual(set(files), set(files0))
        for name in files:
            if name != "out.c":
                self.assertEqual(files[name], files0[name], name)
        delta = [d for d in difflib.ndiff(files0["out.c"].split("\n"), main.split("\n"))
                 if d[:1] in "+-" and d[:2] != "? "]
        removed = [d for d in delta if d.startswith("- ")]
        self.assertEqual(len(removed), 1)
        self.assertIn("AOT Enhancement Epoch", removed[0])
        added = [d[2:] for d in delta if d.startswith("+ ")]
        self.assertIn(call, added)
        self.assertTrue(all("AOT Enhancement Epoch" in a or "nk_ace_sh" in a or "nk_ace_soft_shadows_declare" in a
                            or a.startswith(("    {0x", "/* Ace soft shadows", " * texture's keyed digest",
                                             "/* src/rt/ace.h */", "                                const uint8_t",
                                             "};"))
                            or a == "" for a in added), added)

    def test_a_code_hook_site_id_a_shadow_texture_uses_is_refused(self) -> None:
        # Texture ids are in the one id namespace across both packages (contract sections 9.8, 9.9).
        with tempfile.TemporaryDirectory() as tmp_dir:
            rc, out, files = self._codegen_with_hook(Path(tmp_dir), 24, "SYNTH_SHADOW_0", self.shadow_package())
        self.assertEqual(rc, 1, out)
        self.assertIn("code-hook site SYNTH_SHADOW_0: the enhancement package has a site with that id", out)
        self.assertEqual(files, {})

    def test_a_code_hook_site_beside_soft_shadows_builds(self) -> None:
        # A soft_shadows part has textures, not sites: a code-hook package beside it still builds.
        with tempfile.TemporaryDirectory() as tmp_dir:
            rc, out, files = self._codegen_with_hook(Path(tmp_dir), 24, "SYNTH_WIDEN",
                                                     self.shadow_package(load=False))
        self.assertEqual(rc, 0, out)
        text = "\n".join(files.values())
        self.assertEqual(text.count("sr_enhancement_widen_clip_x(s->r[2], _nk_clip_off, 1u);"), 1)
        self.assertEqual(text.count("(void)nk_ace_soft_shadows_declare("), 1)


if __name__ == "__main__":
    unittest.main()
