# Forking Nakagawa Recomp

Nakagawa Recomp is GPL-3.0-or-later, and it is meant to outlive any one maintainer. You can fork it
and carry on: fix bugs, add community features, improve Enhanced and Ace modes, and bring up games
nobody has tested yet. This page is the path for a fork maintainer, from the first build to sending
fixes back. It applies to any copy of the repository that is not `Jstar269/Nakagawa-Recomp` itself.

The safety rules come with the fork and do not change: no game data, no keys or credentials, no
firmware, no private captures, no hidden failures ([`AGENTS.md`](../AGENTS.md#hard-limits)). What
changes is who holds the provenance authority. Upstream, the maintainer holds it in a private ledger.
In your fork, you do.

## What a fork inherits, measured

This table was measured on 2026-11-08 at `origin/main` `d573c05`, in a local clone whose remote
pointed at another owner's repository and that had no private ledger. It was never pushed. The clone
added one new tool, one new page and one edit to an existing page, then ran every local gate. The
hosted rows come from reading each workflow, because a clone that is never pushed runs no hosted CI.
"Before" is a fork of that commit; "after" is a fork of a commit that includes this page's tools.

### Hosted checks

| Workflow, job | Before | Why | After |
| --- | --- | --- | --- |
| Every workflow | Off | GitHub turns Actions off in a new fork, and keeps scheduled workflows off, until you turn them on in the fork's **Actions** tab. | Same; turn them on once. |
| `provenance-attestation.yml`, **Trusted provenance attestation** | Fails on every pull request | It needs the `PROVENANCE_AUTHORITY_TOKEN` secret and reads the upstream's private repository, `Jstar269/nakagawa-recomp-history-private`. A fork has neither. | Skipped. It runs only in the upstream, matched by repository ID or name. |
| `provenance-attestation.yml`, **Fork provenance check** | Did not exist | | Runs on a fork's pull requests into its default branch, with no secret (below). |
| `ci-trust-verdict.yml`, **Compute trusted CI verdict** | Fails | It requires **Trusted provenance attestation** to succeed. `VERDICT_READ_TOKEN` is optional; without it the reads use `GITHUB_TOKEN`. | Requires **Fork provenance check** in a fork, everything else unchanged. Advisory unless you add a ruleset (rulesets are not copied). |
| `ci-trust-verdict.yml`, **Record, carry or withdraw CI-change approval** | Never triggers | It reacts to the `ci-change-approved` label, and a fork has no labels. A pull request that changes CI files fails the verdict with `CI_CHANGE_UNAPPROVED` until someone with write access applies it. | Same; create the label once (`gh label create ci-change-approved`). |
| `provenance-attestation-merge-group.yml` | Never triggers | It runs only for merge-queue groups, which a fork has only if it sets up a merge queue. If you set one up, this job needs the upstream's secret and fails. | Same. A fork does not need a merge queue. |
| `ci.yml`, all jobs (**Classify change** to **CI required**) | Run | GitHub-hosted runners. The self-hosted pool is used only in the upstream (`tools/ci_paths.py`, `SELF_HOSTED_REPOSITORY`), and `NK_SELF_HOSTED_*` variables are unset in a fork. On a pull request that adds a file, **Hygiene and security** (publication safety) and **Public export audit** fail until the path is admitted, as they do upstream. | Same. Once you admit the path in your fork (below), they pass. |
| `codeql.yml`, **Trusted CodeQL** | Runs on pushes to the default branch | Pull requests are scanned only on `batch/` branches and merge groups, the upstream's landing routes. | Same. |
| `dependency-review.yml`, `osv-scanner.yml`, `clusterfuzzlite.yml` | Run | No secret or repository condition. | Same. |
| `pages.yml`, **Deploy to GitHub Pages** | Fails on pushes to the default branch | The fork has GitHub Pages turned off. **Build site** passes. | Same; turn Pages on (**Settings**, **Pages**, source **GitHub Actions**) or ignore the job. |
| `scorecards.yml` | Off | Scheduled only (manual runs work). | Same. |
| `provenance-attestation.yml`, **Showcase demo PRXs**, **Candidate binary build and SLSA Build L2** | Manual, from the default branch | Release-candidate jobs. Not run in this measurement. | Same. |

### Local gates

| Gate | Before (a new file in a fork) | After (the fork admitted it) |
| --- | --- | --- |
| `python tools/contrib_refresh.py` | Refused, `NEW_PUBLIC_PATH` | Not needed: the admission writes the controls. |
| `python tools/policy_sync.py` | Fails: 2 tracked paths not classified | Passes |
| `python tools/publish_audit.py --tracked-only --public-scope --provenance-self-consistency` (with and without `--worktree`) | Fails: `POLICY_UNCLASSIFIED`, `UNRESOLVED_PUBLIC` for each new path | Passes |
| `python tools/admit_paths.py ... --private-repo <authority>` | Cannot run: no authority exists (a missing folder ended in a Python traceback) | Admits the paths; a missing folder is refused as `AUTHORITY_MISSING` |
| `python tools/provenance_attest_verify.py ... --trusted-ledger <authority ledger>` | Cannot run | Passes: the full trusted verifier, run locally against your authority |
| `mingw32-make contrib-check` | Fails: `publication-policy` (and `publication-safety` not run) | `publication-policy` and `publication-safety` pass |
| `python tools/tree_lint.py --check`, `python tools/lint_docs.py` | Fail for an unreferenced tool or an unindexed page | Same as upstream: reference the tool and index the page. Not a fork problem. |
| `provenance_record_gap.py --check`, `modified_file_notice_audit.py`, `audit_public_issue_links.py` | Pass | Pass |

## Fork, build and test

1. Fork the repository on GitHub, clone your fork, and turn on Actions in the fork's **Actions** tab.
2. Set up the toolchain with [`SETUP.md`](SETUP.md) on Windows 11, or
   [`LINUX_DEVELOPMENT.md`](LINUX_DEVELOPMENT.md) on Ubuntu 24.04 or WSL 2.
3. Build and test from the repository root, with MSYS2 UCRT64's `bin` on `PATH` on Windows:

   ```powershell
   python -m pip install .
   python tools/nk_doctor.py --scope build
   mingw32-make --no-print-directory display-smoke
   mingw32-make --no-print-directory native-core-tests
   mingw32-make contrib-check
   ```

   On Linux, `make CC=gcc native-core-tests` and `make contrib-check`. Run one Python test module
   with `python -m unittest tools.test_<name>`; hosted CI runs the whole suite.
4. Keep up with the upstream: `git remote add upstream https://github.com/Jstar269/Nakagawa-Recomp.git`,
   then `git fetch upstream` and merge `upstream/main` into your branch. Never rewrite published history.
   Push the merge to your default branch yourself: as a pull request it fails the Fork provenance
   check whenever the upstream changed that check's workflow, the release manifest, an approved
   binary, a VFPU table, any other binary or a publication rule, which only your own push may change.

## Become your fork's provenance authority

Every public file has a provenance entry in `assets/public_provenance_ledger.json`, and every new
file needs an admission before the gates pass. Upstream, admissions come from the maintainer's
private ledger. In your fork, they come from an authority you create once:

```powershell
python tools/fork_init.py init --authority ..\my-fork-authority
```

This creates a Git repository next to your checkout (never inside it: the tool refuses that). It
holds `docs/provenance/IMPLEMENTATION_PROVENANCE.json`, seeded from your fork's public ledger: one
record per record id the ledger names, with the same paths, classification, evidence tier and licence.
Each seeded record says that it adopts the claim the upstream published at that commit; it adds
nothing the public ledger does not already say. The tool then checks that the trusted verifier,
given only your authority, regenerates the committed public ledger byte for byte, and commits the
seed with your Git identity. `python tools/fork_init.py verify --authority ..\my-fork-authority`
repeats that check at any time.

Keep the authority wherever you like: a private repository, a public one, or a local folder you
back up. Nothing in your fork reads it automatically.

### Admit a new file

Say your pull request adds `tools/my_tool.py` and `docs/MY_TOOL.md`.

1. **Record an implementation file.** Documentation, configuration, fixtures and tests are classified
   by their path and need no record. Code needs an exact record, which is your own statement of where
   it came from:

   ```powershell
   python tools/fork_init.py record --authority ..\my-fork-authority --id my-tool-2026-11-08 `
     --path tools/my_tool.py --classification project-authored-independent --evidence-tier S `
     --statement "I wrote tools/my_tool.py for this fork."
   ```

   For code from another project, use `--classification derived-translated` (or
   `upstream-third-party` for a byte-identical import) with `--upstream`, `--upstream-revision` and
   `--upstream-license`, and follow the import rules in [`AGENTS.md`](../AGENTS.md#4-provenance-and-publication):
   a GPL-3.0-or-later compatible licence, a `PROVENANCE.md` row, notices kept, a `CREDITS.md` entry.
2. **Admit the paths** from the pull request's worktree, with the same tool the upstream uses:

   ```powershell
   python tools/admit_paths.py . tools/my_tool.py docs/MY_TOOL.md `
     --approval-basis "My decision of 2026-11-08: admit my_tool" `
     --origin-kind authored_from_scratch --origin "written for this fork" --license GPL-3.0-or-later `
     --private-repo ..\my-fork-authority
   ```

   It writes the admission and its policy delta into your authority, commits them there, builds the
   candidate controls on a local `admit/...` branch, and prints what is left.
3. **Finish** with the steps it prints: merge the `admit/...` branch, run
   `python tools/provenance_refresh.py --base <admission commit> --trusted-ledger ..\my-fork-authority\docs\provenance\IMPLEMENTATION_PROVENANCE.json`,
   commit, and push.
4. **Check it as the upstream would.** The full trusted verifier runs locally against your authority:

   ```powershell
   $base = git merge-base HEAD origin/main
   $ledger = "..\my-fork-authority\docs\provenance\IMPLEMENTATION_PROVENANCE.json"
   python tools/provenance_attest_verify.py --repo . --base $base --trusted-ledger $ledger `
     --workdir $env:TEMP\nk-baseline --emit-authority-baseline $env:TEMP\nk-baseline.json
   $key = python tools/provenance_attest_verify.py --repo . --base $base --candidate HEAD --policy-delta-key-v4
   python tools/provenance_attest_verify.py --repo . --base $base --candidate HEAD --trusted-ledger $ledger `
     --ephemeral --trusted-baseline $env:TEMP\nk-baseline.json --output-dir $env:TEMP\nk-verify `
     --policy-delta-authority "..\my-fork-authority\docs\provenance\policy-deltas-v4\$key.json"
   ```

   It ends `verdict: PASS` when every changed path is backed by your authority.

[`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path)
explains each step and what a skipped one reports. A record is an attestation: write only what you
know to be true, and never record someone else's code as your own.

### What the Fork provenance check does

On a fork, the **Fork provenance check** job in `.github/workflows/provenance-attestation.yml` takes
the place of the trusted attestation. Like it, the job runs from your default branch's copy of the
workflow and tools and reads the pull request as data, so a pull request cannot replace the check
with its own code. It reads no secret and never contacts any authority. It:

- refuses a pull request that changes `provenance-attestation.yml` itself, or whose other workflows
  spell the name **Fork provenance check** or **Trusted provenance attestation**, either of which
  could give a second result under the check's name. The name match is a byte search, a tripwire:
  a job name built by an expression escapes it. So when you add a ruleset, require **Trusted CI
  verdict**, which accepts the check only from the trusted run, not **Fork provenance check** by
  name;
- refuses a pull request that does not contain your default branch as the job checked it out
  (`NOT_UP_TO_DATE`): merge the default branch into it. Everything below is judged against that
  tip, and nothing else is judged until the pull request contains it;
- runs the trusted publication audit on the pull request's exact tree with `--public-scope
  --provenance-self-consistency`, which refuses game data, keys, firmware, private and user paths,
  unclassified paths and an inconsistent public ledger exactly as upstream. The audit judges the
  tree by the pull request's own profile, so the check refuses a pull request that changes the
  profile's rules (`PROFILE_RULES_CHANGED`; it may change `include_paths` only);
- holds the release manifest (`assets/release_manifest.json`) the same way: a pull request may not
  change it (`MANIFEST_CHANGED`). Nor may a pull request change the bytes of a path the manifest marks approved or of a table under
  `assets/vfpu/`, or their mode (`HELD_BYTES_CHANGED`), or add or change any binary file
  (`BINARY_CHANGED`). You change those by your own push to the default branch, after review;
- lists the paths the pull request adds to or removes from the publication profile, and names each
  path the pull request admits itself or whose claim in the public ledger it changes
  (`SELF_ADMITTED`): the first ten in the check's title line,
  the first nine as warnings on the check, and all of them in a list at the top of its summary. An added path must name a file of the pull
  request (`PROFILE_PATH_MISSING`), and a pull request may admit at most 50 paths, with 20,000
  characters of names in all, itself (`SELF_ADMITTED_LIMIT`), so that every one stays in view;
  admit more by your own push.

#### Two settings the check requires

The check judges a pull request's final tree against your default branch as it stood when the job
ran. Two settings make that the change that reaches your default branch. The check cannot set them,
and a fork does not copy rulesets, so add a branch ruleset for your default branch that:

1. **Requires branches to be up to date before merging** (under **Require status checks to pass**,
   with **Trusted CI verdict** required). Without it, a pull request that passed before you changed a
   rule, the release manifest or a held file merges without being judged against that change.
2. **Allows squash merging only** (**Allowed merge methods** under **Require a pull request before
   merging**). Without it, a merge commit or a rebase brings each of the pull request's commits into
   your default branch's history, and the check judged none of them: a file one commit adds and a
   later one removes reaches your history unchecked.

A fork without both settings has no guarantee from the check: a pass then covers only the tree it
judged, at the tip it judged, and your review of each commit has to cover the rest.

**A green Fork provenance check does not mean you admitted anything.** The check cannot tell your
admission from anyone else's: it passes a pull request that adds a path to `include_paths` and the
ledger itself, as long as the audit finds nothing forbidden in the bytes. Your review of the named
paths is the admission. In your fork, your admission and your review are the provenance authority;
the **Trusted CI verdict**'s success text says the same.

The job starts three ways, as the attestation does upstream ([Trusted CI verdict](CI.md#trusted-ci-verdict)):
from `pull_request_target`, when a `CI` run is requested for a pull request (`workflow_run`), and from
a dispatch that names a pull request and its head. GitHub blocks `pull_request_target` by default in
public repositories from 2026-12-02. Your fork needs no Actions policy for it: the `workflow_run` route
checks each pushed head, and the verdict judges the run titled `Provenance attestation of <head>` on
your default branch.

`tools/provenance_mode.py` decides which job runs. A repository is the upstream when its numeric ID
is the upstream's or its name is `Jstar269/Nakagawa-Recomp`, so renaming or transferring the upstream
cannot switch it to the fork check, and no variable, secret or pull request takes part in the
decision. `tools/test_provenance_mode.py` holds the workflow's conditions to the same rule. A
repository that takes over from the upstream by any other road (a fork promoted to be the main one,
or a repository created again under another name) has another ID and name, so it is a fork until both
constants in `tools/provenance_mode.py` are changed to its own.

## Where Enhanced, Ace and per-title work lives

- **Play modes and enhancements.** [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md) is the contract:
  Authentic stays untouched (section 1), each enhancement belongs to one category with fixed limits
  (section 2), and section 10 defines Authentic, Enhanced and Ace. Enhanced parts are project code in
  the player and runtime (`src/rt/ace.h`, `src/rt/gpu_sdl3vk/ace_ui.h`). Ace adds a title's own
  package, today its 60 FPS game logic package (section 9): draft one with
  `python tools/enhancement_package.py --draft`, validate it, and build it with
  `python tools/nk_cli.py build-package <DISC_ID> --enhancement-package <package>`.
- **Title profiles.** Public title manifests live in [`assets/titles/`](../assets/titles/README.md),
  validated by `python tools/title_manifest.py <manifest>`; they hold configuration only, never game
  bytes or hashes of private inputs. [`TITLE_PROFILE_ARCHITECTURE.md`](TITLE_PROFILE_ARCHITECTURE.md)
  explains how a title is identified and launched, and [`PORTING.md`](PORTING.md) walks through a
  manifest step by step.
- **Compatibility.** [`COMPATIBILITY.md`](COMPATIBILITY.md) defines the status words and records each
  title and subsystem. [`TITLE_TESTING.md`](TITLE_TESTING.md) is the qualification route behind a
  status, and [`FAILURE_CODES.md`](FAILURE_CODES.md) names every stop.

## Bring up an untested game

You need your own copy of the game. Everything it produces (the decrypted program, the generated C,
logs, traces, captures) stays in your private work folder and never enters the repository or a
public issue ([`AGENTS.md`](../AGENTS.md#5-private-and-public-boundaries)).

1. **Add the disc and read its checklist.** Add the `.iso` in the player, or run
   `python tools/nk_cli.py bringup <game.iso> --work-dir <private folder> --report <report.json>`.
   A valid disc with no profile yet comes in as **EXPERIMENTAL**; its checklist names whatever is
   missing ([`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md#when-something-is-not-supported-yet)).
2. **Read the report.** The bring-up builds a package, launches it under a time limit and writes a
   public-safe report. Its `failure_class` names the stage and reason it stopped
   ([`FAILURE_CODES.md`](FAILURE_CODES.md)). When the game installs code at run time, `--capture-rounds`
   rebuilds with the captured code each round.
3. **Read the `HLE` lines on stderr.** They appear in every run
   ([`DEBUGGING.md`](DEBUGGING.md#the-hle-lines-on-stderr)):
   - `HLE: unimplemented nid 0x<nid> (<name>)` means the game called an import with no handler; the
     next line names where to add one in `src/rt/hle.c`.
   - `UNSUPPORTED_IMPORT: <name> <detail>; the run stops here (#308)` is an unmodelled stop: a handler
     met a case the runtime does not model. The run exits with code 7 and the player shows an
     `unsupported-import-<name>` card. Model the case; never work around the stop.
   - `controlled refusal`, `compatibility exception` and `unverified` lines say how far to trust what
     ran; the exit summary lists them again.
4. **Turn on tracing for the area that stops.** Set the `SR_DEBUG` bitmask
   ([`DEBUGGING.md`](DEBUGGING.md#debug-categories-sr_debug-bitmask)): `0x02` for HLE calls, `0x04`
   for scheduling, `0x08` for the GE, `0x20` for files, `0x40` for display, `0xFF` for all of them.
   `SR_HLELOG=1` logs every syscall and `SR_NIDLOG=1` writes the call sequence to `nidseq_mine.txt`.
   The manager's `-GuestProfile` shows where guest time goes, and `--instruction-trace` on `bringup`
   writes a guest instruction trace into the work folder. Clear a presence-based switch with
   `Remove-Item Env:NAME`, not `NAME=0`.
5. **Fix the cause, with a test.** Run the guest's own code when it exists (LLE first), add or correct
   an HLE handler with a test that asserts PSP-visible results, or extend the analysis when code is
   missed ([`DEBUGGING.md`](DEBUGGING.md#execution-share-and-static-coverage)). Never add loop caps,
   sleeps, forced returns or fake success. [`PORTING.md`](PORTING.md#step-7-fix-game-specific-issues)
   lists the usual fixes.
6. **Record the result.** Update the title's row in [`COMPATIBILITY.md`](COMPATIBILITY.md), with the
   build and the evidence class, and keep the details in an issue. A title manifest for
   `assets/titles/` is a new path: admit it in your fork as above.

## Send fixes back upstream

1. Branch from current `upstream/main`, not from your fork's `main`, and carry only the fix: your
   fork's admissions, authority and fork-only features stay out.
2. Open the pull request against `Jstar269/Nakagawa-Recomp` from your fork, with **Allow edits by
   maintainers** on, and follow [`CONTRIBUTING.md`](../CONTRIBUTING.md): a test, the commands you ran,
   and an `Assisted-by:` line for any AI help.
3. For each new file, say where it came from in the pull request. Your fork's admission does not carry
   over: the upstream maintainer admits paths in the upstream's own authority, and **Trusted provenance
   attestation** stays red until then. That is expected. Do not include your fork's
   `assets/public_source_profile.json`, `assets/public_provenance_ledger.json` or `PUBLIC_EXPORT.json`
   changes for new paths; for edits to existing files, `make contrib-refresh` is enough.
