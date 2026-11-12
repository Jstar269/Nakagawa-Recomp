#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Validate and verify enhancement package declarations.

Enhancement packages declare optional host capabilities (e.g. presentation
overlays, asset replacements, projection overrides) layered strictly above the
untouched authentic execution baseline.

Following the repository convention of tools/title_manifest.py, this module
provides a self-contained normative hand-written validator with zero third-party
dependencies. The JSON schema in assets/enhancement_package.schema.json serves as
an editor and review aid.

Contract rules enforced:
- Schema version must be 1.
- All required fields (schema_version, id, display_name, version, category,
  target_title_id, capabilities) must be present and well-formed.
- Category must be an explicitly recognized capability category.
- Hook targets and capability names must be symbolic identifiers; raw guest address
  literals (0x-hex, decompiler-style sub_/loc_/func_ stems, bare 6-8 digit hex strings
  including all-letter ones like 'deadbeef', and numeric addresses) are strictly forbidden.
- Hooks contain symbol and type only.
- Undeclared / unknown properties fail closed.
- A published package (ENHANCEMENT_CONTRACT.md section 5.1) adds its disc, a site map
  and executable-keyed stock digests; it carries no game bytes and resolves only
  against the exact executable it names (resolve_published_package).
"""

from __future__ import annotations

import argparse
import bisect
from collections.abc import Callable, Iterator, Mapping, Sequence
import copy
import functools
import hashlib
import hmac
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
from typing import Any, NoReturn

SUPPORTED_SCHEMA_VERSION = 1

AOT_ENHANCEMENT_PACKAGE_EPOCH = "aot-enhancement-package-v1"

VALID_CATEGORIES = frozenset({
    "asset_replacement",
    "presentation_only",
    "guest_memory_code_hook",
    "save_data_modification",
    "camera_projection_override",
    "timing_frame_presentation",
})

HOOK_TYPES = frozenset({
    "pre_call",
    "post_call",
    "replace",
    "notify",
})

SITE_TRANSFORMS = frozenset({
    "count_x2",
    "step_x0.5",
    "decay_k_to_1-sqrt(1-k)",
    "probability_per_update_x0.5",
    "gate_30hz",
    "vblank_wait_2_to_1",
    "retention_m_to_sqrt(m)",
    "accel_x0.25",
    "launch_velocity_x0.5",
    "offset_x2",
    "window_start_x2",
    "argument_x2",
    "count_x2_even",
    "gate_30hz_hold",
    "gate_30hz_alt",
    "gate_30hz_store",
    "clip_x_widen",
    "gate_30hz_phase",
    "velocity_source_x_kappa",
    "launch_from_rest_60hz",
    "value_reference",
})

# Transforms of a guest_memory_code_hook package (contract section 10.8, "Next layer"): they act
# on guest memory after a call returns, never on a frame-rate constant, so a timing package may
# not use them and a code-hook package may use nothing else.
CODE_HOOK_TRANSFORMS = frozenset({
    "clip_x_widen",
})

# Ace parts (contract section 9.8): per-title items of the Ace tier that a timing package carries
# beside its 60 Hz sites. Each part is gated at run time by its own SR_ACE key, never by
# SR_EXPERIMENTAL_60HZ. Their transforms are a set of their own, disjoint from SITE_TRANSFORMS,
# so a 60 Hz transform in ace_parts, or an ace_parts transform in sites, refuses by construction.
ACE_PART_NAMES = frozenset({"load"})
ACE_PART_TRANSFORMS = frozenset({"vblank_wait_skip"})
ACE_PART_KEYS = frozenset({"sites"})
ACE_PART_SITE_KEYS = frozenset({
    "id",
    "function",
    "insn_offset",
    "stock_digest",
    "word_count",
    "transform",
})
# The one import a vblank_wait_skip site may call: codegen checks the jal's target stub.
VBLANK_WAIT_SKIP_NID = 0x984C27E7
VBLANK_WAIT_SKIP_IMPORT = "sceDisplayWaitVblankStart"

# What a site's instruction builds, when its encoding alone cannot say: the upper
# word of a double is built by a lui like a float, with a different exponent field.
SITE_OPERANDS = frozenset({
    "f64_high",
})

# The registers that carry a function's first eight arguments (MIPS EABI): the
# caller rebuilds them for each call, so an argument_x2 site may double them.
# The registers that carry a call's result (MIPS EABI): a clip_x_widen site reads the guest
# pointer a call returned from one of them.
RESULT_REGISTERS = {
    "v0": 2,
    "v1": 3,
}

ARGUMENT_REGISTERS = {
    "a0": 4,
    "a1": 5,
    "a2": 6,
    "a3": 7,
    "t0": 8,
    "t1": 9,
    "t2": 10,
    "t3": 11,
}

# The callee-saved registers a gate_30hz_phase site may read its state pointer from.
PHASE_REGISTERS = {f"s{n}": 16 + n for n in range(8)}

# The stock values a transform's rule is built from (contract section 9.2). Each key holds the id
# of another site of the package, whose instruction pins one float of the stock executable: the
# resolver reads the value there, so a package never carries it.
#   step_site          h, the stock logic step;
#   gravity_site       g, the stock acceleration;
#   damping_site       c, the stock damping coefficient;
#   damping_step_site  L, the step the stock damping multiplies c by (m = 1 - c*L).
SITE_REFERENCES = {
    "gate_30hz_phase": ("step_site",),
    "velocity_source_x_kappa": ("damping_site", "damping_step_site"),
    "launch_from_rest_60hz": ("gravity_site", "damping_site", "damping_step_site", "step_site"),
}
REFERENCE_KEYS = frozenset(key for keys in SITE_REFERENCES.values() for key in keys)
# What a reference may name. A value another transform rescales is not the stock value the title
# then uses, so g, c and L are pinned by sites that rewrite nothing. The step is the exception:
# the site that halves it is the site that says what the 60 Hz step is.
REFERENCE_TARGETS = {
    "gravity_site": ("value_reference",),
    "damping_site": ("value_reference",),
    "damping_step_site": ("value_reference",),
    "step_site": ("value_reference", "step_x0.5"),
}
# A timing transform may write guest memory only where it replaces, at the same place, a value
# the stock logic itself stores there (its 60 Hz image), and only in a package that names the
# capability (contract section 9.2). Any other write needs a guest_memory_code_hook package.
GUEST_WRITE_CAPABILITIES = {
    "launch_from_rest_60hz": "launch_velocity_write",
}

RNG_POLICY_MODES = frozenset({
    "rescale_probability",
    "subtick_30hz",
})

ROOT_KEYS = frozenset({
    "schema_version",
    "id",
    "display_name",
    "version",
    "category",
    "target_title_id",
    "min_runtime_version",
    "capabilities",
    "hooks",
    "game_version",
    "executable_sha256",
    "rng_policy",
    "sites",
    "update_boundary",
    "data_sites",
    "data_files",
    "ace_parts",
})

REQUIRED_ROOT_KEYS = frozenset({
    "schema_version",
    "id",
    "display_name",
    "version",
    "category",
    "target_title_id",
    "capabilities",
})

TIMING_PACKAGE_KEYS = frozenset({
    "game_version",
    "executable_sha256",
    "rng_policy",
    "sites",
})

# Timing-package fields that may be left out.
OPTIONAL_TIMING_PACKAGE_KEYS = frozenset({
    "update_boundary",
    "data_sites",
    "data_files",
    "ace_parts",
})

# A data site: constants in the executable's loaded data, rescaled once at load.
DATA_SITE_KEYS = frozenset({
    "id",
    "symbol",
    "offset",
    "type",
    "count",
    "stock_digest",
    "transform",
})

# Element types a data site may name: (size in bytes, struct format).
DATA_TYPES = {
    "u8": (1, "<B"),
    "s8": (1, "<b"),
    "u16": (2, "<H"),
    "s16": (2, "<h"),
    "u32": (4, "<I"),
    "s32": (4, "<i"),
    "f32": (4, "<f"),
}

# Transforms a data value or a data-file cell may declare: the operand scalings of
# section 9.2. Gating and the vblank wait act on code, not on data.
DATA_TRANSFORMS = frozenset({
    "count_x2",
    "step_x0.5",
    "probability_per_update_x0.5",
    "decay_k_to_1-sqrt(1-k)",
    "retention_m_to_sqrt(m)",
    "accel_x0.25",
})

# A data file: a file the title reads from its disc, rewritten when it is opened.
DATA_FILE_KEYS = frozenset({
    "id",
    "path",
    "sha256",
    "format",
    "header_row",
    "first_data_row",
    "cells",
})

DATA_FILE_FORMATS = frozenset({
    "csv",
})

CSV_CELL_KEYS = frozenset({
    "column",
    "column_index",
    "rows",
    "transform",
    "type",
})

HOOK_KEYS = frozenset({
    "symbol",
    "type",
})

RNG_POLICY_KEYS = frozenset({
    "mode",
    "reason",
})

SITE_KEYS = frozenset({
    "id",
    "function",
    "insn_offset",
    "stock_digest",
    "word_count",
    "transform",
    "operand",
    "arguments",
    "acceleration_site",
    "low_insn_offset",
    "low_stock_digest",
    "stock_values",
    "stock_value_digests",
    "requires",
    "alternates",
    "clip_register",
    "clip_offsets",
    "phase_time",
    "odd_phase_condition",
    "velocity_offset",
    "gravity_axis",
    *REFERENCE_KEYS,
})

# The instruction where one logic update of the title begins: it runs exactly once per update.
UPDATE_BOUNDARY_KEYS = frozenset({
    "function",
    "insn_offset",
    "stock_digest",
})

ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}\Z")
SEMVER_RE = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?\Z"
)
MIN_RUNTIME_VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
TARGET_TITLE_ID_RE = re.compile(r"^([a-zA-Z0-9][a-zA-Z0-9._-]{0,63}|\*)\Z")
GAME_VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,31}\Z")
SHA256_HEX_RE = re.compile(r"^[0-9a-f]{64}\Z")
CAPABILITY_RE = re.compile(r"^[a-z][a-z0-9_]{1,63}\Z")
SYMBOL_RE = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_]{0,127}\Z")
SITE_ID_RE = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_.-]{0,63}\Z")
# A disc path relative to the disc root: forward slashes, no empty, '.' or '..' part.
DATA_FILE_PATH_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]*(?:/[A-Za-z0-9_][A-Za-z0-9_.-]*)*\Z")
CSV_COLUMN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}\Z")

HEX_ADDRESS_RE = re.compile(r"^0[xX][0-9a-fA-F]+$")
DECOMP_ADDRESS_RE = re.compile(r"^(?:sub|loc|func|fun)_[0-9a-fA-F]+$", re.IGNORECASE)
BARE_HEX_RE = re.compile(r"^[0-9a-fA-F]{6,8}$")


class EnhancementPackageError(ValueError):
    """Raised when an enhancement package fails contract validation."""


def fail(path: str, message: str) -> NoReturn:
    """Raise an EnhancementPackageError formatted with failure path."""
    raise EnhancementPackageError(f"{path}: {message}")


def is_address_literal(value: str) -> bool:
    """True when value represents a raw address literal or decompiler symbol.

    Rejects:
    - 0x-prefixed hex literals (e.g. 0x08804000, 0xdeadbeef)
    - Decompiler-generated address symbols (e.g. sub_08804000, loc_08804000, func_08804000)
    - Bare 6-8 digit hex strings (e.g. 08804000, 123456, deadbeef, cafebabe)
    """
    return bool(
        HEX_ADDRESS_RE.match(value)
        or DECOMP_ADDRESS_RE.match(value)
        or BARE_HEX_RE.match(value)
    )


def check_not_address_literal(value: Any, path: str, role: str) -> str:
    """Validate that value is a symbolic identifier and not an address literal."""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        fail(
            path,
            f"Address-literal {role} rejected: expected symbolic name, "
            f"got numeric address {value!r}",
        )
    if not isinstance(value, str):
        fail(path, f"{role} must be a string, got {type(value).__name__}")
    if value != value.strip():
        fail(path, f"{role} must not have leading or trailing whitespace")
    if not value:
        fail(path, f"{role} must not be empty")
    if is_address_literal(value):
        fail(
            path,
            f"Address-literal {role} rejected: symbolic name required, "
            f"found address literal or decompiler symbol {value!r}",
        )
    return value


def load_package(path: Path | str) -> dict[str, Any]:
    """Load an enhancement package JSON file."""
    package_path = Path(path)
    if not package_path.is_file():
        raise EnhancementPackageError(f"Package file not found: {package_path}")
    try:
        with package_path.open("r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as exc:
        raise EnhancementPackageError(f"Failed to parse JSON in {package_path}: {exc}") from exc
    if not isinstance(data, dict):
        raise EnhancementPackageError(f"Package root must be a JSON object, got {type(data).__name__}")
    return data


def load_and_validate_package(path: Path | str) -> dict[str, Any]:
    """Load an enhancement package JSON file and perform full normative validation."""
    return validate_package(load_package(path))


def _check_instruction_location(obj: Mapping[str, Any], path: str, role: str) -> None:
    """Check the symbolic function, 4-byte-aligned insn_offset and stock_digest of one instruction."""
    fn_name = obj["function"]
    fn_path = f"{path}.function"
    check_not_address_literal(fn_name, fn_path, f"{role} function")
    if not SYMBOL_RE.match(fn_name):
        fail(fn_path, f"{role} function {fn_name!r} does not match required pattern {SYMBOL_RE.pattern}")

    insn_offset = obj["insn_offset"]
    if (
        not isinstance(insn_offset, int)
        or isinstance(insn_offset, bool)
        or insn_offset < 0
        or insn_offset > 65536
        or (insn_offset % 4) != 0
    ):
        fail(
            f"{path}.insn_offset",
            f"insn_offset must be a 4-byte-aligned integer in [0, 65536], got {insn_offset!r}",
        )

    stock_digest = obj["stock_digest"]
    if not isinstance(stock_digest, str) or not SHA256_HEX_RE.match(stock_digest):
        fail(
            f"{path}.stock_digest",
            f"stock_digest {stock_digest!r} must be a 64-character lowercase hex SHA-256 digest",
        )


def validate_package(data: Any) -> dict[str, Any]:
    """Validate an enhancement package declaration dictionary.

    Performs full normative hand-written validation without third-party dependencies.

    Args:
        data: Parsed package JSON data.

    Returns:
        The validated package dictionary.

    Raises:
        EnhancementPackageError: If validation fails for any reason.
    """
    if not isinstance(data, dict):
        fail("$", f"Package declaration must be an object, got {type(data).__name__}")
    if "published_package" in data:
        return _validate_published_package(data)

    # Check unknown root properties (fail-closed)
    unknown = sorted(set(data.keys()) - ROOT_KEYS)
    if unknown:
        fail("$", f"Unknown properties forbidden: {unknown}")

    # Check schema_version first
    if "schema_version" not in data:
        fail("$", "Missing required property 'schema_version'")
    schema_ver = data["schema_version"]
    if not isinstance(schema_ver, int) or isinstance(schema_ver, bool) or schema_ver != SUPPORTED_SCHEMA_VERSION:
        fail(
            "schema_version",
            f"Incompatible schema version: expected {SUPPORTED_SCHEMA_VERSION}, got {schema_ver!r}",
        )

    # Check all remaining required root properties
    for req_field in ("id", "display_name", "version", "category", "target_title_id", "capabilities"):
        if req_field not in data:
            fail("$", f"Missing required property {req_field!r}")

    # Validate id
    pkg_id = data["id"]
    if not isinstance(pkg_id, str):
        fail("id", f"id must be a string, got {type(pkg_id).__name__}")
    if pkg_id != pkg_id.strip():
        fail("id", "id must not have leading or trailing whitespace")
    if not ID_RE.match(pkg_id):
        fail("id", f"id {pkg_id!r} does not match required pattern {ID_RE.pattern}")

    # Validate display_name
    name = data["display_name"]
    if not isinstance(name, str):
        fail("display_name", f"display_name must be a string, got {type(name).__name__}")
    if name != name.strip():
        fail("display_name", "display_name must not have leading or trailing whitespace")
    if not (1 <= len(name) <= 128):
        fail("display_name", f"display_name length must be between 1 and 128 characters, got {len(name)}")

    # Validate version
    ver = data["version"]
    if not isinstance(ver, str):
        fail("version", f"version must be a string, got {type(ver).__name__}")
    if ver != ver.strip():
        fail("version", "version must not have leading or trailing whitespace")
    if not SEMVER_RE.match(ver):
        fail("version", f"version {ver!r} must be a valid semantic version")

    # Validate category (single canonical validation point)
    category = data["category"]
    if not isinstance(category, str):
        fail("category", f"category must be a string, got {type(category).__name__}")
    if category not in VALID_CATEGORIES:
        fail(
            "category",
            f"Unknown enhancement category: {category!r}; must be one of {sorted(VALID_CATEGORIES)}",
        )

    # Validate target_title_id
    title_id = data["target_title_id"]
    if not isinstance(title_id, str):
        fail("target_title_id", f"target_title_id must be a string, got {type(title_id).__name__}")
    if title_id != title_id.strip():
        fail("target_title_id", "target_title_id must not have leading or trailing whitespace")
    if not TARGET_TITLE_ID_RE.match(title_id):
        fail("target_title_id", f"target_title_id {title_id!r} does not match required pattern")

    # Validate optional min_runtime_version
    if "min_runtime_version" in data:
        min_rt = data["min_runtime_version"]
        if not isinstance(min_rt, str):
            fail("min_runtime_version", f"min_runtime_version must be a string, got {type(min_rt).__name__}")
        if min_rt != min_rt.strip():
            fail("min_runtime_version", "min_runtime_version must not have leading or trailing whitespace")
        if not MIN_RUNTIME_VERSION_RE.match(min_rt):
            fail("min_runtime_version", f"min_runtime_version {min_rt!r} must be a valid semantic version")

    # Validate capabilities
    caps = data["capabilities"]
    if not isinstance(caps, list):
        fail("capabilities", f"capabilities must be an array, got {type(caps).__name__}")
    if len(caps) < 1:
        fail("capabilities", "capabilities array must contain at least 1 item")
    seen_caps: set[str] = set()
    for idx, cap in enumerate(caps):
        cap_path = f"capabilities[{idx}]"
        # Address-literal check applied to capability names
        check_not_address_literal(cap, cap_path, "capability name")
        if not CAPABILITY_RE.match(cap):
            fail(cap_path, f"capability name {cap!r} does not match required pattern {CAPABILITY_RE.pattern}")
        if cap in seen_caps:
            fail(cap_path, f"duplicate capability {cap!r} in capabilities list")
        seen_caps.add(cap)

    # Validate optional hooks (symbol + type only)
    if "hooks" in data:
        hooks = data["hooks"]
        if not isinstance(hooks, list):
            fail("hooks", f"hooks must be an array, got {type(hooks).__name__}")
        for idx, hook in enumerate(hooks):
            hook_path = f"hooks[{idx}]"
            if not isinstance(hook, dict):
                fail(hook_path, f"hook entry must be an object, got {type(hook).__name__}")
            unknown_hook_keys = sorted(set(hook.keys()) - HOOK_KEYS)
            if unknown_hook_keys:
                fail(hook_path, f"Unknown properties forbidden in hook: {unknown_hook_keys}")
            for req in ("symbol", "type"):
                if req not in hook:
                    fail(hook_path, f"Missing required property {req!r} in hook")

            # Address-literal check applied to hook symbols
            symbol = hook["symbol"]
            symbol_path = f"{hook_path}.symbol"
            check_not_address_literal(symbol, symbol_path, "hook symbol")
            if not SYMBOL_RE.match(symbol):
                fail(symbol_path, f"hook symbol {symbol!r} does not match required pattern {SYMBOL_RE.pattern}")

            hook_type = hook["type"]
            type_path = f"{hook_path}.type"
            if not isinstance(hook_type, str):
                fail(type_path, f"hook type must be a string, got {type(hook_type).__name__}")
            if hook_type not in HOOK_TYPES:
                fail(type_path, f"Unknown hook type: {hook_type!r}; must be one of {sorted(HOOK_TYPES)}")

    # Validate timing / frame-rate rescale fields (fail-closed consistency)
    present_timing_keys = sorted(set(data.keys()) & (TIMING_PACKAGE_KEYS | OPTIONAL_TIMING_PACKAGE_KEYS))
    code_hook = category == "guest_memory_code_hook"
    if present_timing_keys:
        if category != "timing_frame_presentation" and not code_hook:
            fail(
                "category",
                f"Fields {present_timing_keys} are only valid for category "
                f"'timing_frame_presentation' or 'guest_memory_code_hook', got {category!r}",
            )
        if code_hook:
            # A code-hook package pins the executable and lists its sites; it has no frame-rate
            # state, so no RNG policy, update boundary or data rewrite.
            extra = sorted(set(present_timing_keys) - {"game_version", "executable_sha256", "sites"})
            if extra:
                fail("category", f"Fields {extra} are not valid for category 'guest_memory_code_hook'")
        required_keys = (("game_version", "executable_sha256", "sites") if code_hook
                         else ("game_version", "executable_sha256", "rng_policy", "sites"))
        for req_timing in required_keys:
            if req_timing not in data:
                fail("$", f"Missing required {'code-hook' if code_hook else 'timing'} package property {req_timing!r}")
        if title_id == "*":
            fail("target_title_id", ("Code-hook package" if code_hook else "Timing rescale package")
                 + " must target a specific title id, not '*'")

        # Validate game_version
        game_ver = data["game_version"]
        if not isinstance(game_ver, str):
            fail("game_version", f"game_version must be a string, got {type(game_ver).__name__}")
        if game_ver != game_ver.strip():
            fail("game_version", "game_version must not have leading or trailing whitespace")
        if not GAME_VERSION_RE.match(game_ver):
            fail("game_version", f"game_version {game_ver!r} does not match required pattern")

        # Validate executable_sha256
        exe_sha = data["executable_sha256"]
        if not isinstance(exe_sha, str):
            fail("executable_sha256", f"executable_sha256 must be a string, got {type(exe_sha).__name__}")
        if not SHA256_HEX_RE.match(exe_sha):
            fail(
                "executable_sha256",
                f"executable_sha256 {exe_sha!r} must be a 64-character lowercase hex SHA-256 digest",
            )

        # Validate rng_policy
        # A guest_memory_code_hook package carries no rng_policy (its keys are refused above).
        if not code_hook:
            rng = data["rng_policy"]
            if not isinstance(rng, dict):
                fail("rng_policy", f"rng_policy must be an object, got {type(rng).__name__}")
            unknown_rng_keys = sorted(set(rng.keys()) - RNG_POLICY_KEYS)
            if unknown_rng_keys:
                fail("rng_policy", f"Unknown properties forbidden in rng_policy: {unknown_rng_keys}")
            for req_rng in ("mode", "reason"):
                if req_rng not in rng:
                    fail("rng_policy", f"Missing required property {req_rng!r} in rng_policy")
            rng_mode = rng["mode"]
            if not isinstance(rng_mode, str) or rng_mode not in RNG_POLICY_MODES:
                fail(
                    "rng_policy.mode",
                    f"Unknown rng_policy mode: {rng_mode!r}; must be one of {sorted(RNG_POLICY_MODES)}",
                )
            rng_reason = rng["reason"]
            if not isinstance(rng_reason, str) or not rng_reason.strip():
                fail("rng_policy.reason", "rng_policy.reason must be a non-empty string")
            if rng_reason != rng_reason.strip():
                fail("rng_policy.reason", "rng_policy.reason must not have leading or trailing whitespace")

        # Validate sites
        sites = data["sites"]
        if not isinstance(sites, list):
            fail("sites", f"sites must be an array, got {type(sites).__name__}")
        if len(sites) < 1:
            fail("sites", "sites array must contain at least 1 site")
        seen_site_ids: set[str] = set()
        seen_positions: set[tuple[str, int]] = set()
        for idx, site in enumerate(sites):
            site_path = f"sites[{idx}]"
            if not isinstance(site, dict):
                fail(site_path, f"site entry must be an object, got {type(site).__name__}")
            unknown_site_keys = sorted(set(site.keys()) - SITE_KEYS)
            if unknown_site_keys:
                fail(site_path, f"Unknown properties forbidden in site: {unknown_site_keys}")
            for req_site in ("id", "function", "insn_offset", "stock_digest", "transform"):
                if req_site not in site:
                    fail(site_path, f"Missing required property {req_site!r} in site")

            site_id = site["id"]
            id_path = f"{site_path}.id"
            check_not_address_literal(site_id, id_path, "site id")
            if not SITE_ID_RE.match(site_id):
                fail(id_path, f"site id {site_id!r} does not match required pattern {SITE_ID_RE.pattern}")
            if site_id in seen_site_ids:
                fail(id_path, f"duplicate site id {site_id!r}")
            seen_site_ids.add(site_id)

            _check_instruction_location(site, site_path, "site")
            fn_name = site["function"]
            insn_offset = site["insn_offset"]
            pos_key = (fn_name, insn_offset)
            if pos_key in seen_positions:
                fail(
                    site_path,
                    f"duplicate site target ({fn_name!r}, insn_offset={insn_offset})",
                )
            seen_positions.add(pos_key)

            if "word_count" in site:
                word_count = site["word_count"]
                wc_path = f"{site_path}.word_count"
                if (
                    not isinstance(word_count, int)
                    or isinstance(word_count, bool)
                    or word_count < 1
                    or word_count > 16
                ):
                    fail(wc_path, f"word_count must be an integer in [1, 16], got {word_count!r}")
            else:
                site["word_count"] = 1

            transform = site["transform"]
            tf_path = f"{site_path}.transform"
            if not isinstance(transform, str) or transform not in SITE_TRANSFORMS:
                fail(
                    tf_path,
                    f"Unknown site transform: {transform!r}; must be one of {sorted(SITE_TRANSFORMS)}",
                )
            if (transform in CODE_HOOK_TRANSFORMS) != code_hook:
                fail(
                    tf_path,
                    f"site transform {transform!r} "
                    + ("is not a code-hook transform; a guest_memory_code_hook package takes only "
                       f"{sorted(CODE_HOOK_TRANSFORMS)}" if code_hook else
                       "acts on guest memory and needs category 'guest_memory_code_hook'"),
                )
            _validate_site_extras(site, site_path)

        site_ids = {site["id"] for site in sites}
        for idx, site in enumerate(sites):
            accel = site.get("acceleration_site")
            if accel is not None:
                if accel not in site_ids or accel == site["id"]:
                    fail(f"sites[{idx}].acceleration_site",
                         f"acceleration_site {accel!r} must name another site of this package")
                target = next(s for s in sites if s["id"] == accel)
                if target["transform"] != "accel_x0.25":
                    fail(f"sites[{idx}].acceleration_site",
                         f"acceleration_site {accel!r} must be an accel_x0.25 site")
            for key in SITE_REFERENCES.get(site["transform"], ()):
                if site[key] not in site_ids or site[key] == site["id"]:
                    fail(f"sites[{idx}].{key}", f"{key} {site[key]!r} must name another site of this package")
        _validate_reference_roles(sites)
        for idx, site in enumerate(sites):
            capability = GUEST_WRITE_CAPABILITIES.get(site["transform"])
            if capability is not None and capability not in seen_caps:
                fail(f"sites[{idx}].transform",
                     f"{site['transform']} writes guest memory: the package must name the capability "
                     f"{capability!r} in capabilities")
        _validate_requires(sites, _ace_part_ids(data))

        # Validate update_boundary: where one logic update begins. Gated sites run on
        # alternate updates counted there, so a package that gates must declare it.
        if "update_boundary" in data:
            boundary = data["update_boundary"]
            if not isinstance(boundary, dict):
                fail("update_boundary", f"update_boundary must be an object, got {type(boundary).__name__}")
            unknown_boundary_keys = sorted(set(boundary.keys()) - UPDATE_BOUNDARY_KEYS)
            if unknown_boundary_keys:
                fail("update_boundary", f"Unknown properties forbidden in update_boundary: {unknown_boundary_keys}")
            for req_boundary in sorted(UPDATE_BOUNDARY_KEYS):
                if req_boundary not in boundary:
                    fail("update_boundary", f"Missing required property {req_boundary!r} in update_boundary")
            _check_instruction_location(boundary, "update_boundary", "update_boundary")
            if (boundary["function"], boundary["insn_offset"]) in seen_positions:
                fail("update_boundary", "update_boundary must not be a rewritten site")
        elif any(site["transform"] in GATE_TRANSFORMS for site in sites):
            fail(
                "update_boundary",
                "A package with gate_30hz, gate_30hz_hold, gate_30hz_alt or gate_30hz_store sites must declare "
                "update_boundary: "
                "gated sites run on "
                "alternate logic updates, counted where each update begins",
            )

        if "data_sites" in data:
            _validate_data_sites(data["data_sites"])
        if "data_files" in data:
            _validate_data_files(data["data_files"])
        if "ace_parts" in data:
            _validate_ace_parts(data, seen_site_ids)

    return data


def _validate_ace_parts(data: Mapping[str, Any], site_ids: set[str]) -> None:
    """``ace_parts`` (contract section 9.8): named parts, each a list of one-word sites.

    Site ids and instruction words share one namespace with ``sites`` and the
    ``update_boundary``: an ace_parts site may not reuse an id or cover a word either
    already covers, may not declare ``requires``, and no ``requires`` may name it. A
    ``load`` part needs an ``update_boundary``, and none of its sites may be in that
    function. Here words and functions are compared by name; a published package's are
    compared again by site-map address (:func:`_check_ace_part_namespace`), and codegen
    compares the resolved addresses.
    """
    parts = data["ace_parts"]
    if not isinstance(parts, dict) or not parts:
        fail("ace_parts", "ace_parts must be a non-empty object of part name -> part")
    unknown_parts = sorted(set(parts) - ACE_PART_NAMES)
    if unknown_parts:
        fail("ace_parts", f"Unknown ace part {unknown_parts}; must be one of {sorted(ACE_PART_NAMES)}")
    if "load" in parts and "update_boundary" not in data:
        # The boundary marks the logic update's function, which no skipped wait may pace;
        # without it nothing would keep a load site out of the update loop.
        fail("update_boundary", "A package with ace_parts.load must declare update_boundary: no "
                                "vblank_wait_skip site may be in the function where a logic update begins")
    for name in sorted(parts):
        part_path = f"ace_parts.{name}"
        part = parts[name]
        _check_keys(part, part_path, ACE_PART_KEYS, ("sites",), "ace part")
        sites = part["sites"]
        if not isinstance(sites, list) or not sites:
            fail(f"{part_path}.sites", "an ace part's sites must be a non-empty array")
        for idx, site in enumerate(sites):
            path = f"{part_path}.sites[{idx}]"
            if isinstance(site, dict) and "requires" in site:
                fail(f"{path}.requires", "an ace_parts site cannot declare requires: it runs on its own "
                                         "SR_ACE key, never on another site's applications")
            _check_keys(site, path, ACE_PART_SITE_KEYS,
                        ("id", "function", "insn_offset", "stock_digest", "transform"), "ace part site")
            _check_id(site["id"], f"{path}.id", site_ids, "site")
            _check_instruction_location(site, path, "site")
            if "word_count" in site:
                word_count = site["word_count"]
                if not isinstance(word_count, int) or isinstance(word_count, bool) or word_count != 1:
                    fail(f"{path}.word_count", f"an ace_parts site is one word: word_count must be 1, "
                                               f"got {word_count!r}")
            else:
                site["word_count"] = 1
            transform = site["transform"]
            if not isinstance(transform, str) or transform not in ACE_PART_TRANSFORMS:
                fail(f"{path}.transform", f"Unknown ace part transform: {transform!r}; must be one of "
                                          f"{sorted(ACE_PART_TRANSFORMS)}")
    _check_ace_part_namespace(data, lambda fn: fn, "by name")


def _check_ace_part_namespace(data: Mapping[str, Any], fn_key: Any, how: str) -> None:
    """Refuse an ``ace_parts`` site on a word that a ``sites`` entry (any of its ``word_count``
    words) or the ``update_boundary`` covers, or on another part site's word, and a
    ``vblank_wait_skip`` site in the boundary's function. ``fn_key`` maps a function name to
    what identifies the function: the name itself, or a published site map's address, so that
    two names for one address are one function. Only that function is checked: nothing walks
    the call graph, so a wait in a caller or a callee of it passes here and rests on the site's
    own evidence (contract section 11.3.10, "What the tools check")."""
    def word(fn: str, offset: int) -> tuple[Any, int]:
        key = fn_key(fn)
        # An address names the word itself; a name stays paired with its offset.
        return (None, key + offset) if isinstance(key, int) else (key, offset)

    covered: dict[tuple[Any, int], str] = {}
    for site in data.get("sites", ()):
        for i in range(int(site.get("word_count", 1))):
            covered.setdefault(word(site["function"], int(site["insn_offset"]) + 4 * i),
                               f"site {site['id']!r}" + (f" (its word {i + 1})" if i else ""))
    boundary = data.get("update_boundary")
    if boundary is not None:
        covered.setdefault(word(boundary["function"], int(boundary["insn_offset"])), "the update_boundary")
    for path, site in ace_part_sites(data):
        pos = word(site["function"], int(site["insn_offset"]))
        if pos in covered:
            fail(path, f"an ace_parts site may not cover a word {covered[pos]} already covers "
                       f"({site['function']!r}, insn_offset={site['insn_offset']}, compared {how})")
        covered[pos] = f"site {site['id']!r}"
        if (site["transform"] == "vblank_wait_skip" and boundary is not None
                and fn_key(site["function"]) == fn_key(boundary["function"])):
            # The wait would sit in the loop that runs the logic update: skipping it would run
            # updates unpaced, so game time and the update counter would run fast.
            fail(path, f"a vblank_wait_skip site may not be in the update_boundary's function "
                       f"{boundary['function']!r} (compared {how}): skipping that wait would run "
                       f"logic updates unpaced")


def _ace_part_ids(data: Mapping[str, Any]) -> set[str]:
    """The site ids an ``ace_parts`` section declares, read before it is validated."""
    ids: set[str] = set()
    parts = data.get("ace_parts")
    if isinstance(parts, dict):
        for part in parts.values():
            sites = part.get("sites") if isinstance(part, dict) else None
            if isinstance(sites, list):
                ids.update(s["id"] for s in sites if isinstance(s, dict) and isinstance(s.get("id"), str))
    return ids


def _validate_site_extras(site: Mapping[str, Any], path: str) -> None:
    """The optional site fields: operand, arguments, acceleration_site and a pinned low word."""
    transform = site["transform"]
    if transform == "argument_x2":
        arguments = site.get("arguments")
        if (not isinstance(arguments, list) or not arguments
                or not all(isinstance(name, str) and name in ARGUMENT_REGISTERS for name in arguments)
                or len(set(arguments)) != len(arguments)):
            fail(f"{path}.arguments",
                 f"argument_x2 needs a list of distinct argument registers from {sorted(ARGUMENT_REGISTERS)}")
        if site["insn_offset"] != 0 or site["word_count"] != 1:
            fail(path, "argument_x2 names the one instruction at the function entry (insn_offset 0)")
    elif "arguments" in site:
        fail(f"{path}.arguments", "arguments belongs only to argument_x2")
    if "operand" in site:
        if site["operand"] not in SITE_OPERANDS:
            fail(f"{path}.operand", f"Unknown operand {site['operand']!r}; must be one of {sorted(SITE_OPERANDS)}")
        if site["word_count"] != 1:
            fail(f"{path}.operand", "operand f64_high names one lui word")
    if transform == "launch_velocity_x0.5":
        if "acceleration_site" not in site:
            fail(path, "launch_velocity_x0.5 needs acceleration_site, the site of its per-update acceleration")
        if not isinstance(site["acceleration_site"], str):
            fail(f"{path}.acceleration_site", "acceleration_site must be a site id")
        if site["word_count"] != 2:
            fail(f"{path}.word_count", "launch_velocity_x0.5 rewrites a lui/ori float pair (word_count 2)")
    elif "acceleration_site" in site:
        fail(f"{path}.acceleration_site", "acceleration_site belongs only to launch_velocity_x0.5")
    if ("low_insn_offset" in site) != ("low_stock_digest" in site):
        fail(path, "low_insn_offset and low_stock_digest go together")
    if "low_insn_offset" in site:
        low = site["low_insn_offset"]
        if (not isinstance(low, int) or isinstance(low, bool) or not 0 <= low <= 65536 or low % 4
                or low == site["insn_offset"]):
            fail(f"{path}.low_insn_offset", "low_insn_offset must be another 4-byte-aligned offset in [0, 65536]")
        digest = site["low_stock_digest"]
        if not isinstance(digest, str) or not SHA256_HEX_RE.match(digest):
            fail(f"{path}.low_stock_digest", "low_stock_digest must be a 64-character lowercase hex SHA-256 digest")
        if site["word_count"] != 1:
            fail(f"{path}.low_insn_offset", "a pinned low word completes a lone lui (word_count 1)")
    if "stock_values" in site:
        values = site["stock_values"]
        if (not isinstance(values, list) or not values
                or not all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in values)):
            fail(f"{path}.stock_values", "stock_values must be a non-empty list of numbers")
    if "stock_value_digests" in site:
        digests = site["stock_value_digests"]
        if "stock_values" in site:
            fail(path, "a store site declares stock_values or stock_value_digests, not both")
        if (not isinstance(digests, list) or not 1 <= len(digests) <= STOCK_VALUE_DIGESTS_MAX
                or not all(isinstance(d, str) and SHA256_HEX_RE.match(d) for d in digests)):
            fail(f"{path}.stock_value_digests",
                 f"stock_value_digests must be a list of 1 to {STOCK_VALUE_DIGESTS_MAX} lowercase hex SHA-256 digests")
        if len(set(digests)) != len(digests):
            fail(f"{path}.stock_value_digests", "stock_value_digests lists a digest twice (duplicate)")
    for key in ("stock_values", "stock_value_digests"):
        # Only a scaling transform rewrites a stored value (swc1, or sw under count_x2): no other site
        # checks values. gate_30hz_store gates a store but never changes the value it stores.
        if key in site and transform not in STORE_VALUE_TRANSFORMS:
            fail(f"{path}.{key}", f"{key} belongs only to a store site under a scaling transform "
                                  f"({', '.join(sorted(STORE_VALUE_TRANSFORMS))})")
    if "requires" in site:
        required = site["requires"]
        if not isinstance(required, list) or not required or not all(isinstance(r, str) for r in required):
            fail(f"{path}.requires", "requires must be a non-empty list of site ids")
        if transform == "value_reference":
            fail(f"{path}.requires", "a value_reference site has no rewrite for requires to hold back")
    _validate_two_update_extras(site, path)
    if transform == "clip_x_widen":
        # Contract section 10.8: after the call at the site returns, the runtime scales the
        # x output row of the 4x4 clip matrices at clip_offsets from the pointer in clip_register.
        reg = site.get("clip_register")
        if not isinstance(reg, str) or reg not in RESULT_REGISTERS:
            fail(f"{path}.clip_register",
                 f"clip_x_widen needs clip_register, one of {sorted(RESULT_REGISTERS)}")
        offsets = site.get("clip_offsets")
        if (not isinstance(offsets, list) or not 1 <= len(offsets) <= 4
                or not all(isinstance(o, int) and not isinstance(o, bool) and 0 <= o <= 65536 and o % 16 == 0
                           for o in offsets)
                or len(set(offsets)) != len(offsets)):
            fail(f"{path}.clip_offsets",
                 "clip_x_widen needs clip_offsets, 1 to 4 distinct 16-byte-aligned offsets in [0, 65536]")
        if site["word_count"] != 1:
            fail(f"{path}.word_count", "clip_x_widen names one jal (word_count 1)")
    elif "clip_register" in site or "clip_offsets" in site:
        fail(path, "clip_register and clip_offsets belong only to clip_x_widen")
    if transform == "gate_30hz_alt":
        alternates = site.get("alternates")
        if (not isinstance(alternates, list) or not 1 <= len(alternates) <= ALT_GATE_MAX_ALTERNATES
                or not all(isinstance(name, str) for name in alternates)):
            fail(f"{path}.alternates",
                 f"gate_30hz_alt needs alternates, a list of 1 to {ALT_GATE_MAX_ALTERNATES} function symbols")
        for name in alternates:
            check_not_address_literal(name, f"{path}.alternates", "alternate symbol")
            if not SYMBOL_RE.match(name):
                fail(f"{path}.alternates", f"alternate {name!r} does not match required pattern {SYMBOL_RE.pattern}")
        if len(set(alternates)) != len(alternates):
            fail(f"{path}.alternates", "alternates lists a function twice (duplicate)")
        if site["word_count"] != 1:
            fail(f"{path}.word_count", "gate_30hz_alt names one jal (word_count 1)")
    elif "alternates" in site:
        fail(f"{path}.alternates", "alternates belongs only to gate_30hz_alt")
    if transform == "gate_30hz_store" and site["word_count"] != 1:
        fail(f"{path}.word_count", "gate_30hz_store names one store (word_count 1)")


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _validate_two_update_extras(site: Mapping[str, Any], path: str) -> None:
    """The fields of gate_30hz_phase, velocity_source_x_kappa, launch_from_rest_60hz and
    value_reference (contract section 9.2): site references, the phase time and the launch velocity."""
    transform = site["transform"]
    wanted = SITE_REFERENCES.get(transform, ())
    for key in sorted(REFERENCE_KEYS):
        if key in wanted:
            if not isinstance(site.get(key), str) or not SITE_ID_RE.match(site[key]):
                fail(f"{path}.{key}", f"{transform} needs {key}, the id of the site that pins that stock value")
        elif key in site:
            users = sorted(tf for tf, keys in SITE_REFERENCES.items() if key in keys)
            fail(f"{path}.{key}", f"{key} belongs only to {', '.join(users)}")
    if transform in SITE_REFERENCES and site["word_count"] != 1:
        fail(f"{path}.word_count", f"{transform} names one instruction (word_count 1)")
    if transform == "value_reference" and site["word_count"] not in (1, 2):
        fail(f"{path}.word_count", "value_reference names one load, or a lui/ori float pair (word_count 2)")
    if transform == "gate_30hz_phase":
        phase = site.get("phase_time")
        if (not isinstance(phase, dict) or set(phase) != {"register", "offset"}
                or not isinstance(phase["register"], str) or phase["register"] not in PHASE_REGISTERS
                or not _is_int(phase["offset"]) or not 0 <= phase["offset"] <= 65532 or phase["offset"] % 4):
            fail(f"{path}.phase_time",
                 f"gate_30hz_phase needs phase_time, an object with register (one of {sorted(PHASE_REGISTERS)}) "
                 "and offset (4-byte-aligned, in [0, 65532]): where the state's own time is read")
        if "odd_phase_condition" in site and not isinstance(site["odd_phase_condition"], bool):
            fail(f"{path}.odd_phase_condition", "odd_phase_condition must be true or false")
    else:
        for key in ("phase_time", "odd_phase_condition"):
            if key in site:
                fail(f"{path}.{key}", f"{key} belongs only to gate_30hz_phase")
    if transform == "launch_from_rest_60hz":
        offset = site.get("velocity_offset")
        if (not isinstance(offset, int) or isinstance(offset, bool)
                or not 0 <= offset <= 65528 or offset % 4):
            fail(f"{path}.velocity_offset",
                 "launch_from_rest_60hz needs velocity_offset, the 4-byte-aligned offset in [0, 65528] of the "
                 "body's two velocity floats from the pointer in $a0")
        if not _is_int(site.get("gravity_axis")) or site["gravity_axis"] not in (0, 1):
            fail(f"{path}.gravity_axis",
                 "launch_from_rest_60hz needs gravity_axis, 0 or 1: which of the two floats gravity acts on")
    else:
        for key in ("velocity_offset", "gravity_axis"):
            if key in site:
                fail(f"{path}.{key}", f"{key} belongs only to launch_from_rest_60hz")


def _validate_reference_roles(sites: Sequence[Mapping[str, Any]]) -> None:
    """A reference names a site whose value no transform changes, and one site pins one quantity."""
    by_id = {site["id"]: site for site in sites}
    roles: dict[str, str] = {}
    for idx, site in enumerate(sites):
        for key in SITE_REFERENCES.get(site["transform"], ()):
            name = site[key]
            path = f"sites[{idx}].{key}"
            transform = by_id[name]["transform"]
            if transform not in REFERENCE_TARGETS[key]:
                fail(path, f"{key} {name!r} names a {transform} site; it must name a "
                           f"{' or '.join(REFERENCE_TARGETS[key])} site, so the value it pins is the one the "
                           f"title uses")
            if roles.setdefault(name, key) != key:
                fail(path, f"site {name!r} is named as {roles[name]} and as {key}: one site pins one quantity")


def _validate_requires(sites: Sequence[Mapping[str, Any]], ace_ids: frozenset[str] | set[str] = frozenset()) -> None:
    """``requires`` names other sites of the package that declare ``stock_values``, never an
    ``ace_parts`` site (``ace_ids``)."""
    by_id = {site["id"]: site for site in sites}
    for idx, site in enumerate(sites):
        required = site.get("requires")
        if required is None:
            continue
        path = f"sites[{idx}].requires"
        if len(set(required)) != len(required):
            fail(path, "requires lists a site id twice (duplicate)")
        for name in required:
            if name == site["id"]:
                fail(path, f"a site cannot require its own id {name!r}")
            if name in ace_ids:
                fail(path, f"requires names {name!r}, an ace_parts site: a 60 Hz site cannot wait "
                           "on an Ace part, which runs only on its own SR_ACE key")
            target = by_id.get(name)
            if target is None or ("stock_values" not in target and "stock_value_digests" not in target):
                fail(path, f"requires names sites that declare stock_values or stock_value_digests; "
                           f"{name!r} is not one")
            if "requires" in target:
                fail(path, f"required site {name!r} itself declares requires; chains are not allowed")


def _check_id(value: Any, path: str, seen: set[str], role: str) -> None:
    check_not_address_literal(value, path, f"{role} id")
    if not SITE_ID_RE.match(value):
        fail(path, f"{role} id {value!r} does not match required pattern {SITE_ID_RE.pattern}")
    if value in seen:
        fail(path, f"duplicate {role} id {value!r}")
    seen.add(value)


def _check_keys(obj: Any, path: str, allowed: frozenset[str], required: Sequence[str], role: str) -> None:
    if not isinstance(obj, dict):
        fail(path, f"{role} entry must be an object, got {type(obj).__name__}")
    unknown = sorted(set(obj.keys()) - allowed)
    if unknown:
        fail(path, f"Unknown properties forbidden in {role}: {unknown}")
    for req in required:
        if req not in obj:
            fail(path, f"Missing required property {req!r} in {role}")


def _check_int(value: Any, path: str, lo: int, hi: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or not lo <= value <= hi:
        fail(path, f"must be an integer in [{lo}, {hi}], got {value!r}")
    return value


def _validate_data_sites(data_sites: Any) -> None:
    if not isinstance(data_sites, list) or not data_sites:
        fail("data_sites", "data_sites must be a non-empty array")
    seen_ids: set[str] = set()
    seen_spans: list[tuple[str, int, int]] = []
    for idx, site in enumerate(data_sites):
        path = f"data_sites[{idx}]"
        _check_keys(site, path, DATA_SITE_KEYS,
                    ("id", "symbol", "offset", "type", "stock_digest", "transform"), "data site")
        _check_id(site["id"], f"{path}.id", seen_ids, "data site")
        symbol = site["symbol"]
        check_not_address_literal(symbol, f"{path}.symbol", "data site symbol")
        if not SYMBOL_RE.match(symbol):
            fail(f"{path}.symbol", f"data site symbol {symbol!r} does not match required pattern {SYMBOL_RE.pattern}")
        dtype = site["type"]
        if dtype not in DATA_TYPES:
            fail(f"{path}.type", f"Unknown data type {dtype!r}; must be one of {sorted(DATA_TYPES)}")
        size = DATA_TYPES[dtype][0]
        offset = _check_int(site["offset"], f"{path}.offset", 0, 1 << 20)
        if offset % size:
            fail(f"{path}.offset", f"offset {offset} is not aligned to its {size}-byte type")
        count = _check_int(site.setdefault("count", 1), f"{path}.count", 1, 256)
        digest = site["stock_digest"]
        if not isinstance(digest, str) or not SHA256_HEX_RE.match(digest):
            fail(f"{path}.stock_digest", "stock_digest must be a 64-character lowercase hex SHA-256 digest")
        if site["transform"] not in DATA_TRANSFORMS:
            fail(f"{path}.transform",
                 f"Transform {site['transform']!r} has no data rescale; must be one of {sorted(DATA_TRANSFORMS)}")
        lo, hi = offset, offset + size * count
        for other_symbol, other_lo, other_hi in seen_spans:
            if other_symbol == symbol and lo < other_hi and other_lo < hi:
                fail(path, f"data site overlaps another data site of {symbol!r}")
        seen_spans.append((symbol, lo, hi))


def _validate_data_files(data_files: Any) -> None:
    if not isinstance(data_files, list) or not data_files:
        fail("data_files", "data_files must be a non-empty array")
    seen_ids: set[str] = set()
    seen_files: set[tuple[str, str]] = set()
    for idx, entry in enumerate(data_files):
        path = f"data_files[{idx}]"
        _check_keys(entry, path, DATA_FILE_KEYS,
                    ("id", "path", "sha256", "format", "header_row", "first_data_row", "cells"),
                    "data file")
        _check_id(entry["id"], f"{path}.id", seen_ids, "data file")
        file_path = entry["path"]
        if not isinstance(file_path, str) or len(file_path) > 255 or not DATA_FILE_PATH_RE.match(file_path):
            fail(f"{path}.path", f"path {file_path!r} must be a relative disc path of plain names separated by '/'")
        digest = entry["sha256"]
        if not isinstance(digest, str) or not SHA256_HEX_RE.match(digest):
            fail(f"{path}.sha256", "sha256 must be a 64-character lowercase hex SHA-256 digest")
        key = (file_path.lower(), digest)
        if key in seen_files:
            fail(path, f"duplicate data file {file_path!r} with the same sha256")
        seen_files.add(key)
        if entry["format"] not in DATA_FILE_FORMATS:
            fail(f"{path}.format", f"Unknown data file format {entry['format']!r}; must be one of {sorted(DATA_FILE_FORMATS)}")
        header_row = _check_int(entry["header_row"], f"{path}.header_row", 0, 255)
        first_data_row = _check_int(entry["first_data_row"], f"{path}.first_data_row", 1, 256)
        if first_data_row <= header_row:
            fail(f"{path}.first_data_row", "first_data_row must come after header_row")
        cells = entry["cells"]
        if not isinstance(cells, list) or not cells:
            fail(f"{path}.cells", "cells must be a non-empty array")
        # Rows each column has claimed so far; None claims every data row.
        claimed: dict[str | int, set[int] | None] = {}
        for cidx, cell in enumerate(cells):
            cpath = f"{path}.cells[{cidx}]"
            _check_keys(cell, cpath, CSV_CELL_KEYS, ("transform",), "csv cell")
            if ("column" in cell) == ("column_index" in cell):
                fail(cpath, "a csv cell names exactly one of column or column_index")
            if "column" in cell:
                column = cell["column"]
                if not isinstance(column, str) or not CSV_COLUMN_RE.match(column):
                    fail(f"{cpath}.column",
                         f"column {column!r} must be an ASCII header name matching {CSV_COLUMN_RE.pattern}")
            else:
                column = _check_int(cell["column_index"], f"{cpath}.column_index", 0, 255)
            if cell["transform"] not in DATA_TRANSFORMS:
                fail(f"{cpath}.transform",
                     f"Transform {cell['transform']!r} has no data rescale; must be one of {sorted(DATA_TRANSFORMS)}")
            if "type" in cell and cell["type"] not in DATA_TYPES:
                fail(f"{cpath}.type", f"Unknown data type {cell['type']!r}; must be one of {sorted(DATA_TYPES)}")
            if "rows" in cell:
                rows = cell["rows"]
                if not isinstance(rows, list) or not rows:
                    fail(f"{cpath}.rows", "rows must be a non-empty array of data-row indexes")
                for ridx, row in enumerate(rows):
                    _check_int(row, f"{cpath}.rows[{ridx}]", 0, 65535)
                if len(set(rows)) != len(rows):
                    fail(f"{cpath}.rows", "rows must not repeat")
            wanted = set(cell["rows"]) if "rows" in cell else None
            if column in claimed:
                taken = claimed[column]
                if taken is None or wanted is None or taken & wanted:
                    fail(cpath, f"column {column!r} is rescaled twice in the same row")
                claimed[column] = taken | wanted
            else:
                claimed[column] = wanted


def compute_stock_words_digest(words: Sequence[int]) -> str:
    """Compute the SHA-256 digest of a sequence of 32-bit stock MIPS instruction words."""
    if not words:
        raise ValueError("words must not be empty")
    raw = b"".join(struct.pack("<I", int(w) & 0xFFFFFFFF) for w in words)
    return hashlib.sha256(raw).hexdigest()


def compute_package_digest(pkg_data: Mapping[str, Any]) -> str:
    """Compute a deterministic canonical SHA-256 digest of a validated package declaration."""
    canonical = json.dumps(pkg_data, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def extract_elf_func_symbols(elf: Any) -> dict[str, int]:
    """Extract named STT_FUNC symbols from an analyze.Elf instance if .symtab is present."""
    symtab = elf.sec(".symtab")
    strtab = elf.sec(".strtab")
    if not symtab or not strtab or not symtab.get("entsz"):
        return {}
    d = elf.data
    str_bytes = d[strtab["off"] : strtab["off"] + strtab["size"]]
    bias = int(elf.base) if getattr(elf, "reloc", None) is not None and elf.base else 0
    out: dict[str, int] = {}
    for i in range(symtab["size"] // symtab["entsz"]):
        o = symtab["off"] + i * symtab["entsz"]
        if o + 16 > len(d):
            break
        st_name, st_value, _st_size, st_info, _st_other, _st_shndx = struct.unpack(
            "<IIIBBH", d[o : o + 16]
        )
        if (st_info & 0xF) == 2 and st_value != 0 and st_name < len(str_bytes):
            end = str_bytes.find(b"\x00", st_name)
            if end > st_name:
                name = str_bytes[st_name:end].decode("ascii", "replace")
                if SYMBOL_RE.match(name) and not is_address_literal(name):
                    out[name] = (st_value + bias) & 0xFFFFFFFF
    return out


def load_symbol_map(path: Path | str) -> dict[str, int]:
    """Load an optional sidecar symbol map JSON (mapping symbolic function name -> vaddr)."""
    sym_path = Path(path)
    if not sym_path.is_file():
        raise EnhancementPackageError(f"Symbol map file not found: {sym_path}")
    try:
        raw = json.loads(sym_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise EnhancementPackageError(f"Failed to parse symbol map JSON {sym_path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise EnhancementPackageError(f"Symbol map root must be a JSON object in {sym_path}")
    table = raw.get("symbols", raw)
    if not isinstance(table, dict):
        raise EnhancementPackageError(f"Symbol map 'symbols' must be an object in {sym_path}")
    out: dict[str, int] = {}
    for k, v in table.items():
        check_not_address_literal(k, f"symbols.{k}", "symbol name")
        if isinstance(v, int) and not isinstance(v, bool):
            out[k] = v & 0xFFFFFFFF
        elif isinstance(v, str):
            try:
                out[k] = int(v, 16 if v.lower().startswith("0x") else 10) & 0xFFFFFFFF
            except ValueError as exc:
                raise EnhancementPackageError(
                    f"Invalid address {v!r} for symbol {k!r} in {sym_path}"
                ) from exc
        else:
            raise EnhancementPackageError(
                f"Invalid address {v!r} for symbol {k!r} in {sym_path}"
            )
    return out


# --- Published packages (docs/ENHANCEMENT_CONTRACT.md section 5.1) ---------------------------
#
# A published package is a package declaration plus its disc, a site map (symbolic name ->
# address in the one executable it names) and stock digests keyed by that executable. It holds
# no game bytes: a plain SHA-256 of one instruction word can be inverted by trying all 2^32
# words, so every stock digest is an HMAC-SHA-256 whose key only a holder of the executable can
# derive. resolve_published_package() checks it against the user's executable and returns the
# ordinary declaration and symbol map that codegen verifies as before.

PUBLISHED_FORMAT_VERSION = 1
PUBLISHED_DIGEST_SCHEME = "hmac-sha256-executable-key-v1"
PUBLISHED_KEYS = frozenset({
    "published_package",
    "disc_id",
    "disc_version",
    "digest_scheme",
    "site_map",
})
# The key is SHA-256 over this tag and the whole executable: never the published
# executable_sha256, which anyone can read.
_PUBLISHED_KEY_TAG = b"nakagawa-recomp published package stock-digest key v1\x00"
# A stock value's digest message starts with this tag (keyed_stock_value_digest).
_STOCK_VALUE_TAG = b"stock value\0"
STOCK_VALUE_DIGESTS_MAX = 64
DISC_ID_RE = re.compile(r"^[A-Z]{4}[0-9]{5}\Z")
DISC_VERSION_RE = re.compile(r"^[0-9]{1,2}\.[0-9]{2}\Z")
SITE_MAP_ADDRESS_RE = re.compile(r"^0x[0-9a-f]{8}\Z")
PUBLISHED_VERSION_RE = re.compile(
    r"^(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})(?:-[0-9A-Za-z]{1,16}(?:\.[0-9A-Za-z]{1,16})?)?$")
PUBLISHED_REASON_MAX = 1024
ADDRESS_SUFFIX_RE = re.compile(r"_([0-9a-fA-F]{8})\Z")
# The refusals a published package names (section 5.1), in the order they are checked.
PUBLISHED_REFUSALS = ("other_disc", "other_revision", "modified_executable", "site_mismatch")
# No string in a published package carries words, bytes or addresses: every string and every
# object key is checked, except the digest slots below and the site map's address values. A
# run of more than 8 hex digits is longer than one 32-bit word or address.
_PUBLISHED_DIGEST_SLOTS = frozenset({"executable_sha256", "stock_digest", "low_stock_digest", "sha256"})
_PUBLISHED_DIGEST_LISTS = frozenset({"stock_value_digests"})
_PUBLISHED_TEXT_REFUSED = (
    (re.compile(r"0[xX][0-9a-fA-F]{5,}"), "an address literal"),
    (re.compile(r"[0-9a-fA-F]{9,}"), "a run of more than 8 hex digits"),
    (re.compile(r"[A-Za-z0-9+/]{32,}={0,2}"), "a base64-like run of 32 or more characters"),
    (re.compile(r"\\x[0-9a-fA-F]{2}"), "an escaped byte"),
    (re.compile(r"(?<![0-9A-Za-z])[0-9a-fA-F]{8}(?![0-9A-Za-z])"),
     "an 8-hex-digit token (an instruction word, or an address outside the site map)"),
    (re.compile(r"[\x00-\x1f\x7f]"), "a control character"),
)


class PublishedPackageRefusal(EnhancementPackageError):
    """A published package refused by name: one of PUBLISHED_REFUSALS."""

    def __init__(self, code: str, message: str) -> None:
        if code not in PUBLISHED_REFUSALS:
            raise ValueError(f"unknown published-package refusal {code!r}")
        super().__init__(f"{code}: {message} (fail-closed refusal)")
        self.code = code


def is_published_package(data: Any) -> bool:
    """Whether a parsed package declares the published form (section 5.1)."""
    return isinstance(data, Mapping) and "published_package" in data


def keyed_stock_value_digest(key: bytes, site_id: str, bits: int) -> str:
    """The keyed digest of one value a store site may store, as the run-time check computes it:
    HMAC-SHA-256 over the tag, the site id, a NUL and the value's 32 bits, little-endian."""
    message = _STOCK_VALUE_TAG + site_id.encode("ascii") + b"\0" + struct.pack("<I", int(bits) & 0xFFFFFFFF)
    return hmac.new(key, message, hashlib.sha256).hexdigest()


