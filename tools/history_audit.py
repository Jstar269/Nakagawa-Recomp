#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Non-destructive full-history secret, proprietary material, and privacy audit tool for Nakagawa Recomp."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import subprocess

try:
    from .nk_core.git_isolation import isolated_git_env
    from .publication_policy import load_policy
except ImportError:
    from nk_core.git_isolation import isolated_git_env  # type: ignore[no-redef]
    from publication_policy import load_policy  # type: ignore[no-redef]

ROOT = Path(__file__).resolve().parent.parent
#: The canonical publication policy; its ``private_roots`` name the local
#: workspace roots that must never appear in published bytes or messages.
POLICY_PATH = Path("assets") / "public_source_profile.json"

HEX_16_BYTES = re.compile(r"\b[0-9a-fA-F]{32}\b")
PRIVATE_KEY_HEADER = re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----")
# GitHub classic, OAuth, user-to-server, server-to-server and refresh tokens are a
# prefix and exactly 36 characters. A real fine-grained PAT body is 82 characters
# (22, an underscore, 59) and Stripe live keys have grown past 24, so those two take a
# minimum length: a fixed count followed by \b never matches a longer real token,
# because the character after the count is also a word character.
API_TOKEN_PATTERN = re.compile(
    r"\b(?:gh[oprsu]_[a-zA-Z0-9]{36}|github_pat_[a-zA-Z0-9_]{80,}|sk_live_[a-zA-Z0-9]{24,}|AKIA[0-9A-Z]{16})\b"
)

# PRIVATE_KEY_HEADER above stays as it is because redaction wants the bare
# delimiter: if one ever reaches a finding's detail string it must be masked.
# Detection is a different question. An armour delimiter with no key body is not
# a secret, and documentation that tells a maintainer which value to paste into a
# CI secret store quotes those delimiters by design -- docs/PROVENANCE_MERGE_GATE.md
# did exactly that and failed this gate for it, with BEGIN and END on one prose
# line and no bytes between them. Requiring a real base64 body after the header
# keeps actual keys caught, since PEM, OpenSSH and encrypted forms all carry one.
PEM_KEY_MATERIAL = re.compile(
    r"-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----"
    r"(?:[^\S\n]*\n)+"          # end of the header line, then any blank lines
    r"(?:[A-Za-z0-9+/=\s]*?)"   # optional PEM headers (Proc-Type, DEK-Info, ...)
    r"[A-Za-z0-9+/]{40,}",      # at least one full-width base64 body line
)

# Sensitive local path patterns (fragmented to prevent self-match). A drive path's
# separator is one backslash or a run of them: JSON, C and Python string literals store
# each separator doubled, and that is the form a path takes in a committed manifest or
# settings file. A UNC path is matched plain or with every backslash doubled; a mixed
# count is not accepted, so an escaped sequence such as \\n\\t in source is not a path.
WINDOWS_USER_PATH = re.compile(r"[a-zA-Z]:\\+(?:" + r"Us" + r"ers|Documents and Settings)\\+[^\s\\/]+", re.IGNORECASE)
POSIX_USER_PATH = re.compile(r"/(?:" + r"ho" + r"me|Us" + r"ers)/[^\s/]+")
MAC_USER_PATH = re.compile(r"/(?:" + r"Us" + r"ers)/[^\s/]+")
WSL_USER_PATH = re.compile(r"/mnt/[a-z]/(?:" + r"Us" + r"ers)/[^\s/]+")
UNC_PATH = re.compile(
    r"\\\\[a-zA-Z0-9_.-]+\\[a-zA-Z0-9_.-]+"
    r"|\\\\\\\\[a-zA-Z0-9_.-]+\\\\[a-zA-Z0-9_.-]+"
)
ONEDRIVE_PATH = re.compile(r"\bOne" + r"Drive\b(?:\s*-\s*[^/\\]+)?", re.IGNORECASE)
TEMP_PATH = re.compile(r"(?:/tm" + r"p/|/var/tm" + r"p/|[a-zA-Z]:\\+(?:[^\s\\/]+\\+)*(?:Windows\\+Te" + r"mp|AppData\\+Local\\+Te" + r"mp)\\+)[a-zA-Z0-9_.-]+", re.IGNORECASE)

