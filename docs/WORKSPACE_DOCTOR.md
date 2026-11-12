# Workspace Doctor

`tools/nk_doctor.py` is the fail-closed preflight for Nakagawa Recomp. It checks the supported
Windows 11 x64 / PowerShell 7.6+ / CPython 3.15.x (3.14.x also accepted) host contract, current MSYS2 UCRT64 tools,
Vulkan SDK discovery, the repository, private game-input layout, runtime dependencies, and build
products without copying, decrypting, extracting, modifying, or uploading private material.

The simplified `nk.ps1` front end exposes it directly:

```powershell
.\nk.ps1 Doctor
```

Use a narrower scope when diagnosing one layer:

```powershell
.\nk.ps1 Doctor -Scope repo
.\nk.ps1 Doctor -Scope inputs
.\nk.ps1 Doctor -Scope build
.\nk.ps1 Doctor -Scope products
.\nk.ps1 Doctor -Scope run
```

For automation:

```powershell
.\nk.ps1 Doctor -Scope all -Json
```

Without a title selection, the canonical doctor uses `assets/titles/synthetic.json`.
Pass `-TitleManifest` and, when needed, `-GameName` to `nk.ps1` (or `--title-manifest`
and `--game-name` to `tools/nk_doctor.py`) to diagnose another title. `<game>` below
is the selected build target name, not necessarily the manifest ID; private-input
checks depend on the selected manifest.

`-Strict` makes warnings produce a nonzero result. Without it, warnings remain visible but only
`FAIL` results make the action fail.

## Scopes

| Scope | Checks |
| --- | --- |
| `repo` | Required public-facing documents, root GPLv3 text, project-metadata transition warnings, core disclaimers, tracked-private-path hygiene when a local Git checkout is available, linked-worktree/build-output accumulation, and path-length advisory |
| `inputs` | Decrypted MIPS ELF/PRXs, original `~PSP` header, ISO selection/format/disc-ID signal, populated XB extraction tree or configured `SR_DATAROOT`, and save/memstick storage (`SAVE_ROOT`) |
| `build` | Windows 11/x64, PowerShell/Python, UCRT64 compiler and Make tools, SDL3/Vulkan link inputs, and private code-generation inputs |
| `products` | Built `<game>.exe`, `<game>_image.bin`, and recorded build profile (`BUILD_PROFILE`) |
| `run` | Windows/x64/Python, ISO and XB assets (`SR_DATAROOT`), save directory (`SAVE_ROOT`), VFPU tables, runtime DLLs, `<game>.exe`, and `<game>_image.bin` |
| `all` | Every check above |

## What it validates

### Toolchain identity

The doctor requires Windows 11 x64, PowerShell 7.6+ (`pwsh`), and CPython 3.15.x (3.14.x is also accepted; the check is `>=3.14,<3.16`). It resolves
`mingw32-make`, `gcc`, and `g++`, records their first version line, and fails when they do not
resolve from an MSYS2 UCRT64 path. It checks the SDL3 import library and discovers a usable Vulkan
SDK using `-vulkan-sdk`, `VULKAN_SDK`, then the newest numerically named `C:\VulkanSDK\<version>`
installation. SDK validity requires the headers and loader import library needed by the build,
not just a directory name. `glslc` is not required when the checked-in shader source, embedding,
and manifest hashes pass `shader_embed.py verify`; it is checked when that authoritative
provenance gate reports that regeneration is required.

The output uses `ERROR`/`FAIL` for missing or unsupported required prerequisites, `WARNING`/`WARN`
for optional workflow capabilities, and `INFO` for detected state. Private game inputs remain a
separate scope and never turn a toolchain absence into a toolchain pass.

This does not claim that any executable named `gcc` is a supported compiler. A non-UCRT64 compiler
can link against a different C runtime and produce a binary that fails later in ways that look like
runtime defects.

### Private input format

The doctor does more than test path existence:

- The selected title's EBOOT ELF and each PRX its manifest marks `required` (the `modules` entries with
  `role: guest-prx`) must be ELF32, little-endian, MIPS, contain bounded program headers,
  and contain at least one `PT_LOAD` segment.
- When the manifest asks for it (`bss_metadata_source: psp-header`), `EBOOT.BIN` must contain a valid
  `~PSP` header with a bounded segment count and nonzero declared segment sizes.
