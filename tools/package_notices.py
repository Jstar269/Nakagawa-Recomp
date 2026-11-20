#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic third-party notice generation and licensing gate for native packages.

Enumerates bundled binaries, resolves and validates required license notices from
``assets/third_party_components.json`` (the single machine-readable source of truth)
with the local toolchain as a fallback source of the same texts, emits the
THIRD_PARTY_NOTICES bundle and LGPL relinking materials (RELINK.md), and fails
closed if any bundled binary lacks a license record.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from functools import lru_cache
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Any

# Python's embeddable distribution, which the release ZIP bundles for BUILD PACKAGE, never puts the
# script's own folder on sys.path (its ._pth file wins). tools/copy_build_assets.ps1 runs this script at
# the end of every compile, so add tools/ before importing the sibling packages, as nk_cli.py does.
_TOOLS_DIR = str(Path(__file__).resolve().parent)
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

from makefile_text import read_makefile  # noqa: E402
from nk_core.prereq_fetcher import PrerequisiteFetchError, default_data_root  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

#: Machine-readable source of truth for every third-party component that can enter
#: a release artifact. The native package, the SBOM and the release gate all read
#: this one file.
COMPONENTS_IDENTITY = "assets/third_party_components.json"
COMPONENTS_PATH = ROOT / COMPONENTS_IDENTITY

try:
    from title_codegen_plan import PackageRouteError
except ImportError:
    class PackageRouteError(Exception):  # type: ignore[no-redef]
        def __init__(self, code: str, message: str) -> None:
            super().__init__(f"{code}: {message}")
            self.code = code
            self.message = message


@lru_cache(maxsize=16)
def _load_component_inventory_cached(source_path: str) -> dict[str, Any]:
    """Return the third-party component inventory, failing closed when unusable.

    The inventory is the authority for every license record, so a missing or
    malformed file is a named packaging failure rather than a silent fallback to
    an empty record set that would let an unrecorded binary through.
    """
    source = Path(source_path)
    if not source.is_file():
        raise PackageRouteError(
            "PACKAGE_LICENSE_RECORD_MISSING",
            f"LICENSE_RECORD_SOURCE_MISSING: third-party component inventory "
            f"{COMPONENTS_IDENTITY} is missing from the source tree",
        )
    try:
        data = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PackageRouteError(
            "PACKAGE_LICENSE_RECORD_MISSING",
            f"LICENSE_RECORD_SOURCE_UNREADABLE: {COMPONENTS_IDENTITY} could not be parsed: {exc}",
        ) from exc
    dlls = data.get("native", {}).get("dlls")
    if not isinstance(dlls, dict) or not dlls:
        raise PackageRouteError(
            "PACKAGE_LICENSE_RECORD_MISSING",
            f"LICENSE_RECORD_SOURCE_INCOMPLETE: {COMPONENTS_IDENTITY} declares no native.dlls records",
        )
    returned: dict[str, Any] = data
    return returned


def load_component_inventory(path: Path | None = None) -> dict[str, Any]:
    source = (path or COMPONENTS_PATH).resolve()
    return _load_component_inventory_cached(str(source))


def notice_file_name(record: dict[str, Any]) -> str:
    """Return the notice filename for a component record.

    The inventory names it explicitly so the emitted filename stays stable when a
    display name is reworded; a record without one falls back to a slug.
    """
    declared = record.get("notice_file")
    if declared:
        return str(declared)
    return re.sub(r"[^A-Za-z0-9._-]", "_", record["name"]).strip("_") + ".txt"


def component_license_sources(
    record: dict[str, Any], toolchain_root: Path | None, repo_root: Path
) -> list[tuple[Path, str]]:
    """Resolve every declared license text, preferring in-tree copies.

    Each inventory entry is required. The local toolchain is a per-entry
    fallback; finding one declared text must not hide another missing notice.
    """
    sources: list[tuple[Path, str]] = []
    missing: list[str] = []
    for entry in record.get("license_texts", []):
        in_tree = repo_root / entry["file"]
        toolchain_rel = entry.get("toolchain_license_rel")
        toolchain_copy = toolchain_root / toolchain_rel if toolchain_root and toolchain_rel else None
        expected_sha256 = entry.get("sha256")
        if toolchain_copy is not None and toolchain_copy.is_file() and expected_sha256:
            try:
                actual_sha256 = hashlib.sha256(toolchain_copy.read_bytes()).hexdigest()
            except OSError as exc:
                raise PackageRouteError(
                    "PACKAGE_LICENSE_TEXT_MISSING",
                    f"LICENSE_TEXT_DRIFT: cannot read the toolchain license for "
                    f"{record.get('name', 'component')} ({toolchain_rel}): {exc}",
                ) from exc
            if actual_sha256 != expected_sha256:
                raise PackageRouteError(
                    "PACKAGE_LICENSE_TEXT_MISSING",
                    f"LICENSE_TEXT_DRIFT: toolchain license for "
                    f"{record.get('name', 'component')} ({toolchain_rel}) does not match "
                    f"the recorded SHA-256 {expected_sha256}",
                )
        if in_tree.is_file():
            sources.append((in_tree, entry["file"]))
            continue
        if toolchain_copy is not None and toolchain_copy.is_file():
            sources.append((toolchain_copy, toolchain_rel))
            continue
        missing.append(entry["file"])

    if missing:
        component = record.get("name", "component")
        declared = ", ".join(missing)
        raise PackageRouteError(
            "PACKAGE_LICENSE_TEXT_MISSING",
            f"LICENSE_TEXT_MISSING: required license text(s) for {component} "
            f"were not found in the source tree or local toolchain: {declared}",
        )
    return sources