# Content signatures are deliberately contextual.  Public SHA-256 values and
# ordinary PSP NIDs are allowed; the audit looks for credentials, private
# operational vocabulary, known proprietary paths, and binary file signatures.
# Keep these signatures high-confidence.  Generic checkout instructions use
# names such as ``place_game_here`` and ``oracle/`` as *absence* or routing
# examples; treating every such public placeholder as proprietary made the
# candidate audit report its own policy and documentation.  The history gate
# still catches those directories when they occur as actual historical paths
# (``FORBIDDEN_PREFIXES`` above), while content scanning is reserved for
# identifiers that denote a real title/private artifact or operational dump.
# Fragment the private-repository literal so this scanner does not report its
# own source blob as a finding.
PRIVATE_REPO_URL = re.compile(
    r"github\.com/" + r"Jstar269/" + r"nakagawa-recomp-" + r"history-private",
    re.IGNORECASE,
)
# A plain title file name (for example a data archive's name) is a publishable fact under
# the maintainer's 2026-09-25 legal posture, so it is not vocabulary here; key material
# names and private save/trace/dump evidence still are.
PRIVATE_OPERATIONAL_VOCABULARY = re.compile(
    r"(?:HST" + r"_PGD_VKEY(?:_HEX)?|"
    r"private[_ -](?:save|trace|dump)\s*(?:baseline|identity|path|location|hash|capture|evidence)\b)",
    re.IGNORECASE,
)
SUSPICIOUS_ENCODED = re.compile(r"^[A-Za-z0-9+/]{256,}={0,2}$")
#: A NUL-free blob that is not valid UTF-8 is still scanned as text when at most one
#: decoded character in this many is a replacement character (U+FFFD).
MIN_CHARACTERS_PER_REPLACEMENT = 20
REPLACEMENT_CHARACTER = chr(0xFFFD)


# Literal prefilters for the content signatures. Each helper returns exactly the verdict
# of the regex it names: the literal it checks is part of every possible match, so a text
# without it cannot match and the regex is skipped. A case-insensitive pattern is checked
# against the lowercased text. No case-insensitive literal contains an i, because the dotless
# i does not lowercase to i, and the long s (U+017F) is spelled out where a word needs it,
# so no match is ever lost. The API-token literals are case-sensitive, so they are checked
# as written.
def _has_api_token(text: str) -> bool:
    if (
        not any(prefix in text for prefix in ("ghp_", "gho_", "ghr_", "ghs_", "ghu_"))
        and "github_pat_" not in text and "sk_live_" not in text and "AKIA" not in text
    ):
        return False
    return API_TOKEN_PATTERN.search(text) is not None


def _has_windows_user_path(text: str) -> bool:
    if ":\\" not in text:  # every match has a drive colon and then a backslash
        return False
    return WINDOWS_USER_PATH.search(text) is not None


def _has_onedrive_path(text: str, lowered: str) -> bool:
    if "onedr" not in lowered:  # every match contains "OneDr" in some letter case
        return False
    return ONEDRIVE_PATH.search(text) is not None


def _has_temp_path(text: str, lowered: str) -> bool:
    # Matches are "/tmp/", "/var/tmp/" or a drive path that ends in a Temp directory.
    if "tmp/" not in lowered and not ("temp\\" in lowered and ":\\" in text):
        return False
    return TEMP_PATH.search(text) is not None


def _has_private_repo_url(text: str) -> bool:
    if "269/" not in text:  # every match contains the account number and a slash
        return False
    return PRIVATE_REPO_URL.search(text) is not None


def _has_private_operational_vocabulary(text: str, lowered: str) -> bool:
    # HST_PGD_VKEY contains "pgd_v". "private" followed by a separator and save, trace or
    # dump contains "vate", the separator and the word, with the long s written as U+017F.
    if "pgd_v" not in lowered and not any(
        "vate" + separator + word in lowered
        for separator in "_ -" for word in ("trace", "dump", "save", "\u017fave")
    ):
        return False
    return PRIVATE_OPERATIONAL_VOCABULARY.search(text) is not None

FORBIDDEN_EXTENSIONS = {
    ".at3", ".bin", ".chd", ".cso", ".dax", ".dmp", ".edat", ".elf", ".gim",
    ".iso", ".nkcap", ".pbp", ".pmf", ".prx", ".psar", ".psess", ".sfo", ".sqlite", ".trace", ".vag"
}

FORBIDDEN_NAMES = {
    "reference_hashes.json", "vfpu_words.txt", "vfpu_words_local.txt",
    "nidseq_mine.txt", "pgd_keys.txt"
}