def published_digest_key(executable: bytes) -> bytes:
    """The stock-digest key of one executable: only its holder can derive it."""
    return hashlib.sha256(_PUBLISHED_KEY_TAG + bytes(executable)).digest()


def keyed_stock_digest(key: bytes, stock: bytes) -> str:
    """HMAC-SHA-256 of stock bytes under an executable's digest key, lowercase hex."""
    return hmac.new(key, bytes(stock), hashlib.sha256).hexdigest()


def _words_bytes(words: Sequence[int]) -> bytes:
    return b"".join(struct.pack("<I", int(w) & 0xFFFFFFFF) for w in words)


def ace_part_sites(data: Mapping[str, Any]) -> list[tuple[str, Mapping[str, Any]]]:
    """Every ``ace_parts`` site of a validated declaration, with its path, in part-name order."""
    out: list[tuple[str, Mapping[str, Any]]] = []
    for name in sorted(data.get("ace_parts", {})):
        for idx, site in enumerate(data["ace_parts"][name]["sites"]):
            out.append((f"ace_parts.{name}.sites[{idx}]({site['id']})", site))
    return out


def _published_names(data: Mapping[str, Any]) -> set[str]:
    """Every symbolic name a declaration resolves: the names its site map must hold."""
    names: set[str] = set()
    for site in data.get("sites", ()):
        names.add(site["function"])
        names.update(site.get("alternates", ()))
    for _path, site in ace_part_sites(data):
        names.add(site["function"])
    if "update_boundary" in data:
        names.add(data["update_boundary"]["function"])
    for site in data.get("data_sites", ()):
        names.add(site["symbol"])
    for hook in data.get("hooks", ()):
        names.add(hook["symbol"])
    return names