- The EBOOT ELF and `EBOOT.BIN` must agree on their load-segment count.
- exactly one ISO candidate must be selected; ambiguous multiple-ISO workspaces fail closed;
- the ISO must contain an ISO9660 primary volume descriptor;
- the disc ID that the title manifest declares is searched for in the ISO as an additional identity signal; without a manifest disc ID the check is skipped with an `INPUT_DISC_ID` note;
- `xbdata_extracted/` (or configured `SR_DATAROOT`) must contain files rather than merely exist as an empty placeholder;
- `memstick/` (or configured `SR_MEMSTICK`) is verified for directory presence and writability (`SAVE_ROOT`).

The disc-ID scan is deliberately an additional signal, not a cryptographic identity guarantee. The
player's importer reads the disc ID and version from `PARAM.SFO`; the doctor does not bind generated
inputs to a workspace manifest.

### Runtime closure

The run scope verifies:

- all 15 required VFPU table names and exact byte sizes;
- an x86-64 `SDL3.dll` from the build directory, repository root, or configured UCRT64 bin path;
- an x86-64 Vulkan loader;
- an x86-64 `build/<game>/<game>.exe`;
- a nonempty `build/<game>/<game>_image.bin`;
- recorded runtime build profile (`BUILD_PROFILE`).

The doctor's VFPU check remains a name/size baseline for workspace diagnosis; content
authentication, semantic invariants, checked indexing, and thread-safe publication now live in
the runtime loader itself (`src/rt/vfpu_tables.c`, embedded SHA-256 manifest).

### Path length

`check_long_paths` compares the longest tracked file, the deepest expected build output
(`build/synthetic/vfpu_oracle/nakagawa.stdout.txt`), selected title input paths, configured build,
save, and user-data roots, and the documented log, screenshot, oracle, compile-database, scratch, and
agent-state roots. It runs in every scope. The result names the exact longest path and which root
produced it, so a long external disc, asset, save, or user-data path is visible even when the checkout
itself is short. The estimate uses path metadata only; it does not walk private input trees.

Two limits are reported because they answer different questions. `MAX_PATH` is 260 including the
terminating NUL, so that is the length at which a Win32 call fails. The advisory limit is 240: a build
appends suffixes (`.o`, `.d`, `.exe`, `.tmp`) to directories it already holds, so a root that is merely
"under 260" can still fail during the build rather than when the user moves the checkout. When
`LongPathsEnabled` is off, crossing the advisory margin reports the exact path at risk and the number
of characters to remove. The check is advisory: it warns and never fails the doctor, so a deep but
working checkout stays usable.

## Exit status

| Code | Meaning |
| ---: | --- |
| `0` | no failures; warnings are allowed unless strict mode is enabled |
| `1` | one or more fail-closed checks failed |
| `2` | strict mode was requested and one or more warnings remain |

The JSON output includes a schema version, counts, individual results, remediations, and the exit
code. Consumers should use the structured fields rather than scraping the human text.

## Privacy and legal boundaries

The doctor never reads more private data than needed for local validation, and it never transmits
file names, hashes, bytes, or reports. JSON output can still contain local paths, so treat saved
reports as private diagnostic material unless paths have been reviewed and redacted.

The doctor does not determine whether a user is legally entitled to decrypt, adapt, or use a game in
a particular jurisdiction. It verifies only the project's technical input contract. Users must
supply their own copy of the game and are responsible for applicable copyright,
anti-circumvention, contract, and local-law questions. See `NOTICE.md` and
`docs/PUBLICATION_READINESS.md`.

## Deliberate non-goals

The first version does not:

- install MSYS2, Python, SDL3, or Vulkan;
- download game files, keys, firmware, or third-party binaries;
- decrypt `EBOOT.BIN` or PRXs;
- run PPSSPP or collect its dumps;
- extract XB archives;
- create a portable player package;
- certify the repository or a binary as legally cleared, secure, reproducible, or release-ready.

Those operations require separate, transactional workflows with their own provenance and tests.

## Commands the doctor sits beneath

Importing a disc, building a package and starting a game are done in the player (**BUILD PACKAGE**,
**PLAY**) and with `tools/nk_cli.py` (`prepare`, `build-package`, `launch`); the doctor is their
preflight. These questions about the workspace stay open:

1. bind the disc ID/version read from `PARAM.SFO` to every derived input;
2. make extraction interruption recovery and completeness machine-verifiable (containment and
   decode budgets are now repository-owned; `libxb` is no longer an extraction dependency);
3. determine whether a direct XB virtual filesystem can remove the expanded asset tree entirely;
4. separate build-time dependencies from the files required only to run a completed local build;
5. unify the flattened `fs/` and hierarchical `memstick/` mappings under one safe Memory Stick root — **DONE (issue #334)**: ordinary `sceIo*` and savedata share `SR_MEMSTICK` (default `memstick/`) via `sr_ms0_resolve`; `fs/` remains only as a read-only legacy-import source.

## Output roots and clean preview

`tools/nk_clean.py` is the preview-first workspace cleaner for issue #368. It plans deletions
against an explicit allowlist of repository-owned output roots and never selects anything
outside them:

| Root | Category | Contents and handling |
| --- | --- | --- |
| `build/`, `fixtures/psp_oracle/build/` | build and fixture outputs | generated objects, images, runtime products, and the PSP oracle fixture build. The cleaner allowlists these roots; it does not select other `fixtures/` content. |
| `logs/build_out_recomp.log`, `logs/build_err_recomp.log`, `logs/recomp_err.log`, `logs/obj_err.log`, `logs/link_err.log`, `logs/stdout_run.log`, `logs/stderr_run.log`, `link_err.log` | logs | named ephemeral build and run logs. Other log files, including oracle logs, are preserved. |
| `docs/ui-baseline/*.png`, `docs/ui-baseline/*.bmp`, `oracle/`, `*.trace`, `*.ppm` | screenshots and oracle evidence | local runtime screenshots and private oracle captures. Preserve them as evidence; the cleaner does not select them. |
| `tmp/`, `.tmp/`, `scratch/`, `.pytest_cache/`, `.ruff_cache/`, `.mypy_cache/`, `.cache/`, `htmlcov/`, `.coverage`, `.eslintcache` | tool and test scratch | disposable caches and indexes. Many Python tests instead use OS temporary directories and clean up their own fixtures on success; these are outside the repository cleaner. |
| `compile_commands.json` | compile database | refreshed by `nk_manager.ps1`. `python tools/compdb_relocate.py compile_commands.json --root <checkout>` rewrites checkout paths to root-relative form and sets the checkout-specific absolute `directory`; rerun it after moving the checkout. |
| `.claude/`, `.codex/`, `.codex-remote-attachments/`, `.cursor/`, `.gemini/`, `.jules/`, `.kilo/`, `.kilocode/`, `.opencode/`, `.windsurf/`, `.freebuff/` | agent state | ignored local databases, settings, and attachments. Preserve them; the cleaner does not select them. Git's linked-worktree metadata is managed by Git outside the checkout and is only inventoried by Doctor. |

`BUILD_ROOT`, `BUILD_DIR`, and `LOG_DIR` can redirect build and log outputs. Doctor includes those
configured paths in the path-length advisory. External `SR_DATAROOT`, `SR_MEMSTICK`, and
`--user-data-root` paths are also checked by name only; the cleaner does not manage those roots.

Everything else — source, assets, docs, keys, `place_game_here/`, `memstick/`, `fs/`, and any
unlisted file such as `logs/oracle_run.log` — is never selected.
Git-tracked files are always skipped, and reparse points (symlinks and Windows junctions)
are never followed or deleted, so a junction cannot smuggle a protected file into the
deletion set. A scan or containment failure fails closed with a nonzero exit and no
deletion.

The cleaner does not select every ignored or temporary path. Fixture builds belong under `build/`
or the specifically listed `fixtures/psp_oracle/build/`; other fixture source and outputs are not
implicitly disposable. Agent state directories and Git's linked-worktree administration are outside
the cleaner's deletion scope. Doctor reports worktree registrations and build accumulation without
removing them.

The default invocation is a dry run that only prints the grouped plan:

```powershell
.\nk.ps1 Clean
mingw32-make clean-preview
python tools/nk_clean.py
```

Deletion requires an explicit confirmation: `.\nk.ps1 Clean -Yes`, `CONFIRM=1` (which makes
`make clean-preview` pass `--yes`), or `python tools/nk_clean.py --yes`. An optional age
bound previews and deletes only files older than N days:

```powershell
mingw32-make clean-preview OLDER_THAN=7
```

`make clean`, `clean-fixtures`, `distclean`, and `clean-all` keep their existing pinned
semantics; `clean-preview` is additive.

### Paths outside the output roots

Make's build and run targets write only beneath `BUILD_ROOT` and `LOG_DIR`; the
`contrib-refresh` and export targets rewrite tracked controls by design, as the generators
below do. The fixed paths that the runtime and the tools read or write outside those roots
are listed here, each with the reason it stays. `OutsideOutputRootInventoryTests` in
`tools/test_workspace_paths.py` derives the runtime's list from the source and fails on a
name missing here or a row the source no longer names. It finds a `path` default in
`src/rt/env_registry.h`, an `fopen` of a literal name for writing, a `logs/`, `build/` or
`place_game_here/` literal anywhere in `src/`, and a file name with an output extension in
`src/rt`, bare or after `%s/`, self-test outputs aside. A name split around an
`inttypes.h` format macro is joined and listed with the macro's name in place, so
`"snap_f%05" PRIu32 ".ppm"` is `snap_f%05PRIu32.ppm`. Comments are not read. It cannot see a name the code assembles from
separate pieces at run time, or a literal default in code that `env_registry.h` does not
record. The tools table is not derived: the test checks each of its rows against its
tool's source, and requires every written path inside the checkout to be gitignored.

The runtime opens these relative to its working directory, the run folder. `make run` and
the private title routes run from the checkout, so there they land in or read from the
checkout. The player runs a title from its package folder, under the root it was given
(the per-user data by default), when the package is valid, and otherwise from that root.
Each written path is off until the Switch column's variables turn it on, except
`memstick`, which is created the first time the guest touches `ms0:` and written when it
saves. The column says which variable turns an output on and which only moves it. The
four display snapshot rows (`snap_%u`, `snap_v`, `frame_v` and `build/snapshots`) also
need a presenter, stop while `SR_FBDUMP` is nonzero, and stay off when `SR_FBSNAP` is `0`.

| Path | Switch | Direction | Why it stays |
| --- | --- | --- | --- |
| `flight-recorder.json` | on: `SR_FLIGHT`; moved by `SR_FLIGHT_OUTPUT` | written | The flight recorder's dump goes where the run is, so a crash report sits next to the run that made it. Gitignored at the checkout root, with its `.tmp` while it is written. |
| `samples.csv` | on: `SR_SAMPLE_SPEC`; moved by `SR_SAMPLE_OUT` | written | The state-sampling route's output, beside the run it measured. Gitignored at the checkout root. |
| `nidseq_mine.txt` | `SR_NIDLOG` | written | One line per HLE call for comparing call sequences between runs ([DEBUGGING.md](DEBUGGING.md)). Gitignored. |
| `edram.bin` | `SR_FBDUMP` | written | The 2 MB eDRAM image of a framebuffer dump. Gitignored (`*.bin`). |
| `fb_present.ppm` | `SR_FBDUMP` | written | The presented framebuffer of that dump. Gitignored (`*.ppm`). |
| `present_source.ppm` | `SR_FBDUMP` | written | The source image the presenter drew for that frame. Gitignored (`*.ppm`). |
| `fb_content.ppm` | on: `SR_FBDUMP` or `SR_GEDUMP` | written | The framebuffer at the end of a display list that drew textured pixels. Gitignored (`*.ppm`). |
| `snap_f%05PRIu32.ppm` | on: `SR_GESTAT` and `SR_FBSNAP` | written | The GE's snapshots, `snap_f<frame>.ppm`, at most one per 60-vblank `SR_GESTAT` window and 40 by default. Gitignored (`*.ppm`). |
| `snap_%u.ppm` | on: `SR_FBSNAP`, without `SR_FBSNAP_WINDOWS` | written | The display's rolling snapshots, eight names reused. Gitignored (`*.ppm`). |
| `snap_v%u.ppm` | on: `SR_FBSNAP_WINDOWS`; `SR_FBSNAP` sets the interval | written | The same, named by vblank inside the chosen windows. Gitignored (`*.ppm`). |
| `frame_v%u.ppm` | on: `SR_FBSNAP_WINDOWS`; `SR_FBSNAP` sets the interval | written | The capture policy's frame inside the chosen windows. Gitignored (`*.ppm`). |
| `build/snapshots/frame_%04u.ppm` | on: `SR_FBSNAP`, without `SR_FBSNAP_WINDOWS` | written | The capture policy's frames without windows, which `tools/ppmdiff.py` compares with `build/golden` by default; under `build/`, gitignored. |
| `tex_%08PRIx32_f%PRIu32_%dx%d.ppm` | `SR_TEXDUMP` | written | Each distinct texture a draw used, once per run. Gitignored (`*.ppm`). |
| `tex_%08PRIx32_f%PRIu32_%dx%d_a.pgm` | `SR_TEXDUMP` | written | That texture's alpha. Gitignored (`*.pgm`). |
| `vram_first.bin` | `SR_FIRST_FRAME_DUMP` | written | The first presented frame's VRAM, raw. Gitignored (`*.bin`). |
| `frame_first.ppm` | `SR_FIRST_FRAME_DUMP` | written | The first presented frame as an image. Gitignored (`*.ppm`). |
| `logs/ge_capture.ngef` | on: `SR_GE_CAPTURE_FRAME`; moved by `SR_GE_CAPTURE_PATH` | written | A GE command capture; under the run folder's `logs/`, gitignored. |
| `logs/trace_window.txt` | on: `SR_TRACE_PC`; moved by `SR_TRACE` | written | The PC trace window; under `logs/`, gitignored. |
| `logs/ge_transition_trace.jsonl` | `SR_GE_TRANSITION_TRACE` | written | The GE transition trace when the switch is `1`; under `logs/`, gitignored. |
| `ge_replay.ppm` | none | written | The `ge_replay` tool's image when no output is named on its command line. Gitignored (`*.ppm`). |
| `memstick` | moved by `SR_MEMSTICK` | read and written | The guest's `ms0:` root, saves included, in the ignored input root. |
| `fs` | moved by `SR_FSDIR` | read | The legacy flat `ms0:` folder: when the guest opens a file `memstick` lacks, a copy found here is imported into `memstick`. In the ignored input root. |
| `place_game_here/EXTRACTED/decrypted` | moved by `SR_MODULE_DIR` | read | The decrypted guest modules of a private title run, in the ignored input root. |
| `watchpoints.json` | moved by `SR_WATCHPOINTS_FILE` | read | The debugger's watchpoint list, read when present. Gitignored at the checkout root. |
| `assets/vfpu` | moved by `PSP_VFPU_TABLES` | read | The VFPU tables tracked in the repository. |
| `build/display-smoke-v1/staged` | none | neither | The folder named by the synthetic library card that the player's `ready-library` screenshot view shows. The card stays in memory and the folder is never opened. |

These names are joined to a folder the runtime is given or finds, never to the run folder
(the scan sees the ones with an output extension it knows):

| Name | Joined to |
| --- | --- |
| `crash_dump.bin` | the folder holding the running executable |
| `exited.flag` | the folder holding the running executable |
| `record.json` | the recording folder a video recording names |
| `record.json.tmp` | the recording folder, while `record.json` is written |
| `audio.wav` | the recording folder |
| `frames.csv` | the recording folder |
| `vram_%PRIu32.bin` | the folder a VRAM dump request names |
| `vram_%PRIu32.json` | that folder, the dump's GE state beside it |
| `gescene_f%PRIu32.bin` | the folder `SR_GESCENE_DIR` names, a GE scene dump |
| `recording.mkv` | the recording folder |
| `video.mkv` | the recording folder |
| `video.bgr0` | the recording folder |
| `logic.srt` | the recording folder |
| `code_capture_%08PRIx32_%016PRIx64.nkcap` | the folder `SR_CODE_CAPTURE` names |

`src/core/generated/nk_title_catalog.c` is not scanned. Its launch candidates
(`build/<name>/<name>.exe` and the image beside it) and its fixture memory-stick roots are
joined by `src/core/nk_launch.c` to the root it was given, never to the working directory,
and a fixture memory-stick root is created there.

Tools write these fixed paths outside `BUILD_ROOT` and `LOG_DIR`:

| Path | Tool | Why it stays |
| --- | --- | --- |
| `progress.json` | `tools/progress_tracker.py` | The HST progress snapshot, written at the checkout root beside the `build/hst` and `logs` it reads. Gitignored. |
| `visual_regression_report.json` | `tools/ppmdiff.py` | Written to the working directory, where the hosted visual regression step reads it. Gitignored at the checkout root. |
| `third_party/ghidra/projects` | `tools/ghidra_headless.py` | The local Ghidra project of a private title, inside the ignored `third_party/` tree; it never leaves the machine. |
| `third_party/ghidra/exports` | `tools/ghidra_headless.py` | That project's exports, beside it. |
| `%LOCALAPPDATA%\Nakagawa` | `tools/verify_installer.py` | The installer check plants marker files in the per-user data folders to prove an uninstall keeps them, and removes them and any folder it made. |

The generators that `python tools/regen_all.py --list` names write tracked files across the
tree by design: those files are their output, and the generated-file check proves them
current.

## Checkouts at an arbitrary path

An outside contributor clones the public repository wherever their tooling puts it, and a checkout
under a Windows profile directory whose name contains spaces is ordinary
(`C:/path/with spaces/nakagawa`). Such a checkout builds, passes
`mingw32-make native-core-tests`, `mingw32-make check`, `mingw32-make contrib-check` and the Python
tooling suite; `tools/test_relocated_clone.py` runs a bounded contributor command set from a copy of
the tracked tree placed under a directory whose own name contains spaces, so that property is tested
rather than assumed.

That check covers spaces, not every character. A checkout whose path contains non-ASCII characters is
not exercised by any hosted gate, and the selftest respawn sites that start a second copy of their own
binary only began quoting their command line, and decoding `argv[0]` in the active code page rather
than assuming UTF-8, in PRs #680 and #681. Treat such a path as unverified rather than supported.

Two boundaries remain, and both fail closed with a named message instead of a mysterious failure:

- a `BUILD_DIR` containing a space cannot be represented by GNU Make, so the Makefile refuses it
  before any recipe runs and names the remedy (a relative `BUILD_DIR`, or `NK_BUILD_ROOT`); see
  [SETUP.md](SETUP.md#checkout-path-and-build_dir).
- guest input pathnames keep travelling through the process environment rather than through recipe
  text, for the same reason; see [SETUP.md](SETUP.md) and the transport note in the Makefile.

## Linked-worktree and build-output accumulation

A long-lived checkout accumulates Git worktrees, and each worktree accumulates its own
`build/` tree. Neither is visible from inside a single checkout, so `Doctor -Scope repo`
inventories both without writing anything:

| Result | Reported when |
| --- | --- |
| `WORKTREES_COUNT` | more than 16 linked worktrees are registered |
| `WORKTREES_NESTED` | a linked worktree sits inside a checkout of the repository |
| `WORKTREES_UNREGISTERED` | a directory in the sibling `worktrees/` layout is not a Git worktree |
| `WORKTREES_STALE` | a registered worktree path is gone or prunable |
| `WORKTREES_BUILD_SIZE` | regenerable build output exceeds 10 GiB in total, or 5 GiB in one worktree |

These are advisory `WARN` results, never `FAIL`: carrying several lanes is legitimate, and a
diagnostic must not stop a build. The size scan is bounded (20 seconds and 200,000 files by
default) and says so when it stops early, so a reported figure is a lower bound, not a
promise. The remedy is always the same order: export the working-tree diff and a bundle of
the branch, remove the regenerable build output, then remove and prune the worktree. Doctor
never removes, prunes or writes anything itself.

## Build and workspace ergonomics

The state of each build and workspace ergonomics item:

1. [PARTIAL - #368] the Make recipes mix cmd and sh, and no Windows-native wrapper replaces them.
   Most recipes run under both. A recipe in sh syntax starts with `$(NK_REQUIRE_POSIX_SHELL)`, so
   when `mingw32-make` finds no `sh.exe` on `PATH` it stops before that recipe runs, where it used
   to hand sh syntax to `cmd.exe` (which failed late with an unrelated error, or exited 0 having
   done nothing). `RecipeShellGuardTests` in `tools/test_build_truth.py` reads Make's own rule
   database, with the `define` macros each recipe calls, and fails on a recipe in sh syntax
   without the guard, and on a guard over a recipe `cmd.exe` runs correctly. It sees only the
   conditional branches active on the host that runs it, and not a single-line variable that
   expands to shell syntax;
2. [DONE - #368] remove hardcoded machine paths from VS Code, `mem_debug`, and tool discovery:
   toolchain lookup honors an explicit `MSYS_PATH` (never `MSYSTEM_PREFIX`, which Git Bash points at its
   own MinGW), with documented defaults in `nk.ps1`, `nk_manager.ps1`, `tools/nk_doctor.py`, and
   `tools/nk_doctor_checks.py`. `tools/nk_cli.py`, `tools/copy_build_assets.ps1`,
   `tools/format_portability_check.py`, `tools/package_notices.py`, `tools/record_toolchain.py`, and
   `tools/stage_runtime_dlls.py` use the same override where they discover UCRT. `tools/mem_debug.py`
   requires an explicit `--exe` target and dynamically resolves `nm` rather than assuming HST or
   hardcoded directories, keeps the HST build directory only as the named `HST_DEFAULT_BUILD_DIR`
   fallback for the simulation mock state and the unattached artifact root, and fails with a clear
   error when a `--exe` target does not exist on disk;
3. [DONE - #368] make worktree asset resolution explicit and independent of the invoking
   checkout: `nk_cli.py prepare` now names its per-user data root as the preparation engine's
   base folder, where it used to fall back to the shell's working directory, so a compressed disc
   was expanded and a raw disc placed under whichever checkout ran the command
   (`ExplicitAssetRootTests` in `tools/test_workspace_paths.py`). The library classes
   `RuntimeLauncher` defaults to the checkout that holds the module and `PreparationEngine`
   requires an explicit base folder, so neither follows the working directory.
   `TwoCheckoutIsolationTests` in `tools/test_relocated_clone.py` builds a route of generated and
   runtime assets in two copies of the tree at once and checks that each built from its own
   marked source, into its own build folder, that no file it built names the other copy, and
   that neither wrote in the folder the two share. A location elsewhere on the machine is not
   watched, and the route involves no game input or per-user data;
4. [DONE - #368] add a long-path diagnostic to the workspace doctor:
   implemented in `tools/nk_doctor_checks.py` (`check_long_paths`, `query_windows_long_paths_enabled`)
   under code `LONG_PATHS`, reporting Windows `LongPathsEnabled` registry policy and both the
   MAX_PATH (260) limit and the 240-character advisory margin for the longest tracked, expected
   build, configured input, or configured output path, as an advisory `WARN` naming the path at risk;
5. [DONE - #368] define bounded artifact accumulation and a safe, scoped `clean-all` workflow:
6. [PARTIAL - #368] stop root-level artifact pollution and document the intended output roots: the
   `build` lifecycle tests no longer take their `BUILD_DIR` from the repository root, so they cannot
   make Make write `spaces/` litter into a checkout, and a whitespace-bearing `BUILD_DIR` is refused
   outright; `clean-preview` still selects only its bounded allowlist (the disposable roots in the
   table), and linked-worktree build output is now surfaced by `WORKTREES_BUILD_SIZE`. The fixed
   paths a run or a tool reads or writes outside `BUILD_ROOT` and `LOG_DIR` are one inventory,
   [Paths outside the output roots](#paths-outside-the-output-roots), each with its reason, and
   `OutsideOutputRootInventoryTests` derives the runtime half from the source and requires every
   written path in the checkout to be gitignored. The runtime still writes its debug outputs in
   its working directory, which is the checkout under `make run`;
7. [DONE - #368] eliminate absolute paths from generated `compile_commands.json`:
   `python tools/compdb_relocate.py compile_commands.json` rewrites every path under the checkout
   to a root-relative form and keeps only `directory` absolute, so a moved or cloned workspace
   repoints with `--root`;
8. [DONE - #368] audit and bound agent database/worktree accumulation:
   `check_worktree_accumulation` in `tools/nk_doctor_checks.py` inventories every registered
   worktree (count, nesting inside a checkout, unregistered directories in the sibling
   `worktrees/` layout, and stale paths) and the regenerable `build/` output under each one, with
   advisory bounds and a bounded scan; `tools/test_workspace_paths.py` covers every result.