# keys/ holds a user's local KIRK/amctrl constants (.gitignore; AGENTS.md section 5),
# so nothing under it is ever public.
FORBIDDEN_PREFIXES = (
    "build/", "docs/opengrip_ref/", "fs/", "keys/", "logs/", "memstick/", "opengrip_ref/",
    "OpenGrip_For_Inspiration/", "oracle/", "original_game/", "place_game_here/",
    "third_party/ghidra/exports/", "third_party/ghidra/projects/"
)


@dataclass(frozen=True)
class HistoryFinding:
    category: str  # DEFINITE_SECRET, POSSIBLE_CREDENTIAL, PROPRIETARY_ARTIFACT, PRIVACY_METADATA, FALSE_POSITIVE, UNREACHABLE_OBJECT
    code: str
    commit: str
    path: str
    detail: str

    def to_dict(self, redact: bool = True) -> dict:
        d = asdict(self)
        if redact:
            # Mask any accidental sensitive fragments in detail
            detail = d["detail"]
            detail = HEX_16_BYTES.sub("[REDACTED_HEX_KEY]", detail)
            detail = PRIVATE_KEY_HEADER.sub("[REDACTED_PRIVATE_KEY_HEADER]", detail)
            detail = API_TOKEN_PATTERN.sub("[REDACTED_API_TOKEN]", detail)
            d["detail"] = detail
        return d


def _git(cmd: list[str], repo_root: Path = ROOT) -> str:
    res = subprocess.run(
        ["git", *cmd],
        cwd=repo_root,
        env=isolated_git_env(root=repo_root),
        capture_output=True,
        check=False,
    )
    if res.returncode != 0:
        err = res.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(cmd)} failed: {err}")
    return res.stdout.decode("utf-8", errors="replace")


def _object_listing(repo_root: Path) -> list[str]:
    """Every reachable object as a ``<sha> <path>`` line, from one ``rev-list``."""
    return _git(["rev-list", "--objects", "--all"], repo_root=repo_root).splitlines()


#: A gitlink (submodule) entry names a commit, not a blob.
GITLINK_MODE = "160000"


def _path_listing(repo_root: Path) -> list[tuple[str, str, str]]:
    """Every ``(mode, object id, path)`` entry any reachable commit's tree holds, each once.

    ``rev-list --objects`` names each object once, under the first path it meets, so a blob
    whose bytes also sit at an innocent path (an empty file, a copy) never shows its
    forbidden one. Every entry of a reachable commit's tree is either new against the
    commit's first parent or already in that parent's tree, and a root commit adds its
    whole tree; so the first-parent raw diffs of all reachable commits name every
    ``(path, object)`` pair at least once.
    """
    output = _git(
        ["log", "--all", "--root", "--diff-merges=first-parent", "--raw", "--no-renames",
         "--no-abbrev", "-z", "--format="],
        repo_root=repo_root,
    )
    entries: dict[tuple[str, str], str] = {}
    tokens = output.split("\0")
    index = 0
    while index < len(tokens):
        header = tokens[index].lstrip("\n")
        index += 1
        if not header.startswith(":"):
            continue
        if index >= len(tokens):
            raise RuntimeError(f"git log --raw: entry without a path: {header!r}")
        path = tokens[index]
        index += 1
        fields = header[1:].split()
        if len(fields) != 5:
            raise RuntimeError(f"git log --raw: malformed entry header {header!r}")
        _, new_mode, _, new_id, _status = fields
        if set(new_id) == {"0"}:
            continue  # a deletion: the old entry was introduced, and listed, earlier
        entries.setdefault((new_id, path), new_mode)
    return [(mode, object_id, path) for (object_id, path), mode in entries.items()]


def get_repository_baseline(repo_root: Path = ROOT, raw_objects: list[str] | None = None) -> dict:
    commit_count = int(_git(["rev-list", "--count", "--all"], repo_root=repo_root).strip())
    if raw_objects is None:
        raw_objects = _object_listing(repo_root)
    ref_list = _git(["for-each-ref"], repo_root=repo_root).splitlines()
    try:
        main_sha = _git(["rev-parse", "origin/main"], repo_root=repo_root).strip()
    except Exception:
        try:
            main_sha = _git(["rev-parse", "HEAD"], repo_root=repo_root).strip()
        except Exception:
            main_sha = "unknown"

    return {
        "git_commit_main": main_sha,
        "total_commits": commit_count,
        "total_objects": len(raw_objects),
        "total_refs": len(ref_list),
        "ref_sample": ref_list[:15],
    }