def _validate_published_package(data: dict[str, Any]) -> dict[str, Any]:
    """Validate a published package: its own fields, then its declaration (section 5.1)."""
    for key in sorted(PUBLISHED_KEYS):
        if key not in data:
            fail("$", f"Published package is missing required property {key!r}")
    version = data["published_package"]
    if not isinstance(version, int) or isinstance(version, bool) or version != PUBLISHED_FORMAT_VERSION:
        fail("published_package", f"published_package must be {PUBLISHED_FORMAT_VERSION}, got {version!r}")
    if not isinstance(data["disc_id"], str) or not DISC_ID_RE.match(data["disc_id"]):
        fail("disc_id", f"disc_id {data['disc_id']!r} must be a disc ID such as UCUS12345")
    if not isinstance(data["disc_version"], str) or not DISC_VERSION_RE.match(data["disc_version"]):
        fail("disc_version", f"disc_version {data['disc_version']!r} must be the SFO DISC_VERSION, such as 1.01")
    if data["digest_scheme"] != PUBLISHED_DIGEST_SCHEME:
        fail("digest_scheme", f"digest_scheme must be {PUBLISHED_DIGEST_SCHEME!r}, got {data['digest_scheme']!r}")

    declaration = validate_package({k: v for k, v in data.items() if k not in PUBLISHED_KEYS})
    if "executable_sha256" not in declaration:
        fail("executable_sha256", "A published package names the one executable it applies to")
    # Bounded text: a version carries no build metadata and a short pre-release tag at most.
    if not PUBLISHED_VERSION_RE.fullmatch(declaration["version"]):
        fail("version", f"a published package's version {declaration['version']!r} must match "
                        f"{PUBLISHED_VERSION_RE.pattern} (no build metadata)")
    if "rng_policy" in declaration and len(declaration["rng_policy"]["reason"]) > PUBLISHED_REASON_MAX:
        fail("rng_policy.reason", f"a published package's rng_policy.reason is at most {PUBLISHED_REASON_MAX} characters")
    for idx, site in enumerate(declaration.get("sites", ())):
        if "stock_values" in site:
            fail(f"sites[{idx}].stock_values",
                 "A published package carries no values read from the game (section 5.1)")

    site_map = data["site_map"]
    if not isinstance(site_map, dict) or not site_map:
        fail("site_map", "site_map must be a non-empty object of symbolic name -> address")
    # Two names may share an address (the decomp's name and a site's own): that publishes no other address.
    for name, address in site_map.items():
        check_not_address_literal(name, f"site_map.{name}", "site map name")
        if not SYMBOL_RE.match(name):
            fail(f"site_map.{name}", f"site map name {name!r} does not match required pattern {SYMBOL_RE.pattern}")
        if not isinstance(address, str) or not SITE_MAP_ADDRESS_RE.match(address) or int(address, 16) % 4:
            fail(f"site_map.{name}", f"address {address!r} must be a 4-byte-aligned 0x%08x hex string")
        # A project-assigned name for a function without a decompilation name ends in its own address
        # (hst_fn_0001ea64): an 8-hex suffix is that name's address and no other value.
        suffix = ADDRESS_SUFFIX_RE.search(name)
        if suffix and int(suffix.group(1), 16) != int(address, 16):
            fail(f"site_map.{name}", f"name {name!r} ends in an address other than its own ({address})")
    used = _published_names(declaration)
    missing = sorted(used - set(site_map))
    if missing:
        fail("site_map", f"site_map does not name {missing}: a published package resolves only its own map")
    unused = sorted(set(site_map) - used)
    if unused:
        fail("site_map", f"site_map names {unused}, which no site uses: publish no other addresses")
    if "ace_parts" in declaration:
        # Two names may share an address, so the namespace is checked again by address.
        _check_ace_part_namespace(declaration, lambda fn: int(site_map[fn], 16), "by site-map address")
    return data


def published_package_findings(data: Any) -> list[str]:
    """Why a file may not be published as a package; empty when it may (section 5.1).

    The declaration is validated with every property allowlisted, so a package can only
    carry what the format defines: names, addresses, transforms and keyed digests. Every
    string and object key outside the digest slots and the site map's addresses is then
    checked for words, bytes or addresses written into it.
    """
    if not is_published_package(data):
        return ["not in the published form (published_package, disc_id, disc_version, "
                "digest_scheme, site_map): a private declaration or symbol map is never published"]
    try:
        validate_package(data)
    except EnhancementPackageError as exc:
        return [str(exc)]
    findings: list[str] = []
    # The one 8-hex token allowed: a site-map name's own-address suffix (validated above), wherever
    # that name is used. Anything else in the name is checked like any other string.
    own_suffix = {name for name in data["site_map"] if ADDRESS_SUFFIX_RE.search(name)}
    for where, value in _published_strings(data, "$"):
        if value in own_suffix:
            value = value[:-len("_00000000")]
        for pattern, what in _PUBLISHED_TEXT_REFUSED:
            if pattern.search(value):
                findings.append(f"{where} contains {what}")
    return findings


def _published_strings(value: Any, where: str, slot: str = "") -> Iterator[tuple[str, str]]:
    """Every string and object key in a published package, with its path, except the digests
    in its digest slots and the addresses in its site map (validated by shape already)."""
    if isinstance(value, str):
        if slot not in _PUBLISHED_DIGEST_SLOTS and slot not in _PUBLISHED_DIGEST_LISTS:
            yield where, value
    elif isinstance(value, Mapping):
        for key, item in value.items():
            yield f"{where} key", str(key)
            if where == "$" and key == "site_map":
                yield from ((f"$.site_map key {name}", str(name)) for name in item)
                continue
            yield from _published_strings(item, f"{where}.{key}", str(key))
    elif isinstance(value, list):
        for idx, item in enumerate(value):
            yield from _published_strings(item, f"{where}[{idx}]", slot)


def _find_data_file_bytes(entry: Mapping[str, Any], data_root: Path | str,
                          matches: Callable[[bytes], bool], digest_text: str) -> bytes:
    """The bytes of the data file at the entry's disc path whose bytes ``matches`` accepts."""
    root = Path(data_root)
    want = entry["path"].lower().split("/")
    # Walk by hand: a glob matches names with the host's case rules, and the extracted
    # tree keeps the disc's case on a case-sensitive host.
    for folder, _dirs, files in sorted(os.walk(root)):
        for name in sorted(files):
            if name.lower() != want[-1]:
                continue
            candidate = Path(folder) / name
            parts = [p.lower() for p in candidate.relative_to(root).parts]
            if parts[-len(want):] != want:
                continue
            data = candidate.read_bytes()
            if matches(data):
                return data
    fail(f"data_files({entry['id']})",
         f"No file {entry['path']} with {digest_text} under the title's data (fail-closed refusal)")


def _sha256_matcher(want: str) -> Callable[[bytes], bool]:
    return lambda data: hashlib.sha256(data).hexdigest() == want


def _keyed_matcher(key: bytes, want: str) -> Callable[[bytes], bool]:
    return lambda data: hmac.compare_digest(keyed_stock_digest(key, data), want)


ReadFn = Callable[[int, int], Any]


def elf_readers(elf: Any) -> tuple[ReadFn, ReadFn]:
    """``(read_words, read_data)`` over an ``analyze.Elf`` as codegen reads it.

    ``read_words(vaddr, count)`` returns ``count`` little-endian words or ``None``;
    ``read_data(vaddr, size)`` returns initialised bytes of one allocated,
    non-executable PROGBITS section as the loader places them (relocated, for a
    relocatable image), never code or zero-fill, or ``None``. Segments cannot tell,
    since a title may load code and data in one RWX segment.
    """
    def read_words(vaddr: int, count: int) -> list[int] | None:
        out: list[int] = []
        for idx in range(count):
            raw = elf.read_at_vaddr(vaddr + idx * 4, 4)
            if not raw or len(raw) < 4:
                return None
            out.append(int.from_bytes(raw[:4], "little"))
        return out

    def read_data(vaddr: int, count: int) -> bytes | None:
        rebase = elf.reloc.lo if elf.reloc is not None else 0
        for sec in elf.sections:
            if sec["typ"] != 1 or not sec["flags"] & 2 or sec["flags"] & 4:
                continue
            lo = sec["addr"] + rebase
            if lo <= vaddr and vaddr + count <= lo + sec["size"]:
                raw = elf.read_at_vaddr(vaddr, count)
                return None if raw is None else bytes(raw)
        return None

    return read_words, read_data


def elf_constant_reader(elf: Any) -> ReadFn:
    """``read_const(vaddr, size)`` over an ``analyze.Elf``: initialised bytes of one allocated
    PROGBITS section that is neither writable nor executable, as the loader places them, or
    ``None``. That is data the title cannot change while it runs, which is what a site reference
    may pin (contract section 9.2): a value read here at build time is the value at run time."""
    def read_const(vaddr: int, count: int) -> bytes | None:
        rebase = elf.reloc.lo if elf.reloc is not None else 0
        for sec in elf.sections:
            if sec["typ"] != 1 or not sec["flags"] & 2 or sec["flags"] & 5:
                continue
            lo = sec["addr"] + rebase
            if lo <= vaddr and vaddr + count <= lo + sec["size"]:
                raw = elf.read_at_vaddr(vaddr, count)
                return None if raw is None else bytes(raw)
        return None

    return read_const


def _stock_locations(declaration: Mapping[str, Any],
                     symbols: Mapping[str, int]) -> list[tuple[str, Mapping[str, Any], str, int, int]]:
    """Every stock-digest field of a declaration: (path, owner, field, vaddr, word count)."""
    out: list[tuple[str, Mapping[str, Any], str, int, int]] = []
    for idx, site in enumerate(declaration.get("sites", ())):
        path = f"sites[{idx}]({site['id']})"
        fn = int(symbols[site["function"]])
        out.append((path, site, "stock_digest", fn + int(site["insn_offset"]), int(site.get("word_count", 1))))
        if "low_insn_offset" in site:
            out.append((f"{path}.low", site, "low_stock_digest", fn + int(site["low_insn_offset"]), 1))
    if "update_boundary" in declaration:
        boundary = declaration["update_boundary"]
        out.append(("update_boundary", boundary, "stock_digest",
                    int(symbols[boundary["function"]]) + int(boundary["insn_offset"]), 1))
    for path, site in ace_part_sites(declaration):
        out.append((path, site, "stock_digest",
                    int(symbols[site["function"]]) + int(site["insn_offset"]), 1))
    return out


def _data_site_span(site: Mapping[str, Any], symbols: Mapping[str, int]) -> tuple[int, int]:
    return (int(symbols[site["symbol"]]) + int(site["offset"]),
            DATA_TYPES[site["type"]][0] * int(site["count"]))


def resolve_published_package(
    pkg_data: Mapping[str, Any],
    *,
    executable: bytes,
    read_words_fn: ReadFn,
    read_data_fn: ReadFn | None = None,
    data_root: Path | str | None = None,
) -> tuple[dict[str, Any], dict[str, int]]:
    """Check a published package against the user's executable and return the declaration
    with plain stock digests and its symbol map, for verify_and_resolve_sites().

    Refuses by name (PublishedPackageRefusal): ``modified_executable`` when the executable's
    SHA-256 is not the one the package names, ``site_mismatch`` when any keyed stock digest
    does not match the bytes at its site. Nothing is resolved by search: every name comes
    from the package's own site map.
    """
    validated = validate_package(copy.deepcopy(dict(pkg_data)))
    if not is_published_package(validated):
        fail("published_package", "not a published package")
    actual = hashlib.sha256(bytes(executable)).hexdigest()
    if actual != validated["executable_sha256"]:
        raise PublishedPackageRefusal(
            "modified_executable",
            f"the package is for executable {validated['executable_sha256']}, this one is {actual}")
    key = published_digest_key(executable)
    symbols = {name: int(address, 16) for name, address in validated["site_map"].items()}
    declaration = {k: v for k, v in validated.items() if k not in PUBLISHED_KEYS}

    for path, owner, field, vaddr, count in _stock_locations(declaration, symbols):
        words = read_words_fn(vaddr, count)
        if words is None or len(words) != count:
            raise PublishedPackageRefusal("site_mismatch", f"{path}: no {count} instruction word(s) at its site")
        if not hmac.compare_digest(keyed_stock_digest(key, _words_bytes(words)), owner[field]):
            raise PublishedPackageRefusal("site_mismatch", f"{path}: the stock bytes at its site differ")
        owner[field] = compute_stock_words_digest(words)  # type: ignore[index]

    for idx, site in enumerate(declaration.get("data_sites", ())):
        path = f"data_sites[{idx}]({site['id']})"
        if read_data_fn is None:
            fail(path, "a data site needs the executable's data (fail-closed refusal)")
        vaddr, length = _data_site_span(site, symbols)
        stock = read_data_fn(vaddr, length)
        if stock is None or len(stock) != length:
            raise PublishedPackageRefusal("site_mismatch", f"{path}: no {length} bytes of initialised data")
        if not hmac.compare_digest(keyed_stock_digest(key, bytes(stock)), site["stock_digest"]):
            raise PublishedPackageRefusal("site_mismatch", f"{path}: the stock data at its site differ")
        site["stock_digest"] = hashlib.sha256(bytes(stock)).hexdigest()

    for entry in declaration.get("data_files", ()):
        if data_root is None:
            fail(f"data_files({entry['id']})", "data_files need the title's extracted files (fail-closed refusal)")
        want = entry["sha256"]
        stock = _find_data_file_bytes(entry, data_root, _keyed_matcher(key, want), "its keyed digest")
        entry["sha256"] = hashlib.sha256(stock).hexdigest()

    return validate_package(declaration), symbols


def publish_package(
    pkg_data: Mapping[str, Any],
    symbols: Mapping[str, int],
    *,
    executable: bytes,
    disc_id: str,
    disc_version: str,
    read_words_fn: ReadFn,
    read_data_fn: ReadFn | None = None,
    data_root: Path | str | None = None,
    notes: list[str] | None = None,
    read_const_fn: ReadFn | None = None,
) -> dict[str, Any]:
    """The published form of a private declaration and its symbol map (section 5.1).

    Every plain stock digest is first checked against the executable, then replaced by its
    keyed digest. A store site's stock_values become stock_value_digests, and their
    build-time notes (store_value_overlaps) are appended to ``notes``. The site map holds
    only the names the declaration uses. The result must
    resolve back to the same declaration (resolve_published_package), and it is refused
    if published_package_findings() finds anything.
    """
    declaration = validate_package(copy.deepcopy(dict(pkg_data)))
    if is_published_package(declaration):
        fail("published_package", "the package is already published")
    if "executable_sha256" not in declaration:
        fail("executable_sha256", "A published package names the one executable it applies to")
    actual = hashlib.sha256(bytes(executable)).hexdigest()
    if actual != declaration["executable_sha256"]:
        fail("executable_sha256", f"the package is for {declaration['executable_sha256']}, the executable is {actual}")
    used = sorted(_published_names(declaration))
    missing = [name for name in used if name not in symbols]
    if missing:
        fail("site_map", f"the symbol map does not name {missing}")
    site_map = {name: f"0x{int(symbols[name]) & 0xFFFFFFFF:08x}" for name in used}
    resolved = {name: int(site_map[name], 16) for name in used}
    key = published_digest_key(executable)

    for path, owner, field, vaddr, count in _stock_locations(declaration, resolved):
        words = read_words_fn(vaddr, count)
        if words is None or len(words) != count or compute_stock_words_digest(words) != owner[field]:
            fail(path, f"the {field} does not match the executable's words at its site")
        owner[field] = keyed_stock_digest(key, _words_bytes(words))  # type: ignore[index]
    inputs: dict[str, dict[str, float]] = {}
    if any("stock_values" in site and site["transform"] in SITE_REFERENCES
           for site in declaration.get("sites", ())):
        # A store site scaled by a factor the resolver computes: read the stock values it
        # references from the executable, as verify_and_resolve_sites() does.
        located: dict[str, _Located] = {}
        pair_values: dict[str, float] = {}
        for idx, site in enumerate(declaration["sites"]):
            fn = int(resolved[site["function"]])
            words = [int(w) & 0xFFFFFFFF for w in read_words_fn(fn + int(site["insn_offset"]),
                                                                 int(site.get("word_count", 1)))]
            located[site["id"]] = (site, f"sites[{idx}]({site['id']})", fn, fn + int(site["insn_offset"]), words)
            if "low_insn_offset" in site:
                low = read_words_fn(fn + int(site["low_insn_offset"]), 1)
                pair_values[site["id"]] = _lui_ori_value(site, words[0], int(low[0]) & 0xFFFFFFFF)
            elif len(words) == 2:
                pair_values[site["id"]] = _lui_ori_value(site, words[0], words[1])
        inputs = _reference_inputs(located, pair_values, read_words_fn, read_const_fn)
    for site in declaration.get("sites", ()):
        if "stock_values" not in site:
            continue
        # The values the loader stores are read from the game: only their keyed digests are
        # published. The run-time check digests the value being stored (section 5.1).
        word = read_words_fn(int(resolved[site["function"]]) + int(site["insn_offset"]), 1)
        rewrite = plan_site_rewrite(site, int(word[0]), inputs.get(site["id"]))
        if notes is not None:
            notes.extend(store_value_overlaps(site["id"], rewrite))
        site["stock_value_digests"] = [keyed_stock_value_digest(key, site["id"], bits)
                                       for bits, _scaled in rewrite["stock_values"]]
        del site["stock_values"]
    for idx, site in enumerate(declaration.get("data_sites", ())):
        path = f"data_sites[{idx}]({site['id']})"
        if read_data_fn is None:
            fail(path, "a data site needs the executable's data")
        vaddr, length = _data_site_span(site, resolved)
        stock = read_data_fn(vaddr, length)
        if stock is None or len(stock) != length or hashlib.sha256(bytes(stock)).hexdigest() != site["stock_digest"]:
            fail(path, "the stock_digest does not match the executable's data at its site")
        site["stock_digest"] = keyed_stock_digest(key, bytes(stock))
    for entry in declaration.get("data_files", ()):
        if data_root is None:
            fail(f"data_files({entry['id']})", "data_files need the title's extracted files")
        want = entry["sha256"]
        stock = _find_data_file_bytes(entry, data_root, _sha256_matcher(want), f"sha256 {want}")
        entry["sha256"] = keyed_stock_digest(key, stock)

    published = dict(declaration)
    published.update({
        "published_package": PUBLISHED_FORMAT_VERSION,
        "disc_id": disc_id,
        "disc_version": disc_version,
        "digest_scheme": PUBLISHED_DIGEST_SCHEME,
        "site_map": site_map,
    })
    findings = published_package_findings(published)
    if findings:
        fail("$", "the published form is refused: " + "; ".join(findings))
    return published


# Opcodes that write memory: sb, sh, swl, sw, swr, cache, sc, swc1, sv.s, svl.q/svr.q, sv.q.
_STORE_OPS = frozenset((0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x2F, 0x38, 0x39, 0x3A, 0x3D, 0x3E))
_LOAD_OPS = frozenset((0x20, 0x21, 0x23, 0x24, 0x25))  # lb, lh, lw, lbu, lhu
HOLD_CALLEE_MAX_WORDS = 256
EVEN_COUNTER_SCAN_WORDS = 16
_JR_RA = 0x03E00008


def _branch_target(w: int, pc: int) -> int | None:
    """The target of a conditional branch (likely forms included) at ``pc``, else None."""
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    is_branch = (op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17)
                 or (op == 0x01 and rt in (0x00, 0x01, 0x02, 0x03))
                 or (op in (0x11, 0x12) and rs == 0x08))  # bc1*, the VFPU's bvf/bvt
    if not is_branch:
        return None
    simm16 = w & 0xFFFF
    if simm16 & 0x8000:
        simm16 -= 0x10000
    return (pc + 4 + (simm16 << 2)) & 0xFFFFFFFF


def _leaf_refusal(w: int) -> str | None:
    """Why ``w`` cannot appear in a callee whose call may be skipped, or None."""
    op, rs, rt, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0x3F
    if op in (0x02, 0x03):
        return "jumps or calls"
    if op == 0x00 and fn == 0x09:
        return "calls through a register"
    if op == 0x00 and fn == 0x08 and rs != 31:
        return "jumps through a register"
    if op == 0x00 and fn in (0x0C, 0x0D):
        return "traps (syscall or break)"
    if op == 0x01 and rt in (0x10, 0x11, 0x12, 0x13):
        return "calls (a linking branch)"
    if op in _STORE_OPS:
        return "writes memory"
    if op in (0x10, 0x1C):
        return "uses a system instruction"
    if op == 0x11 and rs == 0x06:
        return "writes an FPU control register (ctc1)"
    return None


def _is_likely_branch(w: int) -> bool:
    """A branch-likely: its delay slot runs only when the branch is taken."""
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    return (op in (0x14, 0x15, 0x16, 0x17)
            or (op == 0x01 and rt in (0x02, 0x03))
            or (op in (0x11, 0x12) and rs == 0x08 and rt & 0x02 != 0))  # bc1fl/bc1tl, bvfl/bvtl


def _fpr_uses(w: int) -> tuple[frozenset[int], frozenset[int]]:
    """(FPU registers read, FPU registers written) by ``w``."""
    op, fmt = w >> 26, (w >> 21) & 31
    ft, fs, fd, fn = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 0x3F
    if op == 0x31:  # lwc1 ft
        return frozenset(), frozenset((ft,))
    if op != 0x11:
        return frozenset(), frozenset()
    if fmt == 0x00:  # mfc1 rt, fs
        return frozenset((fs,)), frozenset()
    if fmt == 0x04:  # mtc1 rt, fs
        return frozenset(), frozenset((fs,))
    if fmt in (0x10, 0x14):  # .s and .w arithmetic, conversions and compares
        if fn >= 0x30:
            return frozenset((fs, ft)), frozenset()
        if fn <= 0x03:
            return frozenset((fs, ft)), frozenset((fd,))
        return frozenset((fs,)), frozenset((fd,))
    return frozenset(), frozenset()


def _gpr_destination(w: int) -> int:
    """The general register ``w`` writes; 0 when it writes none, -1 when this check cannot tell."""
    op, rs, rt, rd, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 0x3F
    if op == 0x00:
        if fn in (0x08, 0x0C, 0x0D, 0x0F, 0x11, 0x13) or 0x18 <= fn <= 0x1F:
            return 0  # jr, syscall, break, sync, mthi/mtlo, mult/div/madd forms write HI/LO
        return rd
    if 0x08 <= op <= 0x0F or op in _LOAD_OPS or op in (0x22, 0x26, 0x30, 0x38):
        return rt
    if op == 0x03:
        return 31
    if op == 0x01:
        return 31 if rt in (0x10, 0x11, 0x12, 0x13) else 0
    if op in (0x02, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17) or op in _STORE_OPS:
        return 0
    if op == 0x11:
        return rt if rs in (0x00, 0x02) else 0  # mfc1, cfc1
    if op == 0x31 or op in (0x32, 0x35, 0x36):
        return 0  # lwc1 and the VFPU loads write no general register
    if op in (0x18, 0x19, 0x1B, 0x34, 0x37, 0x3C, 0x3F):
        return 0  # VFPU arithmetic writes only VFPU registers
    if op == 0x12:
        return rt if rs == 0x03 else 0  # mfv / mfvc write rt; the rest of COP2 writes none
    if op == 0x1F:
        return rd if fn == 0x20 else rt if fn in (0x00, 0x04) else -1  # bshfl, ext, ins
    return -1


def _is_jump(w: int) -> bool:
    """An unconditional jump or call: j, jal, jr, jalr, or a linking branch."""
    op, rt, fn = w >> 26, (w >> 16) & 31, w & 0x3F
    return (op in (0x02, 0x03) or (op == 0x00 and fn in (0x08, 0x09))
            or (op == 0x01 and rt in (0x10, 0x11, 0x12, 0x13)))


def _read_word(read_words_fn: Any, addr: int) -> int | None:
    got = read_words_fn(addr, 1)
    if not got:
        return None
    return int(got[0]) & 0xFFFFFFFF


