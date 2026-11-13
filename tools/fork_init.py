#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Make a fork maintainer the provenance authority of their own fork.

The upstream keeps its trusted provenance records in a private repository that only the upstream
maintainer holds. A fork starts without one, so it could never admit a new file. This tool seeds a
fork's own authority from the public ledger the fork already has, after which the fork maintainer
admits new paths with the same ``tools/admit_paths.py`` flow the upstream uses
(``docs/FORKING.md``, "Become your fork's provenance authority")::

    python tools/fork_init.py init --authority ../my-fork-authority
    python tools/fork_init.py verify --authority ../my-fork-authority
    python tools/fork_init.py record --authority ../my-fork-authority --id my-tool-2026-11-08 \\
        --path tools/my_tool.py --classification project-authored-independent --evidence-tier S \\
        --statement "I wrote tools/my_tool.py for this fork."

``init`` creates a Git repository at ``--authority`` (outside the fork's checkout, never inside it)
with ``docs/provenance/IMPLEMENTATION_PROVENANCE.json``. Every record in it is rebuilt from a public
ledger entry that names a record id: its id, its exact paths, the classification and evidence tier
the entry publishes, and the licence of an upstream-derived file. Each record says that it adopts the
claim the upstream published at that commit; it is not a new attestation. It makes one choice of its
own: every project-authored entry gets the detailed class ``project-authored-independent``, because the
public ledger does not say which of the two detailed classes behind its project-authored class
(``project-authored-independent`` or ``behavior-informed``) the upstream recorded. Documentation,
configuration, fixtures and other deterministic paths need no
record. ``init`` then checks that the trusted verifier, given only the new authority, regenerates the
committed public ledger byte for byte, and commits the seed with your Git identity.

``verify`` repeats that check at any revision. ``record`` adds one exact record for a new
implementation path, with the origin statement you give it verbatim, and commits it: that is your
attestation as the fork's authority, and the tool never writes one on its own. ``tools/admit_paths.py
--private-repo <authority>`` then admits the path.

The upstream repository never needs this: its authority is the upstream maintainer's private ledger,
and its attestation job never reads a fork's.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import provenance_attest_verify  # noqa: E402
import provenance_ledger  # noqa: E402

AUTHORITY_LEDGER = "docs/provenance/IMPLEMENTATION_PROVENANCE.json"
PUBLIC_LEDGER = "assets/public_provenance_ledger.json"
AUTHORITY_README = "README.md"

#: The detailed classification a seeded record carries for each public class. The verifier maps
#: each back to exactly the public class and evidence the entry already has
#: (``provenance_ledger._class_for``).
SEED_CLASSIFICATION = {
    "project_authored_attested": "project-authored-independent",
    "upstream_derived": "derived-translated",
    "generated_from_public_source": "derived-data",
}
#: Classifications ``record`` accepts: the detailed classes the classifier maps to an admittable
#: public class.
RECORD_CLASSIFICATIONS = (
    "project-authored-independent",
    "behavior-informed",
    "derived-translated",
    "upstream-third-party",
    "derived-data",
    "generated-project-owned",
)
RECORD_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")


class ForkInitError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _git(repo: Path, *args: str, check: bool = True) -> str:
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", check=False)
    if check and result.returncode:
        raise ForkInitError("GIT_ERROR", (result.stderr or result.stdout).strip() or f"git {' '.join(args)} failed")
    return result.stdout


def _within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _worktrees(repo: Path) -> list[Path]:
    """Every working tree of the fork's repository, this one included."""
    listing = _git(repo, "worktree", "list", "--porcelain")
    return [Path(line[len("worktree "):]) for line in listing.splitlines() if line.startswith("worktree ")] or [repo]


#: The first line of the README `init` writes; `record` refuses an authority without it.
AUTHORITY_MARKER = "# Fork provenance authority"


def _require_fork_authority(authority: Path) -> None:
    """Refuse an authority `init` did not seed, such as the upstream's own, and one not on ``main``."""
    readme = authority / AUTHORITY_README
    first = readme.read_text(encoding="utf-8").splitlines()[:1] if readme.is_file() else []
    if first != [AUTHORITY_MARKER]:
        raise ForkInitError(
            "AUTHORITY_NOT_A_FORK_AUTHORITY",
            f"{authority} was not seeded by `fork_init.py init` ({AUTHORITY_README} does not start with "
            f"{AUTHORITY_MARKER!r}); this tool writes only to a fork's own authority",
        )
    if _git(authority, "rev-parse", "--abbrev-ref", "HEAD").strip() != "main":
        raise ForkInitError("AUTHORITY_NOT_MAIN", "the authority checkout must have main checked out")


def _canonical(document: dict) -> bytes:
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def _repo_root(repo: Path) -> Path:
    return Path(_git(repo, "rev-parse", "--show-toplevel").strip())


def _commit_sha(repo: Path, rev: str) -> str:
    return _git(repo, "rev-parse", "--verify", f"{rev}^{{commit}}").strip()


def seed_records(public_ledger: dict, commit: str) -> list[dict]:
    """One trusted record per record id the public ledger names, adopting its published claim."""
    entries = public_ledger.get("entries")
    if not isinstance(entries, list):
        raise ForkInitError("PUBLIC_LEDGER_INVALID", f"{PUBLIC_LEDGER} has no entries list")
    records: dict[str, dict] = {}
    for entry in entries:
        evidence = entry.get("evidence") if isinstance(entry, dict) else None
        if not isinstance(evidence, dict) or not evidence.get("record_id"):
            continue
        public_class = entry.get("classification")
        detailed = SEED_CLASSIFICATION.get(public_class)
        if detailed is None:
            raise ForkInitError("PUBLIC_LEDGER_INVALID",
                                f"{entry.get('path')}: record-backed class {public_class!r} has no seed mapping")
        record_id = evidence["record_id"]
        claim = {
            "classification": detailed,
            "evidence_tier": evidence.get("evidence_tier"),
        }
        if public_class == "upstream_derived" and evidence.get("license") not in (None, "see NOTICE.md"):
            claim["upstream_license"] = evidence["license"]
        record = records.get(record_id)
        if record is None:
            record = records[record_id] = {
                "id": record_id,
                "paths": [],
                **claim,
                "seeded_from": {
                    "source": PUBLIC_LEDGER,
                    "commit": commit,
                    "statement": (
                        "Adopts the claim the upstream published for these paths in its public "
                        "provenance ledger at this commit. Not a new attestation."
                    ),
                },
            }
        elif any(record.get(key) != claim.get(key) for key in ("classification", "evidence_tier", "upstream_license")):
            raise ForkInitError("PUBLIC_LEDGER_INVALID",
                                f"record {record_id!r} carries different claims on different paths")
        record["paths"].append(entry["path"])
    for record in records.values():
        record["paths"].sort()
    return [records[key] for key in sorted(records)]


def regenerated_public_ledger(repo: Path, rev: str, authority_ledger: Path) -> bytes:
    """The committed public ledger the trusted verifier derives for ``rev`` from ``authority_ledger`` alone."""
    with tempfile.TemporaryDirectory(prefix="nakagawa-fork-init-") as scratch:
        try:
            envelope = provenance_attest_verify.generate_authority_baseline(
                repo=repo, base_rev=rev, trusted_ledger=authority_ledger, workdir=Path(scratch),
            )
        except provenance_attest_verify.VerifyError as error:
            raise ForkInitError(error.code, str(error)) from error
    ledger = json.loads(envelope)["ledger"]
    return bytes(provenance_ledger.claims_ledger_bytes(ledger))


def verify(repo: Path, authority: Path, rev: str = "HEAD") -> str:
    """Check that the authority reproduces the committed public ledger at ``rev``."""
    repo = _repo_root(repo)
    sha = _commit_sha(repo, rev)
    ledger = authority / AUTHORITY_LEDGER
    if not ledger.is_file():
        raise ForkInitError("AUTHORITY_MISSING", f"{ledger} does not exist; run `fork_init.py init` first")
    committed = subprocess.run(["git", "-C", str(repo), "cat-file", "blob", f"{sha}:{PUBLIC_LEDGER}"],
                               capture_output=True, check=False).stdout
    regenerated = regenerated_public_ledger(repo, sha, ledger)
    if regenerated != committed:
        raise ForkInitError(
            "AUTHORITY_MISMATCH",
            f"the authority does not reproduce {PUBLIC_LEDGER} at {sha}: a published path has no qualifying "
            "record, or a record's claim differs from the published one",
        )
    return sha


def _readme(commit: str) -> str:
    return (
        f"{AUTHORITY_MARKER}\n\n"
        "This repository is the provenance authority of a Nakagawa Recomp fork. Keep it private or\n"
        "public as you choose, but never inside the fork's own checkout.\n\n"
        f"`tools/fork_init.py` seeded `{AUTHORITY_LEDGER}` from the fork's public\n"
        f"ledger at commit `{commit}`. Each seeded record adopts the claim published there.\n"
        "New records are your own attestations; add them with `tools/fork_init.py record`, then\n"
        "admit the paths with `tools/admit_paths.py --private-repo <this folder>`.\n\n"
        "See `docs/FORKING.md` in the fork.\n"
    )


def init(repo: Path, authority: Path, rev: str = "HEAD", *, assisted_by: str | None = None) -> dict:
    repo = _repo_root(repo)
    authority = authority.absolute()
    if any(_within(authority, tree) for tree in _worktrees(repo)):
        raise ForkInitError("AUTHORITY_INSIDE_REPOSITORY",
                            "the authority must live outside every checkout of the fork, or it would be published")
    if authority.exists() and not authority.is_dir():
        raise ForkInitError("AUTHORITY_NOT_A_DIRECTORY", f"{authority} exists and is not a directory")
    if authority.exists() and any(authority.iterdir()):
        raise ForkInitError("AUTHORITY_NOT_EMPTY", f"{authority} exists and is not empty")
    sha = _commit_sha(repo, rev)
    raw = subprocess.run(["git", "-C", str(repo), "cat-file", "blob", f"{sha}:{PUBLIC_LEDGER}"],
                         capture_output=True, check=False)
    if raw.returncode:
        raise ForkInitError("PUBLIC_LEDGER_INVALID", f"{PUBLIC_LEDGER} is missing at {sha}")
    records = seed_records(json.loads(raw.stdout), sha)

    # The authority is built in a staging folder beside it and moved into place only once it
    # verifies, so a failed run leaves the target as it found it and the next init can start.
    authority.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".fork-authority-", dir=authority.parent,
                                     ignore_cleanup_errors=True) as scratch:
        staging = Path(scratch)
        _git(staging, "init", "--quiet", "--initial-branch=main")
        ledger_path = staging / AUTHORITY_LEDGER
        ledger_path.parent.mkdir(parents=True, exist_ok=True)
        ledger_path.write_bytes(_canonical({"schema_version": 1, "records": records}))
        (staging / AUTHORITY_README).write_text(_readme(sha), encoding="utf-8", newline="\n")
        verify(repo, staging, sha)
        authority.mkdir(exist_ok=True)
        for child in staging.iterdir():
            child.rename(authority / child.name)

    _git(authority, "add", "--", AUTHORITY_LEDGER, AUTHORITY_README)
    message = f"Seed the fork provenance authority from the public ledger at {sha}"
    if assisted_by:
        message += f"\n\nAssisted-by: {assisted_by}"
    commit = subprocess.run(["git", "-C", str(authority), "commit", "--quiet", "-m", message],
                            capture_output=True, text=True, check=False)
    if commit.returncode:
        raise ForkInitError(
            "AUTHORITY_COMMIT_FAILED",
            (commit.stderr or commit.stdout).strip()
            + "\nThe seed is written but not committed. Set your Git identity and commit it yourself.",
        )
    return {"authority": str(authority), "commit": sha, "records": len(records),
            "paths": sum(len(record["paths"]) for record in records)}