def _path_finding_id(obj_sha: str, mode: str) -> str:
    """The identity a path finding reports for its object.

    A blob carries the same identity as the content findings ("blob:" plus 12 hex), so one
    reviewed blob entry (exact bytes, code and path) can accept every finding on those
    bytes. A gitlink names a commit, so it carries a reviewed commit entry's identity (12
    hex) and a ``commit`` review entry can accept it. The tree entry's mode says which it
    is, so no git process runs per finding. The path listing names blobs and gitlinks
    only, never a tree.
    """
    return obj_sha[:12] if mode == GITLINK_MODE else "blob:" + obj_sha[:12]


def audit_history_tree_paths(repo_root: Path = ROOT,
                             path_entries: list[tuple[str, str, str]] | None = None) -> list[HistoryFinding]:
    """Pass 1: Audit all historical tree entry paths across every reachable commit."""
    findings: list[HistoryFinding] = []
    if path_entries is None:
        path_entries = _path_listing(repo_root)

    for mode, obj_sha, rel_path in path_entries:
        rel_path_clean = rel_path
        while rel_path_clean.startswith("./"):
            rel_path_clean = rel_path_clean[2:]
        lower_p = rel_path_clean.lower()

        # Check forbidden prefixes
        for prefix in FORBIDDEN_PREFIXES:
            if lower_p.startswith(prefix.lower()):
                findings.append(HistoryFinding(
                    category="PROPRIETARY_ARTIFACT",
                    code="HISTORICAL_PATH_PREFIX",
                    commit=_path_finding_id(obj_sha, mode),
                    path=rel_path_clean,
                    detail=f"Reachable historical object under private/proprietary prefix '{prefix}'",
                ))
                break

        # Check forbidden names
        name = Path(rel_path_clean).name
        if name.lower() in FORBIDDEN_NAMES:  # every entry is lowercase
            findings.append(HistoryFinding(
                category="PROPRIETARY_ARTIFACT",
                code="HISTORICAL_GAME_ARTIFACT",
                commit=_path_finding_id(obj_sha, mode),
                path=rel_path_clean,
                detail=f"Reachable historical game-derived file '{name}'",
            ))

        # Check forbidden extensions
        ext = Path(rel_path_clean).suffix.lower()
        if ext in FORBIDDEN_EXTENSIONS:
            findings.append(HistoryFinding(
                category="PROPRIETARY_ARTIFACT",
                code="HISTORICAL_PROHIBITED_EXTENSION",
                commit=_path_finding_id(obj_sha, mode),
                path=rel_path_clean,
                detail=f"Reachable historical blob with prohibited binary extension '{ext}'",
            ))

    return findings


def _named_objects(raw_objects: list[str]) -> dict[str, str]:
    """Each object of a ``rev-list --objects`` listing that has a path, with that path."""
    candidates: dict[str, str] = {}
    for line in raw_objects:
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        object_id, path = parts
        candidates.setdefault(object_id, path)
    return candidates


def _object_info(repo_root: Path, ids: list[str]) -> dict[str, tuple[str, int]]:
    """Type and size of every object in ``ids``, from one ``cat-file --batch-check``.

    A reachable object git cannot read (a corrupt loose object or pack, a promisor object
    absent from a partial clone) is reported as ``missing`` with exit status 0. The audit
    raises on it, and on any id without its line: an object it cannot read is an object
    it has not scanned, so the gate must not report OK.
    """
    if not ids:
        return {}
    proc = subprocess.run(
        ["git", "cat-file", "--batch-check"], cwd=repo_root,
        env=isolated_git_env(root=repo_root),
        input=("".join(f"{object_id}\n" for object_id in ids)).encode("ascii"),
        capture_output=True, check=False,
    )
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git cat-file --batch-check failed: {err}")
    lines = proc.stdout.decode("ascii", errors="replace").splitlines()
    if len(lines) != len(ids):
        raise RuntimeError(f"git cat-file --batch-check answered {len(lines)} of {len(ids)} objects")
    info: dict[str, tuple[str, int]] = {}
    for object_id, line in zip(ids, lines, strict=True):
        fields = line.split()
        if len(fields) != 3 or fields[0] != object_id or not fields[2].isdigit():
            raise RuntimeError(f"reachable object {object_id} cannot be read: {line.strip()!r}")
        info[object_id] = (fields[1], int(fields[2]))
    return info