@lru_cache(maxsize=16)
def _known_runtime_dlls_for(repo_root_path: str) -> dict[str, dict[str, Any]]:
    """Map every lowercased bundleable DLL name to its inventory record.

    Includes ``host_resolved_components`` so a loader a user places beside the
    package still gets a named notice rather than an unexplained refusal.
    """
    repo_root = Path(repo_root_path)
    inventory = load_component_inventory(repo_root / COMPONENTS_IDENTITY)
    records: dict[str, dict[str, Any]] = {}
    for dll_name, record in inventory["native"]["dlls"].items():
        records[dll_name.lower()] = record
    for record in inventory["native"].get("host_resolved_components", []):
        dll = record.get("dll")
        if dll:
            records[dll.lower()] = record
    return records


def known_runtime_dlls(repo_root: Path = ROOT) -> dict[str, dict[str, Any]]:
    return _known_runtime_dlls_for(str(repo_root.resolve()))


def packaged_dll_names(repo_root: Path = ROOT) -> set[str]:
    """Return the lowercased DLL names the packaging routes may copy into a package.

    Deliberately excludes ``host_resolved_components``: the Vulkan loader is
    resolved from the host and this repository does not redistribute it
    (NOTICE.md), so it is recorded for attribution but never staged.
    """
    return {
        name.lower()
        for name, record in load_component_inventory(
            repo_root.resolve() / COMPONENTS_IDENTITY
        )["native"]["dlls"].items()
        if record.get("disposition") == "copied_into_package"
    }


@lru_cache(maxsize=16)
def _system_dll_patterns_for(repo_root_path: str) -> tuple[re.Pattern[str], ...]:
    inventory = load_component_inventory(Path(repo_root_path) / COMPONENTS_IDENTITY)
    return tuple(
        re.compile(pattern, re.IGNORECASE)
        for pattern in inventory["native"]["system_dll_patterns"]
    )


def system_dll_patterns(repo_root: Path = ROOT) -> tuple[re.Pattern[str], ...]:
    return _system_dll_patterns_for(str(repo_root.resolve()))


def __getattr__(name: str) -> Any:
    """Build legacy inventory constants only when an importer asks for them."""
    value: Any
    if name == "KNOWN_RUNTIME_DLLS":
        value = known_runtime_dlls()
    elif name == "SYSTEM_DLL_PATTERNS":
        value = system_dll_patterns()
    else:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    globals()[name] = value
    return value


def installed_package_version(toolchain_root: Path | None, package: str | None) -> str:
    """Return the installed MSYS2 package version for `package`, or "unknown".

    Reads `<msys root>/var/lib/pacman/local/<prefix>-<package>-<version>/desc` for the
    toolchain's own package prefix (for example mingw-w64-ucrt-x86_64-).
    """
    if toolchain_root is None or not package:
        return "unknown"
    local_db = toolchain_root.parent / "var" / "lib" / "pacman" / "local"
    if not local_db.is_dir():
        return "unknown"
    wanted = re.compile(rf"^mingw-w64-[a-z0-9_]+-x86_64-{re.escape(package)}$")
    for entry in sorted(local_db.iterdir()):
        desc = entry / "desc"
        if not desc.is_file():
            continue
        fields = desc.read_text(encoding="utf-8", errors="replace").split("\n\n")
        values = {}
        for field in fields:
            lines = field.strip().splitlines()
            if len(lines) >= 2 and lines[0].startswith("%"):
                values[lines[0]] = lines[1].strip()
        if wanted.match(values.get("%NAME%", "")):
            return values.get("%VERSION%", "unknown")
    return "unknown"


def is_system_dll(name: str) -> bool:
    """Check if a DLL name is a standard Windows system DLL."""
    clean = name.strip()
    return any(pat.fullmatch(clean) is not None for pat in system_dll_patterns())


def _prerequisite_toolchain_root() -> Path | None:
    """The pinned-prerequisite installer's UCRT64 root, when installed.

    the installer extracts each MSYS2 package under ``<data root>/prerequisites/msys64``,
    so its toolchain root is ``<data root>/prerequisites/msys64/ucrt64`` with
    ``bin/`` and ``share/licenses/`` coming from the packages themselves.
    """
    try:
        data_root = default_data_root()
    except PrerequisiteFetchError:
        return None
    returned: Path | None = data_root / "prerequisites" / "msys64" / "ucrt64"
    return returned


def resolve_toolchain_root() -> Path | None:
    """Discover the local compiler toolchain root (e.g. C:/msys64/ucrt64)."""
    def is_toolchain_root(candidate: Path) -> bool:
        # A compiler toolchain root carries its packages' license texts; a bare gcc on PATH
        # (for example one bundled with another application) does not qualify.
        return (candidate / "bin" / "gcc.exe").is_file() and (candidate / "share" / "licenses").is_dir()

    candidates: list[Path] = []
    mingw_env = os.environ.get("MINGW_PREFIX")
    if mingw_env:
        candidates.append(Path(mingw_env))
    msys_bin_env = os.environ.get("MSYS_PATH")
    if msys_bin_env:
        msys_bin = Path(msys_bin_env)
        candidates.append(msys_bin.parent if msys_bin.name.lower() == "bin" else msys_bin)
    gcc_path = shutil.which("gcc")
    if gcc_path:
        gcc_bin = Path(gcc_path).resolve().parent
        if gcc_bin.name.lower() == "bin":
            candidates.append(gcc_bin.parent)
    # The one-consent installer's pinned toolchain outranks the hardcoded MSYS2
    # fallback, so the clean-PATH route stages the closure from the toolchain the
    # build just used.
    prerequisite_root = _prerequisite_toolchain_root()
    if prerequisite_root is not None:
        candidates.append(prerequisite_root)
    candidates.append(Path("C:/msys64/ucrt64"))
    for candidate in candidates:
        if is_toolchain_root(candidate):
            return candidate.resolve()
    return None