def _check_hold_callee(site_path: str, site_addr: int, word: int, read_words_fn: Any) -> None:
    """A gate_30hz_hold call may be skipped only when skipping it drops nothing but its result.

    The callee, read from its entry to its ``jr $ra`` and that delay slot, makes no call,
    jump or trap, writes no memory, branches only inside that body, reads ``f12`` before it
    writes it, and writes ``f0``: the value the hold then takes from ``f12``.
    """
    target = ((site_addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    where = f"gate_30hz_hold callee 0x{target:08x}"
    body: list[int] = []
    for i in range(HOLD_CALLEE_MAX_WORDS):
        w = _read_word(read_words_fn, target + 4 * i)
        if w is None:
            fail(site_path, f"{where} cannot be read at 0x{target + 4 * i:08x} (fail-closed refusal)")
        body.append(w)
        if w == _JR_RA:
            slot = _read_word(read_words_fn, target + 4 * (i + 1))
            if slot is None:
                fail(site_path, f"{where} cannot be read at its return's delay slot (fail-closed refusal)")
            body.append(slot)
            break
    else:
        fail(site_path, f"{where} has no jr $ra in its first {HOLD_CALLEE_MAX_WORDS} instructions "
                        f"(fail-closed refusal)")
    ret = target + 4 * (len(body) - 2)  # the jr $ra; the word after it is its delay slot
    f12_first: str | None = None
    writes_f0 = False
    for i, w in enumerate(body):
        pc = target + 4 * i
        why = _leaf_refusal(w)
        if why is not None:
            fail(site_path, f"{where} {why} at 0x{pc:08x}: a skipped call must drop nothing but its "
                            f"result (fail-closed refusal)")
        written = _gpr_destination(w)
        if written in (29, 31, -1):
            what = {29: "writes $sp", 31: "writes $ra", -1: f"has a word this check cannot read (0x{w:08x})"}
            fail(site_path, f"{where} {what[written]} at 0x{pc:08x}: a skipped call must leave the "
                            f"caller's frame and return as the call would (fail-closed refusal)")
        branch = _branch_target(w, pc)
        if branch is not None and not target <= branch <= ret:
            # A branch to the return's delay slot would run the slot and then fall through
            # past the body this check reads.
            fail(site_path, f"{where} branches from 0x{pc:08x} to 0x{branch:08x}, outside the body "
                            f"up to its jr $ra (fail-closed refusal)")
        reads, writes = _fpr_uses(w)
        if f12_first is None and 12 in reads:
            f12_first = "read"
        elif f12_first is None and 12 in writes:
            f12_first = "written"
        writes_f0 = writes_f0 or 0 in writes
    if f12_first != "read":
        fail(site_path, f"{where} does not read f12 before it writes it: the hold keeps f12 as the "
                        f"value the call advances (fail-closed refusal)")
    if not writes_f0:
        fail(site_path, f"{where} does not write f0, the result the hold replaces (fail-closed refusal)")


def _check_even_counter(site_path: str, fn_addr: int, site_addr: int, word: int, read_words_fn: Any) -> None:
    """count_x2_even compares a counter: its register is loaded from memory in the
    straight-line code just before the compare, with no call or branch in between."""
    rs = (word >> 21) & 31
    lo = max(fn_addr, site_addr - 4 * EVEN_COUNTER_SCAN_WORDS)
    pc = site_addr - 4
    while pc >= lo:
        w = _read_word(read_words_fn, pc)
        if w is None:
            break
        written = _gpr_destination(w)
        if written == rs:
            if (w >> 26) in _LOAD_OPS:
                prev = _read_word(read_words_fn, pc - 4) if pc > fn_addr else None
                if prev is not None and (_is_jump(prev) or _is_likely_branch(prev)):
                    # A call's slot runs before the callee, which may change the counter; a
                    # branch-likely skips its slot on the path that falls through to the compare.
                    fail(site_path, f"count_x2_even's counter ${rs} is loaded at 0x{pc:08x}, in the delay "
                                    f"slot of 0x{prev:08x}: the load does not reach the compare on every "
                                    f"path (fail-closed refusal)")
                return
            fail(site_path, f"count_x2_even's counter ${rs} is computed at 0x{pc:08x} (0x{w:08x}), not "
                            f"loaded from memory: the site must compare a stored count (fail-closed refusal)")
        if written == -1:
            fail(site_path, f"count_x2_even cannot tell whether 0x{w:08x} at 0x{pc:08x} writes the counter "
                            f"${rs} (fail-closed refusal)")
        if _branch_target(w, pc) is not None or _is_jump(w):
            break
        pc -= 4
    fail(site_path, f"count_x2_even's counter ${rs} is not loaded from memory in the straight-line code "
                    f"before the compare (fail-closed refusal)")


# --- gate_30hz_alt: the static proof that the alternates stand for the skipped update ----------
#
# Registers are numbered in one space here: general registers 0-31, FPU registers 64-95, HI 96,
# LO 97 and the FPU condition flag 98. VFPU registers are not tracked.
ALT_GATE_MAX_ALTERNATES = 4
GATE_TRANSFORMS = frozenset(("gate_30hz", "gate_30hz_hold", "gate_30hz_alt", "gate_30hz_store"))
ALT_GATE_MAX_FUNCTION_WORDS = 8192
ALT_GATE_MAX_TREE_FUNCTIONS = 64
ALT_GATE_MAX_TABLE_ENTRIES = 256
ALT_GATE_MAX_CALL_TREE = 2048
ALT_GATE_INPUT_DEPTH = 3
_F = 64
_HI, _LO, _FCC = 96, 97, 98
_CALL_CLOBBERED = frozenset(range(1, 16)) | frozenset((24, 25, 31))
#: Inputs an alternate must not read: it is passed only a0 (EABI arguments 2-8, f12-f15, the flag).
_ALT_INPUTS = frozenset((5, 6, 7, 8, 9, 10, 11)) | frozenset(_F + n for n in (12, 13, 14, 15)) | {_FCC}
#: What a skipped call leaves behind: the caller must not read these before writing them.
_CALL_RESULTS = frozenset((2, 3, _F, _F + 1, _F + 2, _F + 3, _HI, _LO))
_RETURN_READS = frozenset((2, 3, _F))
_EXIT = -1


def _reg_name(r: int) -> str:
    names = ("zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 "
             "s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra").split()
    if r < 32:
        return f"${names[r]}"
    return {_HI: "HI", _LO: "LO", _FCC: "the FPU condition flag"}.get(r, f"f{r - _F}")


def _uses(w: int) -> tuple[frozenset[int], frozenset[int]] | None:
    """(registers read, registers written) by ``w`` in the numbering above; None when unknown."""
    op, rs, rt, rd, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 0x3F

    def g(*regs: int) -> frozenset[int]:
        return frozenset(r for r in regs if r != 0)

    if op == 0x00:
        if fn in (0x00, 0x02, 0x03):
            return g(rt), g(rd)
        if fn in (0x04, 0x06, 0x07) or 0x20 <= fn <= 0x27 or fn in (0x2A, 0x2B, 0x2C, 0x2D):
            return g(rs, rt), g(rd)
        if fn == 0x08:
            return g(rs), frozenset()
        if fn == 0x09:
            return g(rs), g(rd)
        if fn in (0x0A, 0x0B):
            return g(rs, rt, rd), g(rd)
        if fn in (0x0C, 0x0D, 0x0F):
            return frozenset(), frozenset()
        if fn in (0x10, 0x12):
            return frozenset((_HI if fn == 0x10 else _LO,)), g(rd)
        if fn in (0x11, 0x13):
            return g(rs), frozenset((_HI if fn == 0x11 else _LO,))
        if fn in (0x16, 0x17):
            return g(rs), g(rd)
        if 0x18 <= fn <= 0x1B:
            return g(rs, rt), frozenset((_HI, _LO))
        if fn in (0x1C, 0x1D, 0x2E, 0x2F):
            return g(rs, rt) | {_HI, _LO}, frozenset((_HI, _LO))
        return None
    if op == 0x01:
        if rt in (0x00, 0x01, 0x02, 0x03):
            return g(rs), frozenset()
        if rt in (0x10, 0x11, 0x12, 0x13):
            return g(rs), frozenset((31,))
        return None
    if op == 0x02:
        return frozenset(), frozenset()
    if op == 0x03:
        return frozenset(), frozenset((31,))
    if op in (0x04, 0x05, 0x14, 0x15):
        return g(rs, rt), frozenset()
    if op in (0x06, 0x07, 0x16, 0x17):
        return g(rs), frozenset()
    if 0x08 <= op <= 0x0E:
        return g(rs), g(rt)
    if op == 0x0F:
        return frozenset(), g(rt)
    if op == 0x11:
        fs, fd = rd, (w >> 6) & 31
        if rs == 0x00:
            return frozenset((_F + fs,)), g(rt)
        if rs == 0x02:
            return (frozenset((_FCC,)) if fs == 31 else frozenset()), g(rt)
        if rs == 0x04:
            return g(rt), frozenset((_F + fs,))
        if rs == 0x06:
            return g(rt), (frozenset((_FCC,)) if fs == 31 else frozenset())
        if rs == 0x08:
            return frozenset((_FCC,)), frozenset()
        if rs in (0x10, 0x14):
            if fn >= 0x30:
                return frozenset((_F + fs, _F + rt)), frozenset((_FCC,))
            if fn <= 0x03:
                return frozenset((_F + fs, _F + rt)), frozenset((_F + fd,))
            return frozenset((_F + fs,)), frozenset((_F + fd,))
        return None
    if op == 0x12:
        if rs == 0x03:
            return frozenset(), g(rt)
        if rs == 0x07:
            return g(rt), frozenset()
        if rs == 0x08:
            return frozenset(), frozenset()
        return None
    if op == 0x1F:
        if fn == 0x00:
            return g(rs), g(rt)
        if fn == 0x04:
            return g(rs, rt), g(rt)
        if fn == 0x20:
            return g(rt), g(rd)
        return None
    if op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x30):
        return g(rs), g(rt)
    if op in (0x22, 0x26):
        return g(rs, rt), g(rt)
    if op in (0x28, 0x29, 0x2A, 0x2B, 0x2E):
        return g(rs, rt), frozenset()
    if op == 0x2F:
        return g(rs), frozenset()
    if op == 0x38:
        return g(rs, rt), g(rt)
    if op == 0x31:
        return g(rs), frozenset((_F + rt,))
    if op == 0x39:
        return g(rs) | {_F + rt}, frozenset()
    if op in (0x32, 0x35, 0x36, 0x3A, 0x3D, 0x3E):
        return g(rs), frozenset()
    if op in (0x18, 0x19, 0x1B, 0x34, 0x37, 0x3C, 0x3F):
        return frozenset(), frozenset()
    return None


def _function_end(fn_addr: int, known_sorted: Sequence[int], where: str, site_path: str) -> int:
    """The end of the window a function's code is read in: ALT_GATE_MAX_FUNCTION_WORDS from its entry.

    A body is the code reachable from its entry, not the run up to the next known start: codegen's
    known starts include entries inside a function (the arms of its table switches), and its own
    bodies run through them."""
    if fn_addr not in known_sorted:
        fail(site_path, f"{where} is not a known function start (fail-closed refusal)")
    return fn_addr + 4 * ALT_GATE_MAX_FUNCTION_WORDS


class _LazyWords(dict):  # type: ignore[type-arg]
    """Guest words read on demand, one at a time; ``get`` gives None for an unreadable word."""

    def __init__(self, read_words_fn: Any) -> None:
        super().__init__()
        self._read = read_words_fn

    def __missing__(self, addr: int) -> int:
        got = self._read(addr, 1)
        if not got:
            raise KeyError(addr)
        value = int(got[0]) & 0xFFFFFFFF
        self[addr] = value
        return value

    def get(self, addr: int, default: Any = None) -> Any:  # type: ignore[override]
        try:
            return self[addr]
        except KeyError:
            return default


def _jump_table(pc: int, w: int, fn_addr: int, words: Mapping[int, int], read_words_fn: Any,
                where: str, site_path: str) -> list[int]:
    """The targets of ``jr rX`` at ``pc``: a bounded table switch, or a refusal.

    The recognised form is ``sltiu rB, rI, N`` then ``beq``/``beql rB, $zero`` (the out-of-range branch), and
    in the straight-line code up to the ``jr`` a table address built from ``lui``/``addiu`` constants
    plus ``rI << 2``, loaded with ``lw``. Any other write to a register the form uses refuses it.
    """
    reg = (w >> 21) & 31
    start = None
    p = pc - 4
    while p >= max(fn_addr, pc - 4 * 16):
        b = words.get(p)
        prev = words.get(p - 4)
        if (b is not None and prev is not None and (b >> 26) in (0x04, 0x14) and (b >> 16) & 31 == 0
                and (prev >> 26) == 0x0B and (prev >> 16) & 31 == (b >> 21) & 31):
            start = p - 4
            break
        p -= 4
    if start is None:
        fail(site_path, f"{where} jumps through ${reg} at 0x{pc:08x} with no bounded table switch before it "
                        f"(fail-closed refusal)")
    sltiu = words[start]
    index, count = (sltiu >> 21) & 31, sltiu & 0xFFFF
    if not 0 < count <= ALT_GATE_MAX_TABLE_ENTRIES:
        fail(site_path, f"{where} has a table switch of {count} entries at 0x{pc:08x} (fail-closed refusal)")
    val: dict[int, tuple[str, int]] = {index: ("index", 0)}
    # A beql's slot runs only on the out-of-range path; a beq's slot runs on the path to the table too.
    first = start + 12 if (words[start + 4] >> 26) == 0x14 else start + 8
    for q in range(first, pc, 4):
        x = words[q]
        op, rs, rt, rd, fn = x >> 26, (x >> 21) & 31, (x >> 16) & 31, (x >> 11) & 31, x & 0x3F
        simm = (x & 0xFFFF) - 0x10000 if x & 0x8000 else x & 0xFFFF
        dest, new = None, None
        if op == 0x0F:
            dest, new = rt, ("const", (x & 0xFFFF) << 16)
        elif op == 0x09 and val.get(rs, ("", 0))[0] in ("const", "addr"):
            dest, new = rt, (val[rs][0], (val[rs][1] + simm) & 0xFFFFFFFF)
        elif op == 0x00 and fn == 0x00 and (x >> 6) & 31 == 2 and val.get(rt) == ("index", 0):
            dest, new = rd, ("index4", 0)
        elif op == 0x00 and fn == 0x21:
            kinds = sorted((val.get(rs, ("", 0)), val.get(rt, ("", 0))))
            if kinds[0][0] == "const" and kinds[1][0] == "index4":
                dest, new = rd, ("addr", kinds[0][1])
        elif op == 0x23 and val.get(rs, ("", 0))[0] == "addr":
            dest, new = rt, ("entry", (val[rs][1] + simm) & 0xFFFFFFFF)
        uses = _uses(x)
        if new is None and uses is not None:
            for r in uses[1]:
                val.pop(r, None)
        elif new is None:
            val.clear()
        if dest is not None and dest != 0:
            val[dest] = new  # type: ignore[assignment]
    kind, base = val.get(reg, ("", 0))
    if kind != "entry":
        fail(site_path, f"{where} jumps through ${reg} at 0x{pc:08x}, which this check cannot read as a "
                        f"table switch (fail-closed refusal)")
    table = read_words_fn(base, count)
    if not table or len(table) != count:
        fail(site_path, f"{where} has a switch table at 0x{base:08x} this check cannot read (fail-closed refusal)")
    return [int(t) & 0xFFFFFFFF for t in table]


class _Graph:
    """The instruction graph of one function: each node is an address; a transfer's delay slot is
    its own node, whose successors are the transfer's destinations. ``_EXIT`` follows a return."""

    def __init__(self, fn_addr: int, end: int, read_words_fn: Any, where: str, site_path: str,
                 known_sorted: Sequence[int] = ()) -> None:
        self.fn, self.end, self.where = fn_addr, end, where
        starts = frozenset(known_sorted)
        self.words = _LazyWords(read_words_fn)
        if self.words.get(fn_addr) is None:
            fail(site_path, f"{where} cannot be read (fail-closed refusal)")
        self.succ: dict[int, list[int]] = {}
        self.calls: dict[int, int | None] = {}  # delay-slot node -> callee (None through a register)
        self.slots: set[int] = set()
        # Only instructions reachable from the entry are decoded, so alignment padding or data
        # after the last return cannot refuse a function.
        todo = [fn_addr]
        while todo:
            pc = todo.pop()
            if pc in self.slots:
                fail(site_path, f"{where} branches into the delay slot at 0x{pc:08x} (fail-closed refusal)")
            if pc == _EXIT or pc in self.succ:
                continue
            if not fn_addr <= pc < end:
                fail(site_path, f"{where} runs or transfers to 0x{pc:08x}, more than "
                                f"{ALT_GATE_MAX_FUNCTION_WORDS} instructions from its entry (fail-closed refusal)")
            if len(self.succ) >= ALT_GATE_MAX_FUNCTION_WORDS:
                fail(site_path, f"{where} has more than {ALT_GATE_MAX_FUNCTION_WORDS} reachable instructions "
                                f"(fail-closed refusal)")
            w = self.words.get(pc)
            if w is None:
                fail(site_path, f"{where} cannot be read at 0x{pc:08x} (fail-closed refusal)")
            op, rs, rt, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0x3F
            slot = pc + 4
            branch = _branch_target(w, pc)
            if branch is None and not _is_jump(w):
                self.succ[pc] = [] if op == 0x00 and fn == 0x0D else [pc + 4]  # break never returns
                todo.extend(self.succ[pc])
                continue
            if slot >= end:
                fail(site_path, f"{where} ends inside the delay slot of 0x{pc:08x} (fail-closed refusal)")
            if slot in self.succ:
                fail(site_path, f"{where} branches into the delay slot at 0x{slot:08x} (fail-closed refusal)")
            s = self.words.get(slot)
            if s is None:
                fail(site_path, f"{where} cannot be read at 0x{slot:08x} (fail-closed refusal)")
            if _branch_target(s, slot) is not None or _is_jump(s):
                fail(site_path, f"{where} has a transfer in a delay slot at 0x{slot:08x} (fail-closed refusal)")
            self.slots.add(slot)
            likely = _is_likely_branch(w)
            if branch is not None:
                always = (op == 0x04 and rs == 0 and rt == 0) or (op == 0x01 and rt == 0x01 and rs == 0)
                self.succ[pc] = [slot] + ([pc + 8] if likely else [])
                self.succ[slot] = [branch] + ([] if always or likely else [pc + 8])
            elif op == 0x02:
                dest = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                self.succ[pc] = [slot]
                if not fn_addr <= dest < end and dest in starts:
                    self.calls[slot] = dest  # a tail call: the callee returns for this function
                    self.succ[slot] = [_EXIT]
                else:
                    self.succ[slot] = [dest]
            elif op == 0x03 or (op == 0x00 and fn == 0x09):
                self.succ[pc], self.succ[slot] = [slot], [pc + 8]
                self.calls[slot] = ((((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)) if op == 0x03
                                    else None)
            elif op == 0x00 and fn == 0x08 and rs == 31:
                self.succ[pc], self.succ[slot] = [slot], [_EXIT]
            elif op == 0x00 and fn == 0x08:
                dests = _jump_table(pc, w, fn_addr, self.words, read_words_fn, where, site_path)
                self.succ[pc], self.succ[slot] = [slot], sorted(set(dests))
            else:
                fail(site_path, f"{where} has a linking branch at 0x{pc:08x} (fail-closed refusal)")
            todo.extend(n for n in self.succ[pc] if n != slot)
            todo.extend(self.succ[slot])

    def reach(self, starts: Sequence[int], *, avoid: frozenset[int] = frozenset(),
              within: set[int] | None = None) -> set[int]:
        seen: set[int] = set()
        todo = [s for s in starts if s not in avoid and (within is None or s in within)]
        while todo:
            n = todo.pop()
            if n in seen:
                continue
            seen.add(n)
            for m in self.succ.get(n, ()):
                if m not in avoid and m not in seen and (within is None or m in within):
                    todo.append(m)
        return seen

    def returning(self) -> set[int]:
        """Nodes from which a path reaches the return."""
        preds: dict[int, list[int]] = {}
        for n, nxt in self.succ.items():
            for m in nxt:
                preds.setdefault(m, []).append(n)
        seen = {_EXIT}
        todo = [_EXIT]
        while todo:
            for p in preds.get(todo.pop(), ()):
                if p not in seen:
                    seen.add(p)
                    todo.append(p)
        return seen


def _move_source(w: int) -> int | None:
    """The source register of a register move (``addu``/``or``/``addiu 0``), else None."""
    op, rs, rt, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0x3F
    if op == 0x00 and fn in (0x21, 0x25, 0x20):
        if rt == 0:
            return rs
        if rs == 0:
            return rt
    if op in (0x08, 0x09) and w & 0xFFFF == 0:
        return rs
    return None


def _entry_a0_at_calls(graph: _Graph, site_path: str) -> dict[int, bool]:
    """For each call slot: whether $a0 holds the function's entry $a0 there on every path."""
    return _entry_a0_flow(graph)[1]


def _entry_a0_flow(graph: _Graph) -> tuple[dict[int, frozenset[int]], dict[int, bool]]:
    """Which general registers hold the function's entry $a0 before each instruction, on every
    path, and for each call slot whether $a0 does when the call is made."""
    every = frozenset(range(1, 32))
    state: dict[int, frozenset[int]] = {graph.fn: frozenset((4,))}
    at_call: dict[int, bool] = {}
    todo = [graph.fn]
    while todo:
        n = todo.pop()
        cur = state[n]
        w = graph.words[n]
        uses = _uses(w)
        src = _move_source(w)
        out: frozenset[int]
        if uses is None:
            out = frozenset()  # an unreadable word: nothing is known to hold the entry $a0 after it
        else:
            dest = uses[1]
            out = cur - dest
            if src is not None and src in cur:
                out = out | (dest & every)
        if n in graph.calls:
            at_call[n] = at_call.get(n, True) and 4 in out
            out = out - _CALL_CLOBBERED
        for m in graph.succ.get(n, ()):
            if m == _EXIT:
                continue
            old = state.get(m)
            new = out if old is None else old & out
            if old is None or new != old:
                state[m] = new
                todo.append(m)
    return state, at_call


def _check_alt_gate_target(site_path: str, target: int, alternates: Sequence[int], read_words_fn: Any,
                           known_sorted: Sequence[int]) -> None:
    """The gated function calls each alternate exactly once on every path that returns, in list
    order, and passes it the function's own entry $a0."""
    where = f"gate_30hz_alt target 0x{target:08x}"
    graph = _Graph(target, _function_end(target, known_sorted, where, site_path), read_words_fn, where, site_path,
                   known_sorted)
    returning = graph.returning()
    if graph.fn not in returning:
        fail(site_path, f"{where} has no path that returns (fail-closed refusal)")
    a0_ok = _entry_a0_at_calls(graph, site_path)
    nodes: list[int] = []
    for alt in alternates:
        slots = [slot for slot, callee in graph.calls.items() if callee == alt]
        if len(slots) != 1:
            fail(site_path, f"{where} calls alternate 0x{alt:08x} directly {len(slots)} times; it must call it "
                            f"exactly once, so the alternate is part of the update the gate skips "
                            f"(fail-closed refusal)")
        node = slots[0]
        if _EXIT in graph.reach([graph.fn], avoid=frozenset((node,)), within=returning):
            fail(site_path, f"{where} has a path that returns without calling alternate 0x{alt:08x} "
                            f"(fail-closed refusal)")
        if node in graph.reach(graph.succ[node], within=returning):
            fail(site_path, f"{where} can call alternate 0x{alt:08x} more than once on a path that returns "
                            f"(fail-closed refusal)")
        if not a0_ok.get(node, False):
            fail(site_path, f"{where} does not pass alternate 0x{alt:08x} its own entry $a0 on every path "
                            f"(fail-closed refusal)")
        nodes.append(node)
    for i, first in enumerate(nodes):
        for later in nodes[i + 1:]:
            if later in graph.reach([graph.fn], avoid=frozenset((first,)), within=returning):
                fail(site_path, f"{where} can reach a later alternate before 0x{alternates[i]:08x}: the "
                                f"alternates must run in the order the target calls them (fail-closed refusal)")


def _check_caller_ignores_results(site_path: str, fn_addr: int, site_addr: int, read_words_fn: Any,
                                  known_sorted: Sequence[int], transform: str = "gate_30hz_alt") -> None:
    """After a gated call, no path of the caller reads $v0, $v1, f0-f3, HI or LO before writing it,
    up to its next call; a return counts as reading $v0, $v1 and f0."""
    where = f"{transform} caller 0x{fn_addr:08x}"
    graph = _Graph(fn_addr, _function_end(fn_addr, known_sorted, where, site_path), read_words_fn, where,
                   site_path, known_sorted)
    seen: set[tuple[int, frozenset[int]]] = set()
    todo: list[tuple[int, frozenset[int]]] = [(site_addr + 8, frozenset())]
    while todo:
        n, written = todo.pop()
        if (n, written) in seen:
            continue
        seen.add((n, written))
        if n == _EXIT:
            read = _RETURN_READS - written
            if read:
                fail(site_path, f"{where} returns after the gated call without writing "
                                f"{_reg_name(min(read))}: a skipped call leaves it stale (fail-closed refusal)")
            continue
        if n not in graph.words:
            fail(site_path, f"{where} ends before 0x{n:08x}, after the gated call (fail-closed refusal)")
        uses = _uses(graph.words[n])
        if uses is None:
            fail(site_path, f"{where} has a word this check cannot read at 0x{n:08x} after the gated call "
                            f"(fail-closed refusal)")
        read = (uses[0] & _CALL_RESULTS) - written
        if read:
            fail(site_path, f"{where} reads {_reg_name(min(read))} at 0x{n:08x} after the gated call: a "
                            f"skipped call leaves it stale (fail-closed refusal)")
        if n in graph.calls:
            continue
        nxt_written = written | (uses[1] & _CALL_RESULTS)
        for m in graph.succ.get(n, ()):
            todo.append((m, nxt_written))


def _alt_inputs(site_path: str, fn: int, entry: frozenset[int], read_words_fn: Any, known_sorted: Sequence[int],
                summaries: dict[tuple[int, frozenset[int]], frozenset[int] | None],
                depth: int = 0) -> frozenset[int]:
    """Which of ``entry`` (registers still holding the alternate's entry values) function ``fn``
    may read. A call is followed only while some of them are still unwritten, and only to
    ``ALT_GATE_INPUT_DEPTH`` calls below the alternate: past that, and through a register below
    it, a callee is taken to read only the arguments its caller set (the PSP ABI)."""
    key = (fn, entry)
    if key in summaries:
        got = summaries[key]
        return entry if got is None else got  # None: still being analysed (a cycle)
    if len(summaries) >= ALT_GATE_MAX_TREE_FUNCTIONS:
        fail(site_path, f"gate_30hz_alt alternates pass their unset inputs through more than "
                        f"{ALT_GATE_MAX_TREE_FUNCTIONS} calls (fail-closed refusal)")
    summaries[key] = None
    where = f"gate_30hz_alt alternate call 0x{fn:08x}"
    graph = _Graph(fn, _function_end(fn, known_sorted, where, site_path), read_words_fn, where, site_path,
                   known_sorted)
    state: dict[int, frozenset[int]] = {graph.fn: entry}
    reads: set[int] = set()
    todo = [graph.fn]
    while todo:
        n = todo.pop()
        cur = state[n]
        uses = _uses(graph.words[n])
        if uses is None:
            reads |= cur
            out = cur
        else:
            reads |= uses[0] & cur
            out = cur - uses[1]
        if n in graph.calls and out and depth < ALT_GATE_INPUT_DEPTH:
            callee = graph.calls[n]
            if callee is None and depth == 0:
                names = ", ".join(_reg_name(r) for r in sorted(out))
                fail(site_path, f"{where} calls through a register at 0x{n - 4:08x} while {names} may still hold "
                                f"the alternate's entry values (fail-closed refusal)")
            if callee is not None:
                reads |= _alt_inputs(site_path, callee, out, read_words_fn, known_sorted, summaries, depth + 1)
        for m in graph.succ.get(n, ()):
            if m == _EXIT:
                continue
            old = state.get(m)
            new = out if old is None else old | out
            if old is None or new != old:
                state[m] = new
                todo.append(m)
    summaries[key] = frozenset(reads)
    return summaries[key]  # type: ignore[return-value]


def _direct_call_tree(site_path: str, root: int, read_words_fn: Any, known_sorted: Sequence[int]) -> set[int]:
    """Every function reachable from ``root`` through direct calls (``jal``) and tail jumps (``j`` to a
    known start more than the read window away). Each body is the code reachable from its entry
    through branches, jumps and fall-through; after a ``jr`` through a register that is not a table
    switch this check can read, every word up to the next known start is taken as reachable, and
    calls through a register are not followed."""
    starts = sorted(set(known_sorted))
    tree: set[int] = set()
    todo = [root]
    while todo:
        fn = todo.pop()
        if fn in tree:
            continue
        if len(tree) >= ALT_GATE_MAX_CALL_TREE:
            fail(site_path, f"gate_30hz_alt alternate 0x{root:08x} reaches more than {ALT_GATE_MAX_CALL_TREE} "
                            f"functions (fail-closed refusal)")
        tree.add(fn)
        where = f"gate_30hz_alt alternate call tree function 0x{fn:08x}"
        end = _function_end(fn, known_sorted, where, site_path)
        words = _LazyWords(read_words_fn)
        seen: set[int] = set()
        pcs = [fn]
        while pcs:
            pc = pcs.pop()
            if pc in seen:
                continue
            if not fn <= pc < end or len(seen) >= ALT_GATE_MAX_FUNCTION_WORDS:
                fail(site_path, f"{where} runs more than {ALT_GATE_MAX_FUNCTION_WORDS} instructions from its "
                                f"entry (fail-closed refusal)")
            w = words.get(pc)
            if w is None:
                fail(site_path, f"{where} cannot be read at 0x{pc:08x} (fail-closed refusal)")
            seen.add(pc)
            op, rs, fnc = w >> 26, (w >> 21) & 31, w & 0x3F
            dest = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            branch = _branch_target(w, pc)
            transfer = op in (0x02, 0x03) or branch is not None or (op == 0x00 and fnc in (0x08, 0x09))
            if transfer:
                seen.add(pc + 4)  # the delay slot runs with the transfer; it has no successor of its own
            if op == 0x03:
                todo.append(dest)
                pcs.append(pc + 8)
            elif op == 0x02:
                if dest in starts and not fn <= dest < end:
                    todo.append(dest)
                else:
                    pcs.append(dest)
            elif branch is not None:
                pcs += [pc + 8, branch]
            elif op == 0x00 and fnc == 0x09:
                pcs.append(pc + 8)
            elif op == 0x00 and fnc == 0x08:
                if rs != 31:
                    try:
                        pcs += _jump_table(pc, w, fn, words, read_words_fn, where, site_path)
                    except EnhancementPackageError:
                        nxt = bisect.bisect_right(starts, pc)
                        stop = starts[nxt] if nxt < len(starts) else end
                        pcs += range(pc + 8, min(stop, end), 4)
            elif not (op == 0x00 and fnc == 0x0D):
                pcs.append(pc + 4)
    return tree


def _check_alt_gate(site_path: str, fn_addr: int, site_addr: int, word: int, alternates: Sequence[int],
                    read_words_fn: Any, known_sorted: Sequence[int], gated_functions: Mapping[int, str]) -> None:
    """Every static condition of a gate_30hz_alt site (contract section 9.2)."""
    target = ((site_addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    for alt in alternates:
        if alt == target:
            fail(site_path, "an alternate cannot be the gated function itself (fail-closed refusal)")
    _check_alt_gate_target(site_path, target, alternates, read_words_fn, known_sorted)
    _check_caller_ignores_results(site_path, fn_addr, site_addr, read_words_fn, known_sorted)
    summaries: dict[tuple[int, frozenset[int]], frozenset[int] | None] = {}
    for alt in alternates:
        inputs = _alt_inputs(site_path, alt, _ALT_INPUTS, read_words_fn, known_sorted, summaries)
        if inputs:
            names = ", ".join(_reg_name(r) for r in sorted(inputs))
            fail(site_path, f"gate_30hz_alt alternate 0x{alt:08x} may read {names} as its caller left them; "
                            f"an alternate is passed only $a0 (fail-closed refusal)")
        for f in sorted(_direct_call_tree(site_path, alt, read_words_fn, known_sorted)):
            if f in (target, fn_addr):
                fail(site_path, f"gate_30hz_alt alternate 0x{alt:08x} reaches 0x{f:08x}, the "
                                f"{'gated function' if f == target else 'caller'}: an alternate must not "
                                f"re-enter the update it stands for (fail-closed refusal)")
            if f in gated_functions:
                fail(site_path, f"gate_30hz_alt alternate 0x{alt:08x} reaches 0x{f:08x}, which holds gated "
                                f"site {gated_functions[f]}: an alternate runs only on the updates its gate "
                                f"skips, so a gate inside it would never run (fail-closed refusal)")


def _resolve_alternates(pending: Sequence[tuple[Mapping[str, Any], str, int, int, list[int]]],
                        resolved_by_addr: dict[int, dict[str, Any]], symbol_to_addr: Mapping[str, int],
                        read_words_fn: Any, known_functions: Sequence[int]) -> None:
    """Resolve and check each gate_30hz_alt site's alternates, once every site is resolved."""
    alt_sites = [p for p in pending if p[0]["transform"] == "gate_30hz_alt"]
    if not alt_sites:
        return
    known_sorted = sorted(set(int(a) for a in known_functions))
    gated_functions: dict[int, str] = {}
    for entry in resolved_by_addr.values():
        if entry["rewrite"]["kind"] == "gate":
            gated_functions.setdefault(entry["fn_addr"], entry["id"])
    for site, site_path, fn_addr, site_addr, stock_words in alt_sites:
        addrs: list[int] = []
        for name in site["alternates"]:
            if name not in symbol_to_addr:
                fail(f"{site_path}.alternates", f"alternate {name!r} is not in the symbol map (fail-closed refusal)")
            addrs.append(int(symbol_to_addr[name]) & 0xFFFFFFFF)
        if len(set(addrs)) != len(addrs):
            fail(f"{site_path}.alternates", "two alternates name the same function (fail-closed refusal)")
        others = {f: sid for f, sid in gated_functions.items() if sid != site["id"]}
        _check_alt_gate(site_path, fn_addr, site_addr, stock_words[0], addrs, read_words_fn, known_sorted, others)
        resolved_by_addr[site_addr]["rewrite"]["alternates"] = addrs


# --- Stock values a site references, and gate_30hz_phase's static checks (section 9.2) --------

REFERENCE_SCAN_WORDS = 16
PHASE_COMPARE_SCAN_WORDS = 4
PHASE_STORE_SCAN_WORDS = 16

#: One located site: (site, path, function address, site address, stock words).
_Located = tuple[Mapping[str, Any], str, int, int, list[int]]


def _falls_through(w: int, pc: int) -> bool:
    """A conditional branch that is not a likely form and not always taken: on the path that falls
    through it, its delay slot and the code before it have both run."""
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if _branch_target(w, pc) is None or _is_likely_branch(w):
        return False
    return not ((op == 0x04 and rs == rt) or (op == 0x01 and rt == 0x01 and rs == 0))


def _loaded_float_address(path: str, name: str, fn_addr: int, site_addr: int, word: int,
                          read_words_fn: Any) -> int:
    """The address an ``lwc1`` or ``lv.s`` at ``site_addr`` loads from, when its base register is a
    constant built in the straight-line code before it: a ``lui``, then at most one
    ``addiu``/``ori`` of the same register.

    Delay slots are read as the code they are. A load that is itself a delay slot runs after
    everything before its branch, jump or call. A conditional branch that is not a likely form is
    passed: on the path that falls through to the load, its delay slot and the code before it
    both ran, so a ``lui`` in that slot counts. Any other transfer ends the scan: after a jump, a
    call or a branch-likely the code that follows is not reached with that transfer's slot run.

    The taken path of a branch is checked too, where this scan can see it: a branch or jump in
    the instructions read whose target lies at or before the load, and whose taken path skips
    the ``lui`` or the ``addiu``/``ori`` (they lie after its delay slot and before its target),
    reaches the load by a second path around the code that builds the address, and refuses.
    Read backward in address order, like _check_even_counter; a branch from farther away into
    those instructions is not seen."""
    base = (word >> 21) & 31
    imm = word & (0xFFFC if word >> 26 == 0x32 else 0xFFFF)  # lv.s keeps a register bit in its low bits
    offset = imm - 0x10000 if imm & 0x8000 else imm
    low: int | None = None
    low_pc: int | None = None
    lo = max(fn_addr, site_addr - 4 * REFERENCE_SCAN_WORDS)
    pc = site_addr - 4
    while pc >= lo and base != 0:
        w = _read_word(read_words_fn, pc)
        if w is None:
            break
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        transfer = _branch_target(w, pc) is not None or _is_jump(w)
        if transfer and pc != site_addr - 4 and not _falls_through(w, pc):
            break
        written = _gpr_destination(w)
        if written == -1:
            fail(path, f"site {name!r}: cannot tell whether 0x{w:08x} at 0x{pc:08x} writes the base "
                       f"register of its load (fail-closed refusal)")
        if written == base:
            prev = _read_word(read_words_fn, pc - 4) if pc > fn_addr else None
            if (prev is not None and (_branch_target(prev, pc - 4) is not None or _is_jump(prev))
                    and not _falls_through(prev, pc - 4)):
                break  # the slot of a jump, a call or a branch-likely: not run on the way to the load
            if op == 0x0F:
                built = (pc,) if low_pc is None else (pc, low_pc)
                _refuse_second_path(path, name, lo, built, site_addr, read_words_fn)
                return (((w & 0xFFFF) << 16) + (low or 0) + offset) & 0xFFFFFFFF
            if op in (0x09, 0x0D) and rs == base and rt == base and low is None:
                low = _sx16(w & 0xFFFF) if op == 0x09 else w & 0xFFFF
                low_pc = pc
            else:
                fail(path, f"site {name!r}: the base register of its load is computed at 0x{pc:08x} "
                           f"(0x{w:08x}), not a constant address (fail-closed refusal)")
        pc -= 4
    fail(path, f"site {name!r} does not load from a constant address: no lui of its base register in the "
               f"straight-line code before it (fail-closed refusal)")


def _refuse_second_path(path: str, name: str, lo: int, built: tuple[int, ...], site_addr: int,
                        read_words_fn: Any) -> None:
    """No branch or jump among the instructions read before a referenced load (from ``lo``) is
    taken to the load, or before it, around an instruction in ``built`` (the ``lui`` and the
    ``addiu``/``ori`` that build its address). A taken transfer runs its delay slot and skips
    the instructions from the one after that slot up to its target, so only those are compared:
    a branch with the ``lui`` in its slot, or one past code that does not build the address, is
    two paths that both build it."""
    for pc in range(lo, site_addr, 4):
        w = _read_word(read_words_fn, pc)
        if w is None:
            continue
        target = _branch_target(w, pc)
        if target is None and w >> 26 == 0x02:
            target = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        if target is not None and target <= site_addr and any(pc + 8 <= a < target for a in built):
            fail(path, f"site {name!r}: the transfer at 0x{pc:08x} can land at 0x{target:08x}, inside the code "
                       f"that builds its load's address: the load is reached by two paths (fail-closed refusal)")


def _referenced_float(path: str, name: str, located: Mapping[str, _Located],
                      pair_values: Mapping[str, float], read_words_fn: Any,
                      read_const_fn: Any) -> tuple[float, int | None]:
    """The stock float the site ``name`` pins, and the address of its word when it is data: the
    constant its ``lui``/``ori`` builds, or the word of read-only data its ``lwc1`` or ``lv.s``
    loads."""
    address: int | None = None
    if name in pair_values:
        value = pair_values[name]
    else:
        _site, _path, fn_addr, site_addr, words = located[name]
        if len(words) != 1 or words[0] >> 26 not in (0x31, 0x32):
            fail(path, f"site {name!r} does not pin a float: it must be a lui/ori pair (word_count 2, or a "
                       f"lone lui with low_insn_offset), or an lwc1 or lv.s of read-only data "
                       f"(fail-closed refusal)")
        address = _loaded_float_address(path, name, fn_addr, site_addr, words[0], read_words_fn)
        if read_const_fn is None:
            fail(path, f"site {name!r} loads its float from the executable's data, which this resolver "
                       f"was not given (fail-closed refusal)")
        raw = read_const_fn(address, 4)
        if raw is None or len(raw) != 4:
            fail(path, f"site {name!r} loads from 0x{address:08x}, which is not read-only initialised data "
                       f"of the executable: a value the title can change while it runs cannot be pinned "
                       f"(fail-closed refusal)")
        value = struct.unpack("<f", bytes(raw))[0]
    if not math.isfinite(value):
        fail(path, f"site {name!r} pins a float that is not finite (fail-closed refusal)")
    return float(value), address


def _reference_inputs(located: Mapping[str, _Located], pair_values: Mapping[str, float],
                      read_words_fn: Any, read_const_fn: Any,
                      data_words: dict[str, int] | None = None) -> dict[str, dict[str, float]]:
    """For each site that references stock values: what its plan needs, computed from them.
    Every value_reference site is read, referenced or not: one that pins nothing refuses.
    ``data_words`` receives, by site id, the address of each pinned float that is a word of data."""
    cache: dict[str, float] = {}

    def pinned(path: str, name: str) -> float:
        if name not in cache:
            cache[name], address = _referenced_float(path, name, located, pair_values, read_words_fn,
                                                     read_const_fn)
            if address is not None and data_words is not None:
                data_words[name] = address
        return cache[name]

    out: dict[str, dict[str, float]] = {}
    for site, site_path, _fn_addr, _site_addr, _words in located.values():
        if site["transform"] == "value_reference":
            pinned(site_path, site["id"])
        keys = SITE_REFERENCES.get(site["transform"])
        if not keys:
            continue
        values = {key: pinned(f"{site_path}.{key}", site[key]) for key in keys}
        inputs: dict[str, float] = {}
        try:
            if "damping_site" in values:
                kappa, q = two_update_factors(values["damping_site"], values["damping_step_site"],
                                              values.get("step_site"))
                inputs["kappa"] = kappa
                if q is not None:
                    inputs["gravity_offset"] = q * values["gravity_site"]
            else:
                # A gate counts half steps: the state's time must be a sum this model can follow.
                phase_runs(values["step_site"] / 2.0)
                inputs["half_step"] = values["step_site"] / 2.0
        except ValueError as exc:
            fail(site_path, f"{exc} (fail-closed refusal)")
        out[site["id"]] = inputs
    return out


def _check_data_sites_leave_pinned_words(validated: Mapping[str, Any], data_words: Mapping[str, int],
                                         symbol_to_addr: Mapping[str, int]) -> None:
    """No data site rescales a word a site reference pins: a value the package changes at load is
    not the stock value the title then uses. The step is the stated exception (contract section
    9.2): a word named only as ``step_site`` may be halved, by a site or by a data site."""
    steps = {site["step_site"] for site in validated["sites"] if "step_site" in site}
    for idx, data_site in enumerate(validated.get("data_sites", ())):
        if data_site["symbol"] not in symbol_to_addr:
            continue  # verify_and_resolve_data_sites() refuses it by name
        lo, length = _data_site_span(data_site, symbol_to_addr)
        for name, address in sorted(data_words.items()):
            if name not in steps and lo < address + 4 and address < lo + length:
                fail(f"data_sites[{idx}]({data_site['id']})",
                     f"rescales the word at 0x{address:08x} that site {name!r} pins: a pinned value must be "
                     f"the one the title uses (fail-closed refusal)")


def _check_straight_line_site(site_path: str, fn_addr: int, site_addr: int, read_words_fn: Any,
                              what: str) -> None:
    """The site is not the delay slot of a branch, a jump or a call."""
    prev = _read_word(read_words_fn, site_addr - 4) if site_addr > fn_addr else None
    if prev is not None and (_branch_target(prev, site_addr - 4) is not None or _is_jump(prev)):
        fail(site_path, f"{what} is in the delay slot of 0x{prev:08x} at 0x{site_addr - 4:08x} "
                        f"(fail-closed refusal)")


def _check_phase_register(site_path: str, graph: _Graph, site_addr: int, reg: int, is_call: bool) -> None:
    """``reg`` is written exactly once before the site, on every path from the function's entry.

    Of the instructions that can run before the site, along any path of the function's graph,
    exactly one writes ``reg``, and no path from the entry reaches the site without running it. So
    the register holds that one write's value there, never its caller's and never a second
    write's. A write that only follows the site (the epilogue's restore) is not counted. Calls
    are taken to preserve s0-s7 (the PSP ABI)."""
    name = _reg_name(reg)

    def writes(n: int) -> bool:
        w = graph.words[n]
        uses = _uses(w)
        if uses is not None:
            return reg in uses[1]
        dest = _gpr_destination(w)
        if dest == -1:
            fail(site_path, f"{graph.where} has a word this check cannot read at 0x{n:08x} (0x{w:08x}): it "
                            f"may write {name} (fail-closed refusal)")
        return dest == reg

    if site_addr not in graph.succ or site_addr in graph.slots:
        fail(site_path, f"{graph.where} does not reach the site at 0x{site_addr:08x} as an instruction of its "
                        f"own (fail-closed refusal)")
    if is_call and writes(site_addr + 4):
        fail(site_path, f"the delay slot of the gated call writes {name}, the register its phase time is "
                        f"read from (fail-closed refusal)")
    preds: dict[int, list[int]] = {}
    for n, nxt in graph.succ.items():
        for m in nxt:
            preds.setdefault(m, []).append(n)
    before: set[int] = set()
    todo = [site_addr]
    while todo:
        for p in preds.get(todo.pop(), ()):
            if p not in before:
                before.add(p)
                todo.append(p)
    writers = sorted(n for n in before if writes(n))
    if len(writers) != 1:
        at = ", ".join(f"0x{a:08x}" for a in writers) or "none"
        fail(site_path, f"{graph.where} writes {name} {len(writers)} times on its paths to the site ({at}); "
                        f"exactly one write must come before it, so the register holds one known value "
                        f"there (fail-closed refusal)")
    if site_addr in graph.reach([graph.fn], avoid=frozenset(writers)):
        fail(site_path, f"{graph.where} reaches the site on a path that does not write {name} first, at "
                        f"0x{writers[0]:08x}: the register would hold its caller's value (fail-closed refusal)")


def _check_phase_compare(site_path: str, graph: _Graph, site_addr: int) -> None:
    """The flag the compare sets is next read by a ``bc1f``/``bc1t`` in the instructions right after
    it, with no call, jump, other branch or other write of the flag in between."""
    pc = site_addr + 4
    for _ in range(PHASE_COMPARE_SCAN_WORDS):
        w = graph.words.get(pc)
        if w is None:
            fail(site_path, f"{graph.where} cannot be read at 0x{pc:08x}, after the compare (fail-closed refusal)")
        op, rs, fn = w >> 26, (w >> 21) & 31, w & 0x3F
        if op == 0x11 and rs == 0x08:
            return
        if _is_jump(w) or _branch_target(w, pc) is not None:
            fail(site_path, f"a call, jump or branch at 0x{pc:08x} follows the compare before any bc1f or bc1t "
                            f"reads its flag (fail-closed refusal)")
        if op == 0x11 and (rs == 0x06 or (rs in (0x10, 0x14) and fn >= 0x30)):
            fail(site_path, f"the instruction at 0x{pc:08x} writes the FPU condition flag before any bc1f or "
                            f"bc1t reads the compare's (fail-closed refusal)")
        pc += 4
    fail(site_path, f"no bc1f or bc1t reads the compare's flag in the {PHASE_COMPARE_SCAN_WORDS} instructions "
                    f"after it (fail-closed refusal)")


def _check_phase_gate(site_path: str, fn_addr: int, site_addr: int, rewrite: Mapping[str, Any],
                      read_words_fn: Any, known_functions: Sequence[int]) -> None:
    """Every static condition of a gate_30hz_phase site (contract section 9.2)."""
    known_sorted = sorted(set(int(a) for a in known_functions))
    where = f"gate_30hz_phase function 0x{fn_addr:08x}"
    graph = _Graph(fn_addr, _function_end(fn_addr, known_sorted, where, site_path), read_words_fn, where,
                   site_path, known_sorted)
    is_call = rewrite["kind"] == "gate_phase"
    _check_phase_register(site_path, graph, site_addr, int(rewrite["reg"]), is_call)
    if is_call:
        _check_caller_ignores_results(site_path, fn_addr, site_addr, read_words_fn, known_sorted,
                                      "gate_30hz_phase")
    elif rewrite["kind"] == "gate_phase_cond":
        _check_phase_compare(site_path, graph, site_addr)
    else:
        _check_gated_load(site_path, graph, site_addr, rewrite)


def _check_gated_load(site_path: str, graph: _Graph, site_addr: int, rewrite: Mapping[str, Any]) -> None:
    """A gated load is not the load of the state's own time, and the title does not store the
    gated register before it overwrites it.

    The zero is for an effect the value scales. Zeroing the time itself would feed the gate its
    own output, and a store of the register would write to guest memory a zero the stock code
    never stores there. Read forward in address order from the load to the first branch, jump or
    call and its delay slot, PHASE_STORE_SCAN_WORDS instructions at most: a ``swc1`` of a gated
    FPU register, an ``sv.s`` of a gated VFPU register, or a store of a general register that an
    ``mfc1`` or ``mfv`` copied it to, refuses. A later ``lwc1``, ``lv.s`` or FPU result into the
    register ends the scan. Copies through other FPU or VFPU registers, and an ``sv.q``, are not
    followed: every other use of the gated value is the site's evidence."""
    word = graph.words[site_addr]
    base = (word >> 21) & 31
    load_offset = _sx16(word & (0xFFFC if word >> 26 == 0x32 else 0xFFFF))
    if base == rewrite["reg"] and load_offset == rewrite["offset"]:
        fail(site_path, "the gated load is the load of the state's own time: the gate would zero what it "
                        "counts (fail-closed refusal)")
    fpr = rewrite.get("value_reg")
    vreg = rewrite.get("value_vreg")
    copies: set[int] = set()
    pc, left, last = site_addr + 4, PHASE_STORE_SCAN_WORDS, None
    while left > 0:
        w = graph.words.get(pc)
        if w is None:
            return
        op, rs, rt, fs = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        stored = ((op == 0x39 and rt == fpr) or (op == 0x3A and (rt | ((w & 3) << 5)) == vreg)
                  or (op in (0x28, 0x29, 0x2B) and rt in copies))
        if stored:
            fail(site_path, f"the instruction at 0x{pc:08x} stores the gated register: after an odd number of "
                            f"updates it would write a zero the stock code never stores (fail-closed refusal)")
        copied = (op == 0x11 and rs == 0x00 and fs == fpr) or (op == 0x12 and rs == 0x03 and (w & 0x7F) == vreg)
        written = _gpr_destination(w)
        if copied:
            copies.add(rt)
        elif written in copies:
            copies.discard(written)
        if (fpr is not None and fpr in _fpr_uses(w)[1]) or (op == 0x32 and (rt | ((w & 3) << 5)) == vreg):
            return  # the register holds another value from here on
        if last is not None:
            return  # that was the delay slot of the first transfer
        if _branch_target(w, pc) is not None or _is_jump(w):
            last = pc
        pc, left = pc + 4, left - 1


def _store_span(w: int) -> tuple[int, int, int] | None:
    """``(base register, offset, size)`` of a word, float or VFPU store; None for anything else."""
    op, base = w >> 26, (w >> 21) & 31
    if op in (0x2B, 0x39):  # sw, swc1
        return base, _sx16(w & 0xFFFF), 4
    if op in (0x3A, 0x3E):  # sv.s, sv.q: the low two bits belong to the register
        return base, _sx16(w & 0xFFFC), 4 if op == 0x3A else 16
    return None


def _check_launch_callee(site_path: str, site_addr: int, word: int, offset: int, read_words_fn: Any,
                         known_functions: Sequence[int]) -> None:
    """The function a launch_from_rest_60hz site calls stores both velocity floats itself, at
    ``offset`` and ``offset + 4`` from its entry ``$a0``, on every path that returns.

    That is what lets a timing transform write them: after the call it replaces, at the same
    place, a value the stock logic has just stored (contract section 9.2). A store counts when
    its base register holds the entry ``$a0`` on every path (copies are followed, as for
    gate_30hz_alt); a store made by a function the callee calls does not."""
    known_sorted = sorted(set(int(a) for a in known_functions))
    target = ((site_addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    where = f"launch_from_rest_60hz callee 0x{target:08x}"
    graph = _Graph(target, _function_end(target, known_sorted, where, site_path), read_words_fn, where, site_path,
                   known_sorted)
    returning = graph.returning()
    if graph.fn not in returning:
        fail(site_path, f"{where} has no path that returns (fail-closed refusal)")
    holders, _at_call = _entry_a0_flow(graph)
    for at in (offset, offset + 4):
        stores: set[int] = set()
        for n in graph.succ:
            span = _store_span(graph.words[n])
            if span is not None and span[0] in holders.get(n, ()) and span[1] <= at and at + 4 <= span[1] + span[2]:
                stores.add(n)
        if _EXIT in graph.reach([graph.fn], avoid=frozenset(stores), within=returning):
            fail(site_path, f"{where} can return without storing the float at {at} from its entry $a0: a "
                            f"launch map may only replace a value the stock code itself stores there "
                            f"(fail-closed refusal)")


def verify_and_resolve_sites(
    pkg_data: Mapping[str, Any],
    *,
    executable_sha256: str,
    symbol_to_addr: Mapping[str, int],
    read_words_fn: Any,
    known_functions: Sequence[int] = (),
    read_const_fn: Any = None,
) -> dict[int, dict[str, Any]]:
    """Verify package executable and site stock-word digests fail-closed.

    Args:
        pkg_data: Validated package dict.
        executable_sha256: Actual lowercase hex SHA-256 of the guest executable.
        symbol_to_addr: Mapping of symbolic function name -> function entry vaddr.
        read_words_fn: Callable ``(vaddr, count) -> Sequence[int] | None`` reading
            contiguous 32-bit little-endian instruction words from the guest ELF.
        known_functions: Sorted sequence of known function start vaddrs in the ELF.
            Used as a deterministic fallback when ``symbol_to_addr`` does not map a
            symbol and exactly one function in the ELF matches ``(insn_offset, stock_digest)``.
        read_const_fn: Callable ``(vaddr, size) -> bytes | None`` reading read-only initialised
            data of the executable (elf_constant_reader), for a referenced site whose float is
            loaded from data.

    Returns:
        Mapping of resolved instruction ``vaddr -> site dict`` (augmented with
        ``fn_addr``, ``site_addr``, and ``stock_words``).

    Raises:
        EnhancementPackageError: If the executable digest, any symbol resolution, or
            any site stock-word digest does not match (fail-closed refusal).
    """
    validated = validate_package(dict(pkg_data))
    if "sites" not in validated:
        return {}

    expected_exe_sha = validated["executable_sha256"]
    actual_exe_sha = executable_sha256.strip().lower()
    if actual_exe_sha != expected_exe_sha:
        fail(
            "executable_sha256",
            f"Executable SHA-256 mismatch (fail-closed refusal): package expects "
            f"{expected_exe_sha}, executable has {actual_exe_sha}",
        )

    known_set = set(int(a) for a in known_functions)
    resolved_by_addr: dict[int, dict[str, Any]] = {}
    unresolved_fn_cache: dict[str, int] = {}
    # Whole float values of lui/ori sites, by site id, for a launch velocity's acceleration.
    stock_values: dict[str, float] = {}
    pending: list[tuple[Mapping[str, Any], str, int, int, list[int]]] = []

    for idx, site in enumerate(validated["sites"]):
        site_path = f"sites[{idx}]({site['id']})"
        fn_name = site["function"]
        insn_offset = int(site["insn_offset"])
        word_count = int(site.get("word_count", 1))
        expected_digest = site["stock_digest"]

        fn_addr = _resolve_function_address(
            validated, fn_name, site_path, symbol_to_addr, read_words_fn, known_functions,
            unresolved_fn_cache,
        )
        site_addr, words = _read_verified_words(
            fn_name, fn_addr, insn_offset, word_count, expected_digest, site_path,
            read_words_fn, known_set,
        )

        if site_addr in resolved_by_addr:
            fail(
                site_path,
                f"Duplicate resolved instruction address 0x{site_addr:08x} (fail-closed refusal)",
            )

        stock_words = [int(w) & 0xFFFFFFFF for w in words]
        if "low_insn_offset" in site:
            _low_addr, low_words = _read_verified_words(
                fn_name, fn_addr, int(site["low_insn_offset"]), 1, site["low_stock_digest"],
                f"{site_path}.low", read_words_fn, known_set,
            )
            stock_values[site["id"]] = _lui_ori_value(site, stock_words[0], int(low_words[0]) & 0xFFFFFFFF)
        elif word_count == 2:
            stock_values[site["id"]] = _lui_ori_value(site, stock_words[0], stock_words[1])
        if word_count not in (1, 2):
            fail(
                f"{site_path}.word_count",
                "Only one-word sites and lui/ori float pairs have a defined rewrite (fail-closed refusal)",
            )
        pending.append((site, site_path, fn_addr, site_addr, stock_words))

    located: dict[str, _Located] = {entry[0]["id"]: entry for entry in pending}
    data_words: dict[str, int] = {}
    inputs = _reference_inputs(located, stock_values, read_words_fn, read_const_fn, data_words)
    _check_data_sites_leave_pinned_words(validated, data_words, symbol_to_addr)

    for site, site_path, fn_addr, site_addr, stock_words in pending:
        if site["transform"] == "launch_velocity_x0.5":
            accel = site["acceleration_site"]
            if accel not in stock_values:
                fail(f"{site_path}.acceleration_site",
                     f"acceleration site {accel!r} does not pin its whole float: give it word_count 2 "
                     f"or low_insn_offset (fail-closed refusal)")
            rewrites = plan_float_pair_rewrite(site, stock_words[0], stock_words[1],
                                               acceleration=stock_values[accel])
        elif len(stock_words) == 1:
            rewrites = [plan_site_rewrite(site, stock_words[0], inputs.get(site["id"]))]
            if site["transform"] == "gate_30hz_hold":
                _check_hold_callee(site_path, site_addr, stock_words[0], read_words_fn)
            elif site["transform"] == "count_x2_even":
                _check_even_counter(site_path, fn_addr, site_addr, stock_words[0], read_words_fn)
            elif site["transform"] == "gate_30hz_phase":
                _check_phase_gate(site_path, fn_addr, site_addr, rewrites[0], read_words_fn, known_functions)
            elif site["transform"] == "launch_from_rest_60hz":
                _check_launch_callee(site_path, site_addr, stock_words[0], rewrites[0]["offset"], read_words_fn,
                                     known_functions)
            elif rewrites[0]["kind"] == "scale_vq_reg":
                _check_straight_line_site(site_path, fn_addr, site_addr, read_words_fn, "an lv.q site")
        elif site["transform"] == "value_reference":
            # A pinned lui/ori pair: its value is in stock_values, and neither word is rewritten.
            rewrites = [{"kind": "reference"}, {"kind": "reference"}]
        else:
            for key in ("stock_values", "stock_value_digests"):
                if key in site:
                    fail(f"{site_path}.{key}", f"{key} belongs only to swc1 and sw store sites")
            rewrites = plan_float_pair_rewrite(site, stock_words[0], stock_words[1])
        for word_index, rewrite in enumerate(rewrites):
            word_addr = site_addr + 4 * word_index
            if word_addr in resolved_by_addr:
                fail(
                    site_path,
                    f"Duplicate resolved instruction address 0x{word_addr:08x} (fail-closed refusal)",
                )
            resolved_entry = dict(site)
            resolved_entry["fn_addr"] = fn_addr
            resolved_entry["site_addr"] = word_addr
            resolved_entry["stock_words"] = [stock_words[word_index]]
            if "requires" in site:
                rewrite = dict(rewrite, requires=list(site["requires"]))
            resolved_entry["rewrite"] = rewrite
            resolved_by_addr[word_addr] = resolved_entry

    _resolve_alternates(pending, resolved_by_addr, symbol_to_addr, read_words_fn, known_functions)

    if "update_boundary" not in validated:
        gated = sorted(e["id"] for e in resolved_by_addr.values() if e["rewrite"]["kind"] == "gate")
        if gated:
            fail(
                "update_boundary",
                f"Sites {gated} run on alternate logic updates, but the package declares no "
                f"update_boundary to count them (fail-closed refusal)",
            )
    return resolved_by_addr


def verify_and_resolve_update_boundary(
    pkg_data: Mapping[str, Any],
    *,
    symbol_to_addr: Mapping[str, int],
    read_words_fn: Any,
    known_functions: Sequence[int] = (),
) -> dict[str, Any] | None:
    """Resolve and verify a package's ``update_boundary`` fail-closed.

    Returns ``None`` when the package declares none, else a dict with ``fn_addr``,
    ``site_addr`` and ``stock_word``. The executable digest is checked by
    :func:`verify_and_resolve_sites`, which callers run first.
    """
    validated = validate_package(dict(pkg_data))
    boundary = validated.get("update_boundary")
    if boundary is None:
        return None
    fn_name = boundary["function"]
    fn_addr = _resolve_function_address(
        validated, fn_name, "update_boundary", symbol_to_addr, read_words_fn, known_functions, {},
    )
    site_addr, words = _read_verified_words(
        fn_name, fn_addr, int(boundary["insn_offset"]), 1, boundary["stock_digest"],
        "update_boundary", read_words_fn, set(int(a) for a in known_functions),
    )
    return {"fn_addr": fn_addr, "site_addr": site_addr, "stock_word": int(words[0]) & 0xFFFFFFFF}


def verify_and_resolve_ace_parts(
    pkg_data: Mapping[str, Any],
    *,
    symbol_to_addr: Mapping[str, int],
    read_words_fn: Any,
    known_functions: Sequence[int] = (),
) -> dict[int, dict[str, Any]]:
    """Resolve and verify every ``ace_parts`` site fail-closed (contract section 9.8).

    Returns ``vaddr -> site dict`` augmented with ``part``, ``fn_addr``, ``site_addr``,
    ``stock_words`` and its ``rewrite``. A ``vblank_wait_skip`` site must be a ``jal``; that
    its target is the ``sceDisplayWaitVblankStart`` import stub is checked by codegen, which
    owns the import table. The executable digest is checked by :func:`verify_and_resolve_sites`,
    which callers run first.
    """
    validated = validate_package(dict(pkg_data))
    known_set = set(int(a) for a in known_functions)
    cache: dict[str, int] = {}
    resolved: dict[int, dict[str, Any]] = {}
    for name in sorted(validated.get("ace_parts", {})):
        for idx, site in enumerate(validated["ace_parts"][name]["sites"]):
            path = f"ace_parts.{name}.sites[{idx}]({site['id']})"
            fn_addr = _resolve_function_address(
                validated, site["function"], path, symbol_to_addr, read_words_fn, known_functions, cache,
            )
            site_addr, words = _read_verified_words(
                site["function"], fn_addr, int(site["insn_offset"]), 1, site["stock_digest"], path,
                read_words_fn, known_set,
            )
            word = int(words[0]) & 0xFFFFFFFF
            if site["transform"] == "vblank_wait_skip" and (word >> 26) != 0x03:
                fail(path, f"a vblank_wait_skip site must be a jal to {VBLANK_WAIT_SKIP_IMPORT}, "
                           f"got 0x{word:08x} (fail-closed refusal)")
            if site_addr in resolved:
                fail(path, f"Duplicate resolved instruction address 0x{site_addr:08x} (fail-closed refusal)")
            entry = dict(site)
            entry.update(part=name, fn_addr=fn_addr, site_addr=site_addr, stock_words=[word],
                         rewrite={"kind": site["transform"]})
            resolved[site_addr] = entry
    return resolved


def _function_locations(validated: Mapping[str, Any], fn_name: str) -> list[tuple[int, int, str]]:
    """Every (insn_offset, word_count, stock_digest) the package pins inside ``fn_name``."""
    locations = [
        (int(s["insn_offset"]), int(s.get("word_count", 1)), s["stock_digest"])
        for s in validated["sites"] if s["function"] == fn_name
    ]
    locations += [
        (int(s["insn_offset"]), 1, s["stock_digest"])
        for _path, s in ace_part_sites(validated) if s["function"] == fn_name
    ]
    boundary = validated.get("update_boundary")
    if boundary is not None and boundary["function"] == fn_name:
        locations.append((int(boundary["insn_offset"]), 1, boundary["stock_digest"]))
    return locations


def _resolve_function_address(
    validated: Mapping[str, Any],
    fn_name: str,
    path: str,
    symbol_to_addr: Mapping[str, int],
    read_words_fn: Any,
    known_functions: Sequence[int],
    cache: dict[str, int],
) -> int:
    """A symbolic function's entry: the symbol map's, else the one known function whose
    words match every location the package pins in it (a deterministic fallback)."""
    if fn_name in symbol_to_addr:
        return int(symbol_to_addr[fn_name])
    if fn_name in cache:
        return cache[fn_name]
    if known_functions:
        locations = _function_locations(validated, fn_name)
        candidates: list[int] = []
        for cand_fn in known_functions:
            for off, wc, digest in locations:
                words = read_words_fn(int(cand_fn) + off, wc)
                if words is None or len(words) != wc or compute_stock_words_digest(words) != digest:
                    break
            else:
                candidates.append(int(cand_fn))
        if len(candidates) == 1:
            cache[fn_name] = candidates[0]
            return candidates[0]
        if len(candidates) > 1:
            fail(
                f"{path}.function",
                f"Ambiguous symbolic function {fn_name!r} without symbol map: "
                f"matched {len(candidates)} candidate functions (fail-closed refusal)",
            )
    fail(
        f"{path}.function",
        f"Cannot resolve symbolic function {fn_name!r} in guest executable "
        f"(fail-closed refusal)",
    )


def _read_verified_words(
    fn_name: str,
    fn_addr: int,
    insn_offset: int,
    word_count: int,
    expected_digest: str,
    path: str,
    read_words_fn: Any,
    known_set: set[int],
) -> tuple[int, list[int]]:
    """Read the stock words at ``fn_name + insn_offset`` and check their digest fail-closed."""
    if known_set and fn_addr not in known_set:
        fail(
            f"{path}.function",
            f"Resolved address 0x{fn_addr:08x} for {fn_name!r} is not a known function entry "
            f"(fail-closed refusal)",
        )
    site_addr = fn_addr + insn_offset
    words = read_words_fn(site_addr, word_count)
    if words is None or len(words) != word_count:
        fail(
            f"{path}.stock_digest",
            f"Cannot read {word_count} instruction word(s) at {fn_name}+{insn_offset} "
            f"(fail-closed refusal)",
        )
    actual_digest = compute_stock_words_digest(words)
    if actual_digest != expected_digest:
        fail(
            f"{path}.stock_digest",
            f"Stock instruction digest mismatch at {fn_name}+{insn_offset} "
            f"(fail-closed refusal): expected {expected_digest}, got {actual_digest}",
        )
    return site_addr, list(words)


def _scale_value(transform: str, value: float) -> float:
    """The rescaled value of one data operand; the caller checks it is representable."""
    if transform == "count_x2":
        return value * 2
    if transform in ("step_x0.5", "probability_per_update_x0.5"):
        return value / 2
    if transform == "accel_x0.25":
        return value / 4
    if not 0.0 < value < 1.0:
        raise ValueError(f"{value!r} is outside (0, 1)")
    if transform == "decay_k_to_1-sqrt(1-k)":
        # 1 - sqrt(1 - k) without its cancellation, which loses a small k entirely
        return value / (1.0 + math.sqrt(1.0 - value))
    return math.sqrt(value)


def plan_data_value(transform: str, dtype: str, raw: bytes) -> bytes:
    """Rescale one little-endian data element; refuse a value with no exact rescale."""
    size, fmt = DATA_TYPES[dtype]
    (value,) = struct.unpack(fmt, raw)
    if dtype == "f32":
        if not math.isfinite(value):
            raise ValueError(f"float {value!r} is not finite")
        scaled = _scale_value(transform, value)
        out = struct.pack(fmt, scaled)
        if not math.isfinite(struct.unpack(fmt, out)[0]):
            raise ValueError(f"float {value!r} overflows when rescaled")
        return out
    if transform not in ("count_x2", "step_x0.5", "probability_per_update_x0.5", "accel_x0.25"):
        raise ValueError(f"{transform} needs a float, not {dtype}")
    scaled = _scale_value(transform, value)
    if scaled != int(scaled):
        raise ValueError(f"{dtype} {value} has no exact rescale")
    try:
        return struct.pack(fmt, int(scaled))
    except struct.error as exc:
        raise ValueError(f"{dtype} {value} overflows when rescaled") from exc


def verify_and_resolve_data_sites(
    pkg_data: Mapping[str, Any],
    *,
    symbol_to_addr: Mapping[str, int],
    read_data_fn: Any,
) -> list[dict[str, Any]]:
    """Resolve, digest-check and rescale a package's executable data sites fail-closed.

    ``read_data_fn(vaddr, size)`` returns the stock bytes of initialised loaded data
    (never code or zero-fill), or ``None``. Returns one entry per site with its
    ``addr``, ``stock`` bytes and rescaled ``data`` bytes.
    """
    validated = validate_package(dict(pkg_data))
    resolved: list[dict[str, Any]] = []
    for idx, site in enumerate(validated.get("data_sites", ())):
        path = f"data_sites[{idx}]({site['id']})"
        symbol = site["symbol"]
        if symbol not in symbol_to_addr:
            fail(f"{path}.symbol", f"Cannot resolve data symbol {symbol!r} (fail-closed refusal)")
        size = DATA_TYPES[site["type"]][0]
        addr = int(symbol_to_addr[symbol]) + int(site["offset"])
        length = size * int(site["count"])
        stock = read_data_fn(addr, length)
        if stock is None or len(stock) != length:
            fail(path, f"{symbol}+{site['offset']} is not {length} bytes of initialised data (fail-closed refusal)")
        stock = bytes(stock)
        actual = hashlib.sha256(stock).hexdigest()
        if actual != site["stock_digest"]:
            fail(f"{path}.stock_digest",
                 f"Stock data digest mismatch at {symbol}+{site['offset']} (fail-closed refusal): "
                 f"expected {site['stock_digest']}, got {actual}")
        out = bytearray()
        for elem in range(int(site["count"])):
            raw = stock[elem * size:(elem + 1) * size]
            try:
                out += plan_data_value(site["transform"], site["type"], raw)
            except ValueError as exc:
                fail(path, f"element {elem}: {exc} (fail-closed refusal)")
        resolved.append({"id": site["id"], "addr": addr, "stock": stock, "data": bytes(out)})
    resolved.sort(key=lambda e: e["addr"])
    for a, b in zip(resolved, resolved[1:], strict=False):
        if a["addr"] + len(a["stock"]) > b["addr"]:
            fail("data_sites", f"data sites {a['id']} and {b['id']} overlap (fail-closed refusal)")
    return resolved


_CSV_NUMBER_RE = re.compile(rb"^(-?)([0-9]+)(?:\.([0-9]+))?$")


def _rescale_csv_number(transform: str, text: bytes, ctype: str | None = None) -> bytes:
    """Rescale one numeric CSV cell as the title reads it.

    ``ctype`` is the data type the title parses the cell into. Without one, a cell
    written with a decimal point is a float and any other cell an integer.
    """
    m = _CSV_NUMBER_RE.match(text)
    if m is None:
        raise ValueError(f"cell {text!r} is not a plain decimal number")
    sign, whole, frac = m.group(1), m.group(2), m.group(3)
    if ctype != "f32" and (ctype is not None or frac is None):
        if frac is not None:
            raise ValueError(f"{ctype} cell {text.decode('ascii')} has a fractional part")
        value = int(sign + whole)
        if ctype is not None:
            try:
                struct.pack(DATA_TYPES[ctype][1], value)
            except struct.error:
                raise ValueError(f"{ctype} cell {value} is out of range for its type") from None
        if transform not in ("count_x2", "step_x0.5", "probability_per_update_x0.5", "accel_x0.25"):
            raise ValueError(f"{transform} needs a decimal cell, not the integer {value}")
        scaled = _scale_value(transform, value)
        if scaled != int(scaled):
            raise ValueError(f"integer cell {value} has no exact rescale")
        if ctype is not None:
            try:
                struct.pack(DATA_TYPES[ctype][1], int(scaled))
            except struct.error:
                raise ValueError(f"{ctype} cell {value} rescales to {int(scaled)}, out of range") from None
        return str(int(scaled)).encode("ascii")
    # A decimal cell is read into a float: rescale that float and write the shortest
    # decimal that reads back to the same float.
    try:
        value = struct.unpack("<f", struct.pack("<f", float(text.decode("ascii"))))[0]
        scaled = struct.unpack("<f", struct.pack("<f", _scale_value(transform, value)))[0]
    except OverflowError:
        raise ValueError(f"cell {text.decode('ascii')} or its rescale is outside the float32 range") from None
    for digits in range(1, 18):
        rendered = f"{scaled:.{digits}f}"
        if struct.unpack("<f", struct.pack("<f", float(rendered)))[0] == scaled:
            return rendered.encode("ascii")
    raise ValueError(f"cell {text!r} has no decimal rendering")


def transform_data_file(entry: Mapping[str, Any], stock: bytes) -> bytes:
    """Apply a validated ``data_files`` entry to the stock bytes of its file, fail-closed."""
    if hashlib.sha256(stock).hexdigest() != entry["sha256"]:
        fail(f"data_files({entry['id']})", f"{entry['path']} does not match its sha256 (fail-closed refusal)")
    lines = stock.split(b"\n")
    header_row = int(entry["header_row"])
    first = int(entry["first_data_row"])
    if len(lines) <= first:
        fail(f"data_files({entry['id']})", f"{entry['path']} has no data rows (fail-closed refusal)")
    header = lines[header_row].rstrip(b"\r").split(b",")
    data_rows = [i for i in range(first, len(lines)) if lines[i].rstrip(b"\r")]
    rows = [line.split(b",") for line in lines]
    # Resolve every cell's column and lines before rewriting any, so two cells that meet
    # on one line (a name and a number for the same column, say) refuse the package
    # rather than rescale it twice.
    plan: list[tuple[Mapping[str, Any], int, str, list[int]]] = []
    claimed: set[tuple[int, int]] = set()
    for cell in entry["cells"]:
        if "column" in cell:
            label = f"column {cell['column']!r}"
            if header.count(cell["column"].encode("ascii")) != 1:
                fail(f"data_files({entry['id']})",
                     f"{label} is not exactly one header of {entry['path']} (fail-closed refusal)")
            col = header.index(cell["column"].encode("ascii"))
        else:
            col = int(cell["column_index"])
            label = f"column {col}"
            if col >= len(header):
                fail(f"data_files({entry['id']})", f"{entry['path']} has no {label} (fail-closed refusal)")
        wanted = cell.get("rows")
        targets = data_rows if wanted is None else []
        if wanted is not None:
            for r in wanted:
                if r >= len(data_rows):
                    fail(f"data_files({entry['id']})", f"row {r} is past the last data row (fail-closed refusal)")
                targets.append(data_rows[r])
        for line_no in targets:
            if (col, line_no) in claimed:
                fail(f"data_files({entry['id']})",
                     f"line {line_no + 1}, {label} is rescaled twice (fail-closed refusal)")
            claimed.add((col, line_no))
        plan.append((cell, col, label, targets))
    for cell, col, label, targets in plan:
        for line_no in targets:
            fields = rows[line_no]
            last = len(fields) - 1
            if col > last:
                fail(f"data_files({entry['id']})", f"line {line_no + 1} has no {label} (fail-closed refusal)")
            text = fields[col]
            crlf = col == last and text.endswith(b"\r")
            try:
                new = _rescale_csv_number(cell["transform"], text[:-1] if crlf else text, cell.get("type"))
            except ValueError as exc:
                fail(f"data_files({entry['id']})", f"line {line_no + 1}, {label}: {exc} (fail-closed refusal)")
            fields[col] = new + (b"\r" if crlf else b"")
    return b"\n".join(b",".join(fields) for fields in rows)


def find_data_file(entry: Mapping[str, Any], data_root: Path | str) -> bytes:
    """The stock bytes of a data file under the title's extracted data root.

    Any file whose path ends with the entry's disc path (compared without case, as
    the disc's file system does) and whose bytes match ``sha256`` is the file;
    none refuses the package.
    """
    want = entry["sha256"]
    return _find_data_file_bytes(entry, data_root, _sha256_matcher(want), f"sha256 {want}")


# Constant factor each scaling transform applies to the operand the site produces.
SCALE_FACTORS = {
    "count_x2": 2.0,
    "step_x0.5": 0.5,
    "probability_per_update_x0.5": 0.5,
}

# The transforms whose swc1 or sw site may check the values it stores (stock_values): the constant
# scales above, and the scale by kappa, whose factor the resolver computes.
STORE_VALUE_TRANSFORMS = frozenset(SCALE_FACTORS) | {"velocity_source_x_kappa"}

_IMM_ARITH_OPS = (0x08, 0x09, 0x0A, 0x0B)  # addi, addiu, slti, sltiu
_INT_LOAD_OPS = (0x20, 0x21, 0x23, 0x24, 0x25)  # lb, lh, lw, lbu, lhu
_GATE_STORE_OPS = (0x28, 0x29, 0x2B, 0x39)  # sb, sh, sw, swc1
_FPU_RESULT_FUNCS = (0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07)  # add sub mul div abs mov neg
_FPU_ARITHMETIC_FUNCS = (0x00, 0x01, 0x02, 0x03)  # add.s sub.s mul.s div.s
_FPU_MOV_S = 0x06
_FPU_UNARY_FUNCS = (0x05, 0x06, 0x07)  # abs.s mov.s neg.s: the ft field must be 0


def _f32(bits: int) -> float:
    returned: float = struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]
    return returned


def _f32_bits(value: float) -> int:
    returned: int = struct.unpack("<I", struct.pack("<f", value))[0]
    return returned


def _refuse(site: Mapping[str, Any], w: int, why: str) -> NoReturn:
    raise EnhancementPackageError(
        f"site {site['id']}: transform {site['transform']!r} has no defined rewrite for "
        f"instruction 0x{w:08x} ({why}) (fail-closed refusal)"
    )


def _with_imm(w: int, imm: int) -> int:
    return (w & 0xFFFF0000) | (imm & 0xFFFF)


def _lui_ori_value(site: Mapping[str, Any], lui: int, ori: int) -> float:
    """The float a lui and an ori of the same register build, checked fail-closed."""
    rt = (lui >> 16) & 0x1F
    if (lui >> 26) != 0x0F or (lui >> 21) & 0x1F or rt == 0:
        _refuse(site, lui, "the float starts with lui")
    if (ori >> 26) != 0x0D or (ori >> 21) & 0x1F != rt or (ori >> 16) & 0x1F != rt:
        _refuse(site, ori, "the float ends with ori of the lui's register")
    return _f32(((lui & 0xFFFF) << 16) | (ori & 0xFFFF))


def plan_float_pair_rewrite(site: Mapping[str, Any], w0: int, w1: int,
                            acceleration: float | None = None) -> list[dict[str, Any]]:
    """The rewrite of a float constant built by ``lui rt, hi`` then ``ori rt, rt, lo``.

    The two words name the whole 32-bit value, so a scaling transform rewrites it
    exactly (rounded to nearest) instead of only the upper half a lone ``lui`` holds.
    Returns one ``word`` rewrite per instruction.
    """
    w0, w1 = int(w0) & 0xFFFFFFFF, int(w1) & 0xFFFFFFFF
    rt = (w0 >> 16) & 0x1F
    if (w0 >> 26) != 0x0F or (w0 >> 21) & 0x1F or rt == 0:
        _refuse(site, w0, "a two-word site starts with lui")
    if (w1 >> 26) != 0x0D or (w1 >> 21) & 0x1F != rt or (w1 >> 16) & 0x1F != rt:
        _refuse(site, w1, "a two-word site ends with ori of the lui's register")
    tf = site["transform"]
    if tf not in DATA_TRANSFORMS and tf != "launch_velocity_x0.5":
        _refuse(site, w0, "a float pair takes a scaling transform")
    value = _f32(((w0 & 0xFFFF) << 16) | (w1 & 0xFFFF))
    if not math.isfinite(value):
        _refuse(site, w0, f"float constant {value!r} is not finite")
    if tf == "launch_velocity_x0.5":
        # Semi-implicit Euler (v += g; x += v once per update): y_n = n*v0 + g*n(n+1)/2.
        # With g/4 per 60 Hz update, y matches at every second update when v0' = v0/2 + g/8.
        if acceleration is None or not math.isfinite(acceleration):
            _refuse(site, w0, "launch_velocity_x0.5 needs the stock acceleration")
        new_value = value / 2 + acceleration / 8
    else:
        try:
            new_value = _scale_value(tf, value)
        except ValueError as exc:
            _refuse(site, w0, str(exc))
    bits = _f32_bits(new_value)
    if not math.isfinite(_f32(bits)):
        _refuse(site, w0, f"float constant {value!r} overflows when rescaled")
    return [
        {"kind": "word", "word": _with_imm(w0, bits >> 16)},
        {"kind": "word", "word": _with_imm(w1, bits & 0xFFFF)},
    ]


def _store_value_pairs(site: Mapping[str, Any], rewrite: dict[str, Any]) -> list[list[int]]:
    """``[stock bits, scaled bits]`` for each declared stock value of a store site, checked."""
    sid = site["id"]
    float_store = rewrite["kind"] == "scale_f_store"
    pairs: list[list[int]] = []
    seen: set[int] = set()
    for value in site["stock_values"]:
        if float_store:
            try:
                bits = _f32_bits(float(value))
            except OverflowError:
                bits = 0x7F800000
            stock = _f32(bits)
            if not math.isfinite(stock):
                raise EnhancementPackageError(
                    f"site {sid}: stock value {value!r} is not finite as an f32 (fail-closed refusal)")
            try:
                scaled_bits = _f32_bits(stock * rewrite["factor"])
            except OverflowError:
                scaled_bits = 0x7F800000
            if not math.isfinite(_f32(scaled_bits)):
                raise EnhancementPackageError(
                    f"site {sid}: stock value {value!r} scaled is not finite as an f32 (fail-closed refusal)")
        else:
            if not isinstance(value, int):
                raise EnhancementPackageError(
                    f"site {sid}: stock value {value!r} of an integer store must be an integer "
                    "(fail-closed refusal)")
            if not -0x80000000 <= value <= 0x7FFFFFFF or not -0x80000000 <= 2 * value <= 0x7FFFFFFF:
                raise EnhancementPackageError(
                    f"site {sid}: stock value {value} or its double is outside the s32 range "
                    "(fail-closed refusal)")
            bits, scaled_bits = value & 0xFFFFFFFF, (2 * value) & 0xFFFFFFFF
        if bits in seen:
            raise EnhancementPackageError(
                f"site {sid}: duplicate stock value {value!r} (fail-closed refusal)")
        seen.add(bits)
        pairs.append([bits, scaled_bits])
    return pairs


def store_value_overlaps(site_id: str, rewrite: Mapping[str, Any]) -> list[str]:
    """One note per stock value of a store site whose scaled value is also a stock value.

    The run-time check cannot tell that value, stored again after it was scaled, from a fresh
    stock value, so a path that re-stores it would scale it twice. It is not refused, because
    the rows of one table can hold both v and 2v; the package author checks that no such path
    exists.
    """
    pairs = rewrite.get("stock_values", ())
    stock = {bits for bits, _scaled in pairs}

    def text(bits: int) -> str:
        if rewrite["kind"] == "scale_f_store":
            return f"{_f32(bits):g}"
        return str(bits - (1 << 32) if bits & 0x80000000 else bits)

    return [
        f"site {site_id}: stock value {text(bits)} scales to {text(scaled)}, which is also a stock "
        "value; a path that stores an already scaled value again would scale it twice"
        for bits, scaled in pairs if scaled in stock
    ]


def two_update_factors(damping: float, damping_step: float,
                       step: float | None = None) -> tuple[float, float | None]:
    """``(kappa, q)``: the two-update image of one stock update (contract section 9.2).

    Stock runs ``v = (v + g*h + I) * m`` then ``x += v*h`` once per update, with ``m = 1 - c*L``
    (``damping`` c, ``damping_step`` L, ``step`` h). Two updates of ``w = (w + a + I') * mu`` then
    ``x += w*h/2`` reach the same ``x`` at the end of every pair exactly when ``mu = sqrt(m)``,
    ``a = kappa*g*h/2`` and ``w - v* = kappa*(v - v*)``, where ``kappa = 2*mu/(1 + mu)`` and ``v*`` is
    stock's terminal velocity ``g*h*m/(1 - m)``. So a velocity change scales by kappa, and a
    velocity that is set from rest becomes ``kappa*V + q*g`` with ``q = h*mu^2/(1 + mu)^2``.
    ``q`` is None without ``step``. Raises ValueError unless ``0 < c*L < 1`` and ``h > 0``.
    """
    product = damping * damping_step
    if not (math.isfinite(product) and 0.0 < product < 1.0):
        raise ValueError(f"the stock damping per update, c*L = {product!r}, is outside (0, 1)")
    mu = math.sqrt(1.0 - product)
    kappa = 2.0 * mu / (1.0 + mu)
    if step is None:
        return kappa, None
    if not (math.isfinite(step) and step > 0.0):
        raise ValueError(f"the stock step h = {step!r} is not a positive number")
    return kappa, step * mu * mu / ((1.0 + mu) * (1.0 + mu))


#: A state's updates are counted up to this bound; past it a site keeps its stock instruction.
PHASE_UPDATES_MAX = 0x7FFFFFFF
#: A float32 sum of a constant needs a few rows for each binade it passes through, under a hundred
#: in all. A step that would need more than this is one this model does not describe: refused.
PHASE_ROWS_MAX = 512


@functools.lru_cache(maxsize=32)
def phase_runs(half_step: float) -> tuple[tuple[int, int, float, float, float], ...]:
    """Every time a state can read, as the guest's own float32 sum: ``t = 0``, then
    ``t = fl32(t + half_step)`` once per update.

    Rows ``(n0, count, a0, a1, delta)``: after ``n0 + k`` updates the time is exactly
    ``a0 + k*delta`` for ``0 <= k <= count``, and ``a1`` is the last of them. Inside one binade a
    float32 sum of a constant advances by a constant, so a few rows per binade cover the whole
    life of a state. The rows end one update before the sum stops advancing (the value it then
    repeats belongs to no single update, so it is on no row), where it overflows, or at
    PHASE_UPDATES_MAX. A time is matched against them exactly, so rounding error in the sum
    cannot move a phase: it is part of the model. Raises ValueError unless ``half_step`` is a
    positive float32."""
    d = float(half_step)
    if not (math.isfinite(d) and d > 0.0 and _f32(_f32_bits(d)) == d):
        raise ValueError(f"half the stock step, {d!r}, is not a positive float32 a state's time can count")

    def after(a: float) -> float:
        try:
            return _f32(_f32_bits(a + d))  # the double sum of two float32 values this close is exact
        except OverflowError:
            return math.inf  # past the largest float32 the guest's sum is infinite, and stays so

    runs: list[tuple[int, int, float, float, float]] = []
    n, a = 0, 0.0
    stalled = False
    while n < PHASE_UPDATES_MAX:
        delta = after(a) - a
        if delta <= 0.0 or not math.isfinite(delta):
            stalled = delta <= 0.0  # adding the step no longer changes the time; or it overflows
            break

        def repeats(k: int, a: float = a, delta: float = delta) -> bool:
            return after(a + k * delta) == a + (k + 1) * delta

        count = 1
        if a > 0.0 and repeats(1):
            # Two equal steps inside one binade: the rounding has settled, and every step that
            # starts in this binade adds the same (the steps hold on a prefix). Find the last one;
            # a step that starts exactly on the next power of two is taken too when it repeats.
            top = math.ldexp(1.0, math.frexp(a)[1])
            low, high = 1, min(int((top - a) // delta), PHASE_UPDATES_MAX - n - 1)
            while low < high:
                mid = (low + high + 1) // 2
                if repeats(mid):
                    low = mid
                else:
                    high = mid - 1
            count = low + 1
        runs.append((n, count, a, a + count * delta, delta))
        n, a = n + count, a + count * delta
        if len(runs) > PHASE_ROWS_MAX:
            raise ValueError(f"half the stock step, {d!r}, is not a positive float32 a state's time can count: "
                             f"its sum does not settle into {PHASE_ROWS_MAX} rows")
    if stalled and runs:
        # The last value is the one the sum then repeats on every update: no update count, and so
        # no phase, can be read off it. Leave it out, so a state that has stalled is reported.
        n0, count, a0, a1, delta = runs.pop()
        if count > 1:
            runs.append((n0, count - 1, a0, a1 - delta, delta))
    return tuple(runs)


def phase_of_time(time: float, half_step: float) -> int | None:
    """The phase of a gate_30hz_phase site whose state time reads ``time``: 0 after an even number
    of updates since the time was zero (a stock instant: the stock instruction runs), 1 after an
    odd number. None when ``time`` is not a value the state's own float32 sum of ``half_step``
    reaches before it stops advancing (phase_runs): the site then runs its stock instruction and
    reports once. The generated code looks the time up in the same rows
    (nk_enhancement_phase_odd)."""
    x = float(time)
    for n0, _count, a0, a1, delta in phase_runs(half_step):
        if a0 <= x <= a1:
            k = (x - a0) / delta
            return (n0 + int(k)) & 1 if k == math.floor(k) else None
    return None


def plan_site_rewrite(site: Mapping[str, Any], w: int,
                      inputs: Mapping[str, float] | None = None) -> dict[str, Any]:
    """The one defined rewrite of a verified stock word, with a store site's stock values.

    ``inputs`` holds what the resolver computed from the stock values a site references
    (SITE_REFERENCES): ``kappa``, ``gravity_offset`` (q*g) and ``half_step`` (h/2, the step a
    60 Hz update adds to a state's time).
    """
    rewrite = _plan_word_rewrite(site, w, inputs)
    if "stock_value_digests" in site:
        if rewrite["kind"] not in ("scale_f_store", "scale_int_store"):
            raise EnhancementPackageError(
                f"site {site['id']}: stock_value_digests belongs only to a scaled swc1 or sw store "
                "site (fail-closed refusal)")
        # Keyed digests of the stock values: the run-time check digests the value being stored
        # and scales it as _store_value_pairs() does, refusing one that leaves the range.
        rewrite["stock_value_digests"] = list(site["stock_value_digests"])
    elif "stock_values" in site:
        if rewrite["kind"] not in ("scale_f_store", "scale_int_store"):
            raise EnhancementPackageError(
                f"site {site['id']}: stock_values belongs only to a scaled swc1 or sw store site "
                "(fail-closed refusal)")
        rewrite["stock_values"] = _store_value_pairs(site, rewrite)
    elif rewrite["kind"] == "scale_int_store":
        raise EnhancementPackageError(
            f"site {site['id']}: an integer store site needs stock_values (or stock_value_digests), "
            "so every value it stores is checked and its double proven in range (fail-closed refusal)")
    return rewrite


def _resolved_input(site: Mapping[str, Any], inputs: Mapping[str, float] | None, name: str) -> float:
    if inputs is None or name not in inputs:
        raise EnhancementPackageError(
            f"site {site['id']}: transform {site['transform']!r} is planned from the stock values its "
            f"sites reference; resolve the package against its executable (fail-closed refusal)")
    return float(inputs[name])


def _plan_word_rewrite(site: Mapping[str, Any], w: int,
                       inputs: Mapping[str, float] | None = None) -> dict[str, Any]:
    """Return the one defined rewrite of a verified stock word for its declared transform.

    The rewrite depends only on the declared transform and the instruction's own
    encoding; nothing here knows a title. Every (transform, instruction) pair
    without a defined rewrite raises, so one unsupported site refuses the whole
    package instead of emitting a silent no-op.

    Returned kinds:
      ``word``            execute ``word`` instead of the stock word;
      ``gate``            execute the stock instruction on alternate 60 Hz updates only; a gated
                          call with ``hold_f`` sets FPR 0 to FPR ``hold_f`` on the updates it skips;
      ``word_even``       execute ``word``, then clear GPR ``rt`` when GPR ``reg`` was odd;
      ``scale_int_reg``   after the stock load, multiply GPR ``reg`` by 2;
      ``scale_int_args``  after the stock entry instruction, multiply the GPRs ``regs`` by 2;
      ``scale_f_reg``   after the stock instruction, multiply FPR ``reg`` by ``factor``;
      ``scale_f_store``   store FPR ``reg`` multiplied by ``factor`` (the register is kept);
      ``scale_int_store`` store GPR ``reg`` doubled (the register is kept; count_x2 on ``sw``);
      ``scale_f_addend``  run ``fd = fd +/- ft`` with ``ft`` multiplied by ``factor``;
      ``map_f_reg``       after the stock lwc1 or mov.s (or, for retention, the stock
                          single-precision arithmetic), map FPR ``reg`` by ``map`` (decay or
                          retention);
      ``scale_v_reg``     after the stock lv.s, multiply VFPU register ``vreg`` by ``factor``;
      ``scale_vq_reg``    after the stock lv.q, multiply the four lanes of VFPU register ``vreg``
                          by ``factor``;
      ``map_v_reg``       after the stock lv.s, map VFPU register ``vreg`` by ``map``;
      ``gate_phase``      make the stock call unless the float at ``offset`` from GPR ``reg`` is the
                          state's time after an odd number of updates of ``half_step``;
      ``gate_phase_cond`` after the stock compare, on such an odd phase, set the FPU condition
                          flag to ``condition``;
      ``gate_phase_zero_f``, ``gate_phase_zero_v``  after the stock lwc1 or lv.s, on such an odd
                          phase, set FPR ``value_reg`` or VFPU register ``value_vreg`` to +0.0;
      ``launch_from_rest`` after the stock jal returns, map the two floats at ``offset`` from the
                          site's $a0: on ``axis`` the float times ``kappa`` plus
                          ``gravity_offset``; on the other, only the change the call made is
                          scaled by ``kappa``;
      ``reference``       no rewrite: the site pins a stock value other sites reference;
      ``clip_x_widen``    after the stock jal returns, scale the x row of the clip matrices at ``offsets``
                          from GPR ``reg`` by the run-time widen factor (contract section 10.8).
    """
    w = int(w) & 0xFFFFFFFF
    tf = site["transform"]
    op = (w >> 26) & 0x3F
    rs = (w >> 21) & 0x1F
    rt = (w >> 16) & 0x1F
    rd = (w >> 11) & 0x1F
    sa = (w >> 6) & 0x1F
    fn = w & 0x3F
    imm16 = w & 0xFFFF
    simm16 = imm16 - 0x10000 if (imm16 & 0x8000) else imm16
    is_jal = op == 0x03
    is_jalr = op == 0x00 and fn == 0x09
    is_fpu_s = op == 0x11 and rs == 0x10
    vt = rt | ((w & 3) << 5)  # lv.s: the VFPU register number

    if tf == "vblank_wait_2_to_1":
        if op in _IMM_ARITH_OPS + (0x0D,) and simm16 == 2:
            return {"kind": "word", "word": _with_imm(w, 1)}
        _refuse(site, w, "expects an immediate of 2")

    if site.get("operand") == "f64_high":
        # The upper word of a double: sign, 11-bit exponent (bits 4..14 of the lui
        # immediate), top of the fraction. A power-of-two scale is an exponent step.
        steps = {"count_x2": 1, "step_x0.5": -1, "probability_per_update_x0.5": -1, "accel_x0.25": -2}
        if op != 0x0F or tf not in steps:
            _refuse(site, w, "f64_high takes the lui of a double and a power-of-two scale")
        exponent = (imm16 >> 4) & 0x7FF
        new_exponent = exponent + steps[tf]
        if exponent == 0 or exponent == 0x7FF or not 1 <= new_exponent <= 0x7FE:
            _refuse(site, w, "lui value is not a normal double's upper word")
        return {"kind": "word", "word": _with_imm(w, (imm16 & 0x800F) | (new_exponent << 4))}

    if tf == "offset_x2":
        # An additive offset counted in updates (base + n, not an accumulator): n -> 2n.
        if op not in (0x08, 0x09) or simm16 == 0 or not -0x8000 <= 2 * simm16 <= 0x7FFF:
            _refuse(site, w, "offset_x2 doubles the nonzero immediate of an addi/addiu")
        return {"kind": "word", "word": _with_imm(w, 2 * simm16)}

    if tf == "window_start_x2":
        # A window from head + k to head - 1 spans -k - 1 updates; twice that span
        # starts at head + 2k + 1.
        if op not in (0x08, 0x09) or simm16 > -2 or 2 * simm16 + 1 < -0x8000:
            _refuse(site, w, "window_start_x2 takes an addi/addiu of an offset of -2 or less")
        return {"kind": "word", "word": _with_imm(w, 2 * simm16 + 1)}

    if tf == "argument_x2":
        # A helper that takes a duration in updates from every caller doubles it once
        # at its entry. The stock instruction must leave the arguments as they came.
        regs = [ARGUMENT_REGISTERS[name] for name in site["arguments"]]
        if _gpr_written(w) in regs:
            _refuse(site, w, "the entry instruction writes an argument it should double")
        return {"kind": "scale_int_args", "regs": regs}

    if tf == "accel_x0.25":
        # A lone lui of a float: two exponent steps are an exact x0.25.
        if op != 0x0F:
            _refuse(site, w, "accel_x0.25 takes the lui of a float (or a lui/ori pair)")
        exponent = (imm16 >> 7) & 0xFF
        if not 3 <= exponent <= 254:
            _refuse(site, w, "lui value is not a normal float constant")
        return {"kind": "word", "word": _with_imm(w, imm16 - 0x100)}

    if tf == "launch_velocity_x0.5":
        _refuse(site, w, "launch_velocity_x0.5 rewrites a lui/ori float pair")

    if tf == "gate_30hz_hold":
        # A call that advances its first float argument (f12) by one update and returns
        # the result in f0: on a skipped update the call holds that value instead.
        if is_jal:
            return {"kind": "gate", "hold_f": 12}
        _refuse(site, w, "gate_30hz_hold takes a jal")

    if tf == "gate_30hz_alt":
        # A call skipped on odd updates, with the calls named by ``alternates`` made in its
        # place; the resolver adds their addresses after checking them (section 9.2).
        if is_jal:
            return {"kind": "gate", "alternates": []}
        _refuse(site, w, "gate_30hz_alt takes a jal")

    if tf == "count_x2_even":
        # slti/sltiu rt, rs, n where rs counts logic updates: true while rs < 2n and rs is
        # even, so n events still happen, one every second update, at the stock instants.
        if op not in (0x0A, 0x0B) or simm16 <= 0 or 2 * simm16 > 0x7FFF:
            _refuse(site, w, "count_x2_even takes a slti/sltiu with a positive immediate")
        if rt == 0 or rt == rs:
            _refuse(site, w, "count_x2_even needs a destination other than $zero and the counter")
        return {"kind": "word_even", "word": _with_imm(w, 2 * simm16), "reg": rs, "rt": rt}

    if tf == "clip_x_widen":
        if not is_jal:
            _refuse(site, w, "clip_x_widen takes a jal: it acts after the call returns")
        return {"kind": "clip_x_widen", "reg": RESULT_REGISTERS[site["clip_register"]],
                "offsets": [int(o) for o in site["clip_offsets"]]}

    if tf == "gate_30hz_store":
        # The store of a value the code advances by one update from what it loaded: run on
        # even updates only, it does not happen on the odd ones, so each 30 Hz value shows for
        # two updates. A store through $sp writes a slot of the function's own frame, which does
        # not keep its value from one update to the next, so it refuses. Only the base register
        # is read: a frame store through $fp or a copy of $sp is not seen (contract section 9.2).
        if op not in _GATE_STORE_OPS:
            _refuse(site, w, "gate_30hz_store takes an sb, sh, sw or swc1")
        if rs == 29:
            _refuse(site, w, "gate_30hz_store refuses a store to the stack frame ($sp base)")
        return {"kind": "gate"}

    if tf == "value_reference":
        # No rewrite. The resolver reads the float this instruction loads, or builds with its
        # pinned low word (verify_and_resolve_sites), for the sites that reference it.
        if op in (0x31, 0x32) or (op == 0x0F and "low_insn_offset" in site):
            return {"kind": "reference"}
        _refuse(site, w, "value_reference pins the float an lwc1 or lv.s loads, or a lui/ori pair builds")

    if tf == "gate_30hz_phase":
        # The phase comes from the state's own time, not the update count: a call is skipped,
        # and a compare reads as declared, half a stock step after each stock instant.
        phase = {"reg": PHASE_REGISTERS[site["phase_time"]["register"]],
                 "offset": int(site["phase_time"]["offset"]),
                 "half_step": _resolved_input(site, inputs, "half_step")}
        if is_jal:
            if "odd_phase_condition" in site:
                _refuse(site, w, "odd_phase_condition belongs to a compare site; a call is skipped")
            return {"kind": "gate_phase", **phase}
        if is_fpu_s and fn >= 0x30:
            if "odd_phase_condition" not in site:
                _refuse(site, w, "a gate_30hz_phase compare needs odd_phase_condition")
            return {"kind": "gate_phase_cond", **phase, "condition": bool(site["odd_phase_condition"])}
        if op in (0x31, 0x32):
            # A value the stock update uses once per stock step (the size of an impulse): half a
            # step later it loads as zero, so what it scales still runs and adds nothing.
            if "odd_phase_condition" in site:
                _refuse(site, w, "odd_phase_condition belongs to a compare site; a load reads +0.0")
            if op == 0x31:
                return {"kind": "gate_phase_zero_f", **phase, "value_reg": rt}
            return {"kind": "gate_phase_zero_v", **phase, "value_vreg": vt}
        _refuse(site, w, "gate_30hz_phase takes a jal, a single-precision compare (c.cond.s), "
                         "or an lwc1 or lv.s of a value")

    if tf == "launch_from_rest_60hz":
        if not is_jal:
            _refuse(site, w, "launch_from_rest_60hz takes a jal: it acts after the call returns")
        return {"kind": "launch_from_rest", "offset": int(site["velocity_offset"]),
                "axis": int(site["gravity_axis"]), "kappa": _resolved_input(site, inputs, "kappa"),
                "gravity_offset": _resolved_input(site, inputs, "gravity_offset")}

    if tf == "velocity_source_x_kappa":
        # A source of velocity change (an acceleration, an impulse): the value times kappa.
        kappa = _resolved_input(site, inputs, "kappa")
        if op == 0x31:  # lwc1
            return {"kind": "scale_f_reg", "reg": rt, "factor": kappa}
        if op == 0x32:  # lv.s
            return {"kind": "scale_v_reg", "vreg": vt, "factor": kappa}
        if op == 0x39:  # swc1
            return {"kind": "scale_f_store", "reg": rt, "factor": kappa}
        if op == 0x36 and not w & 2:  # lv.q: all four loaded lanes are scaled
            return {"kind": "scale_vq_reg", "vreg": rt | ((w & 1) << 5), "factor": kappa}
        _refuse(site, w, "velocity_source_x_kappa takes an lwc1, an lv.s, an lv.q or a swc1")

    if tf == "gate_30hz":
        if is_jal or is_jalr:
            return {"kind": "gate"}
        if op in (0x08, 0x09) and rt != 0 and rt == rs:
            return {"kind": "gate"}
        if op == 0x00 and fn in (0x20, 0x21, 0x22, 0x23) and rd != 0 and rd in (rs, rt):
            return {"kind": "gate"}
        if is_fpu_s and fn in (0x00, 0x01) and sa in (rd, rt):
            return {"kind": "gate"}
        _refuse(site, w, "gate_30hz needs a call or an accumulating update")

    if tf in ("decay_k_to_1-sqrt(1-k)", "retention_m_to_sqrt(m)"):
        mapping = "decay" if tf == "decay_k_to_1-sqrt(1-k)" else "retention"
        if op == 0x31:  # lwc1 of a per-update factor: map the loaded register
            return {"kind": "map_f_reg", "reg": rt, "map": mapping}
        if op == 0x32:  # lv.s of a per-update factor: map the loaded VFPU register
            return {"kind": "map_v_reg", "vreg": vt, "map": mapping}
        if mapping == "retention" and is_fpu_s and fn in _FPU_ARITHMETIC_FUNCS:
            # The retention is computed (such as 1 - c*L): map the result register.
            return {"kind": "map_f_reg", "reg": sa, "map": mapping}
        if is_fpu_s and fn == _FPU_MOV_S and rt == 0 and sa != rd:
            # mov.s fd, fs copies the factor: map the copy only. The source register may hold the
            # same value for another use (a duration, say) that must keep its stock value, so a
            # copy onto itself (fd == fs) is refused.
            return {"kind": "map_f_reg", "reg": sa, "map": mapping}
        if op != 0x0F:
            _refuse(site, w, "expects the lui of a float constant, an lwc1, an lv.s or a mov.s"
                             + (", or single-precision arithmetic" if mapping == "retention" else ""))
        value = _f32(imm16 << 16)
        if not 0.0 < value < 1.0:
            _refuse(site, w, f"float constant {value!r} is outside (0, 1)")
        bits = _f32_bits(_scale_value(tf, value))
        hi = ((bits + 0x8000) >> 16) & 0xFFFF
        return {"kind": "word", "word": _with_imm(w, hi)}

    factor = SCALE_FACTORS.get(tf)
    if factor is None:
        raise EnhancementPackageError(f"Unknown transform {tf!r} at site {site['id']}")
    halve = factor == 0.5

    if op in (0x08, 0x09) and rt != 0 and rt == rs:
        # Accumulating counter update (x += imm): a unit step under a halved rate
        # (or a unit countdown under a doubled duration) runs on alternate updates.
        if simm16 in (1, -1) and (halve or simm16 == -1):
            return {"kind": "gate"}
        if halve and simm16 % 2 == 0 and simm16 != 0:
            return {"kind": "word", "word": _with_imm(w, simm16 // 2)}
        _refuse(site, w, "accumulating immediate without an exact rescale")
    if op in _IMM_ARITH_OPS:
        if simm16 <= 0:
            _refuse(site, w, "non-positive immediate")
        scaled = simm16 * factor
        if scaled != int(scaled) or int(scaled) > 0x7FFF or int(scaled) < 1:
            _refuse(site, w, f"immediate {simm16} has no exact x{factor:g} rescale")
        return {"kind": "word", "word": _with_imm(w, int(scaled))}
    if op == 0x0C:  # andi period mask 2^n - 1
        if imm16 == 0 or imm16 & (imm16 + 1):
            _refuse(site, w, "andi mask is not 2^n - 1")
        mask = (imm16 << 1) | 1 if not halve else imm16 >> 1
        if mask == 0 or mask > 0xFFFF:
            _refuse(site, w, "andi mask has no exact rescale")
        return {"kind": "word", "word": _with_imm(w, mask)}
    if op == 0x0F:  # lui of a float constant: an exponent step is an exact x2 / x0.5
        exponent = (imm16 >> 7) & 0xFF
        if not (2 if halve else 1) <= exponent <= (254 if halve else 253):
            _refuse(site, w, "lui value is not a normal float constant")
        return {"kind": "word", "word": _with_imm(w, imm16 + (-0x80 if halve else 0x80))}
    if op == 0x00 and fn == 0x00 and w != 0:  # sll: shift count is a power-of-two factor
        new_sa = sa - 1 if halve else sa + 1
        if not 0 <= new_sa <= 31 or (halve and sa == 0):
            _refuse(site, w, "sll shift has no exact rescale")
        return {"kind": "word", "word": (w & ~(0x1F << 6)) | (new_sa << 6)}
    if op == 0x00 and fn in (0x21, 0x23) and halve and rd != 0 and rd in (rs, rt):
        return {"kind": "gate"}  # accumulating addu/subu at half rate
    if op in _INT_LOAD_OPS and not halve and rt != 0:
        return {"kind": "scale_int_reg", "reg": rt}
    if op == 0x31:  # lwc1
        return {"kind": "scale_f_reg", "reg": rt, "factor": factor}
    if op == 0x32:  # lv.s
        return {"kind": "scale_v_reg", "vreg": vt, "factor": factor}
    if op == 0x39:  # swc1
        return {"kind": "scale_f_store", "reg": rt, "factor": factor}
    if op == 0x2B and tf == "count_x2":  # sw
        if rt == 0 or rt == rs:
            _refuse(site, w, "the stored register must not be $zero or also the base register")
        return {"kind": "scale_int_store", "reg": rt}
    if is_fpu_s and fn in (0x00, 0x01) and sa == rd and rt != rd:
        return {"kind": "scale_f_addend", "reg": rt, "factor": factor}
    if is_fpu_s and fn in _FPU_RESULT_FUNCS and (fn not in _FPU_UNARY_FUNCS or rt == 0):
        return {"kind": "scale_f_reg", "reg": sa, "factor": factor}
    if op == 0x11 and rs == 0x14 and fn == 0x20:  # cvt.s.w
        return {"kind": "scale_f_reg", "reg": sa, "factor": factor}
    _refuse(site, w, "instruction class has no defined rescale")


#: Display NIDs a frame-pacing wait is built from.
VBLANK_WAIT_NIDS = {
    0x984C27E7: "sceDisplayWaitVblankStart",
    0x46F186C3: "sceDisplayWaitVblankStartCB",
    0x36CDFADE: "sceDisplayWaitVblank",
    0x8EB9EC49: "sceDisplayWaitVblankCB",
}
VBLANK_WAIT_MULTI_NIDS = {
    0x40F1469C: "sceDisplayWaitVblankStartMulti",
    0x77ED8B3A: "sceDisplayWaitVblankStartMultiCB",
}
REGISTER_SUB_INTR_HANDLER_NID = 0xCA04A2B9
_LOOKBACK = 5


def _sx16(imm: int) -> int:
    return imm - 0x10000 if imm & 0x8000 else imm


def _jal_target(addr: int, w: int) -> int | None:
    return (addr & 0xF0000000) | ((w & 0x3FFFFFF) << 2) if w >> 26 == 0x03 else None


def _global_address(words: Mapping[int, int], addr: int, w: int) -> int | None:
    """The absolute address a load or store reaches through a `lui` of its base register."""
    base = (w >> 21) & 31
    for k in range(1, _LOOKBACK + 1):
        prev = words.get(addr - 4 * k)
        if prev is None:
            return None
        if prev >> 26 == 0x0F and (prev >> 16) & 31 == base:
            return (((prev & 0xFFFF) << 16) + _sx16(w & 0xFFFF)) & 0xFFFFFFFF
        if _gpr_written(prev) == base:
            return None
    return None


def _gpr_written(w: int) -> int | None:
    """The general register `w` writes, or None when it writes none this decoder knows."""
    op = w >> 26
    rs = (w >> 21) & 31
    if op == 0x00 and w & 0x3F not in (0x08, 0x09, 0x0C, 0x0D, 0x0F, 0x11, 0x13, 0x18, 0x19, 0x1A, 0x1B):
        return (w >> 11) & 31
    # ALU immediates; lb lh lwl lw lbu lhu lwr; ll; sc (writes its success flag to rt)
    if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
              0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x30, 0x38):
        return (w >> 16) & 31
    if op == 0x10 and rs in (0x00, 0x02):  # mfc0
        return (w >> 16) & 31
    if op == 0x11 and rs in (0x00, 0x02):  # mfc1, cfc1
        return (w >> 16) & 31
    if op == 0x12 and rs == 0x03:  # mfv, mfvc (VFPU to GPR; mtv and mtvc are rs 7 and write no GPR)
        return (w >> 16) & 31
    if op == 0x1F:
        funct = w & 0x3F
        if funct in (0x00, 0x04):  # ext, ins
            return (w >> 16) & 31
        if funct == 0x20:  # seb, seh, wsbh, bitrev
            return (w >> 11) & 31
    # clz and clo (SPECIAL 0x16, 0x17) are covered by the rd case above.
    return None


def _is_move(w: int) -> bool:
    """`addu/or rd, rs, zero`: a register copy."""
    return w >> 26 == 0x00 and w & 0x3F in (0x21, 0x25) and (w >> 16) & 31 == 0


def _register_values(
    words: Mapping[int, int],
    addr: int,
    reg: int,
    entry: Any,
    callers: Any,
    depth: int = 1,
) -> set[int]:
    """Constants `reg` can hold at `addr`: `lui`+`addiu/ori` built in this routine, copies
    followed back, and, when the value is an argument of the routine, the constants its
    callers pass (one level of wrapper per `depth`)."""
    low = None
    start = entry(addr)
    pc = addr - 4
    # Without a known entry, read a short window: the value is not followed into callers.
    lowest = start if start is not None else addr - 4 * 4 * _LOOKBACK
    while pc >= lowest:
        w = words.get(pc)
        if w is None:
            return set()
        op = w >> 26
        if op in (0x09, 0x0D) and (w >> 16) & 31 == reg and (w >> 21) & 31 == reg and low is None:
            low = _sx16(w & 0xFFFF) if op == 0x09 else w & 0xFFFF
        elif op == 0x0F and (w >> 16) & 31 == reg:
            return {(((w & 0xFFFF) << 16) + (low or 0)) & 0xFFFFFFFF}
        elif _is_move(w) and (w >> 11) & 31 == reg and low is None:
            reg = (w >> 21) & 31
        elif _gpr_written(w) == reg:
            return set()
        pc -= 4
    if start is None or low is not None or not 4 <= reg <= 11 or depth == 0:
        return set()
    found: set[int] = set()
    for call in callers(start):
        found |= _register_values(words, call + 8, reg, entry, callers, depth - 1)
    return found


def draft_timing_package(
    words: Mapping[int, int],
    imports: Mapping[int, int],
    *,
    function_starts: Sequence[int] = (),
) -> dict[str, Any]:
    """Find the frame-pacing waits a 60 Hz package starts from, in one executable.

    `words` maps every code address to its instruction word, `imports` maps import stub
    addresses to NIDs, and `function_starts` adds symbol entries to the `jal` targets that
    name functions. Two forms are recognised, each rewritten by `vblank_wait_2_to_1`:

    * a VBLANK counter poll: a global that one routine increments by 1 (usually a VBLANK
      sub-interrupt handler), that the wait compares with `slti/sltiu rX, 2`, and that the
      wait clears with `sw zero`;
    * `sceDisplayWaitVblankStartMulti(2)` (or its CB form), with the 2 loaded into `a0`.

    Waits that neither form explains (for example two plain `sceDisplayWaitVblankStart`
    calls per frame) are listed for review, since no transform rewrites them. The routines
    that call each pacing wait are update-boundary candidates: the boundary must be the
    routine that runs once per logic update, which only the title's own code can confirm.
    """
    stub_nid = {addr: nid for addr, nid in imports.items()}
    calls: dict[int, list[int]] = {}
    for addr, w in words.items():
        target = _jal_target(addr, w)
        if target is not None:
            calls.setdefault(target, []).append(addr)
    starts = sorted(set(calls) | {int(a) for a in function_starts})

    def function_of(addr: int) -> int | None:
        lo, hi = 0, len(starts)
        while lo < hi:
            mid = (lo + hi) // 2
            if starts[mid] <= addr:
                lo = mid + 1
            else:
                hi = mid
        return starts[lo - 1] if lo else None

    increments: dict[int, list[int]] = {}
    polls: dict[int, list[int]] = {}
    clears: dict[int, list[int]] = {}
    for addr, w in sorted(words.items()):
        op = w >> 26
        if op == 0x23:  # lw rt, G
            counter = _global_address(words, addr, w)
            if counter is None:
                continue
            rt = (w >> 16) & 31
            for k in (1, 2):
                nxt = words.get(addr + 4 * k, 0)
                if (nxt >> 21) & 31 != rt:
                    continue
                if nxt >> 26 in (0x0A, 0x0B) and _sx16(nxt & 0xFFFF) == 2:
                    polls.setdefault(counter, []).append(addr + 4 * k)
                elif nxt >> 26 == 0x09 and _sx16(nxt & 0xFFFF) == 1:
                    increments.setdefault(counter, []).append(addr + 4 * k)
        elif op == 0x2B and (w >> 16) & 31 == 0:  # sw zero, G
            counter = _global_address(words, addr, w)
            if counter is not None:
                clears.setdefault(counter, []).append(addr)

    handlers: set[int] = set()
    for stub, nid in stub_nid.items():
        if nid != REGISTER_SUB_INTR_HANDLER_NID:
            continue
        for call in calls.get(stub, ()):
            # a2 is the handler, set before the call or in its delay slot.
            handlers |= _register_values(words, call + 8, 6, function_of, lambda fn: calls.get(fn, ()))
    # A registered handler is a routine entry even though nothing calls it directly.
    starts = sorted(set(starts) | {h for h in handlers if h in words})

    pacing: list[dict[str, Any]] = []
    explained_waits: set[int] = set()
    poll_functions: set[int] = set()
    for counter in sorted(polls):
        if counter not in increments or counter not in clears:
            continue
        incrementers = sorted({f for a in increments[counter] if (f := function_of(a)) is not None})
        for site in polls[counter]:
            fn = function_of(site)
            if fn is None:
                continue
            pacing.append({
                "kind": "vblank_counter_poll",
                "address": site,
                "function": fn,
                "insn_offset": site - fn,
                "counter": counter,
                "increments": sorted(increments[counter]),
                "clears": sorted(clears[counter]),
                "handler_registered": any(f in handlers for f in incrementers),
            })
            poll_functions.add(fn)
    for stub, nid in sorted(stub_nid.items()):
        if nid not in VBLANK_WAIT_MULTI_NIDS:
            continue
        for call in calls.get(stub, ()):
            for site in [call + 4] + [call - 4 * k for k in range(1, _LOOKBACK)]:
                word = words.get(site)
                if word is None or (word >> 16) & 31 != 4 or (word >> 21) & 31 != 0:
                    continue
                if word >> 26 in (0x09, 0x0D) and word & 0xFFFF == 2:
                    fn = function_of(site)
                    if fn is not None:
                        pacing.append({"kind": "wait_multi_2", "address": site, "function": fn,
                                       "insn_offset": site - fn, "api": VBLANK_WAIT_MULTI_NIDS[nid]})
                        explained_waits.add(call)
                    break
                if _gpr_written(word) == 4:
                    break

    unexplained = []
    for stub, nid in sorted(stub_nid.items()):
        if nid in VBLANK_WAIT_NIDS or nid in VBLANK_WAIT_MULTI_NIDS:
            for call in calls.get(stub, ()):
                if call not in explained_waits and function_of(call) not in poll_functions:
                    unexplained.append({"call": call, "function": function_of(call),
                                        "api": VBLANK_WAIT_NIDS.get(nid) or VBLANK_WAIT_MULTI_NIDS[nid]})

    boundary: dict[int, list[int]] = {}
    for entry in pacing:
        for call in calls.get(entry["function"], ()):
            caller = function_of(call)
            if caller is not None:
                boundary.setdefault(caller, []).append(call)
    return {
        "pacing_sites": pacing,
        "boundary_candidates": [{"function": fn, "calls": sorted(c)} for fn, c in sorted(boundary.items())],
        "unexplained_waits": unexplained,
    }


def draft_package_declaration(
    draft: Mapping[str, Any],
    words: Mapping[int, int],
    *,
    package_id: str,
    title_id: str,
    executable_sha256: str,
    game_version: str,
) -> tuple[dict[str, Any], dict[str, str]]:
    """A validating package with the draft's pacing sites, and its function-name sidecar.

    The update boundary is filled in only when exactly one routine calls the pacing waits;
    otherwise the candidates stay in the draft report for a person to choose from. The
    package is a starting point: the frame-counted constants of the title are not in it.
    """
    if not draft["pacing_sites"]:
        raise EnhancementPackageError("No frame-pacing wait was recognised; nothing to draft.")
    symbols: dict[str, str] = {}

    def name(fn: int) -> str:
        label = f"draft_fn_{fn:08x}"
        symbols[label] = f"0x{fn:08x}"
        return label

    sites = []
    for i, site in enumerate(draft["pacing_sites"], 1):
        sites.append({
            "id": f"pacing_{i}",
            "function": name(site["function"]),
            "insn_offset": site["insn_offset"],
            "stock_digest": compute_stock_words_digest([words[site["address"]]]),
            "transform": "vblank_wait_2_to_1",
        })
    package: dict[str, Any] = {
        "schema_version": SUPPORTED_SCHEMA_VERSION,
        "id": package_id,
        "display_name": "60 FPS game logic (draft)",
        "version": "0.1.0",
        "category": "timing_frame_presentation",
        "target_title_id": title_id,
        "capabilities": ["logic_60hz_rescale", "vblank_wait_2_to_1"],
        "game_version": game_version,
        "executable_sha256": executable_sha256,
        "rng_policy": {
            "mode": "rescale_probability",
            "reason": "Draft: confirm against the title's per-update random rolls before use.",
        },
        "sites": sites,
    }
    if len(draft["boundary_candidates"]) == 1:
        fn = draft["boundary_candidates"][0]["function"]
        package["update_boundary"] = {
            "function": name(fn),
            "insn_offset": 0,
            "stock_digest": compute_stock_words_digest([words[fn]]),
        }
    validate_package(package)
    return package, symbols


def _executable_view(path: Path) -> tuple[dict[int, int], dict[int, int], list[int], str]:
    """Code words, import NIDs, symbol entries and SHA-256 of one PSP executable."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from analyze import Elf, exec_ranges  # noqa: PLC0415 - only the draft command needs them
    from imports import parse_imports  # noqa: PLC0415

    data = path.read_bytes()
    elf = Elf(data, base=0)
    words: dict[int, int] = {}
    for lo, hi in exec_ranges(elf):
        raw = elf.read_at_vaddr(lo, hi - lo)
        if raw is not None:
            for off in range(0, len(raw) - 3, 4):
                words[lo + off] = int.from_bytes(raw[off:off + 4], "little")
    imports = {addr: nid for addr, (_lib, nid) in parse_imports(elf).items()}
    return words, imports, sorted(elf.func_symbols() or ()), hashlib.sha256(data).hexdigest()


VBLANK_HZ = 60000.0 / 1001.0
RATE_EVENT_KINDS = ("count", "change", "lcg")
RATE_MIN_EVENTS = 20
RATE_Z = 2.576  # two-sided 99 %
RATE_SLOWED_BELOW = 57.0  # the runtime's ENHANCEMENT_LOGIC_SLOWED threshold (section 9.5, item 4)
RATE_MIN_COVERAGE = 0.9   # the shorter run must cover this share of the longer run's game time


def parse_rate_event(spec: str) -> dict[str, Any]:
    """Parse ``KIND:COLUMN[:A:C[:BITS]][=RATIO]`` for --event.

    ``count`` sums the increases of a counter column, ``change`` counts the samples whose value
    differs from the previous one, and ``lcg`` counts the steps a linear congruential generator
    ``x' = (A*x + C) mod 2**BITS`` (BITS 32 unless given) took between two samples. ``RATIO`` is
    the expected enhanced/stock rate per game second (1 unless given).
    """
    body, _, ratio_text = spec.partition("=")
    parts = body.split(":")
    if parts[0] not in RATE_EVENT_KINDS or len(parts) < 2 or not parts[1]:
        raise EnhancementPackageError(
            f"--event {spec!r}: expected KIND:COLUMN with KIND one of {', '.join(RATE_EVENT_KINDS)}")
    event: dict[str, Any] = {"kind": parts[0], "column": parts[1], "spec": spec}
    try:
        event["ratio"] = float(ratio_text) if ratio_text else 1.0
        if parts[0] == "lcg":
            if len(parts) not in (4, 5):
                raise ValueError
            event["a"], event["c"] = int(parts[2], 0), int(parts[3], 0)
            event["bits"] = int(parts[4], 0) if len(parts) == 5 else 32
            if not 1 <= event["bits"] <= 64:
                raise ValueError
        elif len(parts) != 2:
            raise ValueError
    except ValueError:
        raise EnhancementPackageError(
            f"--event {spec!r}: lcg takes lcg:COLUMN:A:C[:BITS]; count and change take KIND:COLUMN; "
            "RATIO is a number") from None
    if not math.isfinite(event["ratio"]) or event["ratio"] <= 0:
        raise EnhancementPackageError(f"--event {spec!r}: the expected ratio must be a positive number")
    return event


def load_sample_rows(path: Path | str, columns: Sequence[str]) -> list[dict[str, str]]:
    """Read an ``SR_SAMPLE_OUT`` CSV (``vblank,<label>...``) and check that it names ``columns``."""
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise EnhancementPackageError(f"{path}: cannot read the sample file ({exc})") from None
    if not lines:
        raise EnhancementPackageError(f"{path}: empty sample file")
    header = lines[0].split(",")
    if header[0] != "vblank":
        raise EnhancementPackageError(f"{path}: the first column must be vblank (an SR_SAMPLE_OUT file)")
    missing = [c for c in columns if c not in header]
    if missing:
        raise EnhancementPackageError(f"{path}: no column {', '.join(missing)} (columns: {', '.join(header[1:])})")
    rows = []
    for n, line in enumerate(lines[1:], start=2):
        cells = line.split(",")
        if len(cells) != len(header):
            raise EnhancementPackageError(f"{path}:{n}: {len(cells)} cells, the header has {len(header)}")
        rows.append(dict(zip(header, cells, strict=True)))
    return rows


def _lcg_steps(a: int, c: int, bits: int, start: int, end: int, limit: int) -> int | None:
    mask = (1 << bits) - 1
    x = start & mask
    end &= mask
    for steps in range(limit + 1):
        if x == end:
            return steps
        x = (a * x + c) & mask
    return None


def measure_rates(rows: Sequence[Mapping[str, str]], *, updates: str, nominal_hz: float,
                  phase: str | None = None, events: Sequence[Mapping[str, Any]] = (),
                  lcg_limit: int = 4096, source: str = "samples") -> dict[str, Any]:
    """Count logic updates, vblanks and events per phase of one sampled run.

    A step from one sample to the next belongs to the phase of the earlier sample. A row that
    repeats the previous row exactly (the sampler wrote one vblank twice) is skipped and counted.
    Anything else that would lose time refuses the file: a vblank that is not an integer, does not
    increase, or repeats with different values, and a row without the updates counter. An event
    counts only over the steps where its column has a value on both sides, and its game time is
    the updates of those same steps, so an unreadable stretch biases neither side. The updates
    counter is read as an unsigned 32-bit count, so it may wrap.
    """
    if not math.isfinite(nominal_hz) or nominal_hz <= 0:
        raise EnhancementPackageError(f"{source}: the nominal rate must be a positive number, got {nominal_hz!r}")
    if lcg_limit < 1:
        raise EnhancementPackageError(f"--lcg-limit must be at least 1, got {lcg_limit}")
    phases: dict[str, dict[str, Any]] = {}
    specs = [e["spec"] for e in events]

    def bucket(name: str) -> dict[str, Any]:
        if name not in phases:
            phases[name] = {"vblanks": 0, "updates": 0, "events": dict.fromkeys(specs, 0),
                            "event_updates": dict.fromkeys(specs, 0), "unresolved": dict.fromkeys(specs, 0)}
        return phases[name]

    def number(index: int, column: str) -> int:
        text = rows[index][column]
        try:
            return int(text)
        except ValueError:
            raise EnhancementPackageError(
                f"{source}:{index + 2}: column {column}: {text!r} is not an integer") from None

    kept: list[int] = []
    duplicates = 0
    for i, row in enumerate(rows):
        if kept and row == rows[kept[-1]]:
            duplicates += 1
            continue
        vblank = number(i, "vblank")
        if kept and vblank <= int(rows[kept[-1]]["vblank"]):
            raise EnhancementPackageError(
                f"{source}:{i + 2}: vblank {vblank} does not follow vblank {rows[kept[-1]]['vblank']} with "
                "new values; the file is not one clean capture")
        if not row[updates]:
            raise EnhancementPackageError(
                f"{source}:{i + 2}: no {updates} value; every sample must read the logic-update counter")
        kept.append(i)

    empty = dict.fromkeys(specs, 0)
    per_second: dict[int, int] = {}
    for i, j in zip(kept, kept[1:], strict=False):
        prev, cur = rows[i], rows[j]
        b = bucket(prev[phase] if phase else "all")
        b["vblanks"] += number(j, "vblank") - number(i, "vblank")
        du = (number(j, updates) - number(i, updates)) & 0xFFFFFFFF
        b["updates"] += du
        second = int(number(j, "vblank") / VBLANK_HZ)
        per_second[second] = per_second.get(second, 0) + du
        for e in events:
            spec, column = e["spec"], e["column"]
            if not prev[column] or not cur[column]:
                empty[spec] += 1
                continue
            b["event_updates"][spec] += du
            if e["kind"] == "change":
                b["events"][spec] += prev[column] != cur[column]
                continue
            x, y = number(i, column), number(j, column)
            if e["kind"] == "count":
                b["events"][spec] += max(0, y - x)
            else:
                steps = _lcg_steps(e["a"], e["c"], e["bits"], x, y, lcg_limit)
                if steps is None:
                    b["unresolved"][spec] += 1
                else:
                    b["events"][spec] += steps
    for b in phases.values():
        _derive_rates(b, nominal_hz)
    return {"nominal_hz": nominal_hz, "phases": phases, "duplicate_rows": duplicates, "empty_steps": empty,
            "per_second": [per_second[s] for s in sorted(per_second)]}


def _derive_rates(b: dict[str, Any], nominal_hz: float) -> None:
    b["seconds"] = b["vblanks"] / VBLANK_HZ
    b["game_seconds"] = b["updates"] / nominal_hz
    b["event_game_seconds"] = {spec: n / nominal_hz for spec, n in b["event_updates"].items()}
    b["updates_per_second"] = b["updates"] / b["seconds"] if b["seconds"] else 0.0


def compare_event_rate(n_stock: int, t_stock: float, n_enh: int, t_enh: float, ratio: float) -> dict[str, Any]:
    """Compare two event counts over game time against an expected rate ratio.

    Given both counts, the enhanced share of the events is binomial with p = R*t2 / (t1 + R*t2) for
    the true ratio R; the 99 % Wilson interval of that share gives an interval for R. The verdict is
    PASS when the expected ratio lies inside it, FAIL when it does not, and NOT_JUDGED with fewer
    than RATE_MIN_EVENTS events or no game time on one side: a rare event is not a pass.
    """
    total = n_stock + n_enh
    result: dict[str, Any] = {"stock": n_stock, "enhanced": n_enh, "expected_ratio": ratio}
    if total < RATE_MIN_EVENTS or t_stock <= 0 or t_enh <= 0:
        result.update(verdict="NOT_JUDGED", ratio=None, low=None, high=None)
        return result
    p = n_enh / total
    z2 = RATE_Z * RATE_Z
    centre = (p + z2 / (2 * total)) / (1 + z2 / total)
    half = RATE_Z * math.sqrt(p * (1 - p) / total + z2 / (4 * total * total)) / (1 + z2 / total)
    scale = t_stock / t_enh

    def to_ratio(share: float) -> float:
        return math.inf if share >= 1.0 else share / (1.0 - share) * scale

    low, high = to_ratio(max(0.0, centre - half)), to_ratio(min(1.0, centre + half))
    measured = (n_enh / t_enh) / (n_stock / t_stock) if n_stock else math.inf
    result.update(ratio=measured, low=low, high=high, verdict="PASS" if low <= ratio <= high else "FAIL")
    return result


def compare_rates(stock: Mapping[str, Any], enhanced: Mapping[str, Any],
                  events: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    """Side-by-side report of two measure_rates() results: per phase and over the whole run.

    A FAIL in any row, the whole run or one phase, is a failure of the run: a phase whose rate is
    wrong must not be hidden by the totals. A phase too short to judge adds its events to the whole
    run and is listed as not judged.
    """
    def total(run: Mapping[str, Any]) -> dict[str, Any]:
        out: dict[str, Any] = {"vblanks": 0, "updates": 0, "events": {e["spec"]: 0 for e in events},
                               "event_updates": {e["spec"]: 0 for e in events},
                               "unresolved": {e["spec"]: 0 for e in events}}
        for b in run["phases"].values():
            for k in ("vblanks", "updates"):
                out[k] += b[k]
            for key in ("events", "event_updates", "unresolved"):
                for spec in out[key]:
                    out[key][spec] += b[key][spec]
        _derive_rates(out, run["nominal_hz"])
        return out

    names = sorted(set(stock["phases"]) | set(enhanced["phases"]), key=lambda n: (len(n), n))
    whole_stock, whole_enh = total(stock), total(enhanced)
    rows = [("all", whole_stock, whole_enh)]
    rows += [(n, stock["phases"].get(n), enhanced["phases"].get(n)) for n in names]
    longer = max(whole_stock["game_seconds"], whole_enh["game_seconds"])
    report: dict[str, Any] = {
        "phases": [], "failures": 0, "unresolved": 0, "failed_phases": [], "unjudged_phases": [],
        # A phase that runs no logic (a loading screen) has no game time and no events, so it may
        # appear in one run only; one that runs logic must appear in both.
        "one_sided_phases": [n for n in names if (n in stock["phases"]) != (n in enhanced["phases"])
                             and (stock["phases"].get(n) or enhanced["phases"][n])["updates"]],
        "coverage": min(whole_stock["game_seconds"], whole_enh["game_seconds"]) / longer if longer else 0.0,
    }
    for name, s, e in rows:
        entry: dict[str, Any] = {"phase": name, "stock": None, "enhanced": None, "logic": "ABSENT", "events": []}
        for side, b in (("stock", s), ("enhanced", e)):
            if b is not None:
                entry[side] = {k: b[k] for k in ("vblanks", "updates", "seconds", "game_seconds",
                                                  "updates_per_second")}
        if e is not None and e["updates"]:
            entry["logic"] = "SLOWED" if e["updates_per_second"] < RATE_SLOWED_BELOW else "OK"
        for ev in events:
            spec = ev["spec"]
            cmp = compare_event_rate(s["events"][spec] if s else 0, s["event_game_seconds"][spec] if s else 0.0,
                                     e["events"][spec] if e else 0, e["event_game_seconds"][spec] if e else 0.0,
                                     ev["ratio"])
            cmp["event"] = spec
            cmp["unresolved"] = (s["unresolved"][spec] if s else 0) + (e["unresolved"][spec] if e else 0)
            entry["events"].append(cmp)
            if cmp["verdict"] == "FAIL":
                report["failures"] += 1
                report["failed_phases"].append(f"{name} {spec}")
            if name == "all":
                report["unresolved"] += cmp["unresolved"]
            elif cmp["verdict"] == "NOT_JUDGED" and ((s and s["updates"]) or (e and e["updates"])):
                report["unjudged_phases"].append(f"{name} {spec}")
        report["phases"].append(entry)
    return report


def _fmt(value: float | None, digits: int = 2) -> str:
    if value is None:
        return "-"
    return "inf" if math.isinf(value) else f"{value:.{digits}f}"


def _rates_main(args: argparse.Namespace) -> int:
    events = [parse_rate_event(spec) for spec in args.event]
    needed = [args.updates] + ([args.phase] if args.phase else []) + [e["column"] for e in events]
    stock = measure_rates(load_sample_rows(args.compare_rates[0], needed), updates=args.updates,
                          nominal_hz=args.stock_hz, phase=args.phase, events=events, lcg_limit=args.lcg_limit,
                          source=str(args.compare_rates[0]))
    enhanced = measure_rates(load_sample_rows(args.compare_rates[1], needed), updates=args.updates,
                             nominal_hz=args.enhanced_hz, phase=args.phase, events=events,
                             lcg_limit=args.lcg_limit, source=str(args.compare_rates[1]))
    report = compare_rates(stock, enhanced, events)
    if not 0.0 <= args.min_coverage <= 1.0:
        raise EnhancementPackageError(f"--min-coverage must lie in [0, 1], got {args.min_coverage!r}")
    print(f"{'phase':<10} {'stock s':>8} {'upd/s':>6} {'enh s':>8} {'upd/s':>6}  logic")
    for p in report["phases"]:
        s, e = p["stock"] or {}, p["enhanced"] or {}
        print(f"{p['phase']:<10} {_fmt(s.get('seconds'), 1):>8} {_fmt(s.get('updates_per_second'), 1):>6} "
              f"{_fmt(e.get('seconds'), 1):>8} {_fmt(e.get('updates_per_second'), 1):>6}  {p['logic']}")
        for c in p["events"]:
            print(f"  {c['event']}: {c['stock']} stock, {c['enhanced']} enhanced; enhanced/stock per game second "
                  f"{_fmt(c['ratio'])} (99 % {_fmt(c['low'])}..{_fmt(c['high'])}, expected {c['expected_ratio']:g})"
                  f" {c['verdict']}" + (f", {c['unresolved']} lcg step(s) unresolved" if c["unresolved"] else ""))
    for label, run in (("stock", stock), ("enhanced", enhanced)):
        print(f"{label} updates per second: " + " ".join(str(v) for v in run["per_second"]))
        empty = ", ".join(f"{spec} {n}" for spec, n in run["empty_steps"].items() if n)
        print(f"{label}: {report['phases'][0][label]['game_seconds']:.1f} game seconds; "
              f"{run['duplicate_rows']} repeated row(s) skipped; steps without a value: {empty or 'none'}")
    if args.json is not None:
        Path(args.json).write_text(json.dumps({"stock": stock, "enhanced": enhanced, "report": report},
                                              indent=2) + "\n", encoding="utf-8")
    whole = report["phases"][0]
    unjudged = sum(c["verdict"] == "NOT_JUDGED" for c in whole["events"])
    # Judged per phase: a phase with no updates at all (a loading screen) lowers the whole run's
    # rate without any logic running slow.
    slowed = [p["phase"] for p in report["phases"][1:] if p["logic"] == "SLOWED"]
    short = report["coverage"] < args.min_coverage
    if report["failures"] or report["unresolved"]:
        verdict = "FAIL"
    elif unjudged or not events or slowed or short or report["one_sided_phases"]:
        verdict = "INCOMPLETE"
    else:
        verdict = "PASS"

    def listed(items: Sequence[str]) -> str:
        return ", ".join(items) if items else "none"

    print(f"rates: {verdict} ({len(events)} event(s): {report['failures']} result(s) differ "
          f"[{listed(report['failed_phases'])}], {unjudged} not judged over the whole run, "
          f"{report['unresolved']} lcg step(s) unresolved; logic below {RATE_SLOWED_BELOW:g} updates/s in "
          f"{listed(slowed)}; phases in one run only: {listed(report['one_sided_phases'])}; "
          f"the shorter run covers {report['coverage']:.0%} of the longer one's game time"
          f"{' (below ' + format(args.min_coverage, '.0%') + ')' if short else ''}; "
          f"phase results not judged: {listed(report['unjudged_phases'])})")
    return {"PASS": 0, "FAIL": 1, "INCOMPLETE": 2}[verdict]


def _draft_main(args: argparse.Namespace) -> int:
    words, imports, starts, sha = _executable_view(args.draft)
    draft = draft_timing_package(words, imports, function_starts=starts)
    for site in draft["pacing_sites"]:
        extra = (f" counter 0x{site['counter']:08x}, handler registered: {site['handler_registered']}"
                 if site["kind"] == "vblank_counter_poll" else f" {site['api']}(2)")
        print(f"pacing {site['kind']} at 0x{site['address']:08x} (FUN_{site['function']:08x}"
              f" +{site['insn_offset']}):{extra}")
    for cand in draft["boundary_candidates"]:
        print(f"update boundary candidate FUN_{cand['function']:08x} (calls the wait at "
              + ", ".join(f"0x{c:08x}" for c in cand["calls"]) + ")")
    for wait in draft["unexplained_waits"]:
        fn = f"FUN_{wait['function']:08x}" if wait["function"] is not None else "?"
        print(f"review: {wait['api']} called at 0x{wait['call']:08x} in {fn}; no transform rewrites it")
    try:
        package, symbols = draft_package_declaration(
            draft, words, package_id=args.id, title_id=args.title_id,
            executable_sha256=sha, game_version=args.game_version)
    except EnhancementPackageError as exc:
        print(f"draft: FAIL: {exc}", file=sys.stderr)
        return 1
    out = Path(args.out)
    out.write_text(json.dumps(package, indent=2) + "\n", encoding="utf-8")
    out.with_name(out.stem + ".symbols.json").write_text(json.dumps(symbols, indent=2) + "\n", encoding="utf-8")
    print(f"draft: wrote {out} ({len(package['sites'])} pacing site(s), "
          f"update boundary {'set' if 'update_boundary' in package else 'not chosen'})")
    return 0


def _publish_main(args: argparse.Namespace) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from analyze import Elf  # noqa: PLC0415 - only the publish command reads an executable

    package = load_package(args.publish)
    sidecar = args.publish.with_suffix(".symbols.json")
    symbols = load_symbol_map(sidecar) if sidecar.is_file() else {}
    data = args.elf.read_bytes()
    elf = Elf(data, base=int(args.base, 16) if args.base else None)
    # Names come only from the project's own symbol map (a decompilation's, or names the project
    # chose), never from the executable's symbol table: those are strings copied from the game.
    game_names = sorted(set(symbols) & set(extract_elf_func_symbols(elf)))
    if game_names:
        print(f"publish: FAIL: {len(game_names)} name(s) in {sidecar.name} are the executable's own symbols, "
              f"such as {game_names[0]!r}; a published package names sites with project-authored names only",
              file=sys.stderr)
        return 1
    read_words, read_data = elf_readers(elf)
    notes: list[str] = []
    published = publish_package(
        package, symbols, executable=data, disc_id=args.disc_id, disc_version=args.disc_version,
        read_words_fn=read_words, read_data_fn=read_data, data_root=args.data_root, notes=notes,
        read_const_fn=elf_constant_reader(elf))
    for note in notes:
        print(f"warning: enhancement {note}", file=sys.stderr)
    resolved, _ = resolve_published_package(
        published, executable=data, read_words_fn=read_words, read_data_fn=read_data, data_root=args.data_root)
    # The same declaration, except that each store site's stock_values are now their keyed digests.
    expected = validate_package(copy.deepcopy(package))
    for idx, site in enumerate(expected.get("sites", ())):
        if "stock_values" in site:
            del site["stock_values"]
            site["stock_value_digests"] = published["sites"][idx]["stock_value_digests"]
    if compute_package_digest(resolved) != compute_package_digest(expected):
        print("publish: FAIL: the published package does not resolve back to the same declaration",
              file=sys.stderr)
        return 1
    args.out.write_text(json.dumps(published, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{args.out}: published {len(published.get('sites', ()))} site(s), "
          f"{len(published['site_map'])} name(s); declaration digest {compute_package_digest(resolved)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate Nakagawa enhancement package declarations.")
    parser.add_argument("packages", nargs="*", type=Path, help="Package JSON files to validate.")
    draft = parser.add_argument_group("draft a 60 Hz package from a title's executable")
    draft.add_argument("--draft", type=Path, metavar="ELF",
                       help="Decrypted executable (for example the package cache's selected.elf).")
    draft.add_argument("--title-id", help="The library title_id the package targets.")
    draft.add_argument("--id", default="draft-60hz", help="Package id (default draft-60hz).")
    draft.add_argument("--game-version", default="1.00", help="Game version (default 1.00).")
    draft.add_argument("--out", type=Path, help="Where to write the draft package JSON.")
    rates = parser.add_argument_group("compare a stock and an enhanced run side by side (section 9.7)")
    rates.add_argument("--compare-rates", nargs=2, type=Path, metavar=("STOCK_CSV", "ENHANCED_CSV"),
                       help="Two SR_SAMPLE_OUT files of the same route: the authentic run, then the enhanced run.")
    rates.add_argument("--updates", default="updates", help="Column of the logic-update counter (default updates).")
    rates.add_argument("--phase", help="Column whose value names the phase (for example a game-mode word).")
    rates.add_argument("--event", action="append", default=[], metavar="KIND:COLUMN[=RATIO]",
                       help="count:COL, change:COL or lcg:COL:A:C[:BITS], with an optional expected ratio.")
    rates.add_argument("--stock-hz", type=float, default=30.0, help="Logic updates per game second, stock (30).")
    rates.add_argument("--enhanced-hz", type=float, default=60.0, help="Logic updates per game second, enhanced (60).")
    rates.add_argument("--lcg-limit", type=int, default=4096, help="Most LCG steps searched between two samples.")
    rates.add_argument("--min-coverage", type=float, default=RATE_MIN_COVERAGE,
                       help="Share of the longer run's game time the shorter run must cover (0.9).")
    rates.add_argument("--json", type=Path, help="Also write the measurements and the report as JSON.")
    publish = parser.add_argument_group("publish a package as site data (docs/ENHANCEMENT_CONTRACT.md section 5.1)")
    publish.add_argument("--publish", type=Path, metavar="PACKAGE",
                         help="A private package to publish; its <package>.symbols.json names its sites.")
    publish.add_argument("--elf", type=Path, help="The decrypted executable the package names.")
    publish.add_argument("--base", help="Load address of the executable, in hex, as codegen's --base.")
    publish.add_argument("--disc-id", help="The disc ID (PARAM.SFO DISC_ID), for example UCUS12345.")
    publish.add_argument("--disc-version", help="The disc version (PARAM.SFO DISC_VERSION), for example 1.01.")
    publish.add_argument("--data-root", type=Path, help="The title's extracted files, for data_files.")
    args = parser.parse_args(argv)
    if args.publish is not None:
        if args.elf is None or not args.disc_id or not args.disc_version or args.out is None:
            parser.error("--publish needs --elf, --disc-id, --disc-version and --out")
        try:
            return _publish_main(args)
        except EnhancementPackageError as exc:
            print(f"publish: FAIL: {exc}", file=sys.stderr)
            return 1
    if args.compare_rates is not None:
        try:
            return _rates_main(args)
        except EnhancementPackageError as exc:
            print(f"rates: FAIL: {exc}", file=sys.stderr)
            return 1
    if args.draft is not None:
        if not args.title_id or args.out is None:
            parser.error("--draft needs --title-id and --out")
        return _draft_main(args)
    if not args.packages:
        parser.error("name package files to validate, --draft an executable, or --compare-rates two runs")

    failures = 0
    for pkg_path in args.packages:
        try:
            raw = load_package(pkg_path)
            validate_package(raw)
            print(f"{pkg_path}: OK")
        except EnhancementPackageError as exc:
            print(f"{pkg_path}: FAIL: {exc}", file=sys.stderr)
            failures += 1

    return 1 if failures > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