def _reachable_blob_ids(repo_root: Path,
                        raw_objects: list[str] | None = None,
                        object_info: dict[str, tuple[str, int]] | None = None) -> dict[str, str]:
    """Return each reachable blob object exactly once, with one observed path."""
    if raw_objects is None:
        raw_objects = _object_listing(repo_root)
    candidates = _named_objects(raw_objects)
    if object_info is None:
        object_info = _object_info(repo_root, list(candidates))
    return {
        object_id: path
        for object_id, path in candidates.items()
        if object_info[object_id][0] == "blob"
    }


def _binary_magic(data: bytes) -> str | None:
    if data.startswith((b"~PSP", b"~SCE")):
        return "encrypted PSP module"
    if data.startswith(b"\x7fELF"):
        return "ELF executable"
    if data.startswith(b"\0PBP"):
        return "PSP PBP"
    if data[4:8] == b"PGF0" or data.startswith((b"PGF0", b"\0PGF")):
        return "PSP PGF font"
    if data.startswith((b"\x03\x02\x23\x07", b"\x07\x23\x02\x03")):
        return "SPIR-V shader bytecode"
    if len(data) >= 0x8006 and data[0x8001:0x8006] == b"CD001":
        return "ISO9660 image"
    return None


def _read_blobs(repo_root: Path, blobs: dict[str, str]):
    """Yield ``(object id, path, bytes)`` for every blob, or raise if git could not read all.

    ``git cat-file --batch`` that dies partway (a corrupt object, a failed pack read)
    leaves short output. A scan that stopped there and reported OK would pass a history it
    never read, so the exit status, every header and every byte count are checked.
    """
    ids = list(blobs)
    proc = subprocess.Popen(
        ["git", "cat-file", "--batch"], cwd=repo_root,
        env=isolated_git_env(root=repo_root),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    output, errors = proc.communicate(("".join(f"{object_id}\n" for object_id in ids)).encode("ascii"))
    if proc.returncode != 0:
        err = errors.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git cat-file --batch failed: {err}")
    pos = 0
    for object_id in ids:
        end = output.find(b"\n", pos)
        header = output[pos:end].split() if end >= 0 else []
        if (len(header) != 3 or header[0].decode("ascii", errors="replace") != object_id
                or header[1] != b"blob" or not header[2].isdigit()):
            shown = output[pos:end if end >= 0 else pos + 80]
            raise RuntimeError(f"git cat-file --batch did not return blob {object_id}: {shown!r}")
        size = int(header[2])
        pos = end + 1
        data = output[pos:pos + size]
        if len(data) != size or output[pos + size:pos + size + 1] != b"\n":
            raise RuntimeError(f"git cat-file --batch returned {len(data)} of {size} bytes of {object_id}")
        pos += size + 1
        yield object_id, blobs[object_id], data
    if pos != len(output):
        raise RuntimeError("git cat-file --batch returned output past the last requested blob")


def _has_secret(text: str) -> bool:
    return PEM_KEY_MATERIAL.search(text) is not None or _has_api_token(text)


def _has_local_path(text: str, lowered: str) -> bool:
    """A user, share, cloud-folder or temp path.

    POSIX_USER_PATH also matches wherever MAC_USER_PATH or WSL_USER_PATH would (each of
    their matches contains a macOS user-folder segment that POSIX_USER_PATH covers), so
    those two are not searched again.
    """
    return bool(
        _has_windows_user_path(text) or POSIX_USER_PATH.search(text) or UNC_PATH.search(text)
        or _has_onedrive_path(text, lowered) or _has_temp_path(text, lowered)
    )


def private_root_patterns(roots: tuple[str, ...]) -> list[re.Pattern[str]]:
    """One case-insensitive pattern per distinct private root, in every separator form.

    A root's slash and backslash spellings are one root: each separator matches a slash or a
    run of backslashes (the doubled form JSON, C and Python literals store). The root must
    stand alone, so an escaped newline in source such as ``spec:`` + backslash + ``nkind``,
    or a longer name that only starts with the root, is not a match.
    """
    separator = r"(?:/|\\+)"
    patterns: dict[str, re.Pattern[str]] = {}
    for root in roots:
        parts = [re.escape(part) for part in re.split(r"[\\/]+", root) if part]
        if not parts:
            continue
        body = separator.join(parts)
        body = (separator if root[0] in "/\\" else r"(?<![A-Za-z0-9_])") + body + r"(?![A-Za-z0-9_])"
        patterns.setdefault(body, re.compile(body, re.IGNORECASE))
    return list(patterns.values())


def audit_history_blob_contents(repo_root: Path = ROOT,
                                raw_objects: list[str] | None = None,
                                private_roots: tuple[str, ...] | None = None,
                                object_info: dict[str, tuple[str, int]] | None = None) -> list[HistoryFinding]:
    """Pass 2: scan every reachable blob's content once, not just its path.

    ``private_roots`` defaults to the canonical policy's; the policy file itself is the
    one blob allowed to name them.
    """
    findings: list[HistoryFinding] = []
    roots = policy_private_roots(repo_root) if private_roots is None else private_roots
    root_patterns = private_root_patterns(roots)
    policy_path = POLICY_PATH.as_posix()
    blobs = _reachable_blob_ids(repo_root, raw_objects, object_info)
    if not blobs:
        return findings
    for object_id, path, data in _read_blobs(repo_root, blobs):
        size = len(data)
        commit = f"blob:{object_id[:12]}"
        magic = _binary_magic(data)
        # Path, root, repository and vocabulary signatures are not run over binary blobs:
        # replacement decoding turns random bytes (for example public lookup tables)
        # into plausible path text. A NUL byte marks a blob as binary, and so do bytes
        # that are not UTF-8 when they carry a binary signature or are not mostly text
        # (a NUL-free lookup table decodes to a replacement character in about one
        # character of seven). A control character (an ANSI colour code in a log, a
        # form feed in source) or a stray byte that is not UTF-8 (a cp1252 accent) does
        # not make a text binary: it is scanned, through a replacement decoding where
        # needed, or a private path in a coloured log would pass the gate.
        text: str | None = None
        if b"\0" not in data:
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError:
                replaced = data.decode("utf-8", errors="replace")
                if magic is None and replaced.count(REPLACEMENT_CHARACTER) * MIN_CHARACTERS_PER_REPLACEMENT <= len(replaced):
                    text = replaced
        # Key armour and token prefixes are anchored ASCII signatures that random bytes
        # do not produce, so they are searched in every blob, binary ones included.
        if _has_secret(text if text is not None else data.decode("latin-1")):
            findings.append(HistoryFinding("DEFINITE_SECRET", "HISTORICAL_BLOB_SECRET", commit, path,
                                           "reachable blob contains a private-key or API-token signature [REDACTED]"))
        lowered = text.lower() if text is not None else ""
        if text is not None and _has_local_path(text, lowered):
            findings.append(HistoryFinding("PRIVACY_METADATA", "HISTORICAL_BLOB_LOCAL_PATH", commit, path,
                                           "reachable blob contains a private local path [REDACTED]"))
        if (text is not None and path != policy_path
                and any(pattern.search(text) for pattern in root_patterns)):
            findings.append(HistoryFinding("PRIVACY_METADATA", "HISTORICAL_BLOB_PRIVATE_ROOT", commit, path,
                                           "reachable blob names a configured private root [REDACTED]"))
        if text is not None and _has_private_repo_url(text):
            findings.append(HistoryFinding("PRIVACY_METADATA", "HISTORICAL_BLOB_PRIVATE_REPO", commit, path,
                                           "reachable blob names a private repository [REDACTED]"))
        if text is not None and _has_private_operational_vocabulary(text, lowered):
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_PRIVATE_VOCABULARY", commit, path,
                                           "reachable blob contains private/game-derived operational vocabulary [REDACTED]"))
        if magic:
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_MAGIC", commit, path,
                                           f"reachable blob contains forbidden/proprietary magic: {magic}"))
        # Base64 tools wrap their output (76 columns for MIME and encodebytes), so the
        # payload is matched with its line breaks and other whitespace removed.
        if text is not None and size >= 1024 * 1024 and SUSPICIOUS_ENCODED.fullmatch("".join(text.split())):
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_ENCODED_PAYLOAD", commit, path,
                                           "large reachable blob is a suspicious encoded payload"))
    return findings