def _pe_import_failure(binary_path: Path, detail: str) -> PackageRouteError:
    return PackageRouteError(
        "PACKAGE_BUILD_FAILED",
        f"cannot parse PE imports from {binary_path.name}: {detail}",
    )


def parse_pe_imports(binary_path: Path) -> list[str]:
    """Parse a valid PE import table, refusing malformed binaries instead of claiming no imports."""
    try:
        data = binary_path.read_bytes()
    except OSError as exc:
        raise _pe_import_failure(binary_path, f"file read failed ({type(exc).__name__})") from exc
    if len(data) < 64 or data[:2] != b"MZ":
        raise _pe_import_failure(binary_path, "DOS header is missing or truncated")

    try:
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise _pe_import_failure(binary_path, "PE signature or COFF header is missing")
        num_sections = struct.unpack_from("<H", data, pe_offset + 6)[0]
        optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
        optional_offset = pe_offset + 24
        optional_end = optional_offset + optional_size
        if optional_end > len(data):
            raise _pe_import_failure(binary_path, "optional header is truncated")
        optional_magic = struct.unpack_from("<H", data, optional_offset)[0]
        if optional_magic == 0x10B:  # PE32
            directory_count_offset, directory_offset = 92, 96
        elif optional_magic == 0x20B:  # PE32+
            directory_count_offset, directory_offset = 108, 112
        else:
            raise _pe_import_failure(binary_path, "optional header format is unsupported")
        if optional_size < directory_count_offset + 4:
            raise _pe_import_failure(binary_path, "optional header omits the data-directory count")
        directory_count = struct.unpack_from("<I", data, optional_offset + directory_count_offset)[0]
        if directory_count <= 1:
            return []
        if optional_size < directory_offset + 16:
            raise _pe_import_failure(binary_path, "import data-directory entry is truncated")
        import_rva, import_size = struct.unpack_from(
            "<II", data, optional_offset + directory_offset + 8
        )
        if import_rva == 0:
            if import_size != 0:
                raise _pe_import_failure(binary_path, "empty import RVA has a nonzero size")
            return []
        if import_size < 20 or import_size % 20:
            raise _pe_import_failure(binary_path, "import table size is not a descriptor span")

        section_offset = optional_end
        sections: list[tuple[int, int, int, int]] = []
        for index in range(num_sections):
            header_offset = section_offset + index * 40
            if header_offset + 40 > len(data):
                raise _pe_import_failure(binary_path, "section table is truncated")
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
                "<IIII", data, header_offset + 8
            )
            if raw_size and (raw_offset > len(data) or raw_size > len(data) - raw_offset):
                raise _pe_import_failure(binary_path, "section raw data extends past end of file")
            sections.append((virtual_address, max(virtual_size, raw_size), raw_size, raw_offset))

        def rva_to_file_range(rva: int, size: int = 1) -> tuple[int, int] | None:
            for virtual_address, span, raw_size, raw_offset in sections:
                if virtual_address <= rva < virtual_address + span:
                    delta = rva - virtual_address
                    if delta > raw_size or size > raw_size - delta:
                        return None
                    return raw_offset + delta, raw_offset + raw_size
            return None

        descriptor_range = rva_to_file_range(import_rva, import_size)
        if descriptor_range is None:
            raise _pe_import_failure(binary_path, "import table RVA is not file-backed")
        descriptor_offset, _descriptor_end = descriptor_range

        dlls: list[str] = []
        terminated = False
        for index in range(import_size // 20):
            offset = descriptor_offset + index * 20
            fields = struct.unpack_from("<IIIII", data, offset)
            if not any(fields):
                terminated = True
                break
            name_rva = fields[3]
            name_range = rva_to_file_range(name_rva)
            if name_range is None:
                raise _pe_import_failure(binary_path, "import name RVA is not file-backed")
            name_offset, name_limit = name_range
            name_end = data.find(b"\0", name_offset, name_limit)
            if name_end == -1:
                raise _pe_import_failure(binary_path, "import name is not NUL-terminated")
            try:
                dll_name = data[name_offset:name_end].decode("ascii").strip()
            except UnicodeDecodeError as exc:
                raise _pe_import_failure(binary_path, "import name is not ASCII") from exc
            if not dll_name:
                raise _pe_import_failure(binary_path, "import name is empty")
            dlls.append(dll_name)
        if not terminated:
            raise _pe_import_failure(binary_path, "import table has no terminating descriptor")
        return dlls
    except struct.error as exc:
        raise _pe_import_failure(binary_path, "PE structure is truncated") from exc


def get_binary_imports(binary_path: Path, toolchain_root: Path | None = None) -> list[str]:
    """Deterministically enumerate DLL names imported by a PE binary using objdump -p or PE fallback."""
    objdump = shutil.which("objdump")
    if not objdump and toolchain_root:
        candidate = toolchain_root / "bin" / "objdump.exe"
        if candidate.is_file():
            objdump = str(candidate)

    if objdump:
        try:
            res = subprocess.run(
                [objdump, "-p", str(binary_path)],
                capture_output=True,
                text=True,
                check=True,
            )
            dlls: list[str] = []
            for line in res.stdout.splitlines():
                stripped = line.strip()
                if stripped.startswith("DLL Name:"):
                    dll_name = stripped.split(":", 1)[1].strip()
                    if dll_name:
                        dlls.append(dll_name)
            if dlls:
                return sorted(set(dlls))
        except (subprocess.SubprocessError, OSError):
            pass

    return sorted(set(parse_pe_imports(binary_path)))


def make_relative_path(path: Path, toolchain_root: Path | None, repo_root: Path) -> str:
    """Convert an absolute path to a toolchain-relative or repo-relative POSIX path.

    Never emits absolute local machine paths or user profiles into distribution artifacts.
    """
    resolved = path.resolve()
    if toolchain_root is not None:
        try:
            return resolved.relative_to(toolchain_root.resolve()).as_posix()
        except ValueError:
            pass
    try:
        return resolved.relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        pass
    return path.name


def atrac3p_object_names(repo_root: Path = ROOT) -> list[str]:
    """Return the ATRAC3+ object paths the Makefile builds, derived from ATRAC3P_SRCS."""
    try:
        text = read_makefile(repo_root)
    except OSError:
        return []
    match = re.search(r"^ATRAC3P_SRCS\s*:?=\s*((?:.*\\\n)*.*)$", text, re.MULTILINE)
    if not match:
        return []
    sources = match.group(1).replace("\\\n", " ").split()
    objects = [
        "$(BUILD_DIR)/atrac3p_" + src[len("src/rt/atrac3p/"):-2] + ".o"
        for src in sources
        if src.startswith("src/rt/atrac3p/") and src.endswith(".c")
    ]
    objects.append("$(BUILD_DIR)/atrac3p_bridge.o")
    return objects


def h264_sub_source_files(repo_root: Path = ROOT) -> list[str]:
    """Return the H.264 translation units selected by the Makefile's H264_SUB_SRCS."""
    try:
        text = read_makefile(repo_root)
    except OSError:
        return []
    match = re.search(r"^H264_SUB_SRCS\s*:?=\s*((?:.*\\\n)*.*)$", text, re.MULTILINE)
    if not match:
        return []
    sources = match.group(1).replace("\\\n", " ").split()
    return [
        source for source in sources
        if source.startswith("src/rt/h264_ffmpeg_sub/") and source.endswith(".c")
    ]


def _h264_feature_enabled(
    repo_root: Path,
    object_glob: str,
    package_dir: Path | None = None,
) -> bool:
    recorded = os.environ.get("H264_FFMPEG", "").strip()
    if recorded:
        return recorded == "1"
    if package_dir is not None and any(package_dir.glob("h264sub_*.o")):
        return True
    if any(repo_root.glob(object_glob)):
        return True
    target_os = os.environ.get("NK_TARGET_OS", "").strip().lower()
    if target_os:
        return target_os != "windows"
    return os.name != "nt"


PROJECT_REPOSITORY_URL = "https://github.com/Jstar269/nakagawa-recomp"


def _git_text(repo_root: Path, *args: str) -> str | None:
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo_root), *args],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def source_reference(repo_root: Path = ROOT) -> dict[str, Any]:
    """The exact source a package was built from.

    The project's own code is GPL-3.0-or-later, the statically linked xBRZ
    scaler and the player's pspdecrypt-derived decryption files are
    GPL-3.0-only, and the FFmpeg ATRAC3+ subset is LGPL-2.1-or-later,
    statically linked. The maintainer's decision is
    that the complete corresponding source is the relink mechanism, so every
    package names the repository, commit and (when built from one) tag it came
    from, and says so plainly when the working tree carried local changes.
    Without git, the commit is recorded as unknown rather than guessed.
    """
    commit = _git_text(repo_root, "rev-parse", "HEAD")
    tag = _git_text(repo_root, "describe", "--tags", "--exact-match", "HEAD") if commit else None
    status = _git_text(repo_root, "status", "--porcelain", "--untracked-files=no") if commit else None
    return {
        "repository": PROJECT_REPOSITORY_URL,
        "commit": commit or None,
        "tag": tag or None,
        "local_changes": bool(status) if status is not None else None,
    }


