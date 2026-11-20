#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regenerate every committed generated file, or check that none is stale.

Several committed files are produced by a generator and pinned by a test or a
gate that fails when the committed copy drifts from a fresh generation. Each of
those checks used to name its own generator, so a pull request that touched a
shared input failed one check per CI round trip. This tool runs every public
generator in a fixed order and reports which files changed:

    python tools/regen_all.py            # rewrite the stale files
    python tools/regen_all.py --check    # exit 1 if any file is stale; write nothing
    python tools/regen_all.py --list     # print the steps, their outputs and skips
    python tools/regen_all.py --resolve-merge  # finish a merge whose only conflicts are generated files

``--resolve-merge`` is for a stopped ``git merge``, ``git merge --squash`` or
``git cherry-pick`` (``docs/CI.md``, "Merging regenerated files"). It refuses,
touching nothing, while any conflicted path is not a generated output. Otherwise
it takes our side of each conflicted output (a hand-written file that holds a
generated block merges its own text three ways first), regenerates every output
from the merged tree, stages the results and checks them again; it exits 1 if
anything is still stale. A merge with a ``MERGE_HEAD`` leaves the provenance
controls for their own refresh commit, because their base exists only once the
merge is committed.

Order matters only at the end: the provenance controls hash the staged tree, so
the refresh runs last, after the regenerated files are staged. The refresh is
``tools/contrib_refresh.py``; it reads the staged index, so stage your own
changes first (``git add``). It refuses a new or removed public path and a
changed publication profile, because those need maintainer authority
(``docs/PUBLICATION_READINESS.md``); this tool then reports the refusal as
``SKIP`` with the route to follow and does not fail on it.

A generator whose inputs are private (retail images, the trusted detailed
ledger, a telemetry database) is listed as skipped and never run. Exit codes:
0 nothing stale (or everything rewritten), 1 stale under ``--check`` or a step
failed, 2 a usage error.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Callable

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"

#: The one command every staleness check prints as its fix.
FIX_COMMAND = "python tools/regen_all.py"
#: The command that finishes a merge whose only conflicts are generated files.
MERGE_COMMAND = "python tools/regen_all.py --resolve-merge"

HLE_INVENTORY = "docs/HLE_AND_WORKAROUND_INVENTORY.md"
HLE_CENSUS_BEGIN = "<!-- BEGIN GENERATED HLE STATUS CENSUS -->"
HLE_CENSUS_END = "<!-- END GENERATED HLE STATUS CENSUS -->"

#: contrib_refresh refusals that mean "a maintainer route applies", not "stale".
PROVENANCE_AUTHORITY_CODES = frozenset(
    {"NEW_PUBLIC_PATH", "REMOVED_PUBLIC_PATH", "CHANGED_PATH_NOT_PUBLIC", "UNRESOLVED_ENTRY", "POLICY_CHANGED"}
)


class StepError(RuntimeError):
    """A generator failed; the message is what it printed."""


@dataclasses.dataclass(frozen=True)
class Step:
    name: str
    command: str
    outputs: tuple[str, ...]
    #: Returns {repo-relative path: expected bytes}, generated without touching the tree.
    render: Callable[[Path], dict[str, bytes]] | None = None
    #: Set for a generator that is listed but never run here.
    skip: str = ""