def policy_private_roots(repo_root: Path = ROOT) -> tuple[str, ...]:
    """Return the canonical policy's ``private_roots``; a repository without a policy has none.

    A policy that exists but cannot be loaded raises ``PolicyError``: the audit fails
    closed rather than silently scanning without the deny-list.
    """
    path = repo_root / POLICY_PATH
    if not path.is_file():
        return ()
    return load_policy(path).private_roots


def audit_history_commit_metadata(repo_root: Path = ROOT,
                                  private_roots: tuple[str, ...] | None = None) -> list[HistoryFinding]:
    """Pass 2: audit every reachable commit's full message, not only its subject.

    A squash merge writes the pull-request body into the message, and a body line
    naming a local path reaches public history as surely as a subject does.
    """
    findings: list[HistoryFinding] = []
    roots = policy_private_roots(repo_root) if private_roots is None else private_roots
    roots_lower = [root.lower() for root in roots if root]
    log_output = _git(["log", "--all", "-z", "--format=%H%x1f%B"], repo_root=repo_root)

    for record in log_output.split("\0"):
        commit_sha, separator, message = record.strip("\n").partition("\x1f")
        if not separator:
            continue
        commit_id = commit_sha[:12]
        message_lower = message.lower()

        # The same local-path, repository and vocabulary signatures as the blob pass: a
        # squash merge copies a pull-request body into history as surely as a file.
        if _has_local_path(message, message_lower):
            findings.append(HistoryFinding(
                category="PRIVACY_METADATA",
                code="COMMIT_LOG_LOCAL_PATH",
                commit=commit_id,
                path="<commit_message>",
                detail="Commit message contains local path or directory fragment",
            ))

        if _has_private_repo_url(message):
            findings.append(HistoryFinding(
                category="PRIVACY_METADATA",
                code="COMMIT_LOG_PRIVATE_REPO",
                commit=commit_id,
                path="<commit_message>",
                detail="Commit message names a private repository [REDACTED]",
            ))

        if _has_private_operational_vocabulary(message, message_lower):
            findings.append(HistoryFinding(
                category="PROPRIETARY_ARTIFACT",
                code="COMMIT_LOG_PRIVATE_VOCABULARY",
                commit=commit_id,
                path="<commit_message>",
                detail="Commit message contains private/game-derived operational vocabulary [REDACTED]",
            ))

        if any(root in message_lower for root in roots_lower):
            findings.append(HistoryFinding(
                category="PRIVACY_METADATA",
                code="COMMIT_LOG_PRIVATE_ROOT",
                commit=commit_id,
                path="<commit_message>",
                detail="Commit message names a configured private root [REDACTED]",
            ))

        if _has_secret(message):
            findings.append(HistoryFinding(
                category="DEFINITE_SECRET",
                code="COMMIT_LOG_SECRET",
                commit=commit_id,
                path="<commit_message>",
                detail="Commit message contains private key or API token literal [REDACTED]",
            ))

    return findings