def render_source_notice(
    reference: dict[str, Any], components: list[dict[str, Any]] | None = None
) -> str:
    """SOURCE.txt: where the complete corresponding source of this build is."""
    commit = reference.get("commit") or "unknown (built without git metadata)"
    lines = [
        "COMPLETE CORRESPONDING SOURCE",
        "",
        "This package is Nakagawa Recomp. The project's own code is licensed under",
        "the GNU General Public License version 3 or later. The package also",
        "includes the xBRZ texture scaler, which is licensed under the GNU General",
        "Public License version 3 only, and the Nakagawa player links decryption",
        "files ported from pspdecrypt, which are licensed under the GNU General",
        "Public License version 3 only except the KIRK engine (NOTICE.md in the",
        "source). It statically links a subset of the FFmpeg",
        "ATRAC3+ decoder, which is licensed under the GNU Lesser General Public",
        "License version 2.1 or later. The complete corresponding source of this",
        "build, including that subset, is available from:",
        "",
        f"  Repository: {reference.get('repository', PROJECT_REPOSITORY_URL)}",
        f"  Commit:     {commit}",
    ]
    if reference.get("tag"):
        lines.append(f"  Tag:        {reference['tag']}")
    if reference.get("local_changes"):
        lines.extend([
            "",
            "This build was made from a working tree with local changes to tracked",
            "files; its source is the commit above plus those changes.",
        ])
    source_components = [
        component for component in (components or [])
        if component.get("disposition") in ("copied_into_package", "statically_linked_subset")
        and ("LGPL" in str(component.get("spdx_id", ""))
             or "GPL-" in str(component.get("spdx_id", "")))
    ]
    if source_components:
        lines.extend([
            "",
            "LGPL and GPL components in this package:",
            "",
        ])
        for component in source_components:
            item = component["name"]
            if component.get("binary"):
                item += f" ({component['binary']})"
            lines.append(f"  Component: {item}")
            if component.get("msys2_package"):
                lines.append(
                    f"  MSYS2 package: {component['msys2_package']} "
                    f"{component['msys2_package_version']}"
                )
            elif component.get("version"):
                lines.append(f"  Version: {component['version']}")
            lines.append(f"  Upstream source: {component['upstream_origin']}")
            lines.append("")
        dll_components = [component for component in source_components if component.get("binary")]
        if dll_components:
            lines.extend([
                "WRITTEN SOURCE OFFER",
                "",
                "For three years after distribution of this package, the distributor",
                "offers to provide the complete corresponding source code, including",
                "build and installation scripts, for each LGPL or GPL runtime DLL",
                "listed above. Send a written request through the project's GitHub",
                "Discussions Help category at:",
                "",
                "  https://github.com/Jstar269/Nakagawa-Recomp/discussions/categories/help",
                "",
                "Identify this package, the DLL, and its MSYS2 package version. The",
                "upstream source URLs above identify each component's source project.",
            ])
    lines.extend([
        "",
        "You may modify that source, including the ATRAC3+ subset, rebuild, and",
        "relink the program; RELINK.md describes the steps.",
    ])
    return "\n".join(lines) + "\n"