def _run(argv: list[str]) -> None:
    result = subprocess.run(
        [sys.executable, *argv], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise StepError(f"{' '.join(argv)} exited {result.returncode}: {detail}")


def _text_bytes(path: Path) -> bytes:
    # Generators open their outputs in text mode; normalize to the LF bytes the tree stores.
    return path.read_text(encoding="utf-8").replace("\r\n", "\n").encode("utf-8")


def _single_out(tool: str, output: str, *extra: str) -> Callable[[Path], dict[str, bytes]]:
    def render(tmp: Path) -> dict[str, bytes]:
        out = tmp / Path(output).name
        _run([f"tools/{tool}", "--out", str(out), *extra])
        return {output: _text_bytes(out)}

    return render


def _refuse_baseline_downgrades(fresh: Path) -> None:
    """Never let a refresh absorb a coverage downgrade the import-audit gate exists to catch."""
    sys.path.insert(0, str(TOOLS))
    from import_audit_gate import DOWNGRADE_FROM, DOWNGRADE_TO

    committed_path = ROOT / "tools" / "import_audit_baseline.json"
    if not committed_path.is_file():
        return
    committed = json.loads(committed_path.read_text(encoding="ascii"))
    current = json.loads(fresh.read_text(encoding="ascii"))
    downgrades = sorted(
        f"{nid} {old['name']}: {old['classification']} -> {current[nid]['classification'] if nid in current else 'missing'}"
        for nid, old in committed.items()
        if old["classification"] in DOWNGRADE_FROM
        and (current[nid]["classification"] if nid in current else "missing") in DOWNGRADE_TO
    )
    if downgrades:
        raise StepError(
            "refusing to refresh tools/import_audit_baseline.json over a coverage downgrade "
            f"({'; '.join(downgrades)}); fix the handler, or review the downgrade and run "
            "python tools/hle_manifest.py --write-baseline yourself"
        )


def _render_hle(tmp: Path) -> dict[str, bytes]:
    baseline = tmp / "import_audit_baseline.json"
    census = tmp / "hle_census.md"
    _run([
        "tools/hle_manifest.py",
        "--out", str(tmp / "hle_manifest.json"),
        "--write-baseline", str(baseline),
        "--census-markdown", str(census),
        "--census-heading-level", "3",
    ])
    _refuse_baseline_downgrades(baseline)
    doc = (ROOT / HLE_INVENTORY).read_text(encoding="utf-8")
    if doc.count(HLE_CENSUS_BEGIN) != 1 or doc.count(HLE_CENSUS_END) != 1:
        raise StepError(f"{HLE_INVENTORY} needs exactly one generated census block")
    head, rest = doc.split(HLE_CENSUS_BEGIN, 1)
    _, tail = rest.split(HLE_CENSUS_END, 1)
    block = census.read_text(encoding="utf-8").strip("\r\n")
    spliced = f"{head}{HLE_CENSUS_BEGIN}\n{block}\n{HLE_CENSUS_END}{tail}"
    return {
        "tools/import_audit_baseline.json": _text_bytes(baseline),
        HLE_INVENTORY: spliced.encode("utf-8"),
    }


def _render_verification_ledger(tmp: Path) -> dict[str, bytes]:
    doc, header = tmp / "HLE_COVERAGE.md", tmp / "hle_evidence.h"
    _run(["tools/verification_ledger.py", "--out", str(doc), "--header", str(header)])
    return {"docs/HLE_COVERAGE.md": _text_bytes(doc), "src/rt/hle_evidence.h": _text_bytes(header)}


def _render_title_catalog(tmp: Path) -> dict[str, bytes]:
    _run(["tools/title_catalog_codegen.py", "--output-dir", str(tmp)])
    return {
        f"src/core/generated/{name}": _text_bytes(tmp / name)
        for name in ("nk_title_catalog.h", "nk_title_catalog.c")
    }


def _render_pgf_vectors(tmp: Path) -> dict[str, bytes]:
    # The header's generator lives in its test module (`--regenerate`), which has no --out.
    sys.path.insert(0, str(TOOLS))
    import test_pgf_validate_parity

    return {"tests/native/pgf_golden_vectors.h": test_pgf_validate_parity.golden_header_text().encode("utf-8")}


STEPS: tuple[Step, ...] = (
    Step(
        "recomp helper inventory",
        "python tools/recomp_helper_inventory.py",
        ("docs/RECOMP_HELPER_INVENTORY.md",),
        _single_out("recomp_helper_inventory.py", "docs/RECOMP_HELPER_INVENTORY.md"),
    ),
    Step(
        "NID name table",
        "python tools/gen_nidnames.py",
        ("src/rt/nid_names.h",),
        _single_out("gen_nidnames.py", "src/rt/nid_names.h"),
    ),
    Step(
        "HLE baseline and inventory census block",
        "python tools/hle_manifest.py --write-baseline --census-markdown <tmp> --census-heading-level 3",
        ("tools/import_audit_baseline.json", HLE_INVENTORY),
        _render_hle,
    ),
    Step(
        "HLE and VFPU verification ledger",
        "python tools/verification_ledger.py",
        ("docs/HLE_COVERAGE.md", "src/rt/hle_evidence.h"),
        _render_verification_ledger,
    ),
    Step(
        "waits census",
        "python tools/waits_census.py",
        ("docs/PSP_INTR_WAITS_CENSUS.md",),
        _single_out("waits_census.py", "docs/PSP_INTR_WAITS_CENSUS.md"),
    ),
    Step(
        "public title catalog",
        "python tools/title_catalog_codegen.py",
        ("src/core/generated/nk_title_catalog.h", "src/core/generated/nk_title_catalog.c"),
        _render_title_catalog,
    ),
    Step(
        "nk_ui embedded assets",
        "python tools/gen_ui_assets.py",
        ("src/ui/nk_ui_assets.c",),
        _single_out("gen_ui_assets.py", "src/ui/nk_ui_assets.c"),
    ),
    Step(
        "PGF golden vectors",
        "python tools/test_pgf_validate_parity.py --regenerate",
        ("tests/native/pgf_golden_vectors.h",),
        _render_pgf_vectors,
    ),
    Step(
        "embedded shaders",
        "mingw32-make shaders GLSLC=<glslc>",
        ("src/rt/gpu_sdl3vk/psp_vert.inc", "src/rt/gpu_sdl3vk/psp_frag.inc", "src/rt/gpu_sdl3vk/shader_manifest.json"),
        skip="needs the glslc toolchain, whose version the manifest records; `make shader-verify` checks it",
    ),
    Step(
        "UI embedded shaders",
        "python tools/shader_embed.py regenerate --shader-set ui --glslc <glslc>",
        (
            "src/ui/shaders/ui_vert.inc",
            "src/ui/shaders/ui_frag.inc",
            "src/ui/shaders/waves_frag.inc",
            "src/ui/shaders/shader_manifest.json",
        ),
        skip="needs the glslc toolchain, whose version the manifest records; "
        "`python tools/shader_embed.py verify --shader-set ui` checks it",
    ),
    Step(
        "profile-zero prebuilt digests",
        "see tests/fixtures/profile_zero/README.md",
        ("tests/fixtures/profile_zero/prebuilt/SHA256SUMS",),
        skip="needs the pinned PSPDEV toolchain; CI checks it with sha256sum --check",
    ),
    Step(
        "import-stub census",
        "python tools/import_stub_census.py --corpus <staged titles> --markdown <out>",
        (),
        skip="needs private inputs: decrypted title images; its output is never committed",
    ),
    Step(
        "benchmark report",
        "python tools/generate_benchmarks.py --db dev.db --output <out>",
        (),
        skip="needs private inputs: the local telemetry database; its output is never committed",
    ),
)

PROVENANCE = Step(
    "provenance controls",
    "python tools/contrib_refresh.py",
    ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"),
)

#: Outputs that a step writes inside a hand-written file, between these two markers. A merge resolves
#: the text around the block three ways and only the block from one side.
SPLICED: dict[str, tuple[str, str]] = {HLE_INVENTORY: (HLE_CENSUS_BEGIN, HLE_CENSUS_END)}


def _current(path: str) -> bytes | None:
    target = ROOT / path
    return target.read_bytes() if target.is_file() else None


def _git(*args: str) -> subprocess.CompletedProcess:
    sys.path.insert(0, str(TOOLS))
    from nk_core.git_isolation import isolated_git_env

    return subprocess.run(
        ["git", *args], cwd=ROOT, env=isolated_git_env(root=ROOT), capture_output=True, text=True
    )


def _without_admission(ledger: bytes | None) -> dict | None:
    if ledger is None:
        return None
    try:
        document = json.loads(ledger)
    except ValueError:
        return None
    document.pop("admission", None)
    returned: dict[str, Any] | None = document
    return returned


def _without_labels(document: dict | None) -> dict | None:
    """``document`` with each record-backed entry's public label text blanked.

    The label is derived from the entry's trusted record, which this public-only check cannot
    read, and the label set changes over time (``provenance_ledger.PUBLIC_LABEL_SETS``). Two
    ledgers equal here differ only in that text; the trusted attestation derives it from the
    records and refuses any label they do not back.
    """
    if document is None:
        return None
    sys.path.insert(0, str(TOOLS))
    import provenance_ledger

    known = provenance_ledger._known_public_labels()
    result = json.loads(json.dumps(document))
    for entry in result.get("entries", []) if isinstance(result.get("entries"), list) else []:
        if not isinstance(entry, dict):
            continue
        field = provenance_ledger.PUBLIC_LABEL_FIELDS.get(entry.get("classification"))
        evidence = entry.get("evidence")
        if field and isinstance(evidence, dict) and evidence.get(field) in known:
            evidence[field] = None
    returned: dict[str, Any] = result
    return returned


def _provenance(check: bool, base: str | None) -> tuple[str, list[str], str]:
    """Return (status, stale or changed paths, detail) for the provenance controls."""
    sys.path.insert(0, str(TOOLS))
    import contrib_refresh
    import provenance_attest_verify as verifier

    try:
        ledger_bytes, export_bytes, _ = contrib_refresh._generate(ROOT, base)
    except contrib_refresh.RefreshRefused as error:
        where = f" ({error.path})" if error.path else ""
        if error.code in PROVENANCE_AUTHORITY_CODES:
            return (
                "SKIP",
                [],
                f"{error.code}{where}: a new, removed or reclassified public path needs maintainer "
                "authority; follow docs/PUBLICATION_READINESS.md "
                "(step by step: a pull request that adds a public path)",
            )
        return "FAIL", [], f"{error.code}{where}: {error.detail}"
    except verifier.VerifyError as error:
        return "FAIL", [], f"{error.code}: {error}"

    stale = [
        path
        for path, expected in ((contrib_refresh.LEDGER, ledger_bytes), (contrib_refresh.EXPORT, export_bytes))
        if _current(path) != expected
    ]
    note = ""
    current = _without_admission(_current(contrib_refresh.LEDGER))
    expected = _without_admission(ledger_bytes)
    if stale == [contrib_refresh.LEDGER] and current == expected:
        # Right after an admission merge the base ledger carries an `admission` audit block. The
        # trusted refresh drops it and contrib_refresh keeps it; the per-path entries agree.
        stale, note = [], "differs only by the base ledger's admission block, which the trusted refresh drops"
    elif stale == [contrib_refresh.LEDGER] and current is not None and _without_labels(current) == _without_labels(
        expected
    ):
        # The trusted refresh writes the emitted public label set, and contrib_refresh keeps the
        # base's; the claims agree, and the hosted attestation checks each label against its record.
        stale, note = [], "differs only in public label text, which the trusted attestation derives from the records"
    if check or not stale:
        return ("STALE" if stale else "OK"), stale, note
    # Write the bytes generated above, as contrib_refresh.py writes and stages them,
    # rather than running that tool to generate the same controls a second time.
    controls = ((contrib_refresh.LEDGER, ledger_bytes), (contrib_refresh.EXPORT, export_bytes))
    try:
        for path, data in controls:
            target = ROOT / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    except OSError as error:
        return "FAIL", [], f"writing the provenance controls: {error}"
    staged = _git("add", "--", *(path for path, _ in controls))
    if staged.returncode:
        return "FAIL", [], f"staging the provenance controls: {staged.stderr.strip()}"
    return "WROTE", stale, ""


def run(check: bool, base: str | None, *, provenance: bool = True, require_provenance: bool = False) -> int:
    """Regenerate (or with ``check``, compare) every output. ``provenance=False`` leaves the controls alone;
    ``require_provenance`` fails when contrib_refresh refuses to refresh them (a ``SKIP``)."""
    failed = False
    stale_paths: list[str] = []
    written: list[str] = []
    # Every step renders before any output is written, so a failing generator
    # leaves the tree untouched instead of half regenerated and unstaged.
    pending: list[tuple[Step, dict[str, bytes], list[str]]] = []
    with tempfile.TemporaryDirectory(prefix="regen-all-") as scratch:
        for step in STEPS:
            if step.skip:
                print(f"SKIP  {step.name}: {step.skip}")
                continue
            tmp = Path(scratch) / step.name.replace(" ", "_")
            tmp.mkdir()
            try:
                if step.render is None:
                    raise StepError(f"{step.name} has no renderer")
                expected = step.render(tmp)
            except (StepError, OSError, UnicodeError) as error:
                print(f"FAIL  {step.name}: {error}")
                failed = True
                continue
            except Exception as error:  # any other generator bug is a named FAIL, not a traceback
                print(f"FAIL  {step.name}: {type(error).__name__}: {error}")
                failed = True
                continue
            changed = [path for path, data in expected.items() if _current(path) != data]
            if not changed:
                print(f"OK    {step.name}")
                continue
            if check:
                print(f"STALE {step.name}: {', '.join(changed)}")
                stale_paths += changed
                continue
            pending.append((step, expected, changed))

    if failed:
        print("regen_all: a generator failed; nothing was written; fix it before refreshing the provenance controls")
        return 1

    for step, expected, changed in pending:
        for path in changed:
            (ROOT / path).write_bytes(expected[path])
        print(f"WROTE {step.name}: {', '.join(changed)}")
        written += changed

    if written:
        # The provenance refresh hashes the index, so it must see the regenerated bytes.
        staged = _git("add", "--", *written)
        if staged.returncode:
            print(f"FAIL  staging the regenerated files: {staged.stderr.strip()}")
            return 1

    if provenance:
        status, paths, detail = _provenance(check, base)
    else:
        status, paths, detail = (
            "NOT_RUN",
            [],
            "a merge has no provenance base until it is committed; refresh the controls in their own commit after it",
        )
    label = f"{status:<5} {PROVENANCE.name}"
    print(f"{label}: {', '.join(paths) or detail}" if (paths or detail) else label)
    if require_provenance and status == "SKIP":
        print("regen_all: the provenance controls conflicted and could not be refreshed here; a maintainer route applies")
        return 1
    if status == "STALE":
        stale_paths += paths
    elif status == "WROTE":
        written += paths

    if check:
        if stale_paths:
            print(f"regen_all: {len(stale_paths)} generated file(s) are stale. Fix with: {FIX_COMMAND}")
            return 1
        if status == "FAIL":
            return 1
        unchecked = " (the provenance controls were not checked)" if status == "NOT_RUN" else ""
        print(f"regen_all: every generated file is current{unchecked}")
        return 0
    if status == "FAIL":
        return 1
    if written:
        print(f"regen_all: rewrote and staged {len(written)} file(s): {', '.join(written)}")
    else:
        print("regen_all: every generated file was already current")
    return 0


def _git_bytes(*args: str) -> subprocess.CompletedProcess[bytes]:
    sys.path.insert(0, str(TOOLS))
    from nk_core.git_isolation import isolated_git_env

    return subprocess.run(["git", *args], cwd=ROOT, env=isolated_git_env(root=ROOT), capture_output=True)


def _blob(sha: str) -> bytes:
    result = _git_bytes("cat-file", "blob", sha)
    if result.returncode:
        raise StepError(f"git cat-file blob {sha} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def _resolvable_paths() -> set[str]:
    """Outputs this tool regenerates. A skipped step's outputs need a person (or its own toolchain)."""
    return {path for step in (*STEPS, PROVENANCE) if not step.skip for path in step.outputs}


def _unmerged() -> dict[str, dict[int, str]]:
    """{path: {stage: blob}} for every conflicted index entry (stage 1 base, 2 ours, 3 theirs)."""
    result = _git_bytes("ls-files", "-u", "-z")
    if result.returncode:
        raise StepError(f"git ls-files -u failed: {result.stderr.decode(errors='replace').strip()}")
    entries: dict[str, dict[int, str]] = {}
    for record in result.stdout.split(b"\0"):
        if not record:
            continue
        meta, path = record.split(b"\t", 1)
        _, sha, stage = meta.decode("ascii").split()
        entries.setdefault(path.decode("utf-8"), {})[int(stage)] = sha
    return entries


def _split_block(data: bytes, begin: bytes, end: bytes) -> tuple[bytes, bytes, bytes] | None:
    if data.count(begin) != 1 or data.count(end) != 1:
        return None
    head, rest = data.split(begin, 1)
    if end not in rest:
        return None
    block, tail = rest.split(end, 1)
    return head, block, tail


def _merge_spliced(base: bytes, ours: bytes, theirs: bytes, begin: bytes, end: bytes) -> bytes | None:
    """Merge the hand-written text around a generated block three ways; keep our block for regeneration.

    Each side's block is emptied first, so only the text a person wrote can conflict. Returns None when
    that text conflicts or a side does not hold exactly one block.
    """
    base_parts = _split_block(base, begin, end)
    our_parts = _split_block(ours, begin, end)
    their_parts = _split_block(theirs, begin, end)
    if base_parts is None or our_parts is None or their_parts is None:
        return None
    with tempfile.TemporaryDirectory(prefix="regen-merge-") as scratch:
        names = []
        for name, (head, _, tail) in (("ours", our_parts), ("base", base_parts), ("theirs", their_parts)):
            target = Path(scratch) / name
            target.write_bytes(head + begin + end + tail)
            names.append(str(target))
        result = _git_bytes("merge-file", "-p", *names)
    if result.returncode != 0:
        return None
    merged = _split_block(result.stdout, begin, end)
    if merged is None:
        return None
    return merged[0] + begin + our_parts[1] + end + merged[2]


def resolve_merge(base: str | None) -> int:
    """Finish a stopped merge whose only conflicts are generated outputs; refuse anything else."""
    try:
        unmerged = _unmerged()
        resolvable = _resolvable_paths()
        resolved: dict[str, bytes] = {}
        refused: list[str] = []
        for path, stages in sorted(unmerged.items()):
            if path not in resolvable:
                refused.append(f"{path}: not a generated file; resolve it by hand and git add it")
            elif 2 not in stages or 3 not in stages:
                refused.append(f"{path}: deleted on one side; decide by hand whether the output still exists")
            elif path in SPLICED:
                begin, end = (marker.encode("utf-8") for marker in SPLICED[path])
                merged = (
                    _merge_spliced(_blob(stages[1]), _blob(stages[2]), _blob(stages[3]), begin, end)
                    if 1 in stages else None
                )
                if merged is None:
                    refused.append(f"{path}: the hand-written text around its generated block conflicts; "
                                   "resolve it by hand and git add it")
                else:
                    resolved[path] = merged
            else:
                resolved[path] = _blob(stages[2])
    except (StepError, OSError, UnicodeError, ValueError) as error:
        print(f"FAIL  reading the conflicted index: {error}")
        return 1
    if refused:
        for line in refused:
            print(f"CONFLICT {line}")
        print(f"regen_all: nothing resolved; fix the conflicts above, then run {MERGE_COMMAND}")
        return 1

    for path, data in resolved.items():
        (ROOT / path).write_bytes(data)
        print(f"TOOK  {path}: {'merged its hand-written text' if path in SPLICED else 'our side'}, to regenerate")
    if resolved:
        staged = _git("add", "--", *resolved)
        if staged.returncode:
            print(f"FAIL  staging the taken files: {staged.stderr.strip()}")
            return 1
    if not unmerged:
        print("regen_all: no conflicted paths; regenerating and checking anyway")

    # During `git merge` the base contrib_refresh needs (merge-base of the merge commit and origin/main)
    # does not exist yet; a squash merge or a cherry-pick commits onto HEAD, whose base already does.
    merging = _git("rev-parse", "-q", "--verify", "MERGE_HEAD").returncode == 0
    controls = any(path in PROVENANCE.outputs for path in resolved) and not merging
    if run(False, base, provenance=not merging, require_provenance=controls) or run(
        True, base, provenance=not merging, require_provenance=controls
    ):
        print("regen_all: refusing to finish the merge: a step failed or a generated file is still stale; "
              f"fix that, then run {MERGE_COMMAND} again")
        return 1
    if merging:
        print(f"regen_all: merge resolved and staged. Commit it (git commit), then run {FIX_COMMAND} "
              "and commit the provenance refresh on its own")
    else:
        print("regen_all: merge resolved and staged; review git diff --cached and commit")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="exit 1 if any generated file is stale; write nothing")
    mode.add_argument("--list", action="store_true", help="print the steps in order and exit")
    mode.add_argument(
        "--resolve-merge",
        action="store_true",
        help="finish a stopped merge whose only conflicts are generated files: take one side, regenerate, check",
    )
    parser.add_argument(
        "--base", default=None, help="provenance base commit (default: merge-base(HEAD, origin/main))"
    )
    args = parser.parse_args(argv)
    if args.list:
        for step in (*STEPS, PROVENANCE):
            outputs = ", ".join(step.outputs) or "-"
            print(f"{step.name}: {step.command} -> {outputs}" + (f" [skipped: {step.skip}]" if step.skip else ""))
        return 0
    os.chdir(ROOT)
    if args.resolve_merge:
        return resolve_merge(args.base)
    return run(args.check, args.base)


if __name__ == "__main__":
    raise SystemExit(main())