def audit_large_blobs(repo_root: Path = ROOT, size_threshold: int = 500 * 1024,
                      raw_objects: list[str] | None = None,
                      object_info: dict[str, tuple[str, int]] | None = None) -> list[dict]:
    """Pass 3: Inventory large objects in history packfiles.

    ``object_info`` is the report's one ``cat-file --batch-check`` answer for the same
    listing; without it this pass asks git itself.
    """
    large_blobs: list[dict] = []
    if raw_objects is None:
        raw_objects = _object_listing(repo_root)

    # Map sha to path
    sha_to_path: dict[str, str] = {}
    for line in raw_objects:
        parts = line.strip().split(None, 1)
        if len(parts) == 2:
            sha_to_path[parts[0]] = parts[1]

    if object_info is None:
        # The same checked batch-check as the other passes: a git failure or an object it
        # cannot read raises instead of reporting no large blobs.
        object_info = _object_info(repo_root, list(sha_to_path))

    large_blobs = [
        {"sha": sha, "size": object_info[sha][1], "path": path}
        for sha, path in sha_to_path.items()
        if object_info[sha][0] == "blob" and object_info[sha][1] >= size_threshold
    ]
    return sorted(large_blobs, key=lambda b: b["size"], reverse=True)


REVIEWED_FINDINGS_PATH = ROOT / "assets" / "history_audit_reviewed.json"


def load_reviewed_findings(path: Path = REVIEWED_FINDINGS_PATH) -> list[dict]:
    """Maintainer-reviewed historical findings: an exact (blob or commit, code, path) triple each.

    A blob id names immutable content and a commit id names an immutable message, so
    an entry can never excuse different bytes; any new blob or commit with the same
    problem is still a finding. Each entry names exactly one of ``blob`` and
    ``commit`` and must say why it was accepted. A malformed file fails closed.
    """
    if not path.exists():
        return []
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError(f"{path}: expected schema_version 1")
    entries = data.get("reviewed")
    if not isinstance(entries, list):
        raise ValueError(f"{path}: 'reviewed' must be a list")
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ValueError(f"{path}: reviewed[{index}] must be an object")
        kinds = [key for key in ("blob", "commit") if key in entry]
        if len(kinds) != 1:
            raise ValueError(f"{path}: reviewed[{index}] must name exactly one of 'blob' or 'commit'")
        object_id = entry[kinds[0]]
        if not isinstance(object_id, str) or not re.fullmatch(r"[0-9a-f]{40}", object_id):
            raise ValueError(f"{path}: reviewed[{index}].{kinds[0]} must be a full 40-hex {kinds[0]} id")
        for key in ("code", "path", "reason"):
            if not isinstance(entry.get(key), str) or not entry[key].strip():
                raise ValueError(f"{path}: reviewed[{index}].{key} must be a non-empty string")
    return entries