def render_relink_guide(
    repo_root: Path = ROOT,
    *,
    h264_enabled: bool | None = None,
    h264_object_glob: str = "build/*/h264sub_*.o",
) -> str:
    """Generate the RELINK.md instructions explaining how to relink the runtime against modified ATRAC3+."""
    if h264_enabled is None:
        h264_enabled = _h264_feature_enabled(repo_root, h264_object_glob)
    objects = atrac3p_object_names(repo_root)
    object_lines = "\n".join(f"- `{obj}`" for obj in objects) or "- (see `ATRAC3P_SRCS` in the Makefile)"
    guide = f"""# RELINK.md — Relinking Against Modified FFmpeg ATRAC3+ Objects

Nakagawa Recomp incorporates a subset of the FFmpeg ATRAC3+ audio decoder (`src/rt/atrac3p/`), licensed under the GNU Lesser General Public License version 2.1 or later (LGPL-2.1-or-later).

Under LGPL v2.1 Section 6 you may modify the ATRAC3+ decoder source code and relink the recompiled application executable with your modified library objects.

The project's own code is licensed under the GNU General Public License version 3 or later. The program also includes the xBRZ texture scaler, which is licensed under the GNU General Public License version 3 only, and the Nakagawa player links decryption files ported from pspdecrypt, which are licensed under the GNU General Public License version 3 only except the KIRK engine (`NOTICE.md` in the source). The complete corresponding source is published, so you can rebuild and relink every part of it, not only the decoder. `SOURCE.txt` in this package names the exact repository commit this build came from.

## 1. ATRAC3+ Source Files

The decoder subset lives in `src/rt/atrac3p/` (import record: `src/rt/atrac3p/PROVENANCE.md`). The bridge interface is `src/rt/atrac3p_bridge.c` and `src/rt/atrac3p_bridge.h`.

## 2. Decoder Object Files

The Makefile (`ATRAC3P_SRCS`) builds these objects in `$(BUILD_DIR)`:

{object_lines}

## 3. How to Relink

1. **Modify the source** inside `src/rt/atrac3p/`.
2. **Rebuild the runtime objects** (this includes the ATRAC3+ objects):

   ```powershell
   $env:Path = "C:\\msys64\\ucrt64\\bin;$env:Path"
   mingw32-make runtime-objects GAME_NAME=<game>
   ```

3. **Relink the application**:

   ```powershell
   mingw32-make compile GAME_NAME=<game>
   ```

   The `compile` target links the generated game translation units, the runtime objects and the rebuilt ATRAC3+ objects into `<game>.exe`.
"""
    if not h264_enabled:
        return guide
    h264_sources = h264_sub_source_files(repo_root)
    h264_source_lines = "\n".join(f"- `{source}`" for source in h264_sources)
    if not h264_source_lines:
        h264_source_lines = "- (see `H264_SUB_SRCS` in the Makefile)"
    return guide + f"""
## 4. FFmpeg H.264 Source Files and Objects

This package includes the FFmpeg H.264 decoder subset in `src/rt/h264_ffmpeg_sub/`, licensed under LGPL-2.1-or-later. The Makefile's `H264_SUB_SRCS` selects these translation units:

{h264_source_lines}

Their object files match `{h264_object_glob}`. Rebuild them with `H264_FFMPEG=1`, then relink the application with the same `compile` target above. `SOURCE.txt` names the pinned FFmpeg source revision.
"""


def _validate_spdx_id(spdx_id: Any, comp_type: str, comp_name: str) -> None:
    if not spdx_id or "NOASSERTION" in str(spdx_id) or "UNKNOWN" in str(spdx_id):
        raise PackageRouteError(
            "PACKAGE_LICENSE_RECORD_MISSING",
            f"LICENSE_RECORD_INCOMPLETE: {comp_type} {comp_name!r} "
            f"lacks a valid SPDX license identifier ({spdx_id!r})",
        )


