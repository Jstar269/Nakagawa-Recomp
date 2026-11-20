#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Materialize and audit an exact-ref public-safe source candidate."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from typing import Any

try:
    from . import publish_audit
except ImportError:  # direct script execution
    import publish_audit  # type: ignore[no-redef]

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PROFILE = ROOT / "assets" / "public_source_profile.json"


def load_profile(path: Path) -> dict:
    # Validate through the canonical policy loader first, so schema version,
    # minimum tool version, the mandatory REJECT default and include/exclude
    # self-consistency are all enforced here too. Every consumer of the profile
    # must fail on the same conditions; a second, laxer parser is how a policy
    # ends up meaning different things to different tools.
    try:
        from . import publication_policy
    except ImportError:
        import publication_policy  # type: ignore[no-redef]

    publication_policy.load_policy(path)

    data = json.loads(path.read_text(encoding="utf-8"))
    prefixes = data.get("exclude_prefixes")
    globs = data.get("exclude_globs")
    paths = data.get("exclude_paths")
    if not isinstance(prefixes, list) or not isinstance(globs, list) or not isinstance(paths, list):
        raise ValueError("profile requires exclude_prefixes, exclude_globs, and exclude_paths arrays")
    for value in [*prefixes, *globs, *paths]:
        if not isinstance(value, str) or not value or PurePosixPath(value).is_absolute() or ".." in PurePosixPath(value).parts:
            raise ValueError(f"unsafe exclusion: {value!r}")
    returned: dict[str, Any] = data
    return returned


def is_excluded(relative_path: str, profile: dict) -> bool:
    normalized = PurePosixPath(relative_path).as_posix()
    while normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized in profile["exclude_paths"] or any(
        normalized.startswith(prefix) for prefix in profile["exclude_prefixes"]
    ) or any(fnmatch.fnmatchcase(normalized, pattern) for pattern in profile["exclude_globs"])


def _git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, check=False
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.strip()


def materialize(ref: str, destination: Path, profile_path: Path) -> dict:
    """Materialize the candidate at ``destination`` and return its manifest metadata.

    The manifest is written beside the destination as
    ``<destination>.PUBLIC_CANDIDATE.json``, never inside the tree: the tree must
    stay exactly equal to its declared public scope, and a self-manifest inside it
    is rejected by the export's own audit (#368). Line endings are pinned to LF
    rather than the host default.
    """
    destination = destination.resolve()
    if destination == ROOT or destination.is_relative_to(ROOT):
        raise ValueError("candidate destination must be outside the repository")
    if destination.exists():
        raise FileExistsError(f"candidate destination already exists: {destination}")

    profile = load_profile(profile_path)
    commit = _git("rev-parse", "--verify", f"{ref}^{{commit}}")
    # The tree is built in a sibling staging directory and renamed to the
    # destination only once every entry is written. A refused entry or an OSError
    # partway leaves no half-written destination to block the next run: the
    # staging directory goes with its context.
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nakagawa-public-candidate-") as temp_raw, \
            tempfile.TemporaryDirectory(
                prefix=f".{destination.name}.", suffix=".partial", dir=destination.parent,
            ) as staging_raw:
        archive = Path(temp_raw) / "source.zip"
        _git("archive", "--format=zip", f"--output={archive}", commit)
        staging = Path(staging_raw).resolve() / "tree"
        staging.mkdir()
        copied: list[str] = []
        ref_profile_sha256: str | None = None
        with zipfile.ZipFile(archive) as source_zip:
            for info in source_zip.infolist():
                name = info.filename[:-1] if info.is_dir() else info.filename
                parts = name.split("/")
                # The same refusals as publish_audit._committed_tree_entries: a tree
                # made on another system can carry a drive letter ("D:"), a backslash
                # or an empty or dot part, which a Windows path join would read as a
                # location outside the destination.
                if (name.startswith("/") or "\\" in name or ":" in name
                        or any(part in ("", ".", "..") for part in parts)):
                    raise ValueError(f"unsafe archive path: {info.filename}")
                relative = PurePosixPath(name)
                if not info.is_dir() and relative.as_posix() == "assets/public_source_profile.json":
                    ref_profile_sha256 = hashlib.sha256(source_zip.read(info)).hexdigest()
                if info.is_dir() or is_excluded(relative.as_posix(), profile):
                    continue
                unix_mode = info.external_attr >> 16
                if stat.S_ISLNK(unix_mode):
                    raise ValueError(f"candidate archive contains symlink: {info.filename}")
                target = staging.joinpath(*parts)
                if not target.resolve().is_relative_to(staging):
                    raise ValueError(f"unsafe archive path: {info.filename}")
                target.parent.mkdir(parents=True, exist_ok=True)
                with source_zip.open(info) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
                copied.append(relative.as_posix())
        staging.rename(destination)

    metadata = {
        "profile": profile["name"],
        "source_commit": commit,
        # The profile that filtered the tree, and the one the exported ref itself
        # carries (None when the ref has none); the two differ for an older --ref.
        "profile_sha256": hashlib.sha256(profile_path.read_bytes()).hexdigest(),
        "ref_profile_sha256": ref_profile_sha256,
        "file_count": len(copied),
    }
    manifest = destination.parent / f"{destination.name}.PUBLIC_CANDIDATE.json"
    manifest.write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    return metadata


def audit_candidate(
    destination: Path, provenance_ledger: Path | None = None, profile_path: Path = DEFAULT_PROFILE,
) -> list[publish_audit.Finding]:
    """Audit the materialized tree as a candidate root, judged by the profile that selected it.

    The same mode as ``publish_audit.py --candidate-root --candidate-tree --public-scope
    --policy <profile>``: the bytes are the directory's, the audit is exhaustive, and the
    policy is the one ``materialize`` filtered with, never the checkout's default.
    """
    entries = publish_audit._get_filesystem_entries(destination)
    return publish_audit.audit_entries(
        entries,
        manifest_path=destination / "assets" / "release_manifest.json",
        public_scope=True,
        repo_root=destination,
        exhaustive=True,
        is_candidate_root=True,
        policy_path=profile_path,
        provenance_ledger_path=provenance_ledger,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--ref", default="HEAD", help="Exact Git ref to export (default: HEAD)")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument(
        "--provenance-ledger",
        type=Path,
        default=None,
        help="Release-controlled provenance ledger generated from the private detailed ledger; "
             "required because the materialized candidate's own checked-in ledger is "
             "candidate-controlled evidence and can never be the attestation anchor",
    )
    args = parser.parse_args(argv)
    if args.provenance_ledger is None:
        print(
            "public candidate: FAIL: --provenance-ledger is required; the candidate's own "
            "checked-in ledger cannot attest its own provenance",
            file=sys.stderr,
        )
        return 1
    try:
        profile_path = args.profile.resolve()
        metadata = materialize(args.ref, args.destination, profile_path)
        findings = audit_candidate(args.destination.absolute(), args.provenance_ledger.resolve(), profile_path)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        print(f"public candidate: FAIL: {error}")
        return 2
    if findings:
        for finding in findings:
            print(f"{finding.code}: {finding.path}: {finding.detail}")
        print(f"public candidate: FAIL ({len(findings)} audit findings)")
        return 1
    print(
        f"public candidate: OK ({metadata['file_count']} files from "
        f"{metadata['source_commit']}, profile {metadata['profile']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
