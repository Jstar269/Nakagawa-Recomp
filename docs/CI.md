# Continuous integration and dependency maintenance

The public workflow (`.github/workflows/ci.yml`) is intentionally one
always-present workflow with a cheap classifier followed by job-level
applicability checks. The classifier lives in
[`tools/ci_paths.py`](../tools/ci_paths.py), and its regression tests are in
[`tools/test_ci_paths.py`](../tools/test_ci_paths.py). The stable aggregate is
implemented by [`tools/ci_required.py`](../tools/ci_required.py) and tested in
[`tools/test_ci_required.py`](../tools/test_ci_required.py). It fails closed when
the change set cannot be determined, so an uncertain checkout runs the broader
gates instead of silently skipping them.

Name-status parsing is structural: ordinary records contain exactly one path,
rename/copy records contain both endpoints, and any malformed record becomes a
history-unavailable sentinel that selects the full matrix. `CI required` also
requires every `RUN_*` and `ALLOW_SUBSTANTIVE` output to be an explicit
case-insensitive `true` or `false`; missing or malformed control state is red.

## Workflow topology

| Event/change | Jobs that run | Jobs intentionally skipped |
| --- | --- | --- |
| Pull request, quick run: a Draft, and, while `NK_CI_TIERS` is `on`, every pull request from a branch of this repository that is not a `batch/`, `carry/` or `desk/` branch and has no `ci-full-matrix` label | the quick checks: classification, hygiene/security with its Markdown step, the public export audit, `Generated files and repository lints` and `CI quick checks` (see [Quick and full checks](#quick-and-full-checks)) | Python tooling, native/translation, the GCC 16 canary, the Android launcher build, Windows, the profile-zero fixture build and `CI required`, whatever the changed paths |
| Pull request, full run, docs-only | classification, hygiene/security with its Markdown step, CI required | Python/native, Windows |
| Pull request, full run, native C/build files | classification, hygiene/security, Python tooling, native/translation, Windows, Windows installer, CI required | none of the substantive public gates |
| Pull request, full run, recipe-only edit of phony targets in `Makefile` | classification, hygiene/security, Python tooling on the Makefile-coupled modules over four shards, native/translation, Windows, Windows installer, CI required | the Python modules that cannot observe a Makefile recipe |
| Pull request, full run, ordinary tools Python file | classification, hygiene/security, Python tooling, CI required | native/translation, Windows |
| Pull request, full run, installer inputs (`assets/installer/**`, the Inno lock, the installer, install-stager and runtime-DLL-stager tools and tests, `LICENSE`) | classification, hygiene/security, Windows (all four groups; the smoke group also stages the package), Windows installer, CI required; Python tooling when a Python tool changes | native/translation unless another changed path routes it |
| Pull request, full run, only `tools/test_*.py` modules that no native or Windows gate runs | classification, hygiene/security, Python tooling, CI required | native/translation, Windows (see [Tests-only changes](#tests-only-changes)) |
| Marked Ready (`ready_for_review`) | a new run on the head the Draft's run covered: the quick checks again when the head runs light, the path-applicable jobs when it runs full | as for that tier |
| Workflow/CI configuration | classification, hygiene/security, Python tooling, native/translation, Windows, Windows installer, CI required | none of the substantive public gates |
| Dependency-only metadata (`.github/dependabot.yml`) | classification, hygiene/security, CI required | Python/native, Windows |
| Mixed documentation/native changes | classification, hygiene/security, Python tooling, native/translation, Windows, Windows installer, CI required | none of the applicable product gates |
| Ordinary push to `main` after a validated merge, including one with a path the classifier does not recognise | classification, hygiene/security with its Markdown step when needed and its compact main smoke steps, CI required | expensive platform matrix; the merged PR (or batch) carried it, and an unrecognised path already forced the full matrix there |
| Workflow push to `main` (a changed workflow, action, CI classifier, aggregate or shard planner), or a push whose change set cannot be read | the full applicable validation above plus the main smoke steps | none of the substantive public gates |
| Manual `workflow_dispatch` | the full matrix, Windows installer, full Python tier and C coverage, regardless of paths | none |
| Nightly schedule (`17 3 * * *` UTC) | the full matrix, Windows installer, full Python tier and C coverage | none |
| Merge-queue group (`merge_group`, `checks_requested`) | the full matrix, as for a manual dispatch but without C coverage, regardless of paths; every required check reports on the group commit | none |

### What else starts on a pull request push

The table above is the `CI` workflow. Other workflows start on the same push, and
two of them are now path-gated too (measured on the repository's Actions usage export
of 2026-11-02, whole history, hosted runners):

| Workflow | Per push before | Now | Measured cost |
| --- | --- | --- | --- |
| `CodeQL wake` then `Trusted CodeQL` | 1 wake + prepare + 3 analyses + upload (6 jobs) | none on a pull request head that is not a `batch/` branch of this repository; on a batch head, 6 jobs when the diff touches C/C++, Python or workflow files, otherwise 3 (wake, prepare, upload) | `codeql.yml` is 38.5k hosted minutes over 2,732 runs, about 14 minutes a run |
| `ClusterFuzzLite` | 1 job, every pull request | 1 job only when the fuzz target's inputs change; none otherwise | 6.8k minutes over 1,767 runs, about 3.8 minutes a run |
| `OSV-Scanner`, `Dependency Review`, `Provenance attestation` | 1 job each | unchanged | cheap per run (4.5-5.6k runs) but supply-chain controls |
| `Trusted CI verdict` | about 4 runs, 1 job each | 2 runs (a new head, CI's completion); none for a superseded head's cancelled CI. `ready_for_review` starts none itself; the CI run it starts re-judges the head when it completes | 16 s average run |

### Jobs per run

GitHub's free plan runs about 20 jobs at once for this repository. A full `CI` run (a workflow
change, a manual run, the nightly schedule, a merge group) used to need 23 jobs, so a second
pull request's run waited for runners while the first ran. The same checks then ran in 14
jobs; the Android launcher build (`android_build`), added later, makes 15, a fourth Python
shard makes 16, a fourth Windows group makes 17, and installer and Windows-runtime changes add
one path-gated Windows installer job. The nightly schedule and a manual run add the informational
C coverage job (`c_coverage`): 19 jobs in all, with the installer job they also run. Nothing was dropped and no
command changed; jobs were merged and their steps run one after another. Three things differ in
when or how a step runs, and none of them skips a step
where it ran before:

- The main smoke steps no longer wait for the public-export job. The old `main_smoke` job
  needed `public_export` to succeed; as hygiene steps they wait only for hygiene's own steps,
  so they run in more cases, not fewer.
- The profile-zero steps now also wait for the export step to succeed, because they follow
  it in one job. Their consumers already needed `public_export` to succeed, and a failed
  export still fails `CI required`.
- The profile-zero steps' `python3` is the job's CPython 3.15.2, not the runner image's own.

The jobs:

| Job | Before (run 42270920833) | Now |
| --- | --- | --- |
| `classify` | 1 job | unchanged |
| `hygiene` ("Hygiene and security") | 3 jobs: `hygiene` 2.2 min, `markdown` 0.4 min, `main_smoke` (pushes to `main` and full-matrix events only) | 1 job, about 2.5 min: hygiene's steps, then the main smoke steps, then the Markdown lint |
| `public_export` | 2 jobs: `public_export` 0.2 min, `profile_zero_fixture` 0.4 min | 1 job, about 0.7 min: the export audit, then the profile-zero fixture build |
| `linux_gcc16` | 1 job, 3.3 min | unchanged (not required, a container job on a hosted runner) |
| `c_coverage` ("C coverage (informational)") | not present | 1 job, added later, on the nightly schedule and a manual run only (not required, a hosted runner; [Coverage](#coverage)) |
| `native_tools` | 3 parts, 7.9-9.2 min | unchanged |
| `python_tools` | 6 shards, 4.2-18.2 min | 4 shards: `test_production_smoke` alone, about 16 min, and three of about 14 min |
| `windows_runtime` | 6 parts, 4.2-9.7 min (4.5-12.0 in run 42258417250) | 4 groups of parts, 8.1-13.9 min in run 43055225113 (smoke 8.1, routes 13.9, selftests 9.3, ladder-fuzz 9.7) |
| `windows_installer` | — | 1 `windows-2025` job on installer and Windows-runtime routes; reuses the Windows player package artifact |
| `android_build` ("Android launcher build") | not present | 1 job, added later |
| `ci_required` | 1 job | unchanged on a full run; does not run on a quick one |
| `quick_checks` ("Generated files and repository lints") | not present | 1 job, on a quick run only ([Quick and full checks](#quick-and-full-checks)) |
| `ci_quick` ("CI quick checks") | not present | 1 job, on a quick run only, where it stands in for `ci_required` |

- **Hygiene.** The check keeps its name, which the ruleset and the
  [Trusted CI verdict](#trusted-ci-verdict) require. The main smoke steps carry
  `run_main_smoke` and follow hygiene's own steps, so, as before, they run only after every
  hygiene check passed. The Markdown steps carry `run_markdown` with `!cancelled()`: the
  Markdown job never waited for hygiene, so its lint still runs and reports when an earlier
  step failed. Node.js is set up after pre-commit, so no hook's environment changes.
- **Public export.** The export step runs first, on the clean checkout, under its own
  condition; the job and that step still read no path output. The profile-zero steps follow,
  under the former job's condition (`allow_substantive` and `run_python` or `run_windows`,
  carried by the job's `BUILD_PROFILE_ZERO`), and upload the same `profile-zero-fixture`
  artifact. Their `python3` is now the job's CPython 3.15.2 rather than the runner image's.
  The Python, native and Windows jobs wait for this job as before; native now also waits for
  the fixture build, about 25 seconds.
- **Python.** Four shards over the full suite, and four over the Makefile-coupled subset
  (see [Python test sharding](#python-test-sharding)). `test_production_smoke` alone takes
  about 16 minutes and has a shard to itself, so a fifth shard would not shorten the job.
- **Windows.** The six parts keep their steps and conditions (`contains(matrix.parts, '<part>')`)
  and run in four matrix groups: `smoke`, `routes`, `ladder-fuzz` (ladder, fuzz-0, fuzz-1)
  and `selftests`. A group runs the shared setup and the runtime-object
  compile gate once, then its parts in the workflow's step order. Parts in one group share the
  checkout and its build tree. The ladder builds under `build/platform-ladder`. The two fuzz
  shards used to run on separate runners and now share `build/windows-runtime`; that is
  harmless, because each regenerates its cases header and `vfpu_fuzz_build` has no output
  file, so it always relinks and reruns the fuzzer. `smoke` and `routes` ran as one group
  until run 42870221118 measured it at 20 minutes, the longest job of a full run: the shared
  setup about 2.3 minutes, smoke's steps about 7 and routes' about 10.6 (`native-core-tests`
  alone 6.7). Apart, run 43055225113 measured `smoke` at 8.1 minutes and `routes` at 13.9,
  with compiler caches restored from `main` entries of 2026-11-06, when the two last ran apart
  (79 % and 62 % hits; the 20-minute run had 85 %). Neither
  uses what the other builds: `native-core-tests` and `profile-zero-e2e` build their own inputs,
  and the player targets build the libchdr archive that `native-core-tests` used to leave behind
  (a Make prerequisite). The compiler cache is keyed on the group, so after a change to the groups
  a group restores only an older entry under its name, if one is left, until a run on `main` has
  saved a current one. When `run_installer` applies, the smoke group stages the already-built
  player and its runtime closure into a package artifact; an installer-only change therefore
  runs all four Windows groups, and a failed staging or upload step fails `windows_runtime`.
  The separate `windows_installer`
  job downloads that artifact, downloads the locked Inno Setup (up to three attempts) and checks its
  SHA-256 before a silent `/CURRENTUSER` install under the runner temp folder, and runs
  `tools/test_installer.py` with `NK_INSTALLER_REQUIRE_COMPILER=1`, so the compiler-backed tests
  (reproducible builds, and `RealProbeTests`, which run the script's uninstall-data and upgrade
  delete routines on temp folders with junctions, symbolic links and a read-only file, and its path
  compare on the 8.3 short and long spellings of one folder) fail instead of skipping. It then builds
  `Setup.exe` and runs `tools/verify_installer.py`, which installs, uninstalls and checks an upgrade
  in place over an older synthetic release, then repeats the build and compares both SHA-256 values.
  Each install runs with Setup's `/LOG=`, and a failed one reports Setup's PrepareToInstall message
  (or the log's last lines) beside its exit code. The job requires
  `allow_substantive == true`, so quick runs skip this heavy gate.

`CI required` needs `classify`, `hygiene`, `public_export`, `python_tools`, `native_tools`,
`windows_runtime`, `windows_installer` and `android_build`. The installer result is required
only when `run_installer` is true. The Markdown lint, the main smoke and the fixture build reach it through the
two always-required jobs they now belong to, so `tools/ci_required.py` no longer takes a result
or run flag of their own. Each merged job's `timeout-minutes` covers its parts: hygiene 35
(15 + 10 + 10), public export 30 (15 + 15), each Windows group 25 per part (50, 75 and
25), and each Python shard 60, twice the former 30 because it does about twice the work.

The trade-offs: a step that fails stops the later steps of its own job, so a pull request that
fails, for example, the Windows smoke part does not learn about its routes part in the same run
(the Markdown lint is the exception above). A Python shard now takes about as long as the old
slowest shard, and the longest Windows group about 13 minutes instead of 10-12, which is still
shorter than a Python shard, so a run on idle runners takes about as long as before, and less
time waiting when other runs are queued.
`tools/test_public_ci_wiring.py` (`PublicExportJobTests`, `WindowsRuntimePartitionTests`)
pins the merged jobs: every former step with its command and condition, its order, and every
Windows part in exactly one group.

#### Tests-only changes

A `tools/test_<subject>.py` file used to route by its subject: `test_codegen_x.py` counted
as a `codegen` change and started the native and Windows jobs, about nine runner legs. Those
jobs never run a test module unless a build or CI descriptor names it (the Makefile, `mk/`,
`CMakeLists.txt`, the workflows) or a tool, driver or fixture script does: they run make
targets and a few fixed `unittest` module names, not a discovery sweep. `tools/ci_paths.py`
therefore reads those files (`_native_gate_sources`) and treats a test module as checked by
the Python shards alone unless one of them names it as a whole identifier, or a module that
is run imports it or names it as a string, transitively (`_gate_executed_tests`;
`tests/native/test_player_ui.py` imports `tools/test_player_package_route`, which therefore
stays routed). Anything else keeps its route: a module a descriptor names (for example
`test_flash0_font` and `test_public_ci_wiring`, which `ci.yml` runs on Windows), a deleted
module a descriptor still names, a classifier test (`test_ci_paths`, `test_ci_required`,
`test_ci_test_shards`), any non-test file beside it, an unparsable gate module (every test
then counts as run), and every unreadable-descriptor case. A gate that loads a test module
by a computed name would escape this and must name the module literally. The exported subject
flags (`build_system`, `python_tools`, ...) are unchanged; only `run_native` and
`run_windows` skip.

#### Trusted CodeQL on pull requests

`Trusted CodeQL` is the producer of the required `CodeQL` status: GitHub's code scanning
creates that check only when a SARIF upload for the pull request's merge commit is
processed, and no job of ours can post it.

**Batch heads only.** A pull request wake is analysed only when the head is a branch of this
repository whose name starts with `batch/`. Those are the batch pull requests the
maintainer lands through. Hosted Actions runs about 20 jobs at once, and an analysis takes
13 to 75 minutes, so analysing every push of every pull request and then the batch that
carries it spent the runner budget twice. Each change is now analysed once in its batch,
and again on the push to `main`. The rule is the `prepare` job's `if:` in the default
branch's `codeql.yml`; `github.event.workflow_run.head_repository` and `head_branch` come
from GitHub's run record, not from the candidate. A fork head named `batch/...`, a missing
head repository and any other branch all skip `prepare`, and with it `analyze` and
`upload`. Such a head has no `CodeQL` check, not an empty or neutral one, so it cannot be
merged directly. That rests on the `main-minimal` ruleset requiring `CodeQL` and keeping its
`bypass_actors` list empty (both read from the ruleset on 2026-11-05;
[`PROVENANCE_MERGE_GATE.md`](PROVENANCE_MERGE_GATE.md#maintainer-configuration)).

For an early scan of any open pull request, or before landing one outside a batch, run
`gh workflow run codeql.yml -f pull_request_number=<n>`. That run is titled
`Trusted CodeQL (PR #<n>)`, so a dispatch can be matched to its pull request; every other
run is titled `Trusted CodeQL`. A merge-queue group, a push to
`main`, the weekly schedule and a dispatch always run `prepare`.

On a batch head or a dispatched pull request, `prepare` and `upload` always run, and
`prepare` decides, from the default branch's `tools/ci_paths.py --codeql-scope BASE
MERGE`, whether the diff can change a result:

- **Analysed in full:** any added, changed, deleted or renamed C/C++ source or header, Python
  file, workflow, action or `.github/codeql/` file; an unreadable diff or any name that cannot
  be classified with confidence (the diff is read with `git diff -z`, so names are verbatim;
  a quote, backslash, control character or edge whitespace still forces a scan); a
  merge-queue group; a push to `main`; the weekly schedule; and a manual dispatch with `force_scan`.
- **Skipped:** a diff made only of other files (documentation, assets, `Makefile` and `mk/`,
  JSON, PowerShell). `build-mode: none` means the C analysis reads source files and never
  builds, so a build file cannot change it. The `upload` job then writes one empty SARIF
  result for each of the three categories, each carrying a note that the analysis was
  skipped and why, and sends it through the same stale-head checks and the same upload as a
  real result. The required `CodeQL` check reports on the exact merge commit as before.

The decision is made by the default branch's copy of the workflow and classifier, from git
objects, so a pull request cannot widen or narrow its own scan. `codeql-wake.yml` has no
path filter. Its `wake` job runs only for a merge group or for a pull request from a
`batch/` branch of this repository, the same heads `prepare` accepts, so other pushes use
no runner. That file is candidate-controlled: a pull request that edits the filter can drop
only its own wake, which leaves its required check missing, and a wake it adds back is still
refused by `prepare`.

If an empty upload is ever refused, `gh workflow run codeql.yml -f pull_request_number=N
-f force_scan=true` analyses that pull request in full.

#### Why the verdict waited about nine minutes

`Compute trusted CI verdict` averages 16 s of run time but 531 s of queue time in the usage
export. Nothing in `ci-trust-verdict.yml` explains that. Its concurrency group is per head
SHA with `cancel-in-progress: false`, so runs for one head queue behind each other, but a
run waiting on its group has no job yet, so that wait is not part of the job's queue time.
It runs on `ubuntu-26.04` with no `needs` on any other workflow (`workflow_run` starts it
after each producer finishes; it does not wait for them). Sampling the 87 verdict jobs of
2026-11-01 to 2026-11-02 through the API gave a mean queue of 18 s, a median of 2 s, a
90th percentile of 62 s and a maximum of 267 s; the four longest waits (169 to 267 s) all
fell within seven minutes of 2026-11-02, when many other jobs were also waiting. The
historical 531 s average is therefore the runner limit (about 20 concurrent jobs) and not
a property of the workflow: the verdict job is created when a producer finishes, which is
when the repository's own matrix has most jobs queued, and it waits in the same queue as
the Windows and Python legs. The way to shorten it is to start fewer jobs per push, which
is what the path gates above do. The verdict does not run on `ready_for_review` itself: that
event starts a full CI run, whose completion re-judges the head, and a verdict run on the
transition could only judge the previous CI run. Every
label event still re-judges, whatever the label. A newer run for a head replaces that head's
pending run, so a run that judged nothing could leave a verdict standing that a withdrawn
approval should have changed.

#### Why the verdict waited over an hour (2026-11-05)

On 2026-11-05 the account ran at its ceiling of 20 hosted jobs for 196 of the 320 minutes
from 14:30 to 19:50 UTC, on every label at once (`ubuntu-24.04`, `ubuntu-26.04`,
`windows-2022`). CI used about 7,600 of the 8,350 runner-minutes and the verdict jobs 74,
yet 206 verdict jobs waited a median of 43 minutes for a runner (90th percentile 91). The
verdict is a victim of that queue, not its cause, but it added to it in two ways:

- **Pending-only runs.** CI finished last on all 181 heads whose three `pull_request`
  producers passed on 2026-11-04 and 2026-11-05. A run started when `OSV-Scanner` or
  `Dependency Review` finished could only re-post `pending`, which the `pull_request_target`
  run had already posted, and it held the head's concurrency group, so the run CI's
  completion started waited for it to get a runner first. Of 31 `workflow_run` verdicts
  queued at 19:35 UTC, 14 were of that kind.
- **Superseded heads.** `CI` cancels a pull request's run when a newer head arrives. The
  cancelled run's completion started a verdict for a head no pull request still had, which
  the verifier answers by posting nothing. Seven of the 31 were of that kind.

The workflow therefore re-triggers on `CI` alone, and its verdict job skips (no runner) when
the triggering run is a cancelled first attempt. Both are fail-closed: a skipped run leaves
the head at `pending`, never `success`. If the current head's CI is cancelled, a re-run
(attempt 2 or later) is judged as usual; if `OSV-Scanner`, `Dependency Review` or the
provenance attestation finishes after `CI`, dispatch the verdict (see "Re-running" under
[Trusted CI verdict](#trusted-ci-verdict)). Together they would have removed 21 of those 31 queued runs.
They do not change the ceiling: verdict latency still follows the CI queue.

### OpenSSF Scorecard

The OpenSSF Scorecard workflow (`.github/workflows/scorecards.yml`) runs daily, and on demand from the
Actions tab. It doesn't run on every push to `main`, because each run spends the repository's shared hourly
`GITHUB_TOKEN` API budget. It uploads SARIF to GitHub code scanning and publishes results to scorecard.dev
for the README badge.
Triage findings through the usual issue process. The workflow does not change branch-protection
settings.

### Quick and full checks

Hosted CI has two tiers. The **quick checks** are `Classify change`, `Hygiene and security` (with its
Markdown step), the public export audit, `Generated files and repository lints` and their aggregate,
`CI quick checks`: about 5 runner-minutes. The **full checks** add the Python tooling gates, the native
and translation gates, the GCC 16 canary, the Android launcher build, the Windows runtime gate, the
Windows installer job and the profile-zero fixture build, as the changed paths route them, and their
aggregate, `CI required`: about 144 runner-minutes for a change that routes everything. OSV-Scanner,
Dependency Review, the provenance attestation and the Trusted CI verdict run on every push in both
tiers.

The repository variable `NK_CI_TIERS` switches the tiers. While it is not `on`, only a Draft runs the
quick checks: every other pull request runs the full checks on each push, its head lands on its own
`success` or in a batch, and the `ci-full-matrix` label changes nothing. While it is `on`, a pull
request from a branch of this repository runs the quick checks on each push and gets the full checks
when it is batched for landing or labelled. The rest of this section says which rules apply only
while it is `on`.

`tools/ci_paths.py` exports `tier`. Its value `light` is what this page calls a quick run, and the jobs
of a quick run are the quick checks; `full` is a full run. A run is `full` for a manual dispatch, the
nightly schedule, a merge group, a push to `main`, and a pull request that is not a Draft and is any
of: a `batch/` branch of this repository, from a fork, a `carry/` or `desk/` branch (the desk's copy of
a fork's pull request), labelled `ci-full-matrix`, or run while the repository variable `NK_CI_TIERS`
is not `on`. Every other pull-request run is `light`. On a pull request `allow_substantive` is true
exactly when `tier` is `full`, and every substantive job gates on it. The path flags (`run_python`,
`run_native`, `run_windows`, `run_installer`, `run_markdown`) do not depend on the tier, so a full run
routes exactly the gates the quick run of the same change skipped. What a push to `main` runs is
unchanged: `allow_substantive` still narrows it there, and `CI required` still runs.

`Generated files and repository lints` runs only on a quick run. It holds checks that a quick run
would otherwise leave to the landing batch, each of which the full tier runs inside a longer job:
`python -m unittest tools.test_lint_docs`, `python tools/format_portability_check.py`,
`python tools/regen_all.py --check`, and the tools mypy baseline (`tools/mypy_baseline.py --scope
issue-1391`, then without `--scope`, with the Python tooling job's pinned install). Five lane runs of
2026-11-09 failed only in a heavy job; the first three checks would have turned every one of them red
on a quick run, and the mypy baseline was the most frequent lane failure of that week.

A quick run is never green for landing. `CI required` does not run on it, and `tools/ci_required.py`
fails any pull-request run whose substantive gates were suppressed, so the
[Trusted CI verdict](#trusted-ci-verdict), which reports `success` only when `CI required` succeeded at
the exact head, reports `pending` with `LIGHT_CI_GREEN` instead (a Draft gets `DRAFT_LIGHT_CI`). While
`NK_CI_TIERS` is `on`, the landing desk batches heads in that state; the batch's head gets the full checks, the attestation, CodeQL and its own verdict, and only
that head merges. `tools/test_public_ci_wiring.py` (`TwoTierInvariantTests`) evaluates the workflow's
own conditions and fails if `CI required` can be green while a routed gate did not run.

**Asking for the full checks on one pull request.** While `NK_CI_TIERS` is `on`, a maintainer adds the
`ci-full-matrix` label and
re-runs all jobs of the newest `CI` run at the head (`gh run rerun <run-id>`); any later push runs
them too. "Re-run failed jobs" does not change the tier, because it keeps the first attempt's
classification. The classifier reads the label from GitHub when the run starts, because a re-run
reuses the first attempt's event. The label is not an approval: the verdict never reads it. Removing
it makes the next run a quick one. Do not dispatch `CI` on the branch by hand instead: a `CI required`
check from a run that is not a pull-request run is a `FOREIGN_PRODUCER` finding until the next push.

**Waiting on a pull request.** While `NK_CI_TIERS` is `on`, `Trusted CI verdict` stays `pending` on a
quick-green head, so
`gh pr checks --watch` does not return; watch the run instead (`gh run watch <run-id>`).

**Turning it off.** Delete `NK_CI_TIERS` or set it to anything but `on`: the next push of every pull
request runs the full checks again.

#### Draft pull requests

A Draft always runs the quick checks, whatever its label or the switch, and the verdict reports
`DRAFT_LIGHT_CI` for it, so no Draft joins a landing batch. The verdict reads the draft state from
GitHub, and a draft state that is not `false` counts as a Draft. Marking it Ready starts a run at the
same head (`ready_for_review`), whose completion judges the head again. `converted_to_draft` starts a
quick run, and the shared concurrency group cancels a full run still in progress, so a Draft holds no
substantive runner. `provenance-attestation.yml` lists `ready_for_review` as well.

`CI required` is the aggregate of a full run. It runs with `always()` whenever the tier is not
`light`, accepts an intentionally skipped irrelevant job, and fails when a classifier-applicable job
fails, is cancelled, or is otherwise incomplete. A failed hygiene/security job or public-export job is
never hidden by the aggregate. The `main` ruleset does not name this job: it requires the
[Trusted CI verdict](#trusted-ci-verdict), which reports `success` only when this job succeeded.

### ClusterFuzzLite C fuzzing

Pull requests that change the fuzzed code run the pinned ClusterFuzzLite workflow in
`.github/workflows/clusterfuzzlite.yml`: `src/core/nk_json.c`, `src/core/nk_json.h`,
`tests/fuzzers/`, `.clusterfuzzlite/` and the workflow file itself (a path filter; the
check is not a required status, so a pull request that skips it waits on nothing). A nightly
and a manual run fuzz the whole target for five minutes in `batch` mode, since there is no
diff for `code-change` mode to follow. Its C target
`tests/fuzzers/test_nk_json_fuzzer.c` calls the public `nk_json_parse` API
with bounded synthetic bytes and exercises nesting at the `NK_JSON_MAX_DEPTH`
boundary. The project configuration is `.clusterfuzzlite/project.yaml`; the
Clang/libFuzzer build is defined by `.clusterfuzzlite/Dockerfile` and
`.clusterfuzzlite/build.sh`. AddressSanitizer code-change fuzzing is capped
at 60 seconds with a 4 KiB maximum input. The target does not use title files,
retail inputs, or hardware.

### Project website (GitHub Pages)

`.github/workflows/pages.yml` publishes the project website,
<https://jstar269.github.io/Nakagawa-Recomp/>, from [`docs/index.html`](index.html). Pages paths
are case-sensitive, so the lowercase address returns 404. The repository's Pages source is "GitHub
Actions". The page is one static file with inline CSS and two small inline scripts (one applies a
remembered theme before the first paint; the other is the theme picker, which keeps a choice in the
visitor's own browser), and no web fonts, external scripts or third-party requests. It shows two
launcher screenshots,
`docs/media/launcher-library.png` and `docs/media/launcher-building.png`, with their notice, and link
previews show the share card `docs/images/brand/og-card.png`. The artifact also carries the presentation
clip `docs/media/hst-rally.mp4` for the README's link. The build job copies an explicit list of
files into the artifact, so nothing else in the tree is published, and fails when the page
names a local file the list does not copy. Before it uploads the artifact, it runs
`tools/test_site_mobile.py` on the assembled site: headless Chromium (Playwright, hash-pinned)
renders the page at 360x780, 390x844, 430x932 and 1280x800 in Auto with a light system, Auto with
a dark system, picked Light and picked Dark, and fails on sideways overflow, a section whose
background does not match the active theme, text below WCAG AA contrast, a phone tap target
under 44 px, a theme picker that does not switch the whole page, or a missing image or link
target. Locally the browser half reports SKIP when Playwright or its Chromium is missing; the
workflow sets `NK_SITE_TEST_REQUIRED=1` so it cannot skip there. A pull request that touches
those paths, or the test, runs the build job only; a push to `main`, or a manual run on `main`, also deploys through
`actions/deploy-pages`. Only the deploy job holds `pages: write` and `id-token: write`, and
the `github-pages` environment accepts deployments from `main` alone. To preview the site,
open `docs/index.html` from a checkout: its relative image paths resolve the same way. Keep
the page's status text in step with `README.md`, `docs/COMPATIBILITY.md` and the release notes.

### Hygiene beside the substantive gates

The native job waits only for `classify` and the public-export audit
(`needs: [classify, public_export]`), as do the Python and Windows jobs, which also use
the profile-zero fixture that job builds. None of them waits for `hygiene`, which runs
beside them. Before this change they waited for hygiene too,
so its all-files pre-commit run, Betterleaks history scan and canaries (98 s median
across 40 recent pull requests, as measured from their job timings) sat in front of
every expensive gate. The public-export audit stays a dependency: it is the
publication-integrity boundary, it runs in about 52 s, and it keeps the candidate
export ordered before any substantive job starts. `CI required` still requires the
hygiene result (`HYGIENE_RESULT`) and the public-export result, so no failure is
hidden. The trade-off is that a pull request that fails hygiene now also spends
runner time on the expensive gates that started beside it. The main smoke is not
changed: its steps, now in the hygiene job after hygiene's own, run on pushes to `main` and
on manual and merge-queue runs (the full-matrix events), never on pull requests, and they are
not a substantive gate.

### Native gates in parallel parts

The `native_tools` job runs as three matrix parts, as `windows_runtime` does (#694). Run
serially, its gates took about 14.4 minutes and set the length of every CI run, while its
setup took about 25 seconds with warm caches. Every part repeats that setup: the apt
packages, the pinned SDL3 and SDL3_ttf installs, PSPDEV and the build directory. Each
gate runs in exactly one part with the command it had in the serial job:

- `core`: the FFmpeg H.264 media selftest, the import-audit and provenance-gap gates, the
  portable objects and format check, and the native core suite and its coverage upload.
- `translation`: the HLE, scheduler and dispatch selftests, cosimulation and its mutation
  corpus, the GPU selftests on lavapipe, the interpreter, VFPU, strbuf and ATRAC3+
  selftests, and the microtest codegen gates.
  On the nightly schedule only, `translation` also runs `make CC=gcc selftest-repeat` after
  the single scheduler and HLE thread runs: it reruns the three `sched_selftest_*` binaries
  and `hle_thread_selftest` they built `SELFTEST_REPEAT` times (default 20), stops at the
  first failure and prints the iteration and binary that failed (#1846). Pull requests, pushes,
  merge groups and manual runs skip it, so their time is unchanged.
- `player`: present pacing, the player UI tests and regressions, the CMake player, the
  showcase and the two player smokes that stage its ISO.

Steps that read what an earlier step built share its part. `needs.native_tools.result`
aggregates the parts, so `CI required` fails when any part fails.
`tools/test_public_ci_wiring.py` (`NativeToolsPartitionTests`) fails if a serial gate is
dropped, runs in more than one part, or changes its command.

### Ubuntu packages and mirror outages

Every step that installs Ubuntu packages (the native parts' dependencies, and
`libc6-dev-i386` for the quick checks' 32-bit format check) runs one helper, the
workflow-level `NK_APT_INSTALL`. It installs only the packages that are missing, so an
image that carries them needs no mirror. Otherwise it probes each mirror in
`NK_APT_MIRRORS` once for the release's `InRelease`, points the Ubuntu sources at the
mirrors that answered, in list order, and makes up to three attempts (3 minutes for the
index update, 8 for the install), moving the head mirror to the end of the list after a
failed one. The list starts with the Azure mirror and `archive.ubuntu.com` and goes on to
mirrors outside `ubuntu.com`: on 2026-11-11 the hosted runners reached none of Canonical's
hosts, and the image's own mirror list holds only those. apt checks every index against
the Ubuntu archive key whichever mirror serves it: the helper keeps each source's
`Signed-By`, and no workflow may switch the check off. When no mirror answers or every
attempt fails, the step fails; it is never skipped.

On a self-hosted `nk-wsl` runner ([below](#self-hosted-linux-runners)) the helper changes
nothing while the image carries every package. When one is missing, it edits the
runner's own apt configuration as it does on a hosted runner: it points the Ubuntu
sources at `/etc/apt/apt-mirrors.txt`, writes that list and
`/etc/apt/apt.conf.d/79-nk-apt-network`, and installs the package. Those changes last
until the runner is discarded: each pool runner is a container that takes one job, so they
never reach the pool's image or the next job, and every job repeats the fetch until the
image carries the package.

`tools/test_public_ci_wiring.py` (`AptInstallHelperTests`) runs the helper against stubs. It
fails if a step calls `apt-get` itself, if the helper drops a source's `Signed-By`, or if
any workflow switches apt's signature check off: `--allow-unauthenticated`,
`--allow-insecure-repositories`, `--allow-downgrade-to-insecure-repositories`, `force-yes`
as a flag or setting, a `trusted` source option with any value, or an `AllowInsecure*`,
`AllowDowngradeToInsecure*` or `AllowUnauthenticated` setting.

### Android launcher build

The `android_build` job (#360) runs whenever the native gates do (`run_native`), beside
them, and `CI required` reads its result (`ANDROID_RESULT`). On `ubuntu-24.04` it installs
the SDK platform, build-tools, NDK and CMake versions that `android/app/build.gradle.kts`
names, downloads the Gradle distribution named in
`android/gradle/wrapper/gradle-wrapper.properties` and checks it against that file's
`distributionSha256Sum`, then builds the launcher-only debug APK for arm64-v8a
(`gradle -p android assembleDebug -Pnk.abis=arm64-v8a`) and checks that it carries
`libmain.so`, `libSDL3.so` and `libSDL3_ttf.so` for that ABI and no other ABI. No title is
bundled and nothing runs: running needs a device or an emulator
([`ANDROID_DEVELOPMENT.md`](ANDROID_DEVELOPMENT.md)). The APK is kept for 14 days as the
artifact `nakagawa-android-arm64-debug`, for installing on a phone
([Test on a phone](ANDROID_DEVELOPMENT.md#test-on-a-phone)). The repository keeps no Gradle wrapper
jar or scripts, as it keeps no other generated executable (#1100).
`tools/test_public_ci_wiring.py` (`AndroidBuildWiringTests`) fails when the job's versions,
the Gradle files and `ANDROID_DEVELOPMENT.md` disagree.

The full-tree pre-commit run retains the publication audit and the Betterleaks
current-tree scan; the hook runs Betterleaks in directory mode over the files it
is given (every tracked file under `--all-files`, the staged files at the commit
stage). The hook is commit-stage only: at pre-push pre-commit supplies no file
names, which would turn directory mode into a whole-working-tree scan. Hygiene then runs an explicit Betterleaks reachable-history
scan in git mode and the synthetic canary gate. It also runs the core purity gate,
[`tools/purity_gate.py`](../tools/purity_gate.py): a hexadecimal literal in PSP user RAM
(`0x08800000`-`0x09ffffff`) in `src/rt/` code fails it, outside title-config files, native
self-tests, registered NIDs and the reviewed allowlist in that file (one entry per literal,
with its reason). A fix for one game goes in its title config, not the core. Markdown linting is separate from the native and Windows jobs. Those jobs remain
synthetic/public-input gates; no private game input is put in Actions.

The shared pre-commit hooks include `mypy-tools`, which requires mypy 2.4.1 and checks the clean
issue #1391 scope. Hygiene skips that local-system hook because the `python_tools` job installs
the same version from a hash-locked requirements file, runs the clean-scope check, then runs the
full `tools/` per-module ratchet. This keeps the developer hook and hosted checks on one pin
without repeating the full scan in two jobs.

The Windows job also runs `mingw32-make production-smoke` in the existing MSYS2 UCRT64/GCC,
SDL3, and Vulkan environment. That target generates its PSP-shaped input from committed source,
uses the ordinary loader/import/analyzer/codegen pipeline, links the complete public-safe
production runtime and real driver, and then reaches a registered HLE NID through the scheduler.
Its pass condition is a relocation-dependent guest-memory sentinel checked by the production
driver. The link map and runtime markers make reduced `gate_stub` substitution or omitted critical
objects fail closed. This is a production-composition integration test, not PSP-hardware or private
title acceptance evidence.

The Windows job also runs `production-smoke-gap`: the same fixture with its helper omitted from
native emission at build time, proving region A reaches the omitted guest address through the
ordinary production `dispatch()` seam. Analyzer-owned executable-span registration permits only
those guest bytes to enter the fail-closed interpreter; the gate then requires a registered AOT
region-B handoff, real HLE call, and final `0x00001235` production-driver assertion.

The Windows job also runs a headless staging smoke after it links the player (#308). Because the
runner has no PSPDEV, `python fixtures/showcase/showcase.py stage-iso` wraps the source-owned
display guest in the showcase's synthetic TEST00007 identity. The built `nakagawa_player.exe
--stage-only` must then report `STAGING_RESULT status=PASS` within 60 seconds. LOCALAPPDATA,
APPDATA and USERPROFILE point under the runner temporary directory. The Linux `native_tools` job
stages the PSPDEV-built scene instead. Neither run is PSP-hardware or consumer-ISO evidence.

When `tools/ci_paths.py` routes the native jobs, both hosts run `psmf-media-selftest`, the
source-authored PSMF/H.264 fixture behind the `sr_h264` decoder seam (#283), each on its own
backend. The Windows job runs it in its `selftests` part on Media Foundation
(`src/rt/h264_mf.c`), then requires `psmf-media-selftest-csc-mutant` to be killed. The Linux
`native_tools` job runs it on the vendored FFmpeg subset (`H264_FFMPEG=1`). The same Windows part
runs it a second time with `H264_FFMPEG=1` in its own build directory, so the subset's `_WIN32`
branches, which the Linux job never compiles, build and decode the fixture there too. When no
decoder instance can be created, the selftest reports `SKIP` for its decode checks and still exits
0, so both Windows steps fail on any `SKIP` line. Neither run is private media acceptance or
PSP-hardware evidence.

The Windows job also runs `cosim-selftest` and `cosim-mutants`. The first executes the same
source-owned guest bytes twice — once as generated native code, once through the production
interpreter floor — and reports the first difference in the canonical instruction trace, the
ordered guest writes, the guest memory window, or the architectural state vector. The second
rebuilds that comparator against deliberately mutated copies of the interpreter and requires each
defect class to fail the gate; a mutant that only breaks the build is rejected as `INVALID`, not
counted as a kill. Both are source-owned and need no game input. See
[`fixtures/cosim/README.md`](../fixtures/cosim/README.md) for the comparison contract and the
limits of the evidence.

Both hosts run `snapshot-selftest` (#1842): the Windows job in its `selftests` part and the Linux
`native_tools` job in its `translation` part. It builds `src/rt/snapshot_runner.c` against a
source-owned synthetic module (`fixtures/snapshot/`), runs each case's translated function and
the reference interpreter in `src/ref` from the same snapshot, and requires the exact verdict
for every case, a named refusal for every malformed snapshot file, and `MISMATCH r11` from a
runner built with one immediate changed in the generated C. The verdicts are described in
[`docs/DEBUGGING.md`](DEBUGGING.md#snapshot-execution).

Both hosts run `verify-selftest` (#1843) in the same two jobs and parts. It builds the verify
harness (`src/rt/verify_harness.c`, `src/rt/verify.c`, `-DSR_VERIFY`) against a source-owned
synthetic module (`fixtures/verify/`), drives the samplers, the rate budget, the range filter
and the skip classes, and requires a report with the expected counts for each. It then builds a
harness and a snapshot runner whose generated C has one changed immediate and requires the
`MISMATCH`, a capture, and the same first difference from the replay. It also requires that
`make VERIFY=1` keeps the generated-code compile flags, that every runtime `_Exit` and `_exit` writes the
verify report first, and that the default `recomp.c` and `hle.c`, the two hooked units, carry
no verify hook once preprocessed. See
[`docs/DEBUGGING.md`](DEBUGGING.md#sampled-in-play-checking).

Both hosts run `hle-thread-selftest`, the executable production-HLE suite that links
the runtime HLE sources (`src/rt/hle.c` and its subsystem files, #306, #307): the Windows job in its
`selftests` part, and the Linux `native_tools` job after the pinned SDL3 install, because the suite
links SDL3 for the scheduler's host clock. Each host runs its own arms of the file, directory and
host-data seams (`hle_host.c`, `hle_io.c`, `hle_data.c`). A few checks exist on
Windows only, because what they check is Win32: the `FindFirstFileW` error classifier, the
`CreateProcessW` command-line quoting of the exit-game respawn, and the `mklink` junction (Linux
plants a symbolic link in its place).
The Linux job also runs `hle-title-selftest`, the same harness built under the generic and both
fixture title configurations.

Most Linux jobs run on Ubuntu 26.04. The `native_tools` job stays on Ubuntu 24.04 because its
translation gate installs `gcc-mipsel-linux-gnu`, which is absent from the Ubuntu 26.04 package
archive ([Ubuntu 24.04 package](https://packages.ubuntu.com/noble/gcc-mipsel-linux-gnu)). It builds
SDL3 from its pinned 3.4.20 release commit with X11 and Vulkan enabled, and SDL3_ttf 3.2.2 from upstream commit
`a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b`, with vendored FreeType and HarfBuzz enabled.
The SDL3 and SDL3_ttf staged installs are cached under keys naming their pinned source versions
and commits, the SDL3 pin where applicable, the gcc and cmake versions, and the workflow file
hash. A cache hit skips the build while the pkg-config and runtime feature checks still run. The
SDL3_ttf build asserts that CMake selected vendored FreeType and HarfBuzz; a runtime probe prints
their versions and fails if either backend is unavailable. The job installs Mesa's Vulkan drivers, Vulkan tools,
the Khronos validation layer, Xvfb, and the X11/XTest development libraries needed by the pinned
SDL3 build. `vulkaninfo --summary` selects the lavapipe ICD and must list llvmpipe. The
`gpu-capture-selftest` runs through Xvfb with SDL's X11 driver and the validation layer. Its
terminal present-fault cases each get a fresh process, because the renderer must not reuse a
device after a terminal failure; an exit 77 skip fails the gate. A separate step runs
`gpu-coherence-selftest` the same way on lavapipe: the Vulkan backend's render-target coherence,
texture cache, per-draw state template, filtering and blend cases against the software GE, where
an exit 77 skip also fails. Its scaled-upload case then runs alone at render scales 2 and 4, where
every device pixel of an uploaded target must hold its guest pixel, and so does its mip-lod case,
where the shader's per-fragment mip level must not change with the render scale, and so does its
readback-rows case, where guest memory must get the top-left device pixel of each cell while the
rows it does not keep stay out of the staging buffer, then once more on the `SR_GPU_READBACK_FULL`
reference path, and so does its sprite-scaled case (also at scale 3), where a nearest 1:1
sprite must show the PSP's texel in every device pixel and every other sprite must sample at
each device pixel's own position, and so does its uv-edge-clamp case (also at scale 3), where the
device pixels outside a screen-aligned strip's or sprite's first and last pixel centres must show
that edge pixel's 1x sample, with linear and nearest filtering and with repeat and clamp wrapping,
while a sheared strip stays unclamped.
The dst-read cells (#2109: order, mask, alpha, mixed, depth, ctest, zrange, sfail, feedback, ztest and pins) run alone at render scales 1 and 4,
once forced to the snapshot destination-read path and once to the interlock path (`--dst-read`). On the
interlock path, overlapping shader-blend primitives must equal the software GE in colour with no snapshot copy,
and the depth-tested pair of `dst-read-ztest` must equal it in depth too. `dst-read-depth`, `dst-read-ctest`,
`dst-read-zrange` and `dst-read-sfail` write depth under a kill the early depth test cannot undo (the alpha test,
the color test, a narrow transform-mode depth range, a stencil-fail KEEP): on the interlock path their op must take
the interlock with depth tested and written in the shader and equal the software GE in colour and depth, an
overlapping pair, a primitive behind another in the same batch, a blended primitive under another depth test with
depth writes off, and an opaque sprite behind the rejected one included; on the snapshot path their depth must equal it, and `dst-read-sfail` its colour too (a stencil
batch that blends splits where its primitives overlap, a snapshot copy each). The cells run once more on the
interlock path with `--depth 32` (#2051): there those four keep the snapshot path, with no depth run in the
shader, because the shader's depth copy is 16-bit. `dst-read-feedback`, a stencil state that textures from
its own target, must keep the snapshot path on both paths: the destination copy is the texture its second sprite
samples. On
the snapshot path, the known difference must stay inside each overlap, and the device must hold no interlock
state. `stencil-once` runs alone too, at scales 1 and 4 on the snapshot path and at scale 1 on the interlock
path: a write-once stencil state (the first fragment to pass at a pixel stores a stencil that rejects the later
ones) must draw overlapping primitives in one batch with no split and equal the software GE bit for bit, over a
triangle mesh, a 7200-state matrix in each framebuffer format and three depth legs. The dst-read cells then run
with the host twin (#2050) on, native and full colour, on both paths at scales 1 and 4, and `stencil-once` with
the native twin on the interlock path: the image and the path taken must be those of the twin-off run, a native
twin must equal its image, and a full-colour twin of the overlapping pair must hold the blends at 8 bits per
channel, in drawing order on the interlock path. A device that refuses the
interlock prints one SKIP line for each interlock leg. The lavapipe step prints
the device's interlock features from `vulkaninfo` first, so the log shows which path the hosted legs took. The
pipeline cache (#2149) runs next, as three processes over one empty data root: cold, warm and reject. Four
mutant targets follow, and each broken variant must fail its case with its own message:
`gpu-coherence-selftest-snapbar-mutants` breaks the destination-snapshot copy's barriers three ways
(`SR_GPU_SNAPBAR_MUTANT`, the submit-batch case), `gpu-coherence-pcache-mutant` makes pipelines bypass the
pipeline cache (pipeline-cache-cold), and `gpu-dst-read-mutants` breaks the destination-read paths sixteen ways
(`SR_GPU_DSTREAD_MUTANT`; mutants 1 to 3 and 5 to 17 run on the interlock path and are a SKIP on a device without it,
the hosted lavapipe device included; 8 and 9 break the host twin's use of the interlock, 10 runs mutant 9 with the
full-colour twin, 3 sends all four kills back to the early depth test and 6, 11 and 12 one each, 13 makes the shader's depth
test always pass, 14 skips the copy of the shader's depth back to the depth image, and 15 keeps such an op on the
snapshot path as before the shader owned depth, 16 copies the twin's shader depth back into the image's, and 17
writes depth for a batch whose depth writes are off),
and `gpu-stencil-once-mutants` breaks the write-once stencil batch three ways (`SR_GPU_STENCIL_ONCE_MUTANT`: no
reordering, every state called write-once, no depth condition).
`fsr3-pass-selftest` runs FSR 3's eight compute passes on
lavapipe, headless, under the validation layer with synchronisation validation of shader accesses,
and an exit 77 skip fails it, including one for synchronisation validation that is not active.
`fsr3-fg-pass-selftest` runs frame generation's device refusal (G-R) there: lavapipe's subgroups
are too small for optical flow, so the pass must refuse it with one reason and no Vulkan call; the
frame cells need a GPU the pass accepts and run locally (`make fsr3-fg-pass-selftest`,
`make fsr3-fg-pass-mutants`). A further step downloads the DirectXShaderCompiler release that `tools/shader_embed.py` pins, checks its SHA-256 before use, and
recompiles every FSR 3 pass from the imported HLSL: any byte that differs from the embedded SPIR-V,
or a compiler that is not the pinned one, fails it. Then `ge-first-bad-draw-selftest` runs
`ge_replay --first-bad-draw` on a generated capture: the unchanged backend must report
`NO_DIFFERENCE`, and two executed mutants that change one draw's Vulkan blend or depth state
(`SR_GE_FBD_MUTANT`) must each be named as that draw, in linear and bisect mode. A second generated
capture holds one PRIM of two overlapping translucent sprites on a 16-bit target, and the tool must
find exactly its overlap as the first bad draw on the snapshot destination-read path (the known
shader-blend batch-order difference, #2109), and report `NO_DIFFERENCE` on the interlock path. On a
device that refuses the interlock, the interlock leg is a `SKIP`. Four more generated captures check that the
tool fails closed: a scissor past 512 columns (both renderers draw up to the stride, 512 at most;
a 512 scissor on a 512 stride is compared), 33 targets or a stride of
1088 are `INCOMPLETE`, and a capture with no draw is `NO_DRAWS`. The same step then runs
`vram-view-identity-selftest` (#314): the sources and decode the pause menu's VRAM viewer runs,
called at a paused VBLANK with the arguments `gui.c` passes, must name the expected surfaces and
leave later presented frames, guest memory and the GE state as a run without them does, and a
mutant viewer that stores one byte must fail. The job also runs
`make CC=gcc player-ui-tests` with SDL's dummy video and software render drivers, configures and
builds the CMake player with `BUILD_PLAYER=ON`, runs CTest, and requires the `nakagawa_player
--help` usage marker. It downloads the PSPDEV archive identified by
[`pspdev.lock.json`](../assets/upstream/pspdev.lock.json), checking its
SHA-256 before use. The job runs `make CC=gcc showcase-linux` to build and boot the two
source-owned showcase packages with dummy SDL video and audio drivers. A failed smoke uploads its
`runtime.log` files and frame captures as the `showcase-smoke-linux-attempt-<n>` artifact. After a
passing smoke, the job passes the generated
TEST00007 ISO to the CMake-built player's bounded `--stage-only` route and requires
`STAGING_RESULT status=PASS`. This is build, test, and headless staging evidence for the named
synthetic fixtures. Separately, the job runs `make CC=gcc player-ui-regressions`: scripted native
player UI event and recovery flows, which are UI evidence rather than staging evidence. The test
harness (`tests/native/test_player_ui.py`) forces SDL's dummy video and software render drivers for
every player it spawns, so the gate is headless without any step environment. The target runs the
suite as four parallel shards (`PLAYER_UI_SHARDS`, `NAKAGAWA_PLAYER_UI_SHARD`) that together run
every test exactly once; each test already uses its own temporary data roots. The CMake player is configured with CMake's default, unoptimized build type, so
the gate covers only that configuration: an optimized Release configuration fails under the global
`-Werror` on existing truncation warnings in `src/core/nk_launch.c`, a separate defect, and this
job does not claim that configuration builds. Consumer ISO compatibility on Linux is not supported
(#306); these checks do not establish PSP hardware acceptance or interactive desktop presentation.

### GCC 16 canary

The `linux_gcc16` job ("Linux GCC 16 canary") builds and runs the native core suite
(`native-core-tests`) and the headless display smoke (`display-smoke-run`) with a released
GCC 16.2 on Linux. It runs beside `native_tools`, which stays on `ubuntu-24.04` with GCC 13.3,
under the same condition (`run_native`). Windows already compiles with GCC 16.2: MSYS2 UCRT64
ships it and `assets/prereq_manifest.json` pins it. This job adds GCC 16 against glibc.

- **Image and packages.** No hosted Ubuntu image ships GCC 16, so the job runs in the
  `fedora:45` container, pinned by digest, on a hosted `ubuntu-26.04` runner. A container job
  needs Docker on the runner, so it does not use the self-hosted pool. `dnf --repo=fedora`
  installs from Fedora 45's GA repository only. That repository never changes after release,
  and the `updates` repository is never enabled.
  - Every package the job names is pinned to an exact version: `gcc-16.2.1-2.fc45.1` (Red Hat's
    GCC 16.2.1 20260819, the 16.2 release plus release-branch fixes), its `libasan` and
    `libubsan` runtimes, and the build and test tools.
  - Their dependencies come from the same frozen repository, and the image digest fixes what
    is preinstalled.
  - Fedora's SDL3 package (`SDL3-devel-3.4.18-1.fc45`) is deliberately not the pinned 3.4.20
    that `native_tools` builds from source. This job tests the compiler, and `native_tools`
    keeps the SDL pin.
  - The container runs with `--init`. Without it, PID 1 in a container job is
    `tail -f /dev/null`, which never reaps orphans. A process killed after its parent exits
    then stays a zombie that `kill(pid, 0)` still finds, and `test_posix_process`'s
    process-tree subtest fails although the tree was terminated. On a hosted runner, systemd
    reaps them.
  - The job sets `SR_SOURCE_COMMIT` to the checked-out revision, as `windows_runtime` does.
    It runs as root in the container and the checkout belongs to the runner's user, so git
    refuses the repository and the display smoke's flight step would have no build identity.
  - The job needs network access to the Docker registry and Fedora's mirrors. An
    unauthenticated Docker Hub pull limit or a mirror outage can fail it without any code
    change.
- **Not required.** `ci_required` does not need the job, so `CI required`, the one required
  context, never reads it. A failure shows red on this job and on the run without blocking a
  merge. The job has no `continue-on-error`, so a failure is never reported green.
  `tools/test_public_ci_wiring.py` (`Gcc16CanaryWiringTests`) pins all of this.
- **Moving the pins.** Pins move only in a reviewed pull request. Before a digest bump, rerun
  the job's steps locally against the new digest: the packages the image preinstalls must
  match the GA repository's development packages, or the install fails on an exact-version
  dependency. A newer GCC (16.3, or 17) means moving to the next Fedora release. Fedora 45 is
  supported until 2027-12-08.
- **Known GCC 16 warning.** `src/rt/atrac3p/libavutil/mem.c:272` gets `-Wdiscarded-qualifiers`
  (`end = memchr(s, 0, len)`): GCC 15 and later default to `-std=gnu23`, where glibc 2.43 and
  later declare the qualifier-preserving `memchr`. The file is a byte-identical FFmpeg import,
  and FFmpeg n9.0.2 has the same line. The FFmpeg update work decides it; this job does not
  silence it.

**Making it required.** All of the following must hold, and the owner decides:

1. At least 20 runs over at least 14 days, from any event, each green by the job's own
   conclusion, with none failed by the container rather than the code (an image pull, a
   mirror or dnf).
2. A median job time of at most 6 minutes, and a 95th percentile of at most 10 minutes.
3. A recorded decision on every warning only GCC 16 raises (today the `mem.c` line above).
4. Any failure only GCC 16 shows, found in that window, is fixed at its source, not waived.

Then one pull request adds `linux_gcc16` to `ci_required`'s `needs` and to
`tools/ci_required.py`, under `RUN_NATIVE`, and changes the wiring tests to pin it as required.
Branch protection does not change: `CI required` stays the one required context.

## Local readiness before opening a pull request

Discover the available build and verification surfaces first:

```bash
mingw32-make --no-print-directory help
```

For the public checkout's inner loop, run the public-safe composition before
the strict authority-bound gate:

```bash
mingw32-make --no-print-directory check
```

`check` covers documentation and policy checks, the provenance record and
disposition gap check, both publication-audit legs, the native host-core tests,
and a fast Python subset. It does not replace `make readiness`: readiness
additionally verifies the exact candidate against the external detailed ledger
and therefore remains `BLOCKED` when `NK_TRUSTED_LEDGER` is unavailable.
`make provenance-refresh` is the single local command for regenerating the
tracked public controls. It calls the same `generate_ephemeral_controls()`
implementation as the hosted provenance attestation, reads the trusted public
ledger from the exact base commit, and requires the external detailed ledger
through `NK_TRUSTED_LEDGER`. Stage the intended candidate changes first. The
target stages the generated controls and the profile when `--apply-policy` is
requested; it does not stage the rest of the worktree. The committed ledger
stays a per-path document (no `refresh` audit block), and the export is computed
from those generated ledger bytes in the same invocation, so a second pass is
not needed. For contributors without access to the private trusted ledger,
`python tools/contrib_refresh.py` allows refreshing both public controls for
changed existing public paths from the public controls alone (PR #849).

The `provenance_record_gap.py --check` gate (`make provenance-record-gap-check`)
enforces that every upstream-derived production code path (under `src/` or
`tools/`, excluding `.md`/`.txt` notice and format documents) has an explicit
disposition in [`docs/INDEPENDENCE_CAMPAIGN.md`](INDEPENDENCE_CAMPAIGN.md). Paths with
`status=missing` fail closed. A reviewed baseline (`KNOWN_DISPOSITION_GAPS`)
allows existing gaps to shrink but never grow; stale baseline entries fail
closed. The check is wired into `make check`, `make readiness`, pre-commit, and
hosted CI (`native_tools` and the main smoke steps of `hygiene`).

The base defaults to `merge-base(HEAD, origin/main)`. When the pull request's
exact base differs, set `PROVENANCE_BASE_SHA` to its full 40-character commit
SHA before running `mingw32-make provenance-refresh`. A missing detailed
authority or base ledger is a named failure; the command does not construct
either trusted input. For a changed publication profile the command finds the
V4 authority named after the change beside the trusted ledger; set
`PROVENANCE_POLICY_DELTA_AUTHORITY` only for a V3 authority, or for a V1/V2
record together with `PROVENANCE_TRUSTED_CANDIDATE_POLICY`, each naming an
external reviewed input.

Run the strict aggregate, then the Python modules for the files you changed. Treat any
failure as blocking. Hosted CI runs the full Python suite; run it locally only when
hosted CI cannot run for the change ([what hosted CI does not run](#what-hosted-ci-does-not-run)):

```bash
NK_TRUSTED_LEDGER=<external detailed ledger> make readiness
```

`make readiness` runs the publication/policy checks in cheapest-first order and stops
at the first failure. Prefer it to assembling the checklist by hand.

### What `readiness` runs, if you need a step alone

```bash
python tools/policy_sync.py
python tools/lint_docs.py
python tools/provenance_record_gap.py --check
python tools/publish_audit.py --tracked-only --public-scope --provenance-self-consistency
python tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
python tools/provenance_attest_verify.py --repo . --candidate <exact HEAD sha> --base <exact BASE sha>     --require-immutable-revisions --trusted-ledger <external detailed ledger>     --workdir <scratch outside the repo>
git diff --check <exact BASE sha>..HEAD
```

Pass **exact commit SHAs**, not `origin/main`. A moving ref stops naming the
branch point as soon as it advances, and the hosted job passes the immutable
SHAs from the pull-request event for exactly that reason.

### publish_audit passing is not the attestation gate passing

These two are routinely confused, and the confusion is the single most common
reason a branch looks finished and is not:

| Gate | Compares against | Can detect an unapproved path or content change |
| --- | --- | --- |
| `publish_audit` | the candidate's **own checked-in ledger** | **no** — the candidate supplies both sides |
| `provenance_attest_verify` | the **external private authority** | yes; normal mode checks path authority and content binding; strict mode also checks legacy blob approvals |

A tree can report `publication audit: OK (<N> tracked files)` and
`verdict: FAIL (<N> fatal findings)` at the same commit. "Gates green" that means
only the first is not evidence that the branch can merge.

The attestation verifier is also the one most easily forgotten. Existing
implementation paths remain authorized across ordinary revisions; automated
content binding and CI validate each new commit without a second private blob
approval. A genuinely new implementation path still needs exact external path
authority, and `--require-reviewed-blobs` remains available for high-assurance
legacy runs. See [`docs/PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) for
the authority boundaries and what each class needs.

### Regenerating every generated file at once

Several committed files are generator output, and a test or gate fails when the
committed copy differs from a fresh generation. Run one command instead of the
generator each failure names:

```bash
python tools/regen_all.py            # rewrite and stage the stale files
python tools/regen_all.py --check    # exit 1 if any is stale; write nothing
python tools/regen_all.py --list     # the steps, their outputs and what is skipped
```

Every staleness check prints `python tools/regen_all.py` as its fix. The steps
run in a fixed order:

| Step | Generator | Committed output |
| --- | --- | --- |
| recomp helper inventory | `tools/recomp_helper_inventory.py` | `docs/RECOMP_HELPER_INVENTORY.md` |
| NID name table | `tools/gen_nidnames.py` | `src/rt/nid_names.h` |
| HLE baseline and census block | `tools/hle_manifest.py` | `tools/import_audit_baseline.json`, the generated block in `docs/HLE_AND_WORKAROUND_INVENTORY.md` |
| waits census | `tools/waits_census.py` | `docs/PSP_INTR_WAITS_CENSUS.md` |
| public title catalog | `tools/title_catalog_codegen.py` | `src/core/generated/nk_title_catalog.{h,c}` |
| PGF golden vectors | `tools/test_pgf_validate_parity.py --regenerate` | `tests/native/pgf_golden_vectors.h` |
| provenance controls (last) | `tools/contrib_refresh.py` | `assets/public_provenance_ledger.json`, `PUBLIC_EXPORT.json` |

The provenance refresh runs last because it reads the staged tree, so the tool
stages the files it regenerated before it runs. The committed ledger carries
claims only (#1673), so for a change to existing files the refresh normally
changes nothing. Stage your own changes
first (`git add`); with unstaged edits the refresh refuses with
`CANDIDATE_INDEX_STALE`. It also refuses a new or removed public path and a
changed publication profile. Those need maintainer authority, so the tool
reports them as `SKIP` and points at
[`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path).

The tool never refreshes `tools/import_audit_baseline.json` over a coverage
downgrade (a dedicated or controlled-unsupported NID becoming fake-success or
missing). It fails and names the NIDs; fix the handler, or review the downgrade
and run `python tools/hle_manifest.py --write-baseline` yourself.

Some generators are listed but not run, and `--list` says why:

- embedded shaders (`make shaders GLSLC=...`): regenerating needs `glslc`, whose
  version the manifest records; `make shader-verify` checks the committed files;
- profile-zero prebuilt digests: rebuilding needs the pinned PSPDEV toolchain;
  CI checks `SHA256SUMS`;
- the import-stub census and the benchmark report: both need private inputs
  (decrypted title images, the local telemetry database), and neither output is
  committed.

The hand-kept lists the tests pin, such as the fake-success exceptions in
`tools/test_hle_manifest.py`, are review decisions, not generator output, so the
tool does not touch them.

### The generated control files must be in the commit

Adding or removing a tracked file makes two generated files stale:
`assets/public_source_profile.json` (classification) and
`assets/public_provenance_ledger.json` (the path's claim). Changing an existing file
makes neither stale: the committed ledger carries claims, not content hashes (#1673).
`PUBLIC_EXPORT.json` goes stale only when the profile's name, version or exclusion
lists change: its committed control omits the tree-derived digests and the policy
digest, which are recomputed at verification, so admitting a path leaves it
byte-identical and two path-adding pull requests do not conflict on it. Regenerating
them in the working tree is not enough — they have to be **staged and committed**
with the change they describe (a refresh after merging `main` is its own commit; see
`REVIEW.md`), or the commit ships a new path without the evidence for it and fails
with `POLICY_UNCLASSIFIED`, `UNRESOLVED_PUBLIC` and `PROVENANCE_MISSING` together.

`make readiness` re-runs the export generator at the end and fails if that
changes anything, which catches precisely this.

These checks have no local equivalent and are never implied by a local pass:
CodeQL, `dependency-review`, the Betterleaks history scan, the Windows runtime
compile gate, and the main integration smoke.

## Text, encoding, and line-ending contract

The repository's text bytes are **UTF-8 without BOM, LF, with a final newline**.
`.gitattributes` (`* text=auto eol=lf`) and `.editorconfig`
(`charset = utf-8`, `end_of_line = lf`, `insert_final_newline = true`) are the
authority; this section says how to *produce* bytes that satisfy them.

Git normalises on commit, so the index is clean by construction — a scan of all
tracked files finds no CRLF, no BOM and no UTF-16. The damage happens
elsewhere: in working trees and in generated artifacts. A checkout polluted with
CRLF makes every touched file's content hash disagree with the provenance
ledger, and the resulting wall of `PROVENANCE_CONTENT_MISMATCH` names the
symptom rather than the cause. That is why `publish_audit` now reports
`TEXT_LINE_ENDING_CRLF`, `TEXT_ENCODING_BOM`, `TEXT_ENCODING_UTF16` and
`TEXT_FINAL_NEWLINE` directly, skipping anything binary.

**Python.** Text mode translates `
` to the host newline unless told otherwise,
so on Windows `write_text(s, encoding="utf-8")` silently emits CRLF and a
different SHA-256 than the same code on Linux. Every writer that produces
canonical, tracked, or hash-participating text must pin it:

```python
path.write_text(text, encoding="utf-8", newline="
")   # canonical text
path.write_bytes(canonical_bytes)                        # already-canonical bytes
```

`CanonicalWriterTests` asserts this for the canonical writers, so the rule
cannot quietly regress.

**PowerShell.** Use PowerShell 7.6+, which the project already requires: in
Windows PowerShell 5.1 `-Encoding utf8` means UTF-8 **with** BOM, and in 7+ it
means without. `Set-Content` and `Out-File` also join with the host newline. For
anything byte-exact, bypass the text pipeline entirely:

```powershell
[System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllBytes($path, $bytes)
```

**Never route exact bytes through a text pipeline.** `git show ... | Set-Content`
and `<binary> | Out-File` re-encode and re-line-end their input. To extract exact
repository bytes use `git archive`, `git cat-file`, or a redirect from a
binary-safe shell. No tracked script does this today; keep it that way.

**Bash and WSL** must likewise preserve LF and must not rewrite bytes
incidentally — a heredoc that reflows content is a rewrite.

**Generated evidence outside the worktree** follows the same contract whenever it
participates in a SHA-256 manifest or a forensic byte comparison, for the obvious
reason: a manifest that changes with the shell that produced it proves nothing.
Historical raw or binary capture evidence is never normalised.

The invariant, stated once: **the same logical generated text must produce the
same bytes and the same SHA-256 whether it came from PowerShell, Windows Python,
Bash, or WSL.**

## Python test sharding

`python_tools` runs `tools/test_*.py` on parallel runners. The module lists come
from [`tools/ci_test_shards.py`](../tools/ci_test_shards.py), not from the file
names: it packs modules longest-first onto the least-loaded shard (ties broken by
name and then shard index) using the measured per-module seconds in
[`tools/ci_test_weights.json`](../tools/ci_test_weights.json). A module without a
measurement uses the file's `default_seconds`, so a new test needs no data edit to
run. Modules in one `separate` group never share a shard; `test_title_catalog` and
`test_publication_policy_gate` deliberately mutate tracked repository state and
form such a group. The plan depends only on the tracked module names and the data
file, so every runner computes the same partition independently.

`tools/test_ci_test_shards.py` runs the planner exactly as the workflow does and
proves that every `tools/test_*.py` module is scheduled on exactly one shard, that
the separated modules stay apart, and that the workflow captures the planner's
output before using it (a failing planner must stop the step instead of handing
`unittest` an empty module list, which would fall back to discovery). Refresh the
weights when the suite's cost moves materially, or when the shards' step times drift
apart. The shard step runs `unittest -v`, and every hosted log line carries a
timestamp, so each module's time on the hosted runner, under coverage, is the time
from its first test line to the next module's first test line (a shard's last module
ends at unittest's `Ran N tests` line). The current weights are the per-module medians
of five landing-batch runs on 2026-11-10 (runs 42735543679, 42740278379, 42752233270,
42756075084 and 42806248089), because one hosted runner can be half again as slow as
another: `test_production_smoke` took 10 to 17.5 minutes across them. Those runs used three
shards planned from run 42270920833's weights (2026-11-06); every shard planned to 945
seconds, but the suite had grown to about 58 minutes, and the slowest shard took 21 to 24
minutes. The suite now runs on four shards: `test_production_smoke` alone, about 16
minutes, and three of about 14 minutes each, for the full suite and the Makefile-coupled
subset alike. That is the fewest shards at which the heaviest module sets the job, the rule
`tools/test_ci_test_shards.py` holds both counts to; see [Jobs per run](#jobs-per-run).
The job limit stays at 60 minutes: on the busy self-hosted pool a step that planned to
about 7 minutes took 17-20 minutes on 2026-11-01.
`python tools/ci_test_shards.py --check` rejects weights for modules that no longer
exist, and `--plan` prints the resulting partition with per-shard estimates.

### Python test tiers

When the classifier routes the Python suite, every PR runs all discovered modules
in the PR tier. Only cases marked `NIGHTLY_TIER:` are deferred: the composite
public-export gate report, the profile-zero package/runtime route, the slow
make-built platform-ladder runtime checks, the full-history report generation,
and the matrix inventory report. The profile-zero route still runs on every routed
PR, through its own make gate (`make profile-zero-e2e` sets `NK_TEST_TIER=full`)
in `windows_runtime` and `native_tools` below. Their constituent legal-blocker, publication,
fail-closed NID/dispatch, cosimulation, provenance, import-audit, and security
gates remain in their normal PR jobs and tests. `python_tools` includes a
one-line summary of skips by `NIGHTLY_TIER`, `WINDOWS_ONLY`, and `PRIVATE_INPUT`.

The nightly `schedule` and manual `workflow_dispatch` both force the full CI
matrix and set `NK_TEST_TIER=full`, so every nightly case runs. The normal local
command uses the PR tier; set the environment variable to run the full tier:

```bash
NK_TEST_TIER=full python -m unittest discover -s tools -p "test_*.py"
```

PowerShell:

```powershell
$env:NK_TEST_TIER = "full"
python -m unittest discover -s tools -p "test_*.py"
Remove-Item Env:NK_TEST_TIER
```

### What hosted CI does not run

Hosted CI runs the Python suite on Linux (`python_tools`) when `tools/ci_paths.py` routes
Python: the full suite for `python_scope` `all`, and only the Makefile-coupled modules when
a recipe-only `Makefile` change is the only Python-routing change (`python_scope`
`makefile`). An exact head whose required hosted CI is green, including `python_tools` when
the change routes Python, needs no local full run; a change that routes no Python needs none
either.

No hosted job runs the Windows-only cases in `tools/test_asset_index_c.py`,
`tools/test_scratch_dir.py`, `tools/test_player_package_route.py`,
`tools/test_manager_safety.py`, `tools/test_visual_oracle.py`, `tools/test_library_sweep.py`
and `tools/test_stage_install.py`. A change under `tools/`, `src/core/`, `src/player/`, the
`Makefile` or any `.ps1` file (including the root `nk_manager.ps1`) runs those modules locally
on Windows. The full local suite builds the native fixtures and takes over an hour on
Windows. Make-based tests need a POSIX `sh` on `PATH` (MSYS2 `usr/bin` or Git for Windows
`bin`).

`make dmac-mutants` is also left to local runs: it builds `hle-thread-selftest` against each mutated copy
of `src/rt/hle_cache_dma.c`, one selftest build per mutant (`NK_DMAC_MUTANTS_ONLY=<part of a name>` runs one).

## Coverage

Python coverage is reported as XML and C coverage is collected from raw gcov data
whenever their jobs run; neither can change a gate's result.

- **Python tooling (`python_tools`).** Each shard runs the module list it always
  ran, through `python -m coverage run --parallel-mode --source=tools -m unittest
  -v ...`, then combines its own data and writes `coverage-python.xml`. The
  measured surface is the tooling, not the tests: `--source=tools` and
  `omit = ["tools/test_*.py"]` in `[tool.coverage.run]` (`pyproject.toml`) keep
  the test modules themselves out of the number. Coverage records the run and
  exits with unittest's own status, so a failing shard still fails the job.
- **C runtime (`c_coverage`).** A job of its own, beside `native_tools`, builds and runs the
  self-tests `native_tools` runs on Linux, from `native-core-tests` to the GPU self-tests on
  lavapipe, a second time with gcov instrumentation (#2394).
  It runs only on the nightly schedule and on a manual run (`workflow_dispatch`), under
  `native_tools`' own condition (`run_native`), on a hosted `ubuntu-24.04` runner and never on
  the self-hosted pool, so it cannot take a runner from a gate. `ci_required` neither needs nor
  reads it: a failure shows red on this job and on the run without blocking anything. It stays
  off pull requests, landing batches and merge groups because the
  [Trusted CI verdict](#trusted-ci-verdict) is computed when the whole CI run completes, and
  this job outlasts the required jobs: in run 43094169097 it took 24.6 minutes and the last
  required job ended after 18.0, so on every pull request it would hold the verdict back
  (figures below). Between measured commits Codecov carries
  the `c` flag forward (`carryforward: true`), so a pull request's report shows the last
  nightly figure. `native_tools`' gates run unprofiled and no longer wait for a measurement. Its
  setup steps are `native_tools`' own, byte for byte (packages through `NK_APT_INSTALL`, the
  pinned SDL3 and SDL3_ttf, PSPDEV), so the two jobs share their caches.

  After the setup, wrappers named `gcc` and `g++` go first on `PATH`: each runs the real
  compiler with `--coverage` added. The measuring steps then run the gates' commands
  (`make CC=gcc native-core-tests`, and so on; only an isolated build root or directory moves
  under `build/`), and every compile and link is instrumented, including the targets that
  hard-code their own compile flags and the C++ reference interpreter. Before #2394 the C number came
  from one extra build of `native-core-tests` in `native_tools`' core part, with `--coverage`
  appended to the Makefile's `CFLAGS` and `LDFLAGS`, which measured only the targets that
  compile through those variables. The setup builds the pinned libraries with the real
  compilers, before the wrappers exist, so they stay out of the number.

  The collection step runs `gcov -p -b -r` once per `*.gcno` notes file under `build/`, from
  the repository root, where the objects' recorded `src/...` source paths resolve. The
  action's own `gcov` plugin is not used (`plugins: noop`): it runs gcov from the build
  directory, cannot open any source there, and uploads header-only reports that Codecov
  rejects. One run per notes file keeps a source shared by several self-tests from getting
  duplicate line records, and a notes file whose program never ran still reports its lines,
  as not executed. Notes files of the mutants built inside measured targets (every path with
  `mutant` in it) are skipped: lines only a mutant define compiles would otherwise count as
  covered. A mutant that links a shared, unmutated object still adds its run to that object's
  counts: the GPU coherence and first-bad-draw mutants link `ge.o`, so their runs count in
  `src/rt/ge.c`. `-r` keeps system headers out, and reports for the tests' own sources (`tests/`,
  `fixtures/`, `*selftest.c`, `*selftest.cpp`, `*selftest.inc`, `src/player/player_ui_test.c`) and for C
  generated into `build/` are dropped, as `tools/test_*.py` is for Python. Everything is
  uploaded once, under `flags: c`. A failure in any earlier step skips the upload, so a
  partial run uploads nothing and the `c` flag carries the last complete report forward.

  Measured, with the command `native_tools` runs for each:

  - `native-core-tests`: the CPU self-tests, Ace Mode's among them (`ace-selftest`,
    `ace-ui-selftest`, `ace-smooth-selftest`), the player state and input settings tests and
    the player screens self-test (`nk-ui-player-screens-selftest`).
  - `psmf-media-selftest` with the vendored FFmpeg H.264 subset (`H264_FFMPEG=1`).
  - The HLE thread and title self-tests, the AOT return-boundary self-test, the scheduler
    matrix (`sched-selftest`), dispatch isolation, cosimulation, snapshot execution and the
    verify self-test.
  - The self-tests `native_tools` compiles directly instead of through make, their steps
    copied byte for byte: the C++ interpreter unit tests, the FPU/VFPU conversion, VFPU table
    loader, watchpoints-file parser, strbuf and VFPU interpreter self-tests, and the ATRAC3+
    decoder and bridge self-tests, UBSan builds included.
  - `gui-present-selftest`, the player UI regressions and the player UI tests (overlays,
    fonts, art).
  - On lavapipe under Xvfb, as in `native_tools`: the GPU snapshot sync, capture, coherence,
    VRAM view identity and texture scaling self-tests, the FSR 3 pass self-test and the FSR 3
    frame generation refusal (cell `G-R`). As in `native_tools`, the step fails when the
  device is not llvmpipe or the texture scaling self-test prints a `SKIP`, so a skip cannot
  shrink the number unseen.

  Not measured, each `native_tools` step named in `tools/test_public_ci_wiring.py`
  (`CoverageWiringTests.C_NOT_MEASURED`), so a gate added later fails that test until it is
  measured or named:

  - Mutants: the separate mutant steps (`gpu-texscale-mutant`, the cosimulation mutation
    corpus) are left out, and the objects the mutants inside measured targets compile for
    themselves are skipped at collection. They run deliberately broken code. Their runs still
    count in the unmutated objects they link.
  - Not C self-tests: the import and fake-success audit, the provenance gap check, the 32-bit
    format portability build (syntax only), the portable-object and GE profiling compiles, and
    the FSR 3 shader reproduction.
  - End-to-end routes: the CMake player build, test and smokes, the showcase, the installed
    player smoke, the profile-zero route, and the codegen gates (run_elf, the MIPS
    microtests, their golden traces and the framebuffer parity checks).
  - The nightly selftest repeat, which reruns binaries already measured.

  The Windows-only self-tests run in `windows_runtime` and are not measured. Self-tests that
  need a real GPU (the FSR 3 frame generation cells beyond `G-R`) run in no hosted job.

**Figures before and after #2394.** Codecov's totals per flag, from real runs:

| Flag | Before: `main` at `8257cfc27` (run 43078241056) | After: `0889c5d90` (manual run 43094169097) |
| --- | --- | --- |
| Combined | 50.35 % of 141,165 lines in 355 files | 61.82 % of 234,377 lines in 597 files |
| `python` | 80.59 % of 76,104 lines in 194 files | 80.62 % of 76,108 lines in 194 files |
| `c` | 14.98 % of 65,061 lines in 161 files | 52.79 % of 158,269 lines in 403 files |
| `c`, `src/` only | 15.48 % of 61,555 lines | 59.11 % of 131,899 lines |

- **More lines, not only more hits.** The `c` flag now counts every file the self-tests
  compile, so files no target built under `CFLAGS` before (the player, `src/core`, and the
  vendored libraries the player tests link) enter the denominator. Vendored code under
  `third_party/` is 25,875 of the lines, at 19.69 % (libchdr, rcheevos and dr_libs are linked
  whole and exercised in part). The `src/` row leaves it out, and is the figure for the
  project's own code.
- **One correction after the run.** Run 43094169097 still counted `src/ref/selftest.cpp`
  (495 lines at 98.99 %), the C++ interpreter's test source, which the filter now drops. Run
  43102622567 on `301ccf224`, without it, measured `c` at 52.64 % of 157,773 lines in 402
  files and combined at 61.74 % of 233,881 lines in 596 files; the `src/` row already leaves
  it out.

**Wall time.** `native_tools`' core part took 12 min 33 s in run 43078241056, 4 min 46 s of
it the coverage build. Without that build it took 6 min 15 s in run 43089041907 and 8 min 10 s
in run 43094169097. `c_coverage` took 24 min 35 s in run 43094169097:

- 2 min 30 s of setup, 2 min 3 s of it rebuilding SDL3 and SDL3_ttf, because a changed
  workflow file misses their cache;
- 21 min 32 s measuring (native core suite 6 min 36 s, GPU self-tests 7 min 3 s);
- 16 s for gcov collection and 14 s for the upload.

It took 19 min 42 s in run 43102622567, on the head that added these figures.

Both reports are uploaded with `codecov/codecov-action` (pinned by commit SHA)
under `flags: python` and `flags: c`, authenticated with the job's OIDC identity
(`use_oidc: true`, which is why `python_tools` and `c_coverage` -- and only those two -- declare
`id-token: write` beside `contents: read`). `fail_ci_if_error: false` keeps an
upload or Codecov outage from failing the job.

**The numbers are informational.** [`.github/codecov.yml`](../.github/codecov.yml) marks the
project and patch status checks `informational: true`, so neither can block a
merge, and sets `carryforward: true` for both flags, so a run that measures only
one surface keeps the other flag's last report. It also sets `comment: false`: the
two status checks carry the numbers and link to the full report, so Codecov posts
no pull request comment. The OpenSSF Best Practices
Silver criterion "statement coverage >= 80%" is the target these numbers are
watched against; it is not a gate, and promoting either status to a required
check is a separate maintainer decision. Python's `coverage` package is installed
from `tools/requirements-lock.txt` with `--require-hashes`; the C reports come
from the runner's own `gcov` and add no Python dependency. A change to `.github/codecov.yml` matches no classifier
predicate, so it fails closed to the full matrix like any other unknown path.

## Classifier invariants

`tools/ci_paths.py` decides which gates run. The only failure that matters is a
**false negative** — a build-affecting change classified as documentation or
tooling and therefore skipping a native or Windows gate. These invariants exist
to prevent that, and `tools/test_ci_paths.py` asserts each one:

- **Unknown paths fail closed.** Any path matching no predicate forces the full
  matrix. Adding a new kind of file makes CI more expensive, never less. The one
  exception is a push to `main`: it lands a tree whose pull request (or batch) already
  ran that full matrix because of the same path, so the push keeps its compact run.
  Only a changed CI definition (`_is_workflow_ci`) or a change set that cannot be read
  (no paths, or `<history-unavailable>`) re-runs the matrix on a main push. Here a CI definition
  means exactly these paths: anything under `.github/workflows/` or `.github/actions/`, and
  `tools/ci_paths.py`, `tools/ci_required.py`, `tools/ci_test_shards.py` and their
  `tools/test_` modules (`test_ci_paths.py`, `test_ci_required.py`, `test_ci_test_shards.py`).
  Other files that configure CI, such as `tools/requirements-lock.txt`, are not in that set. Unknown paths
  made 32 of 79 main pushes repeat the matrix from 2026-11-02 to 2026-11-07 (and 28 of 77
  from 2026-10-31 to 2026-11-05, when three of those 28 failed: two on a tree that had
  passed on its batch head, the third with no same-tree pull request run in the sampled
  window). The nightly schedule still runs the full matrix on `main`.
- **Every change type counts.** The changed-file query uses name-status without
  a narrowing diff filter, and retains both sides of a rename. Filtering to
  `ACMR` or keeping only a rename's new name can drop a build-affecting source,
  making a commit that removes or renames C code classify as docs-only and skip
  the native and Windows compile gates.
- **An empty or unobtainable file list forces the full matrix**, so a shallow
  clone or an unusual event payload cannot quietly narrow the run.
- **A merge-queue group is never path-gated.** `FULL_MATRIX_EVENTS` in
  `tools/ci_paths.py` (`workflow_dispatch`, `merge_group`, and `schedule`) forces
  the complete matrix, including the main smoke, and allows substantive work. A group's change
  set is its payload range (`merge_group.base_sha` to `head_sha`), and a payload
  without that range fails closed to the full matrix. The classifier output for a
  group is identical to a manual dispatch for the same paths, which
  `MergeGroupRoutingTests` pins.
- **A Draft runs only the quick jobs.** For a `pull_request` event whose payload
  says `draft: true`, the classifier exports `allow_substantive=false` and
  `draft=true`; every substantive job gates on `allow_substantive`, and `CI required`
  fails on `draft=true` (`DRAFT_LIGHT_CI`). The path flags do not change, so a Draft
  and its Ready run route the same gates (`DraftLightRoutingTests`). No other event
  can be a Draft: merge groups, the schedule, manual dispatches and pushes classify
  as before. `.github/workflows/ci.yml` lists `ready_for_review` and
  `converted_to_draft` beside the head-changing events (`opened`, `synchronize`,
  `reopened`), so each transition starts the run it needs.
- **`hygiene` is ungated, and that is load-bearing.** The all-files pre-commit
  run — which includes `publish_audit --provenance-self-consistency`,
  `policy_sync`, and the Betterleaks scan — executes on every event, so the
  security and publication boundary is never path-gated. This is what makes the
  cheap paths safe: every tracked file is inside the published surface and needs
  a publication-policy decision and a provenance claim, so a change that adds,
  removes or reclassifies a path fails the audit here until its controls are
  regenerated, whatever paths it touches. A file's bytes are bound by the trusted
  provenance attestation, which regenerates every hash from the private
  authority (#1673). A docs-only change may therefore skip the Python suite only
  because the audit still runs here.
  `PublicationCoverageInvariantTests` pins both halves so this cannot regress
  into a path-gated audit.
- **Only a structurally proven recipe-only `Makefile` edit narrows the build
  route.** `makefile_recipe_only_targets` parses the base and head `Makefile` and
  accepts the change only when every changed line is a comment, a blank line, or a
  recipe line of an explicit rule whose targets are all literal `.PHONY` names, and
  every unchanged line keeps its kind and owning targets. A variable, flag,
  prerequisite, rule header, pattern rule, file-producing recipe, conditional,
  include or `.PHONY` edit keeps `build_system`, as does a `Makefile` that either
  revision cannot provide. A recipe-only edit still runs the native and Windows
  gates, which execute recipes. Python runs only the modules whose transitive tool
  closure reads the `Makefile` or runs `make` (`ci_test_shards.py --select
  makefile`), on four shards; any other Python-routing change in the same diff
  restores the full suite, also on four. That closure covers most of the suite's
  weight, because most heavy modules build through `make`, so the saving is the
  unrelated modules rather than the Python job as a whole or a runner.
- **The published surface is derived, not listed.** `_is_public_surface` asks the
  publication policy instead of maintaining a second list that can drift; it
  fails closed to "published" when the policy cannot be read. The `public_surface`
  output is exported so a local readiness check can route the same decision,
  where no ungated hygiene equivalent runs.

Test modules use their logical implementation subject for classification:
`tools/test_<subject>.py` is evaluated through the same subsystem predicates as
`tools/<subject>.py`. This keeps build-relevant HST, title, codegen, and native
tool tests on the native and Windows gates without making every Python test
expensive. A new native-relevant tool should therefore be named and classified
like its implementation; add a predicate only when the implementation itself
belongs to a new subsystem.

## Cost and caching rules

GitHub-hosted Windows time is billed at a higher multiplier than Linux time. The
workflow therefore starts the Windows runner only after the cheaper Linux classify
and public-export jobs (the second also builds the fixture), cancels superseded PR runs, and avoids repeating the full matrix on
ordinary main pushes. Besides dependency and tool caches (pip, the SDL3 installs),
the only cache is the Windows compiler cache below. Build trees and generated
shader/code output are not cached across jobs or runs, so the repository's
content-addressed invalidation and freshness checks remain the source of truth.
No volatile dollar figure is part of the repository contract.

Every `windows_runtime` compile goes through `ccache` (#735 item 2). Inside one group it serves repeated compiles of the
same sources (the platform ladder builds the runtime once per rung, the smoke part
once per smoke). Between runs it is persisted with the Actions cache:

- Each matrix group restores its own entry. The key names the group, the toolchain (the
  installed package versions and `gcc -v`), the flags (`Makefile` and `mk/`) and
  the exact hashes of every file a compile can read or be generated from. Without
  an exact match it restores the newest entry for the same group and toolchain,
  preferring one with the same flags.
- ccache keys each object on the compiler's contents (`compiler_check=content`,
  since every run installs the toolchain afresh), the full command line and the
  contents of every header. A restored entry is used only for an identical
  compile, and Make still decides what to build.
- Only a run of `main` on `main` saves it: a push, the nightly schedule or a manual
  dispatch (`save_compiler_cache` from `tools/ci_paths.py`). Pull requests and merge
  groups restore it and never write it. A push runs the Windows gates only when it
  changes a CI definition (see [Classifier invariants](#classifier-invariants)), so most
  saves come from the nightly run; in ladder parts sampled on 2026-11-05, hit rates from
  92% to 100% made no measurable difference to the step time. Before a save, entries
  the run did not use are evicted, so the saved cache holds exactly that run's compiles.

`gcc` is a native symlink to the UCRT64 toolchain's `ccache.exe`, so `CC` stays
`gcc`, and SDL3 discovery and the toolchain check (`tools/nk_doctor_checks.py`)
follow the link into `/ucrt64/bin`. Each group's last step prints the cache
statistics, including which saved entry was restored, and fails if no compile
went through the cache.

Hosted GitHub Actions execution is active. The `main` ruleset requires `Trusted CI verdict`
and `CodeQL` on exact pull-request heads (since 2026-10-29, #373). Path-gated workflows also run the applicable
classifier, Markdown and main-smoke steps (in hygiene), native/translation, Python, and
Windows gates. A green public-safe run proves only the paths it executes; it is
not a complete private-title gameplay route, and local verification remains
local-only.

`CI required`, `Hygiene and security`, `OSV Vulnerability Scan` and `dependency-review` still
run on every pull request, but they come from `pull_request` workflows whose definitions the
pull request itself controls, so the ruleset no longer requires them by name.
[Trusted CI verdict](#trusted-ci-verdict) judges each of them by its producer at the exact
head, and that verdict is what the ruleset requires.

The required `CodeQL` check is the code-scanning result GitHub reports after it
processes the SARIF that `.github/workflows/codeql.yml` ("Trusted CodeQL")
uploads; no upload means no check. GitHub runs that workflow from the default
branch: a `workflow_run` after the `CodeQL wake` workflow
(`.github/workflows/codeql-wake.yml`) completes on a pull request (forks
included) or on a merge-queue group, a push to `main`, a weekly schedule, or a
manual dispatch naming a pull request. The wake run starts with the pull request
head or the group commit, so the trusted
analysis runs alongside CI rather than after the whole CI workflow finishes. CI
itself is not a trigger: the analysis does not wait for the slowest CI job, and
CI's result never reaches the trusted run.

The wake run is a signal only. It is a `pull_request` run, or a `merge_group` run for
a merge-queue group (candidate-controlled, like every pull request workflow), declares
`permissions: {}`, checks nothing out,
runs no repository code, and has no secrets for fork pull requests. Its result is
never evidence: the trusted workflow resolves the exact pull request from GitHub's
API and refuses any head that no longer matches. A pull request can decide whether
its own wake run exists, and so cause an analysis of its own exact head or none at
all (no `CodeQL` check, so the merge stays blocked). It cannot make the trusted run
analyse another commit or repository, run its own code in that context, or supply
the required check's evidence. Its trusted-run concurrency group is the head
repository and branch, so a newer head of a branch cancels the analysis of the
older one; repositories never share a group, so forks cannot cancel one another.
The wake workflow's name is part of the trust path: `codeql.yml` names it in
`workflows:`, and `tools/test_public_ci_wiring.py` pins that the two agree.

A pull request whose head predates `codeql-wake.yml` has no wake run, so it
receives no `CodeQL` check until it merges `main` (which carries the wake workflow).

The trusted workflow never checks out or executes candidate files. For a pull request it reads the
exact merge tree as blobs and binds the upload to the merge ref; for a push or
schedule it scans the `main` commit. For a merge-queue group it reads the group
commit's tree as blobs and binds the upload to the queue ref and the group commit
(see [Merge-queue groups](#merge-queue-groups)). It analyzes `actions`, `c-cpp` and `python`
without uploading partial results and uploads all three in one SARIF. Each run's
`automationDetails.id` is `/language:<lang>/`: code scanning stores a run under the
part of the id before its last `/`, so the trailing slash keeps the three languages
in separate categories instead of letting one replace the others. TRAP caching is off:
a run that restored the cache was observed to extract no C/C++ file at all. An edit to this
workflow reaches pull requests only after it lands on `main`, or through a manual
dispatch run with `--ref` naming the branch.

## Merge-queue groups

A merge queue lands each reviewed pull request on a *group commit*: the trunk, every
queued pull request ahead of it, and this one. Each group is a ref in this repository
(`refs/heads/gh-readonly-queue/main/pr-<n>-<sha>`), and the ruleset's required contexts must pass
before `main` moves. The ruleset requires only `Trusted CI verdict` and `CodeQL` (see above).
`ci-trust-verdict.yml` has no `merge_group` trigger, so a group gets no verdict yet
([Trusted CI verdict](#trusted-ci-verdict)). The table lists the checks the verdict judges on a pull
request and what each does on a group commit. These workflows carry a `merge_group` trigger
(`checks_requested`). `codeql.yml` does not: CodeQL on a group comes from the group's CodeQL wake run,
as described below.

| Check the verdict judges | Behaviour on a group commit |
| --- | --- |
| `CI required` | `ci.yml` runs the complete matrix, as for a manual dispatch, except the informational C coverage job. `hygiene` and `public_export` run ungated. |
| `Hygiene and security` | Ungated, as on every event. |
| `OSV Vulnerability Scan` | `osv-scanner.yml` already triggers on `merge_group`. |
| `dependency-review` | Passes `base-ref` and `head-ref` from the group payload. On a pull request they are empty, which the action reads as unset. |
| `CodeQL` (required by the ruleset itself) | The group's CodeQL wake (`merge_group` run) starts the trusted `codeql.yml` through `workflow_run`, which analyses the group commit (below). |

**Trust argument.** A group exists only because a user with write access enqueued a
pull request that was reviewed. `codeql.yml` runs from the default branch for both
pull requests and merge groups: its `workflow_run` definition is the trusted
default-branch copy, whichever wake started it. The wake is only a signal. It declares
no permissions, checks nothing out, and its result is never read as evidence. The
trusted workflow reads the queue ref from GitHub's API, requires the queue branch to
belong to this repository, and requires the ref to name the commit the wake ran on
before analysis and again before upload. A queued change can therefore alter its own
group's wake run, but not the trusted definition, the commit analysed, or the evidence.
The other merge-group checks (`ci.yml`, `dependency-review.yml`, `osv-scanner.yml`) run
their definitions from the group commit, so a queued change to `.github/workflows/` is
still reviewed for that change and enqueued alone. Whether GitHub reads those
definitions from the group commit is an assumption to confirm in the trial.

**CodeQL on a group.** The wake's `merge_group` run starts the trusted workflow. Its
prepare job accepts the wake only when the head repository is this repository, the head
branch is a `gh-readonly-queue/` branch, and the queue ref names exactly the wake's
commit. The ref is read through the reserved `{+ref}` expansion, because `{ref}` would
percent-encode its slashes. The group tree is read as blobs with the same validator as
the pull-request route. The upload is bound to the queue ref and the group commit, with
the same languages, query configuration and per-language categories. Before the upload
the queue ref is checked again, so a group that was removed or moved during analysis
uploads nothing. The `CodeQL` check is the code-scanning result for the group commit.

**Latency.** The wake is a short signal run that starts when the group is created, so
CodeQL starts about as early as `CI` and runs alongside it. `CI`'s result never reaches
the trusted run. Serialising CodeQL after `CI`, by a `workflow_run` of the merge-group
`CI` run, would cost about 5.6 minutes median per group (round-1 measurement). That
alternative is not used.

**Provenance on a group.** `.github/workflows/provenance-attestation-merge-group.yml`
observes each group commit with the default-branch provenance verifier. It runs as a
`workflow_run` of the group's `CI` run, filtered to `merge_group` runs of this
repository's `gh-readonly-queue/` branches, and reads the group commit as data only,
without checking it out. Its verdict base is the merge base of the group with the
default branch, because a group's parent may be an earlier group that never landed. It
is observation only and not a required check; the hosted `Trusted provenance
attestation` remains the pull-request gate.

## Merging parallel additions

Some files get a new item from almost every pull request: a self-test block in `mk/selftests.mk`, a
target in `mk/public_targets.mk`, a source in `mk/runtime_sources.mk` or `CMakeLists.txt`, a row in a
`docs/DEBUGGING.md` table. Two pull requests that add at the same place conflict in Git although
neither touches the other's lines. `.gitattributes` marks those five files `merge=nk-append`, and
`tools/merge_append.py` is the merge driver behind that name. It runs the ordinary three-way merge and
reads each conflict. It keeps ours and then theirs only when all of these hold:

- The common ancestor has nothing there: both sides only inserted lines. If one side changed or
  removed a line, the ordinary conflict markers stay.
- The two insertions share no non-blank line. Git's diff3 output does not factor out lines both sides
  inserted, so a union would repeat them (ours `X` against theirs `X`, `Y` would give `X X Y`).
- No tab-indented line of either insertion belongs to a rule header outside that insertion. The driver
  finds each such line's owner by walking up the merged text and skipping blank lines and `#` comments
  (make ignores both inside a recipe); the first other non-tab line is the owner. The order of recipe
  lines matters (`rm -f x` before `gen x`), and the union order would depend on which side is "ours", so a
  person decides, also when the insertion starts with a comment or blank line. Inserts between targets,
  new target blocks that bring their own header and tab-indented items of a variable such as
  `PUBLIC_TARGETS` still merge by themselves.

Git's own `merge=union` is not used because it keeps both versions of a line that both sides edited and
leaves no marker.

The driver only acts on the five paths named in `ALLOWED_PATHS` inside `tools/merge_append.py` itself.
`.gitattributes` is read from the tree being merged, so a pull request that edits it could otherwise attach
the driver to a code file. For any other path the driver exits 1 and leaves the ordinary conflict.

The driver runs Git by an absolute path that it finds itself on `PATH`, skipping empty, `.` and relative
entries and any entry in or equal to the working directory (the root of the tree being merged), so a
`git.exe` at that root cannot run on Windows. If it finds no Git it writes nothing and exits 1. For its
own analysis it asks Git for conflict markers longer than any run of `<`, `|`, `=` or `>` that starts a
line in the three files, so a content line such as `=======` cannot pass for a marker. The conflict
written for a person keeps the marker size Git asked for.

`docs/ENHANCEMENT_CONTRACT.md` conflicts often too, but it stays out. Its additions are not independent
items: 18 of the 22 commits that changed it from 2026-11-01 to 2026-11-07 edited existing lines (49 of their
74 hunks), so most of its conflicts are edits the driver would leave anyway. The rest add numbered sections
(`10.12`, `11.3.4`), and two pull requests that each add the next number would merge into two sections with
one number and no marker.

Turn it on once per clone (worktrees of a clone share the setting). In a contributor's own clone:

```bash
git config merge.nk-append.name "append-only list merge"
git config merge.nk-append.driver "python tools/merge_append.py %O %A %B %L %P"
```

**The landing desk must not use that relative form.** `tools/merge_append.py` would then be the file in the
tree being merged, and an earlier pull request in the batch that edits it would run its own code on the
desk's machine. The desk sets the driver in its own clone's config to an absolute path to a reviewed copy
outside every worktree, and runs it with `-I` so the current directory and the user site are ignored:

```bash
git config merge.nk-append.driver "python -I /path/to/reviewed/merge_append.py %O %A %B %L %P"
```

That copy is refreshed only from a reviewed `origin/main` commit (never from a worktree during a batch), and
the desk compares it with `origin/main:tools/merge_append.py` at the start of each batch.

Without the setting, Git ignores the attribute and merges as usual. The driver runs wherever Git
merges on that machine (`git merge`, `git merge --squash`, `git cherry-pick`, `git rebase`). GitHub's
own mergeability check and its **Update branch** button do not run it, so a pull request can still show
a conflict on GitHub that a local merge resolves. The rest of the conflict-prone files stay out on
purpose, and `tools/test_merge_append.py` keeps the list honest:

- `docs/ENHANCEMENT_CONTRACT.md`, `docs/CI.md` and `docs/COMPATIBILITY.md` are prose. Parallel edits
  there rewrite the same paragraph or renumber a section, and the driver would only leave them as
  conflicts. Keep a change to a shared paragraph small, and put a new item in its own bullet or row.
- `src/rt/gpu_sdl3vk/ge_gpu_selftest.inc`, `src/player/player_state.c`, `tools/codegen.py` and every other
  C or Python file are code. Two insertions at one spot there can need an order or a shared brace.
- The root `Makefile` only includes the `mk/` files, so it is no longer an append point.

Add a file to the list only when every conflict it can have is two independent items, and add it to
`ALLOWED_PATHS`, `.gitattributes` and the test together.

## Trusted CI verdict

`.github/workflows/ci-trust-verdict.yml` and `tools/ci_trust_verdict.py` (#373, which also
carries #364) post one commit status, `Trusted CI verdict`, on a pull request's exact head.

**The problem.** `CI required`, `Hygiene and security`, `OSV Vulnerability Scan` and
`dependency-review` come from `pull_request` workflows. GitHub reads those definitions from
the pull request's merge commit, so a candidate can keep a job's name and make it `exit 0`,
or add a workflow with a job of the same name. GitHub matches a required check by name and
integration, and every Actions job is integration 15368. A name alone cannot tell the
intended job from a candidate's.

**The gate.** The workflow runs only the default branch's copy: `pull_request_target`
(opened, synchronize, reopened, labeled, unlabeled), a `workflow_run` when
`CI` completes (CI finishes last; a cancelled first attempt, a superseded head, starts no
verdict job), and a manual `workflow_dispatch` naming a pull request. GitHub blocks
`pull_request_target` by default in public repositories from 2026-12-02 (it stopped every
landing for two hours in the brownout of 2026-11-09), so judging a head and approving a change
do not depend on that event: `CI`'s completion and a dispatch judge a head, a dispatch records
an approval, and the provenance attestation has its own second route (below). One thing still
does: removing the approval label re-judges the head only through that event's `unlabeled`.
It checks out the
default branch, fetches `refs/pull/<n>/head` as
objects only, and never checks out or runs candidate files. The verdict job has `actions`,
`checks`, `contents`, `issues` and `pull-requests` read, and `statuses: write`; separate
jobs with no way to post a status record an approval: one with `pull-requests: write`
records, carries or withdraws it on label events and pushes, and a read-only one records it
for a dispatch (see "Approving a CI change"). The verifier
reads the head's workflow runs, latest-attempt jobs, check runs, statuses and label events
from GitHub's API and posts `success`, `pending` or `failure` (or `error` if it cannot judge
the head, which replaces an older success).

Those reads use the `VERDICT_READ_TOKEN` secret when it is set. It is the maintainer's
fine-grained token, read-only and for this repository only. It moves the verifier's dozen or so
reads per judgement off the hourly `GITHUB_TOKEN` budget that every workflow shares; on
2026-10-31 that budget ran out and failed Trusted CodeQL. The status itself is always posted
with `GITHUB_TOKEN`, since the ruleset accepts it only from GitHub Actions. A read the token is
refused is retried with `GITHUB_TOKEN`. Each run's log ends with a line giving its request count
and each token's remaining budget.

| Code | Fails when |
| --- | --- |
| `CI_CHANGE_UNAPPROVED` | the head changes `.github/workflows/**`, `.github/actions/**`, `.pre-commit-config.yaml`, a CI tool (`tools/ci_paths.py`, `tools/ci_required.py`, `tools/ci_test_shards.py`, `tools/ci_test_weights.json`, `tools/requirements-lock.txt`, `tools/ci_trust_verdict.py`, `tools/provenance_mode.py`), the publication gate's code (`.gitattributes`, `tools/publish_audit.py`, `tools/policy_sync.py`, `tools/modified_file_notice_audit.py`, `tools/provenance_record_gap.py`, `tools/history_audit.py` and every module they load or run; `PROTECTED_EXTRA` in the verifier is the list), a `tools/` module named like a standard-library one (it would be imported in place of it) or any `tools/` file a default-branch workflow runs, without a current approval: the `ci-change-approved` label and a record of this head's protected content written after it |
| `CI_DEFINITION_BEHIND` | `main` changed one of those files after the branch point; the integrator merges `main` when it lands the pull request; the author merges it only for a real conflict in content that is not generated, or when asked (`AGENTS.md` section 10). No label waives it |
| `CI_CONTEXT_COLLISION` | another workflow names `Trusted CI verdict`, or a check run uses that name |
| `FOREIGN_PRODUCER` | a check run or status under an intended context is not from the intended workflow path and event at this head |
| `JOB_MISSING`, `JOB_FAILED`, `JOB_CANCELLED`, `DUPLICATE_CONTEXT` | the latest non-cancelled intended run lacks the job, the job did not succeed, every run was cancelled, or two jobs share the name |
| `STALE_HEAD`, `RUN_HEAD_MISMATCH` | the head is no longer the pull request's head, or a result names another SHA |
| `RUN_FOR_OTHER_PR`, `AMBIGUOUS_HEAD` | the intended run belongs to another pull request, or another open pull request has the same head |
| `PR_NOT_OPEN`, `BASE_NOT_DEFAULT` | nothing to judge for a merge into `main` |
| `DRAFT_LIGHT_CI` | the pull request is a Draft, or its draft state cannot be read: CI ran only the quick checks ([Draft pull requests](#draft-pull-requests)); mark it Ready for review |

`LIGHT_CI_GREEN` is not in that table because it is not a failure. The newest CI run at the head was a
quick one: `CI required` did not run and `CI quick checks` succeeded. The verdict is then `pending`, and its
description starts with the code, which is how the landing desk tells a head that is ready for a batch from
one whose checks are still running. Any other way for `CI required` not to succeed stays `JOB_FAILED`, which
names `CI quick checks` when that is the job that failed ([Quick and full checks](#quick-and-full-checks)).

The publication gate's code is protected because nothing else refuses a file that is outside the
publication profile and is not implementation, such as game data or a key. The trusted attestation judges
the profile's paths and new implementation paths only, and the audit runs from the pull request's own
checkout, in the hygiene and public-export jobs. Without the approval a pull request could loosen the
audit and pass it in the same change. The trusted attestation (`provenance-attestation.yml`) also runs the
default branch's own `publish_audit.py --committed-tree` over the candidate, reading its tree from the
objects (`git ls-tree`, `git cat-file`) and auditing exactly the files that tree lists. No `.gitattributes`
or `.gitignore` line, VCS-looking directory name or repository-shaped root of the candidate can hide or
rewrite a file, and the audit runs no Git command in the written tree; its own Git commands use a bare
repository only when they name one (`safe.bareRepository=explicit`). The audit is also given the verdict base
(`--trusted-base`): at a path that base already holds, a binary waiver covers only bytes the base vouches for,
and the pull request's own manifest approves nothing there
([Approved binaries](PUBLICATION_READINESS.md#approved-binaries-in-the-release-manifest)). The
merge-group observation (`provenance-attestation-merge-group.yml`) does not run it, as it is not a required
check. `GateCodeTests` in `tools/test_ci_trust_verdict.py` fails when one of those tools gains a local
import or a `tools/` script it runs that is not protected, or pre-commit gains a hook that is not.
`tools/verify_sbom.py`, which the public-export job runs to check the SBOM, is exempt: it does not decide
which files may be published, and it loads the analyzer and code generator. That step accepts its expected
`PROVENANCE_UNVERIFIED` failure only when the tool printed no `[FAILED CLOSED]` line and no gate's `[FAIL]`
line, and the token follows the tool's last `[ALL GATES PASSED]` line. So a gate's printed output cannot
turn a failed gate into the expected result. Code that runs in the pull request's own job could still change
files on disk, which no output check sees; the control against a changed audit is the trusted attestation's
own run of the default branch's audit.

Whatever the `CI_DEFINITION_BEHIND` finding text says, who merges `main`, and when, is decided by
[`AGENTS.md` section 10](../AGENTS.md#10-pr-and-integration-authorization), as the row above states.

The intended producers are `CI required` and `Hygiene and security` from `ci.yml`, `OSV
Vulnerability Scan` from `osv-scanner.yml`, `dependency-review` from `dependency-review.yml`
(all `pull_request`), and the provenance attestation from `provenance-attestation.yml`.
In a fork, where the attestation job is skipped, that last producer is the
same workflow's **Fork provenance check** instead, by the same routes (`producers_for` in the verifier,
[`FORKING.md`](FORKING.md)); the upstream, matched by repository ID or name, always judges the
attestation.
A run still in progress gives `pending`. While any producer has
no run at the head yet, or its newest run is unfinished, the verifier reads only the head's
workflow runs and posts `pending`. The full read, about a dozen API calls, waits for the
last producer to finish. Running it on every completion exhausted the repository's
`GITHUB_TOKEN` rate limit, and Trusted CodeQL's upload then failed. Label events are read
only when the `ci-change-approved` label is on the pull request. The protected `tools/`
list is derived on each run from the default branch's workflows (a path named outside a
comment line), so it follows the workflows as they change.

**The attestation's two routes.** The provenance attestation is accepted from either of:

- a `pull_request_target` run at the exact head, as for the other producers;
- a `workflow_run` or `workflow_dispatch` run of `provenance-attestation.yml`. The workflow
  starts one when a `CI` run is requested for a pull request, and a maintainer dispatches one to
  attest again (see "Re-running"). GitHub lists such a run under the default branch, not under
  the pull request head, so it is bound to the head by its run name,
  `Provenance attestation of <head SHA>`, which the workflow's `run-name` sets from the head it
  attests; its job fails
  unless exactly one open pull request into the default branch has that head. The verifier lists
  the workflow's default-branch runs created since the head's first run and takes one only when
  its branch is the default branch and it ran the default branch's current copy of the workflow:
  the commit it started from is on the default branch (`git merge-base --is-ancestor` in the
  trusted checkout) and that commit's copy of the file is byte-identical to the trusted
  checkout's. A dispatch from another branch runs that branch's copy and is ignored. So is one
  from a tag named like the default branch, unless the tag's commit passes the same two tests.
  A commit on the default branch is not enough by itself: a rebase merge lands every commit of a
  pull request, and only the pull request's last tree was judged. An attestation made under an
  older copy of the workflow therefore stops counting when the workflow changes; such a head is
  `CI_DEFINITION_BEHIND` in any case and is attested again once `main` is merged into it.

The newest run of either route decides, and a run is as new as its latest attempt: the order is
by when that attempt started, then by run id. Re-running a run therefore makes it the newest,
whichever route it came from. That matters because the pull request's check list shows only the
`pull_request_target` run, while the `workflow_run` run, created a few seconds later, is listed
under `main`: re-running the check a person can see has to count. Only a user with write access
can re-run, and a re-run attests the same head again with the same definition. One exception to
"newest": a `pull_request_target` run that ended `startup_failure`, which is how GitHub ends a
run whose event it refused, is ignored when the second route has a run that was not cancelled.
So is a `pull_request_target` run that completed without starting any job, which is how GitHub
ended some refused runs on 2026-11-09 (`failure`, "workflow file issue"). Neither kind attested
or failed anything; with no run of the second route either one still fails the head.

**How long a second-route run stays findable.** GitHub answers a filtered run list with its
newest 1,000 runs. Every `CI` run starts one attestation run (pushes to `main` and the nightly
run included; those only skip), about 185 a day in early November 2026, so a `workflow_run`
attestation leaves the list after about five days. A dispatched attestation is in a separate,
far shorter list and practically never leaves it. The verifier compares the list's
`total_count` with what it received. A truncated list has lost every run created before the
oldest run it returned; that time is the list's horizon. The run that would be judged for the
head must have started at or after the horizon, so that nothing the list lost could be newer.
Otherwise the attestation is reported as not found on **both** routes and the head is `pending`
("not among its workflow's newest 1000 runs; dispatch it again"): a lost run may have been a
failure, so no older result is judged in its place, whether it is a `pull_request_target` run
or a run from the other event's complete list. One dispatch of the attestation recovers the
head, and so does re-running a run of the head that is still listed. A run that has left the
list is never read, so re-running *it* changes nothing.

The list of approval dispatches (next section) has the same limit, counted from the label:
about 1,000 verdict dispatches, roughly a month in November 2026. A record that has left it
is not seen and the head is `CI_CHANGE_UNAPPROVED`; the finding then says the list was
truncated, and the remedy is to dispatch the approval again.

**Known limits of the second route.**

- *A second pull request at the same head.* The resolver needs exactly one open pull request
  at the head, so while two are open the `workflow_run` attestation of that head fails for both
  (and a run still going for the first is cancelled, because the concurrency group is per
  head). Anyone can open such a duplicate. The verdict already reports `AMBIGUOUS_HEAD` while
  both are open; the difference is that the failed run is still the head's newest after the
  duplicate is gone. Remedy: close the duplicate, then dispatch the attestation for the head.
- *Runs on `main`.* A `workflow_run` attestation's check run is attached to the tip of `main`,
  not to the pull request. `main`'s commit list therefore shows a red
  `Trusted provenance attestation` whenever some pull request's attestation fails, which is
  expected before an admission. It says nothing about `main`.
- *A pull request into another branch* gets a failed run where the `pull_request_target` route
  skips: the resolver finds no pull request into the default branch.

**Approving a CI change.** Two steps, always both, in this order. A maintainer reviews the
change and applies the `ci-change-approved` label, then records which head was reviewed, from
the default branch:

```bash
gh workflow run ci-trust-verdict.yml -f pull_request_number=<n> -f approve_head_sha=<reviewed head SHA>
```

The label counts only when its latest event is `labeled` and the user who applied it has write,
maintain or admin permission. It names no bytes, so by itself it approves nothing: a head is
approved when a record of its protected content (next paragraph) was written under that
`labeled` event: by a run created at or after it, in a job that completed after it. Removing
the label withdraws every earlier record. The label waives only
`CI_CHANGE_UNAPPROVED`: an approved change still needs every intended job, once, green, from
its intended workflow.

**After any push, the label is removed and applied again, after reading the new head.** The
label names no commit. While every push ran the handler that removes it, "the label is on"
meant "this head was read"; once GitHub refuses `pull_request_target` nothing removes it, and a
label given for one head is still on the pull request at the next. The dispatch therefore
refuses a label that is not newer than the head it is asked to approve
(`LABEL_PREDATES_HEAD`): the label's latest `labeled` event must be later than the push that
made that commit the head. The one case that needs no new label is a push that left the
protected files' digest unchanged: the existing record still approves it, and no dispatch is
needed.

What the dispatch compares the label with, and what each refusal means:

| Refusal | The label is not later than | Remedy |
| --- | --- | --- |
| `LABEL_PREDATES_HEAD` | the push that made the commit the tip of the head branch, or a `head_ref_force_pushed`, `head_ref_restored` or `reopened` event of the pull request | remove the label, read the head, apply the label, dispatch |
| `LABEL_PREDATES_BASE` | a `base_ref_changed` event: the pull request was retargeted after the label | the same |
| `HEAD_NOT_NEWEST_PUSH` | (the branch's newest push record is for another commit, so the list does not show when this one became the tip) | push the branch again, then label and dispatch |
| `HEAD_TIME_UNREADABLE` | (no dated record: a deleted fork, a fork the token may not read, a list that cannot be read) | for a fork that cannot be read, land the change through a branch of this repository |

- The push time comes from GitHub's repository activity for the head branch: its records of the
  branch's updates, each with the commit before, the commit after and the time. The branch's
  newest record must be for the commit being approved. A force push back to an earlier commit is
  thus dated by its latest arrival, not by the commit's first runs.
- That list is not a complete log. It can lag a push, and it has been seen to lack a record. So
  a record of the commit somewhere in the list is not enough, and when the newest record is
  another commit's the dispatch is refused instead of dating the head by an older arrival.
- The branch's records date the branch, not the pull request. GitHub leaves a closed pull
  request's head where it was, and commits pushed meanwhile arrive when it is re-opened; a
  retarget changes what the head is compared with. So the label must also be later than every
  such event of the pull request.
- Times are compared to the second, and the pull request shows a new head a second or two after
  the activity time of its push. A label in the same second as the push or event is refused; a
  label applied in the gap just after it is not. Apply the label after reading the head on the
  pull request's page, not from a script racing the push.

The dispatch is the step to rely on, not an extra. Applying the label also starts a
`pull_request_target` run that writes a record, but that run cannot be counted on: GitHub
refuses the event in a brownout and by default from 2026-12-02; on 2026-11-09 one label's run
ended with no job at all and another wrote its record 45 minutes after the label; and a label
run waiting in its head's concurrency group is replaced by any later run for that head and then
writes nothing. The label-event record also approves whatever head is current when the label
is clicked, so a push seconds before the click is recorded; only the dispatch names the bytes
that were reviewed. The approval dispatch has its own concurrency group, so nothing replaces
it, and it judges the head when it has written the record.

**Withdrawing an approval.** Rule: removing the label is always followed by a verdict dispatch
for that pull request, as one action:

```bash
gh workflow run ci-trust-verdict.yml -f pull_request_number=<n>
```

Removing the label withdraws the approval in the verifier at once, but a status is only what the
last verdict run posted. The only event that re-judges a head when a label is removed is
`pull_request_target`'s `unlabeled`, and when GitHub refuses that event nothing runs: the head's
last `success` stands, and the ruleset accepts it, until a verdict is dispatched. Nobody merges
a pull request whose `Trusted CI verdict` status is older than the last event of its
`ci-change-approved` label.

The verifier used to accept a label applied after the head's first pull-request run. That was
unsound: a pull request can be pushed back to a commit that ran before the label, and when
`pull_request_target` is blocked nothing removes the label on that push.

**Approval records.** A record binds an approval to bytes, so a push that changes no CI-relevant
file keeps it, and a maintainer does not approve again for a documentation line, a test entry or
a merge of `main` that left the protected files alone. "CI-relevant" is the same classification the verdict
uses for `CI_CHANGE_UNAPPROVED`: the protected paths above, listed by `protected_paths` in
`tools/ci_trust_verdict.py` from the default branch's workflows. There is no second list.

- *The digest.* `approval_digest` is a SHA-256 over the mode and SHA-256 of the bytes of every
  protected path present in the head's tree, in path order, under a version tag. The path set is
  the verdict's own, plus every `tools/` file that a workflow or action of the head itself names
  outside a comment, so a gate tool that only the candidate's workflow runs (a new tool, or an
  existing one newly run) is hashed, and a later push that turns it into `exit 0` withdraws the
  label. A protected path added to or removed from the head changes it, so an added workflow or action file, a
  deleted gate tool and a rename (a removal plus an addition) all change it. A mode change
  changes it. The candidate's copy of `tools/ci_trust_verdict.py`, `tools/ci_paths.py`, the
  verdict workflow and the other gate files are protected paths, so changing the
  classification itself changes it. Changing a protected file and reverting it gives the
  original digest again. While the same label stands, a dispatched record then approves the
  head once more, because the bytes are the ones that were approved; a label-event record does
  so only at the very head it was written at.
- *The dispatched record.* The dispatch above runs the `approval_dispatch` job: the default
  branch's copy of the tool, with the candidate fetched as objects only. It hashes the head the
  dispatch names, provided the label's latest event is `labeled` by a user with write access,
  the pull request is open against the default branch, and `refs/pull/<n>/head` still is that
  head, before and after hashing; a pull request that has moved on is refused, so nobody
  approves bytes they did not name, and so is a label older than that head
  (`LABEL_PREDATES_HEAD`, above), so nobody approves bytes under a label given for others. The
  next job is named
  `CI approval record v2 pr<number> <head> <digest>`, every part an output of the tool. The
  record is read back only from a
  successful job of that name in a `workflow_dispatch` run of
  `.github/workflows/ci-trust-verdict.yml` whose branch is the default branch, that ran the
  default branch's current copy of that workflow (the same two tests as for the attestation),
  and whose actors (the dispatcher, and whoever re-ran it) have write
  access. A dispatch from any other ref runs that ref's copy of the workflow and is ignored. A
  workflow's own token can dispatch a workflow, and its actor is a bot with no collaborator
  permission, so a workflow run cannot approve. The record counts for any head of that pull
  request whose digest it names.
- *The label-event record.* On a `labeled` event the workflow's `approval` job does the same for
  the head the event named. The next job, `approval_record`, is named `CI approval
  record v1 pr<number> <digest>`; the pull request number and the digest are the job's name.
  The record is read back only from a successful job of that name, in a `pull_request_target`
  run of `.github/workflows/ci-trust-verdict.yml`, at exactly the head being judged and for
  that pull request's number (one SHA can head two pull requests). GitHub takes a
  `pull_request_target` run's workflow definition from the pull request's base branch, not from
  the pull request, so a pull request cannot produce such a job. That base is the default
  branch only for a pull request that targets it: a run GitHub reports for another base or
  another pull request is ignored, and a pull request that was ever retargeted (a
  `base_ref_changed` event) gets no label-event record, because GitHub does not report a fork
  pull request's runs against it; approve it by dispatch, which never takes its workflow from a
  pull request's base. A job in a `pull_request` run, another workflow, another
  head or a failed job is ignored.
- *The carry.* On a `synchronize` event the `approval` job reads the digests the current label
  already approved: those recorded at the push's previous head (`github.event.before`) and
  those recorded by dispatch, in both cases under the label's latest `labeled` event. It
  recomputes the digest for the new head with trusted code and succeeds only on an exact
  match, with the label still on the pull request from a
  write-access user and the head still current. It then writes a record for the new head, so
  the next push compares with this one. Any other outcome (no record, a record from before the
  current label, a different digest, an
  API or git failure, a head that moved, a reopen) removes the label; the step failing is the
  same as the step not running.
- *The verdict.* The verdict recomputes the head's digest itself and approves the head only when
  a record with that digest was written under the label's latest `labeled` event: a dispatched
  record for this pull request, or a label-event record at this very head. "Under" means the
  record's run was created at or after the event and its job completed after it. A run answers
  the event that created it, and re-running it does not change when it was created, so a run
  from before the label cannot write a record for it. A label with no such record, or a record
  whose digest the head no longer has, fails with `CI_CHANGE_UNAPPROVED` whether or not the
  workflow removed the label.

The digest covers the same boundary `protected_paths` draws and nothing wider: it does not hash a helper
module that a gate tool imports, or a script outside `tools/` that a workflow runs, so review those
in the head that introduces them.

A force-push of an unrelated history is judged by content alone: the previous head is the
push's `before`, whatever its ancestry, and an identical digest is identical bytes. Two pushes
in quick succession can find no record yet at the first one's head and withdraw the label;
so can a labeling run that a newer pending run replaced (the workflow's concurrency group
keeps one pending run per head). Both fail closed: record the approval by dispatch after
reviewing the head. An approval dispatch has its own concurrency group, so a later dispatch
for the same pull request cannot replace it before the record is written. A merge of `main`
that changes a protected file also changes the digest; re-review it.

**Re-running.** Every run reads GitHub's state afresh. Only `CI` is a `workflow_run`
trigger. The provenance workflow cannot be one, because its verifier refuses any other
workflow that names its context, and `OSV-Scanner` and `Dependency Review` are not, because
they finish before `CI` (see
[Why the verdict waited over an hour](#why-the-verdict-waited-over-an-hour-2026-11-05)).
When one of them finishes after `CI` (for example a re-run, or the provenance attestation
after an admission), the verdict stays `pending`; run:

```bash
gh workflow run ci-trust-verdict.yml -f pull_request_number=<n>
```

To attest a head again, for example after its admission reached the private authority's
`main`, dispatch the attestation from the default branch, then the verdict as above:

```bash
gh workflow run provenance-attestation.yml -f pull_request_number=<n> -f head_sha=<head SHA>
```

The dispatch is the newest run for that head, so its result replaces an earlier failure.
Re-running an attestation run of the head works too, the one on the pull request's check list
included: the run whose latest attempt started last decides. The exception is a run that has
left GitHub's run list (see "How long a second-route run stays findable"); dispatch instead.
The dispatched run is listed under the default branch, so the pull request's check list does
not show it; `gh run list --workflow provenance-attestation.yml --branch main` does, by its
title.

**What stays outside it.** Candidate tests and build files are still candidate code: the
gate proves the intended jobs ran from trusted definitions, not that the tests are honest.
A workflow on a same-repository branch could post a commit status under `Trusted CI
verdict` with its own token; fork pull requests get a read-only token and cannot. The
approval record is not a status for that reason: a same-repository workflow can post a status
with any text, but it cannot make a job of a default-branch run of the verdict workflow carry
a name. A dispatched verdict and one an event started are in different concurrency groups (a
dispatch names a pull request, not a head), so two verdict jobs can post out of order; each
reads GitHub's state afresh, and the window is one job's read-to-post time. A GitHub
App that posts the status, pinned by `integration_id` in the ruleset, would close that (see
[`PROVENANCE_MERGE_GATE.md`](PROVENANCE_MERGE_GATE.md#required-check-identity-what-github-can-and-cannot-enforce-here)).
A closed pull request with the same head and an older base branch could leave a later run
behind; the open-pull-request check catches it only while that pull request is open. Merge
groups get no verdict yet, so enabling a merge queue together with this gate needs a
`merge_group` route first.

**The ruleset step (applied 2026-10-29).** The owner set the `main-minimal` ruleset's
required status checks to `Trusted CI verdict` (integration 15368, GitHub Actions) and
`CodeQL` (integration 57789), kept `strict_required_status_checks_policy` on, and created the
`ci-change-approved` label. Pull requests opened before the change got a status by the
dispatch command above. No repository setting changes in this repository's code, so re-read
the ruleset before relying on this; `tools/test_public_ci_wiring.py` holds the snapshot this
file and `PROVENANCE_MERGE_GATE.md` must match.

**Live canary, after the ruleset step.** Use a fork or a second account, and keep every pull
request a draft that is closed unmerged:

1. Open a draft pull request that breaks a Python test, adds `.github/workflows/canary.yml`
   with a job named `CI required` and a job named `Trusted CI verdict`, each `sleep 300` then
   `exit 0`. Expect `Trusted CI verdict` to fail with `CI_CHANGE_UNAPPROVED`,
   `FOREIGN_PRODUCER` and `CI_CONTEXT_COLLISION`, and the merge box to stay blocked after
   the canary jobs finish. This also confirms the GitHub rule the design relies on: when a
   check run and a status share a required name, both must pass.
2. Delete `canary.yml` and change `ci.yml` so `CI required` runs `exit 0`. Expect
   `CI_CHANGE_UNAPPROVED`. Do not label it: the label is the human approval that such a
   change is wanted.
3. Revert `ci.yml`, apply the label to an earlier head first, then push. Expect the label
   removed and `CI_CHANGE_UNAPPROVED` or `JOB_FAILED` for the new head.

While each pull request is a Draft the verdict also reports `DRAFT_LIGHT_CI` and the broken
Python test does not run; mark it Ready (it still cannot merge) to see the test fail.

Record the run links in #373.

## Windows hosted runner policy

The Windows jobs run on `windows-2025`. That label is a GitHub-hosted Windows
Server image rather than an end-user support promise; the supported developer
platform is Windows 11 x64 as documented in [SETUP.md](SETUP.md). Consult the current
[Windows 2025 image inventory](https://github.com/actions/runner-images/blob/main/images/windows/Windows2025-Readme.md)
instead of treating a dated image version or tool inventory as an evergreen
repository guarantee.

## Self-hosted Linux runners

GitHub's free plan runs about 20 jobs at once for this repository, and one full CI run
needs 17 of them ([Jobs per run](#jobs-per-run)), so a second run can wait for runners. To keep landings moving, the
maintainer runs a small pool of self-hosted Linux runners, labelled `nk-wsl`, on their
own PC. Contributors need nothing from it: every run they can start uses GitHub's
hosted images, exactly as before.

**What runs there.** The classifier's `linux_runner` and `linux_runner_2604` outputs
(`tools/ci_paths.py`) name the runner for every Linux job of CI except `classify`, which
decides and so stays hosted: the native and translation gates and the Python shards
(hosted fallback `ubuntu-24.04`), and hygiene with its Markdown and main smoke steps,
the export audit with the profile-zero fixture build, and `CI required` (hosted fallback
`ubuntu-26.04`; the
pool's image is Ubuntu 24.04 for all of them). The Windows gates stay on GitHub's
Windows runners. The outputs name the pool only for a trusted run, whose code is
already the maintainer's:

- a push to `main` of this repository;
- a merge-queue group (`merge_group`) of `main`;
- a pull request whose head branch is in this repository (not a fork), which the
  repository owner opened and whose event the owner sent. Branches that may carry an
  outside contributor's commits are excluded: the desk's `carry/pr-<N>` branches for locked
  fork pull requests and its `desk/` working branches. The owner is the only collaborator
  with push access. Apps the owner installed, such as a coding agent, can also push branches,
  but a pull request they open or a push they make isn't the owner's event, so its runs stay on
  hosted runners. The code on the owner's own branches already runs on the owner's PC.

Trusted CodeQL (`.github/workflows/codeql.yml`) sends its three analyses there for the
same targets: its `prepare` job, which stays hosted, runs the default branch's
classifier (`python tools/ci_paths.py --codeql-runner`) and the `analyze` jobs run on
the runner it names. That workflow always runs the default branch's definition and
reads the candidate tree as data (`build-mode: none`), and it takes the pool only when
it was woken by one of the owner's own pull requests above or a merge group, or for a
push to `main`. Every other pull request's analysis stays on `ubuntu-26.04`.

Every other run, a fork's pull request above all, gets the job's hosted image
(`ubuntu-24.04` or `ubuntu-26.04`, whichever that job names when it is not on the pool). Two
repository variables, which only the maintainer can set, gate it further:
`NK_SELF_HOSTED_LINUX` must be `on`, `landing` or `batch`, and `NK_SELF_HOSTED_HEARTBEAT`
must be a Unix time less than ten minutes old. `on` sends every trusted run above to the
pool. `landing` sends only the trusted runs that land work: a push to `main`, a merge group,
a desk batch branch (`batch/`) and a Ready pull request; for Trusted CodeQL, a push to `main`
and a wake of a merge group or a batch. A Draft pull request's CI then stays hosted, so a
small pool is kept for landings. `batch` is narrower still: a push to `main`, a merge group
and a desk batch branch, so a Ready pull request's own CI stays hosted as well (the desk
tests it again in its batch) and the pool runs only the batches; Trusted CodeQL takes the
same targets as in `landing`. Each value only narrows the trusted runs above, never widens
them, and any other value is off. The pool's supervisor refreshes the heartbeat every
two minutes while the pool is healthy, sets the switch to the mode it was started with (`on`,
`landing` or `batch`), and turns it `off` when it stops or the pool fails, so a sleeping PC or a
dead pool sends trusted runs back to hosted runners within minutes instead of leaving them
queued. Only `runs-on` changes: every step, check name, `CI required` and the Trusted CI
verdict are the same on either runner.

**The routing is not the security boundary.** On a `pull_request` event, `ci.yml` and
`tools/ci_paths.py` both come from the candidate's own tree, so a fork could edit either
one, or name the pool's labels directly, to send its jobs to the pool. The routing only
keeps this repository's own runs where they belong. What stops a fork is:

- **The runner's job-started hook, which is the boundary.** It is baked into the pool's
  image (outside any checkout) and runs before a job's first step. It decides only from
  values the runner provides: `GITHUB_REPOSITORY`, `GITHUB_EVENT_NAME`, `GITHUB_REF`,
  `GITHUB_WORKFLOW_REF` and the event payload at `GITHUB_EVENT_PATH`, never from a file
  in the repository. It refuses every job that is not one of the trusted runs above (for
  `workflow_run`, only the default branch's `codeql.yml` woken by one of the owner's own
  pull requests or a merge group), so a fork's job that asks for the pool fails before it
  runs anything.
- **The fork approval policy.** The repository requires approval before any workflow runs
  for an outside contributor's pull request (`all_external_contributors`).
- **Isolation.** Each runner is an ephemeral container that takes one job and is
  discarded, has no access to the PC's files, Docker or local network, and holds no
  secrets (its one-job registration comes from the GitHub API when it starts).

`tools/test_ci_paths.py` (`SelfHostedRunnerRoutingTests`, `CodeqlRunnerRoutingTests`,
`LandingOnlyPoolRoutingTests`, `BatchOnlyPoolRoutingTests`) fails if a fork or ordinary
pull request could route to the pool, or a mode took more than it names, and
`tools/test_public_ci_wiring.py` (`SelfHostedRunnerWiringTests`) fails if any workflow
names the pool's labels directly or a job other than the listed ones reads
`linux_runner` or CodeQL's `analyze_runner`.

**Stuck jobs.** A run routed to the pool whose jobs then cannot start (the pool broke
after `classify` decided) would wait for a self-hosted runner for up to 24 hours, because
`timeout-minutes` does not count queue time. While the pool is unhealthy, its supervisor
turns the switch off, cancels every run that has had a job queued for the pool for more
than five minutes, and re-runs it once cancelled; the re-run classifies again and goes to
hosted runners. If the whole PC is off, queued jobs wait and start when the pool returns.
By hand: set `NK_SELF_HOSTED_LINUX` to `off`, then `gh run cancel <run>` and, once it has
stopped, `gh run rerun <run>`.

**The supervisor's GitHub access.** It needs to create one-job runner registrations,
delete stale ones, set the two variables, and cancel and re-run runs: repository
Administration, Variables and Actions, write. That credential stays on the PC, outside the
WSL distro and every runner container; a fine-grained token scoped to this repository
with only those permissions is the intended form.

The pool's image matches what the hosted Ubuntu 24.04 jobs rely on (the native apt
packages, Python 3.15.2 in the tool cache, CMake 3.31.6 so the SDL3 caches match, and
the CodeQL bundle the pinned CodeQL action uses). Its
build files and supervisor are kept in the maintainer's workspace, not in this
repository, because nobody else can run them.

## Windows Python and PATH contract

Repository tools in hosted jobs must not depend on which Python happens to be
first on `PATH` (#294). The contract is explicit:

| Job / environment | Python running `tools/*` and the fixture generators |
| --- | --- |
| Linux jobs (`classify`, `hygiene`, `public_export`, `python_tools`, `native_tools`, `ci_required`) | `actions/setup-python` CPython 3.15.2 |
| `windows_runtime` (MSYS2 UCRT64 shell) | Windows CPython 3.15.2 from `actions/setup-python`, passed through `PYTHON`; MSYS2's UCRT64 Python 3.14.8 is not used for repository tools |
| Local Windows runs | Windows CPython 3.15.x from the python.org installer; set `PYTHON` explicitly when MSYS2 comes first on `PATH` |

Stages that invoke the repository's PowerShell asset-copy script opt into
the runner's Windows `PATH` explicitly with `MSYS2_PATH_TYPE: inherit` and assert
`command -v pwsh` themselves. Under `inherit` the MSYS2 UCRT64 directories still
precede the inherited Windows `PATH`; `windows_runtime` passes Windows CPython's
absolute path through `PYTHON` and converts it to an MSYS path only for direct
shell invocations. No hosted step relies on undeclared `PATH` order.

That same MSYS2 shell has no `git` on `PATH`, so the Makefile cannot resolve a
revision there. `windows_runtime` therefore sets `SR_SOURCE_COMMIT` to
`github.sha` for every step: the identity the binaries of that job record
(flight recorder `build.build_id`, #532) is the revision `actions/checkout`
actually checked out, and the build never depends on finding `git`. The Linux
jobs run with `git` on `PATH` and resolve the same revision themselves, through
one guarded resolution that yields an empty identity -- and no build-log noise --
on a host that has none. A build with no identity is not a silent pass:
`tools/flight_diff.py` refuses such a bundle fail closed.

### Comparator comparability contract (#320)

`tools/flight_diff.py` compares two flight-recorder bundles under an explicit
comparability contract, so a MATCH is only ever printed for two captures that
can actually vouch for each other. The CLI verdicts map to exit codes
`MATCH` 0, `DIVERGENCE` 1, error 2 (unchanged), and `INCOMPARABLE` 3:

- **Comparable and matching (MATCH):** every compared field agrees. Build
  identity (`build_id`, `source_date_epoch`, `compiler`, `pointer_bits`) and
  the recorder limit never block a comparison; both identity lines are printed
  on stdout, because comparing different builds is a legitimate use.
- **Incomparable (INCOMPARABLE):** differing bundle schema versions, runtime
  blocks, enabled event classes, or armed trigger policies; any dropped event
  on either side (the ring slides, so a divergence inside the dropped window
  is invisible).
- **Divergent (DIVERGENCE):** the first differing event (sequence or class
  alignment), then the terminal reason/sequence/kind/arg0, then, for schema 5
  and 6, the refusal block's count, first_nid, first_pc, first_sequence and nids. Same
  retained events with a different terminal outcome or refusal block is a
  divergence, not a MATCH. A terminal difference is never an incomparability: a
  `running` terminal is a mid-run cut rather than a ring overflow, so over a
  complete window it hides no events and stays an ordinary compared value.
- **Malformed (error):** a bundle that fails schema validation, sanitization,
  or the recorder truncation invariants (retained sequences exactly
  `dropped+1..recorded`, `dropped == recorded - min(recorded, limit)`) exits 2
  on stderr as before.

MATCH is a statement about the capture, never about the whole program. It
certifies exactly these things: that the two runs agree on every event retained
for the enabled classes over a complete window that dropped nothing, that both
reached the same terminal outcome, and, for schema 5 and 6, that both recorded
the same refusal block. It does not certify behaviour outside
the enabled classes, where a class one side never recorded is silence rather
than agreement; behaviour before the recorded window; or behaviour the recorder
does not model at all. Two builds that MATCH here can still differ anywhere the
recorder was not looking, so the CLI prints the enabled class list that bounds
the verdict next to the two identity lines.

A schema 6 `threads` block is the thread state at the cut. It is validated and
never compared. The CLI prints each side's waiting threads after every verdict,
INCOMPARABLE included, so a stall comparison shows what each side waits on.

A bundle that records any dropped event can never produce a MATCH against any
other bundle; re-capture with a larger limit when a comparison is the goal.

## Public release-path gates (#294)

The hosted matrix now exercises the same public, source-owned release path a
developer runs locally, without private inputs:

- `windows_runtime` links the native player (`mingw32-make player`), runs
  `player-ui-tests`, executes headless native player UI regressions
  (`mingw32-make --no-print-directory CC=gcc VULKAN_SDK=/ucrt64 player-ui-regressions`,
  `smoke` part only; the harness forces SDL's dummy video and software render drivers),
  runs the complete platform ladder
  (`mingw32-make --no-print-directory platform-ladder`), and runs
  `profile-zero-e2e` for the two profile-zero manifests. That gate
  validates the guest ProgramImage, generates a public AOT package, launches it
  through the headless production runtime, and checks all seven named
  guest-service cases for each manifest. It runs in the `routes` part, in the
  `routes` group (25 minutes) of the `windows_runtime` job, and took about 123 seconds locally for both package
  builds and runtime launches. The guest bytes are built from the fixture's own
  `main.c` and `Makefile` by the profile-zero steps of the `public_export` job (pinned PSPDEV, checked
  against the committed `fixtures/profile_zero/prebuilt/SHA256SUMS`) and downloaded
  as an artifact, so the repository stores no generated executable (#1100) and the
  Windows runner still executes the whole route; where PSPDEV is installed, the
  gate rebuilds the fixture itself and fails if any byte differs from `SHA256SUMS`.
  The make target runs the route case at the full test tier, and the step sets
  `NK_PROFILE_ZERO_REQUIRED=1`, so a missing Vulkan SDK or SDL3 fails it rather than
  skipping it. The `player` part of `native_tools` runs the same gate on Linux, where
  PSPDEV is installed and the guest is rebuilt from source. On both hosts the gate also
  runs each package on the deterministic guest clock and compares digests of its
  result file, save and first rasterized frame with the values pinned in
  `tools/test_profile_zero_e2e.py` (`CROSS_HOST_DIGESTS`), so Windows and Linux must
  produce the same bytes (#306).
  The same job builds Nakagawa Font Copy (`src/psp_companion/Makefile`), the PSP app
  behind the player's font import, through its user-mode import gate; nothing from
  that build is uploaded. Public
  runtime link dependencies remain external; when they are unavailable, the
  test reports an explicit `SKIP` with the printed missing-dependency reason.
  The job also runs the production smoke with its executable staged into a fresh
  directory outside the build tree (`production-smoke-staged`).
- The `public_export` job ("Public export audit and profile-zero fixture") is ungated
  and runs in parallel with `hygiene` (#735 item 5). Its step "Exercise public-export
  generation and candidate audit" runs on `security_publication` changes and every
  manual `workflow_dispatch`, and the job reports success when the step is skipped.
  `ci_required` requires the job to succeed. One
  `build_public_export.py --export-dir ... --public-safe-profile` invocation
  runs the publication gates, generates the public-safe candidate, and audits
  the candidate-tree staging bytes. Hosted CI holds no trusted ledger, so the
  export must fail closed with `PROVENANCE_UNVERIFIED` and promote nothing; the
  step asserts that named boundary and the #293 immutability rule (no candidate
  at the requested path). Clearing a candidate belongs to the release flow and
  requires the release-controlled trusted ledger.

## Dependabot policy

`.github/dependabot.yml` checks GitHub Actions and root pip and
pre-commit ecosystems monthly. Minor and patch updates are grouped per ecosystem;
major updates remain standalone because they can change APIs, runners, or build
semantics. Security updates remain enabled and are not suppressed by the routine
groups. The open-PR limits keep routine maintenance from crowding out focused
engineering work.