def _promote_notice_bundle(package_dir: Path, staged_root: Path) -> None:
    """Replace all generated notice outputs as a rollback-capable transaction."""
    outputs = ("THIRD_PARTY_NOTICES", "SOURCE.txt", "RELINK.md", "THIRD_PARTY_NOTICES.txt")
    backup_root = staged_root / ".previous-notices"
    backup_root.mkdir()
    moved_previous: list[str] = []
    moved_new: list[str] = []
    try:
        for name in outputs:
            target = package_dir / name
            if target.exists() or target.is_symlink():
                target.rename(backup_root / name)
                moved_previous.append(name)
        for name in outputs:
            (staged_root / name).rename(package_dir / name)
            moved_new.append(name)
    except OSError as exc:
        for name in reversed(moved_new):
            target = package_dir / name
            if target.is_dir() and not target.is_symlink():
                shutil.rmtree(target)
            elif target.exists() or target.is_symlink():
                target.unlink()
        for name in reversed(moved_previous):
            (backup_root / name).rename(package_dir / name)
        raise PackageRouteError(
            "PACKAGE_BUILD_FAILED",
            f"cannot replace the generated notice bundle ({type(exc).__name__})",
        ) from exc


def _copyright_notices(record: dict[str, Any], label: str) -> list[str]:
    """The record's copyright_notices lines, refusing anything but a list of strings."""
    notices = record.get("copyright_notices")
    if notices is None:
        return []
    # A string would iterate as characters and a dict as keys: refuse anything but a list of lines.
    if not isinstance(notices, list) or not all(isinstance(line, str) for line in notices):
        raise PackageRouteError(
            "PACKAGE_LICENSE_RECORD_MISSING",
            f"LICENSE_RECORD_MALFORMED: {label} "
            f"copyright_notices must be a list of strings in {COMPONENTS_IDENTITY}",
        )
    return list(notices)