def _is_reviewed(finding: HistoryFinding, reviewed: list[dict]) -> dict | None:
    for entry in reviewed:
        if finding.code != entry["code"] or finding.path != entry["path"]:
            continue
        if "blob" in entry and finding.commit == "blob:" + entry["blob"][:12]:
            return entry
        if "commit" in entry and finding.commit == entry["commit"][:12]:
            return entry
    return None


def generate_full_history_audit_report(repo_root: Path = ROOT,
                                       reviewed_path: Path = REVIEWED_FINDINGS_PATH) -> dict:
    # One object listing serves the baseline, blob and size passes instead of three
    # separate rev-list runs, and one batch-check answers their type and size
    # questions. The path pass reads every commit's tree entries instead, because
    # the object listing names each object under one path only.
    raw_objects = _object_listing(repo_root)
    object_info = _object_info(repo_root, list(_named_objects(raw_objects)))
    baseline = get_repository_baseline(repo_root, raw_objects)
    tree_findings = audit_history_tree_paths(repo_root)
    metadata_findings = audit_history_commit_metadata(repo_root)
    blob_findings = audit_history_blob_contents(repo_root, raw_objects, object_info=object_info)
    large_blobs = audit_large_blobs(repo_root, raw_objects=raw_objects, object_info=object_info)
    reviewed = load_reviewed_findings(reviewed_path)

    all_findings: list[HistoryFinding] = []
    reviewed_findings: list[dict] = []
    for finding in tree_findings + metadata_findings + blob_findings:
        entry = _is_reviewed(finding, reviewed)
        if entry is None:
            all_findings.append(finding)
        else:
            reviewed_findings.append({**finding.to_dict(redact=True), "reason": entry["reason"]})

    category_counts: dict[str, int] = {}
    for f in all_findings:
        category_counts[f.category] = category_counts.get(f.category, 0) + 1

    return {
        "status": "FAIL" if all_findings else "OK",
        "baseline": baseline,
        "summary": {
            "total_findings": len(all_findings),
            "category_counts": category_counts,
            "large_blobs_over_500kb": len(large_blobs),
            "reviewed_findings": len(reviewed_findings),
        },
        "reviewed_findings": reviewed_findings,
        "large_blobs": large_blobs[:10],  # Top 10 largest blobs
        "findings": [f.to_dict(redact=True) for f in all_findings],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print JSON report to stdout")
    parser.add_argument("--out", type=Path, help="Write publication-safe audit JSON report to path")
    args = parser.parse_args(argv)

    report = generate_full_history_audit_report(ROOT)

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote publication-safe full-history audit report to {args.out}")

    if args.json:
        print(json.dumps(report, indent=2))
        return 1 if report["summary"]["total_findings"] > 0 else 0

    print("=======================================================")
    print(" Nakagawa Recomp Full-History Non-Destructive Audit")
    print("=======================================================")
    print(f"Reachable Commits: {report['baseline']['total_commits']}")
    print(f"Reachable Objects: {report['baseline']['total_objects']}")
    print(f"Reachable Refs:    {report['baseline']['total_refs']}")
    print(f"Status:            {report['status']}")
    print(f"Total Findings:    {report['summary']['total_findings']}")
    print(f"Reviewed (accepted historical blobs): {report['summary']['reviewed_findings']}")
    print("Category Breakdown:")
    for cat, count in report['summary']['category_counts'].items():
        print(f"  - {cat}: {count}")

    if report["findings"]:
        print("\nFindings Summary:")
        for f in report["findings"]:
            print(f"  [{f['category']}] {f['code']}: {f['path']} ({f['commit']}) - {f['detail']}")
        return 1

    print("\nFull-history audit: OK (0 sensitive findings across all reachable commits & objects)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
