# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The public PSP homebrew corpus (#2144): load, validate, and check it against its pins.

The corpus lists open-source PSPSDK sample programs. Their sources are not copied into
this repository: they ship in the PSPDEV release archive that ``assets/upstream/pspdev.lock.json``
pins by SHA-256, and each file is named here by the Git blob SHA-1 it has at one exact PSPSDK
commit. ``verify_sources`` recomputes those blob IDs from an installed PSPDEV, so a PSPDEV that
carries different sources than the pin is reported, never used silently.

Nothing here touches the network or builds anything.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any, NoReturn

ROOT = Path(__file__).resolve().parent.parent
CORPUS_PATH = ROOT / "fixtures" / "homebrew_corpus" / "corpus.json"
PSPDEV_LOCK_PATH = ROOT / "assets" / "upstream" / "pspdev.lock.json"

#: Licences GPL-3.0-or-later can take in (AGENTS.md Section 4); the corpus admits only these.
COMPATIBLE_LICENSES = frozenset({"BSD-3-Clause", "BSD-2-Clause", "MIT", "Apache-2.0", "Zlib", "CC0-1.0", "Unlicense"})
MIN_PROGRAMS = 3

_SHA1 = re.compile(r"^[0-9a-f]{40}$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_ID = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")


class CorpusError(ValueError):
    """The corpus file is malformed, unpinned, or disagrees with the PSPDEV lock."""


def load_corpus(path: Path = CORPUS_PATH) -> dict[str, Any]:
    corpus: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    return corpus


def git_blob_sha1(data: bytes) -> str:
    """The Git object ID of a file's bytes: sha1 of ``blob <size>``, a NUL byte, then the bytes."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()  # noqa: S324 (a Git ID, not security)


def _fail(message: str) -> NoReturn:
    raise CorpusError(message)


def _check_file_entry(program_id: str, entry: dict[str, Any], seen: set[str]) -> None:
    path = entry.get("path")
    if not isinstance(path, str) or not path or path.startswith("/") or ".." in path.split("/"):
        _fail(f"{program_id}: a file path must be relative and stay inside the samples root: {path!r}")
    if path in seen:
        _fail(f"{program_id}: {path} is listed twice")
    seen.add(path)
    if not _SHA1.match(str(entry.get("git_blob_sha1", ""))):
        _fail(f"{program_id}: {path} has no 40-hex git_blob_sha1")
    upstream = entry.get("upstream_path")
    if not isinstance(upstream, str) or not upstream.startswith("src/samples/"):
        _fail(f"{program_id}: {path} must name its upstream path under src/samples/")


def validate(corpus: dict[str, Any], lock: dict[str, Any] | None = None) -> None:
    """Raise CorpusError unless the corpus is complete, pinned and licence-compatible."""
    if corpus.get("schema_version") != 1:
        _fail("schema_version must be 1")
    source = corpus.get("source", {})
    if source.get("license_expression") not in COMPATIBLE_LICENSES:
        _fail(f"licence {source.get('license_expression')!r} is not GPL-3.0-or-later compatible")
    if not re.match(r"^[0-9a-f]{40}$", str(source.get("commit", ""))):
        _fail("source.commit must be an exact 40-hex revision, never a branch or tag")
    if not _SHA1.match(str(source.get("license_blob_sha1", ""))):
        _fail("source.license_blob_sha1 is missing")
    delivery = source.get("delivery", {})
    if not _SHA256.match(str(delivery.get("archive_sha256", ""))):
        _fail("source.delivery.archive_sha256 is missing")

    if lock is None:
        lock = json.loads(PSPDEV_LOCK_PATH.read_text(encoding="utf-8"))
    pinned = lock["components"]["pspsdk"]
    if source["commit"] != pinned["commit"]:
        _fail(f"corpus pins PSPSDK {source['commit']}, the PSPDEV lock pins {pinned['commit']}")
    if source["license_expression"] != pinned["license_expression"]:
        _fail("corpus licence differs from the licence the PSPDEV lock records for PSPSDK")
    dist = lock["distribution"]
    if delivery.get("archive_sha256") != dist["archive_sha256"] or delivery.get("release") != dist["release"]:
        _fail("corpus delivery differs from the release and SHA-256 the PSPDEV lock pins")

    vocabulary = set(corpus.get("check_vocabulary", []))
    statuses = set(corpus.get("statuses", []))
    programs = corpus.get("programs", [])
    if len(programs) < MIN_PROGRAMS:
        _fail(f"the corpus needs at least {MIN_PROGRAMS} programs, it has {len(programs)}")
    ids: set[str] = set()
    for program in programs:
        program_id = program.get("id", "")
        if not _ID.match(program_id) or program_id in ids:
            _fail(f"program id {program_id!r} is malformed or repeated")
        ids.add(program_id)
        seen: set[str] = set()
        for entry in program.get("files", []) + program.get("shared_files", []):
            _check_file_entry(program_id, entry, seen)
        sample_dir = program.get("sample_dir")
        if not isinstance(sample_dir, str) or not sample_dir:
            _fail(f"{program_id}: sample_dir is required")
        if f"{sample_dir}/Makefile" not in seen:
            _fail(f"{program_id}: the program's Makefile is not pinned")
        if not program.get("copyright") or not program.get("title") or not program.get("target"):
            _fail(f"{program_id}: title, target and copyright holders are required")
        checks = program.get("smoke_checks", [])
        if "first_frame" not in checks or not set(checks) <= vocabulary:
            _fail(f"{program_id}: smoke_checks must include first_frame and use only the vocabulary")
        expected = program.get("expected", {})
        status = expected.get("status")
        if status not in statuses:
            _fail(f"{program_id}: expected.status {status!r} is not one of {sorted(statuses)}")
        if status == "PASS":
            _fail(f"{program_id}: PASS is refused: the corpus schema has no evidence field yet and every result "
                  "stays UNMEASURED until a smoke run has measured it")
        if "evidence" in expected:
            _fail(f"{program_id}: expected.evidence is refused on {status}: only a measured PASS can carry evidence")
        issue = expected.get("issue")
        if status == "KNOWN_FAIL" and not (type(issue) is int and issue > 0):
            _fail(f"{program_id}: KNOWN_FAIL needs the number of the issue that tracks it")


def verify_sources(corpus: dict[str, Any], samples_root: Path) -> list[str]:
    """Recompute every pinned file's Git blob ID under ``samples_root``; return the differences."""
    differences: list[str] = []
    for program in corpus["programs"]:
        for entry in program["files"] + program["shared_files"]:
            file_path = samples_root / entry["path"]
            if not file_path.is_file():
                differences.append(f"{entry['path']}: missing under {samples_root}")
                continue
            actual = git_blob_sha1(file_path.read_bytes())
            if actual != entry["git_blob_sha1"]:
                differences.append(f"{entry['path']}: blob {actual}, pinned {entry['git_blob_sha1']}")
    return differences