def generate_package_notices(
    package_dir: Path,
    *,
    repo_root: Path = ROOT,
    toolchain_root: Path | None = None,
    custom_license_map: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Generate THIRD_PARTY_NOTICES bundle, copy verbatim license texts, and enforce licensing gates.

    Raises:
        PackageRouteError("PACKAGE_LICENSE_RECORD_MISSING", ...): if any bundled binary lacks a license.
        PackageRouteError("PACKAGE_LICENSE_TEXT_MISSING", ...): if required license text is missing.
    """
    package_dir = package_dir.resolve()
    if toolchain_root is None:
        toolchain_root = resolve_toolchain_root()

    # Discover all bundled binaries (.exe and .dll)
    binaries: list[Path] = sorted(
        [
            p for p in package_dir.iterdir()
            if p.is_file() and p.suffix.lower() in {".exe", ".dll"}
        ],
        key=lambda p: p.name.lower(),
    )

    # Check imports of all binaries to ensure any dependent toolchain runtime DLLs are staged if available
    if toolchain_root:
        bin_dir = toolchain_root / "bin"
        if bin_dir.is_dir():
            staged_names = packaged_dll_names()
            for binary in list(binaries):
                imports = get_binary_imports(binary, toolchain_root)
                for imp in imports:
                    imp_lower = imp.lower()
                    if imp_lower in staged_names:
                        dst = package_dir / imp
                        if not dst.is_file():
                            src = bin_dir / imp
                            if src.is_file():
                                shutil.copyfile(src, dst)
                                binaries.append(dst)

    binaries = sorted(set(binaries), key=lambda p: p.name.lower())

    # Build component license records
    components: list[dict[str, Any]] = []
    license_files_to_copy: list[tuple[str, Path, str]] = []  # (target_filename, src_path, display_name)

    # Linked/host-resolved components, driven by the inventory rather than a second
    # literal. The LGPL subset ships inside every runtime executable, so it is
    # named whenever the package contains one.
    inventory = load_component_inventory()
    h264_record = next(
        (component for component in inventory["native"].get("linked_components", [])
         if component.get("id") == "ffmpeg-h264"),
        None,
    )
    h264_object_glob = (
        str(h264_record.get("object_glob", "build/*/h264sub_*.o"))
        if h264_record is not None else "build/*/h264sub_*.o"
    )
    h264_enabled = _h264_feature_enabled(repo_root, h264_object_glob, package_dir)
    runtime_dlls = known_runtime_dlls()
    for linked in inventory["native"].get("linked_components", []):
        if linked.get("id") == "ffmpeg-h264" and not h264_enabled:
            continue
        _validate_spdx_id(linked.get("spdx_id"), "linked component", linked.get("name", "linked"))
        sources = component_license_sources(linked, toolchain_root, repo_root)
        if not sources:
            declared = [entry["file"] for entry in linked.get("license_texts", [])]
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                f"LICENSE_TEXT_MISSING: {linked['name']} license text is missing from the "
                f"source tree; {COMPONENTS_IDENTITY} declares {declared}",
            )
        slug = notice_file_name(linked)
        component: dict[str, Any] = {
            "name": linked["name"],
            "spdx_id": linked["spdx_id"],
            "source_path": make_relative_path(sources[0][0], toolchain_root, repo_root),
            "license_file": slug,
            "upstream_origin": linked.get("upstream_origin", "NOASSERTION"),
            "disposition": linked.get("disposition", "linked"),
        }
        if linked.get("upstream_revision"):
            component["version"] = linked["upstream_revision"]
        if linked.get("relink_doc"):
            component["relink_doc"] = linked["relink_doc"]
        notices = _copyright_notices(linked, f"linked component {linked.get('id', linked['name'])!r}")
        if notices:
            component["copyright_notices"] = notices
        components.append(component)
        license_files_to_copy.append((slug, sources[0][0], linked["name"]))
        for extra_src, extra_display in sources[1:]:
            extra_name = f"{Path(slug).stem}-{Path(extra_display).name}"
            component.setdefault("additional_license_files", []).append(extra_name)
            license_files_to_copy.append(
                (extra_name, extra_src, f"{linked['name']} ({Path(extra_display).name})")
            )
        if "object_glob" in linked:
            raise PackageRouteError(
                "PACKAGE_LICENSE_RECORD_MISSING",
                f"LICENSE_RECORD_MALFORMED: linked component {linked.get('id', linked['name'])!r} "
                f"uses the retired single-pattern object_glob; record object_globs in {COMPONENTS_IDENTITY}",
            )
        object_globs = linked.get("object_globs")
        if object_globs is not None:
            # Objects live both beside the build directory and in per-library
            # subdirectories, so one glob cannot name them all: require a list.
            if (
                not isinstance(object_globs, list)
                or not object_globs
                or not all(isinstance(glob, str) and glob for glob in object_globs)
            ):
                raise PackageRouteError(
                    "PACKAGE_LICENSE_RECORD_MISSING",
                    f"LICENSE_RECORD_MALFORMED: linked component {linked.get('id', linked['name'])!r} "
                    f"object_globs must be a non-empty list of glob strings in {COMPONENTS_IDENTITY}",
                )
            component["relink_objects"] = list(object_globs)

    # Process bundled binaries
    for binary in binaries:
        bin_name = binary.name
        bin_lower = bin_name.lower()

        if binary.suffix.lower() == ".exe":
            # Recompiled executable: governed by the project license recorded for
            # the package output, and by the LGPL relink obligations above.
            runtime_record = inventory["native"].get("project_output")
            if runtime_record is None:
                raise PackageRouteError(
                    "PACKAGE_LICENSE_RECORD_MISSING",
                    f"LICENSE_RECORD_SOURCE_INCOMPLETE: {COMPONENTS_IDENTITY} declares no "
                    f"native.project_output record for the packaged executable",
                )
            _validate_spdx_id(runtime_record.get("spdx_id"), "project output", bin_name)
            runtime_sources = component_license_sources(runtime_record, toolchain_root, repo_root)
            if not runtime_sources:
                raise PackageRouteError(
                    "PACKAGE_LICENSE_TEXT_MISSING",
                    f"LICENSE_TEXT_MISSING: project license text is missing for the packaged "
                    f"executable {bin_name!r}",
                )
            components.append({
                "name": f"{runtime_record['name']} ({bin_name})",
                "binary": bin_name,
                "spdx_id": runtime_record["spdx_id"],
                "source_path": make_relative_path(runtime_sources[0][0], toolchain_root, repo_root),
                "license_file": runtime_record.get("notice_file", "Nakagawa-Recomp-LICENSE.txt"),
                "upstream_origin": runtime_record.get("upstream_origin", "NOASSERTION"),
                "disposition": runtime_record.get("disposition", "project_output"),
            })
            license_files_to_copy.append((
                runtime_record.get("notice_file", "Nakagawa-Recomp-LICENSE.txt"),
                runtime_sources[0][0],
                runtime_record["name"],
            ))
            continue

        # For DLLs, match known runtime DLLs or custom mapping
        record = None
        if custom_license_map and bin_lower in custom_license_map:
            record = custom_license_map[bin_lower]
        elif bin_lower in runtime_dlls:
            record = runtime_dlls[bin_lower]

        if record is None:
            raise PackageRouteError(
                "PACKAGE_LICENSE_RECORD_MISSING",
                f"bundled binary {bin_name!r} lacks a license record; packaging refused",
            )

        # Locate the license text files. A caller-supplied license_path wins; every
        # other record resolves through the inventory's license_texts, preferring the
        # in-tree copy so notices generation does not depend on a host toolchain.
        bundled_sources: list[tuple[Path, str]] = []
        if "license_path" in record and Path(record["license_path"]).is_file():
            bundled_sources.append((Path(record["license_path"]), record["license_path"]))
        else:
            bundled_sources = component_license_sources(record, toolchain_root, repo_root)

        if not bundled_sources:
            declared = [entry["file"] for entry in record.get("license_texts", [])]
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                f"LICENSE_TEXT_MISSING: license text for bundled binary {bin_name!r} "
                f"({record.get('name')}) was not found in the source tree or the "
                f"local toolchain; {COMPONENTS_IDENTITY} declares {declared}",
            )

        _validate_spdx_id(record.get("spdx_id"), "bundled binary", bin_name)
        clean_slug = notice_file_name(record)
        target_lic_name = clean_slug
        rel_src = make_relative_path(bundled_sources[0][0], toolchain_root, repo_root)

        version = record.get("version") or installed_package_version(
            toolchain_root, record.get("pacman_package")
        )
        msys2_package = record.get("pacman_package")
        spdx_id = str(record["spdx_id"])
        requires_source_offer = "LGPL" in spdx_id or "GPL-" in spdx_id
        upstream_origin = record.get("upstream_origin", "NOASSERTION")
        if record.get("disposition") == "copied_into_package" and requires_source_offer:
            package_version = installed_package_version(toolchain_root, msys2_package)
            if (
                not msys2_package
                or package_version == "unknown"
                or not isinstance(upstream_origin, str)
                or not upstream_origin.startswith("https://")
            ):
                raise PackageRouteError(
                    "PACKAGE_LICENSE_RECORD_MISSING",
                    f"LICENSE_RECORD_INCOMPLETE: {bin_name!r} needs its exact MSYS2 package "
                    "version and an HTTPS upstream source URL for the written source offer",
                )
            version = package_version
        components.append({
            "name": record["name"],
            "binary": bin_name,
            "version": version,
            "spdx_id": record["spdx_id"],
            "source_path": rel_src,
            "license_file": target_lic_name,
            "upstream_origin": upstream_origin,
            "disposition": record.get("disposition", "bundled_binary"),
        })
        if record.get("disposition") == "copied_into_package" and requires_source_offer:
            components[-1]["msys2_package"] = msys2_package
            components[-1]["msys2_package_version"] = version
        # A credit a licence requires in the distribution documentation (the FTL's
        # "based in part on the work of the FreeType Team") travels with the DLL's record.
        bundled_notices = _copyright_notices(record, f"bundled binary {bin_name!r}")
        if bundled_notices:
            components[-1]["copyright_notices"] = bundled_notices
        license_files_to_copy.append((target_lic_name, bundled_sources[0][0], record["name"]))
        for extra_src, extra_display in bundled_sources[1:]:
            extra_name = f"{Path(clean_slug).stem}-{Path(extra_display).name}"
            components[-1].setdefault("additional_license_files", []).append(extra_name)
            license_files_to_copy.append(
                (extra_name, extra_src, f"{record['name']} ({Path(extra_display).name})")
            )

    # Read and validate every source before replacing any existing package notice.
    license_contents: dict[str, str] = {}
    for target_name, src_file, display_name in license_files_to_copy:
        try:
            text = src_file.read_text(encoding="utf-8", errors="replace").strip()
        except OSError as exc:
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                f"LICENSE_TEXT_MISSING: cannot read the license text for {display_name} "
                f"({type(exc).__name__})",
            ) from exc
        if not text:
            raise PackageRouteError(
                "PACKAGE_LICENSE_TEXT_MISSING",
                f"LICENSE_TEXT_MISSING: license text file for {display_name} is empty",
            )
        license_contents[target_name] = text

    reference = source_reference(repo_root)
    source_content = render_source_notice(reference, components)
    relink_content = render_relink_guide(
        repo_root, h264_enabled=h264_enabled, h264_object_glob=h264_object_glob
    )
    index_data = {
        "schema_version": 1,
        "bundle_type": "native_package_third_party_notices",
        "source": reference,
        "components": components,
    }

    combined_lines = [
        "================================================================================",
        "                       THIRD-PARTY SOFTWARE NOTICES",
        "================================================================================",
        "",
        "This distribution contains third-party software components licensed under",
        "open-source licenses. The complete text of each applicable notice and license",
        "is set forth below. See THIRD_PARTY_NOTICES/index.json for the machine-readable",
        "index, SOURCE.txt for where this build's complete source is, and RELINK.md",
        "for LGPL relinking documentation.",
        "",
    ]
    for comp in components:
        comp_name = comp["name"]
        lic_text = license_contents[comp["license_file"]]
        combined_lines.extend([
            "--------------------------------------------------------------------------------",
            f"Component: {comp_name}",
            f"SPDX License: {comp['spdx_id']}",
            f"Source Path: {comp['source_path']}",
        ])
        if "binary" in comp:
            combined_lines.append(f"Binary: {comp['binary']}")
        if comp.get("version") and comp["version"] != "unknown":
            combined_lines.append(f"Version: {comp['version']}")
        if comp.get("upstream_origin") and comp["upstream_origin"] != "NOASSERTION":
            combined_lines.append(f"Upstream source: {comp['upstream_origin']}")
        if comp.get("msys2_package"):
            combined_lines.append(
                f"MSYS2 package: {comp['msys2_package']} {comp['msys2_package_version']}"
            )
        if "relink_doc" in comp:
            combined_lines.append(f"Relink Guide: {comp['relink_doc']}")
        for notice_line in comp.get("copyright_notices", []):
            combined_lines.append(f"Copyright: {notice_line}")
        combined_lines.extend([
            "--------------------------------------------------------------------------------",
            "",
            lic_text.strip(),
            "",
            "",
        ])
        for extra_file in comp.get("additional_license_files", []):
            combined_lines.extend([license_contents[extra_file].strip(), "", ""])
    combined_content = "\n".join(combined_lines)

    with tempfile.TemporaryDirectory(prefix=".package-notices-", dir=package_dir) as staged_name:
        staged_root = Path(staged_name)
        staged_notices = staged_root / "THIRD_PARTY_NOTICES"
        staged_notices.mkdir()
        for target_name, text in license_contents.items():
            (staged_notices / target_name).write_text(
                text + "\n", encoding="utf-8", newline="\n"
            )
        (staged_root / "SOURCE.txt").write_text(
            source_content, encoding="utf-8", newline="\n"
        )
        (staged_root / "RELINK.md").write_text(
            relink_content, encoding="utf-8", newline="\n"
        )
        (staged_notices / "SOURCE.txt").write_text(
            source_content, encoding="utf-8", newline="\n"
        )
        (staged_notices / "RELINK.md").write_text(
            relink_content, encoding="utf-8", newline="\n"
        )
        (staged_notices / "index.json").write_text(
            json.dumps(index_data, indent=2) + "\n", encoding="utf-8", newline="\n"
        )
        (staged_root / "THIRD_PARTY_NOTICES.txt").write_text(
            combined_content, encoding="utf-8", newline="\n"
        )
        _promote_notice_bundle(package_dir, staged_root)

    return index_data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package_dir", type=Path, help="Directory containing the native package")
    args = parser.parse_args(argv)

    if not args.package_dir.is_dir():
        print(f"PACKAGE_INVALID_PATH: {args.package_dir} is not a directory", file=sys.stderr)
        return 2

    try:
        generate_package_notices(args.package_dir)
        print(f"Generated third-party notices bundle in {args.package_dir}")
        return 0
    except PackageRouteError as exc:
        message = getattr(exc, "message", None)
        if message is None:
            message = str(exc)
        print(f"{exc.code}: {message}", file=sys.stderr)
        return 2
    except Exception as exc:
        print(f"PACKAGE_BUILD_FAILED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