def add_record(
    authority: Path,
    *,
    record_id: str,
    paths: list[str],
    classification: str,
    evidence_tier: str,
    statement: str,
    upstream: str | None = None,
    upstream_revision: str | None = None,
    upstream_license: str | None = None,
    assisted_by: str | None = None,
) -> dict:
    """Append one exact record (the fork maintainer's own attestation) and commit it."""
    if not RECORD_ID.match(record_id):
        raise ForkInitError("RECORD_INVALID", "record id must be letters, digits, '.', '_' or '-'")
    if classification not in RECORD_CLASSIFICATIONS:
        raise ForkInitError("RECORD_INVALID", f"classification must be one of {', '.join(RECORD_CLASSIFICATIONS)}")
    if not statement.strip():
        raise ForkInitError("RECORD_INVALID", "the origin statement is yours to give; it cannot be empty")
    for path in paths:
        if not path or any(character in path for character in "*?\\") or path.startswith("/") or ".." in path.split("/"):
            raise ForkInitError("RECORD_INVALID", f"{path!r} is not an exact repository path")
    if classification in ("derived-translated", "upstream-third-party") and not (
        upstream and upstream_revision and upstream_license
    ):
        raise ForkInitError("RECORD_INVALID",
                            "an upstream-derived record needs --upstream, --upstream-revision and --upstream-license")
    ledger_path = authority / AUTHORITY_LEDGER
    if not ledger_path.is_file():
        raise ForkInitError("AUTHORITY_MISSING", f"{ledger_path} does not exist; run `fork_init.py init` first")
    _require_fork_authority(authority)
    if _git(authority, "status", "--porcelain", "--", "docs/provenance").strip():
        raise ForkInitError("AUTHORITY_DIRTY", "docs/provenance in the authority has uncommitted changes")
    document = json.loads(ledger_path.read_text(encoding="utf-8"))
    records = document.get("records")
    if not isinstance(records, list):
        raise ForkInitError("AUTHORITY_INVALID", f"{AUTHORITY_LEDGER} has no records list")
    if any(record.get("id") == record_id for record in records):
        raise ForkInitError("RECORD_EXISTS", f"record {record_id!r} already exists")
    taken = {path for record in records for path in record.get("paths", [])}
    clash = sorted(set(paths) & taken)
    if clash:
        raise ForkInitError("RECORD_EXISTS", f"{', '.join(clash)} already has an exact record")
    record: dict = {"id": record_id, "paths": sorted(set(paths)), "classification": classification,
                    "evidence_tier": evidence_tier, "statement": statement}
    for key, value in (("upstream", upstream), ("upstream_revision", upstream_revision),
                       ("upstream_license", upstream_license)):
        if value:
            record[key] = value
    records.append(record)
    ledger_path.write_bytes(_canonical(document))
    _git(authority, "add", "--", AUTHORITY_LEDGER)
    message = f"Record {record_id}"
    if assisted_by:
        message += f"\n\nAssisted-by: {assisted_by}"
    _git(authority, "commit", "--quiet", "-m", message)
    return record


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("init", "verify", "record"):
        command = sub.add_parser(name)
        command.add_argument("--authority", type=Path, required=True, help="the fork's authority repository")
        command.add_argument("--repo", type=Path, default=ROOT, help="the fork's checkout (default: this one)")
        if name != "record":
            command.add_argument("--rev", default="HEAD", help="the fork revision whose public ledger is used")
        if name != "verify":
            command.add_argument("--assisted-by", help="append an Assisted-by: trailer to the commit")
        if name == "record":
            command.add_argument("--id", required=True, dest="record_id")
            command.add_argument("--path", action="append", required=True, dest="paths")
            command.add_argument("--classification", required=True, choices=RECORD_CLASSIFICATIONS)
            command.add_argument("--evidence-tier", required=True,
                                 help="how strong the evidence is, in your own scheme (the upstream uses S, R, ...)")
            command.add_argument("--statement", required=True, help="your origin statement, recorded verbatim")
            command.add_argument("--upstream")
            command.add_argument("--upstream-revision")
            command.add_argument("--upstream-license")
    args = parser.parse_args(argv)
    try:
        if args.command == "init":
            result = init(args.repo, args.authority, args.rev, assisted_by=args.assisted_by)
            print(f"fork authority seeded at {result['authority']}: {result['records']} record(s) covering "
                  f"{result['paths']} path(s) from the public ledger at {result['commit']}")
            print("the trusted verifier regenerates the committed public ledger from it byte for byte")
        elif args.command == "verify":
            sha = verify(args.repo, args.authority.absolute(), args.rev)
            print(f"fork authority reproduces {PUBLIC_LEDGER} at {sha}")
        else:
            record = add_record(
                args.authority.absolute(), record_id=args.record_id, paths=args.paths,
                classification=args.classification, evidence_tier=args.evidence_tier, statement=args.statement,
                upstream=args.upstream, upstream_revision=args.upstream_revision,
                upstream_license=args.upstream_license, assisted_by=args.assisted_by,
            )
            print(f"recorded {record['id']} for {', '.join(record['paths'])}")
    except ForkInitError as error:
        print(f"fork_init: {error.code}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
