# Runtime Packaging Architecture

> **Status: CURRENT — maintained packaging decision record.** The process-isolation
> boundary, a local developer AOT package build, and the library's build-package →
> validate → launch route are implemented ([#487](https://github.com/Jstar269/nakagawa-recomp/pull/487)).
> `tools/nk_cli.py build-package <disc_id>` resolves the executable from a library disc
> image, or from the decrypted `EBOOT.elf` in the title's folder for an encrypted disc (the
> player decrypts it from the user's disc, #1494), and the
> player consumes the resulting package. The player checks each disc against Redump's PSP
> list, and the release ZIP carries the pinned build tools in `prereqs/`. Installers and a
> precompiled, compiler-free end-user route (#308) are not built.

## 1. Context and Goals

Nakagawa Recomp is evolving from developer-centric tooling into an authentic cross-platform PSP recompilation platform.
The ultimate end-user experience target is:

~~~text
Download/install Nakagawa Recomp
  → Select a PSP ISO you own
  → Instant recognition & authentic local preparation
  → Launch & Play
~~~

A crucial design decision is how the native player (`nakagawa_player`) and the game execution runtime relate to each other at packaging and execution time.

---

## 2. Evaluation of Architectural Options

### Option A: Monolithic Multi-Title Executable

A single executable contains the UI launcher, the generic PSP runtime, the AOT recompilation chunks for all supported titles, and all HLE/LLE subsystems.

* **Advantages:**
  * Single binary distribution (`nakagawa.exe` / `nakagawa`).
  * No IPC or child process management.
* **Disadvantages:**
  * **Violates title isolation:** Different PSP titles require distinct code chunks, per-title build settings and different executable span allocations.
  * **Binary bloat:** Recompiled C for a large PSP game generates tens of megabytes of native machine code. Bundling 10 games into one executable would result in gigabyte-scale binaries.
  * **Crash vulnerability:** A crash in a guest module immediately terminates the entire launcher application, destroying UI state and unsaved player configuration.
  * **LLE/Fidelity violation:** Guest memory arena mappings (e.g. 192 MiB fixed address reservations) would conflict if multiple titles or sessions were managed in the same address space.

### Option B: Core Engine + Dynamic Title Plug-in (`.dll` / `.so`)

The native player loads a generic runtime engine, which dynamically loads a title-specific shared library (`hst_recomp.dll`, `phase5_recomp.so`) providing the entry points and chunk dispatch tables.

* **Advantages:**
  * Clean modular separation between front-end UI, generic runtime, and title code.
  * Extensible without rebuilding the launcher.
* **Disadvantages:**
  * **Fragile ABI boundary:** Exposing C function pointer tables between host modules across different compilers (e.g. MinGW vs MSVC vs Clang) easily creates ABI mismatches.
  * **Shared address space risks:** Like Option A, guest memory arena collisions and unhandled guest exceptions still jeopardize host launcher stability.

### Option C: Isolated Process per Title + Native Host Launcher (Adopted Architecture)

> **Implementation boundary:** the public source validates and launches a prepared
> runtime as a child process, and the library's **BUILD PACKAGE** action creates that
> runtime from a staged disc entry with live progress. It does not create a runtime
> from an arbitrary ISO in one click; that, and the distribution work in Section 4,
> remain target work.

The native player (`nakagawa_player`) acts as an authentic front-end and library manager. When a game is launched, it constructs a typed `NkLaunchSession` and spawns the title's standalone recompiled runtime as an isolated child process via `nk_platform_spawn_process`.
Communication and handoff occur via:

* Clean environment variables: `PSP_ISO`, `SR_FPS_CAP`, `SR_GPU_GE=1`, `SR_DEBUG`.
* Process lifecycle tracking: native wait, status queries, and platform-specific termination (forced on Windows; SIGTERM with bounded grace and SIGKILL escalation on POSIX).

* **Advantages:**
  * **Total Fault Isolation:** A crash or abort in guest execution never brings down the launcher UI. The launcher detects child process exit, captures the exit code, and reports diagnostic facts cleanly to the user.
  * **Independent Optimization:** Each title binary is compiled with its exact manifest tuning (`RUNTIME_OPT`, `RECOMP_OPT`, custom span definitions) without polluting other titles.
  * **Sandboxing & OS Portability:** Native child processes follow standard OS process lifecycles on Windows, Linux (SteamOS/Steam Deck), and macOS.
  * **Boundary benefit:** Keeps the launcher and title runtime in separate processes;
    regression risk and title acceptance still require focused testing.

---

## 3. Concrete Implementation in `src/core/`

The adopted process boundary is implemented in the following public source slice; this
list is not evidence that a distributable installer or title package exists:

* `src/core/nk_types.h`: `NkGameEntry`, `NkResult`.
* `src/core/nk_platform.h`: `NkProcessHandle`, `nk_platform_spawn_process`, `nk_platform_is_process_running`, `nk_platform_wait_process`.
* `src/core/nk_platform_win32.c`: Win32 `CreateProcessW` with UTF-16 executable/command-line conversion, environment block generation, and wait handles.
* `src/core/nk_platform_posix.c`: POSIX `fork` + `execv` with environment configuration.
* `src/core/nk_launch.h` & `src/core/nk_launch.c`: typed `NkLaunchSession` candidate resolution, environment construction, and process lifecycle management.
* `src/player/ui/player_ui_nk.c`: the launcher's "PLAY" / "STOP GAME" action triggers.

---

## 4. Packaging and Distribution Roadmap

The per-OS choices (the shared install layout and manifest come
first and each OS is a thin wrapper around them):

| Target Environment | Packaging Strategy | Binary Artifacts |
| --- | --- | --- |
| **Windows x86-64** | Portable ZIP, and beside it an optional per-user `Setup.exe` with the same files ([#2208](https://github.com/Jstar269/nakagawa-recomp/issues/2208), [`SETUP.md`](SETUP.md#windows-installer-setupexe)). No MSI or MSIX | `nakagawa_player.exe`, `bin/nakagawa_core.dll` (or static), `build/<game>/<game>.exe`, SDL3.dll, SDL3_ttf.dll + resolved closure, `THIRD_PARTY_NOTICES/` |
| **Linux / Steam Deck** | Tarball plus a per-user `.desktop` entry, written by the same stager (`tools/stage_install.py`). No AppImage. No Flatpak: its sandbox conflicts with the host compiler the player needs | `nakagawa_player`, `bin/<game>`, `libSDL3.so` |
| **macOS ARM64** | Not supported; an App Bundle (`Nakagawa.app`) is the intended form | `Nakagawa.app/Contents/MacOS/nakagawa_player`, helper executables in `MacOS/` |
| **Android** | Not supported for play; a development APK holds the launcher and the showcase demos (see `ANDROID_DEVELOPMENT.md`). The user may move their own packages and saves to their own device | the APK's native libraries |

Roadmap items decided with the table:

* **C toolchain on Linux:** use the distribution's gcc, make and SDL3/Vulkan development packages,
  with guided install commands in the player. The project ships and downloads nothing for them.
* **Python on Linux:** the builder and `pyproject.toml` accept CPython 3.14 and 3.15
  (`src/player/package_builder.c`, `requires-python`). 3.15 stays the default. Widen further only to
  what the tool suite actually passes on, and only after the full suite runs green on that version.
* **One app ID on every host:** `nakagawa-recomp` names the repository and the release files. A
  fresh install's per-user data folder is `Nakagawa` on Windows, Linux and macOS, with `config`,
  `data` and `saves` inside it (section 8); Linux keeps its cache and logs in the XDG cache and
  state homes. A Linux or macOS install from before
  [#324](https://github.com/Jstar269/nakagawa-recomp/issues/324) keeps its `nakagawa-recomp` or
  `NakagawaRecomp` folders while they exist; no move is offered.
* **Code signing:** v0.0.1, v0.0.2 and v0.0.3 ship unsigned. Until a build is signed, each
  release publishes the ZIP's SHA-256 and GitHub build attestations for every binary, and the release notes
  explain the SmartScreen prompt. The signing procedure is prepared
  ([`RELEASE_SIGNING.md`](RELEASE_SIGNING.md#sign-the-windows-binaries-authenticode)) and waits for the
  maintainer's certificate, which stays a maintainer decision.

## 5. Local AOT package contract (v3)

The player accepts both package schema v2 and v3. Existing v2 packages keep
their generated-object list and completion manifest unchanged, and a valid v2
package with an unchanged cache key is reused as it is; new player builds write
v3 runtime packages.

The planner can run the existing analyzer, code generator, and two-phase Make
pipeline for a plaintext executable ELF. Run it from the repository root with the
documented UCRT64 toolchain:

~~~powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
python tools/title_codegen_plan.py assets/titles/my-title.json `
  --package `
  --game-elf place_game_here/EBOOT.elf `
  --output-dir "$env:LOCALAPPDATA\Nakagawa\data\developer-packages\my-title"
~~~

When the manifest requires PSP-header BSS metadata, also pass
`--psp-header <path>`. Guest PRXs are resolved by their manifest names beneath
`--module-dir <directory>`; required PRXs are selected automatically. Add one
`--include-optional-module <manifest-name>` for each optional PRX to include in
the AOT build. `--game-name` defaults to the manifest's `game_name`, then to its
portable `id`. The manifest's `codegen_profile` is authoritative. `--public-safe`
selects the synthetic/public-safe runtime backends for fixture builds.

The output directory must be dedicated and untracked. Retail package identity
contains local executable and module hashes, so the planner refuses to write a
retail package inside the repository; use the per-user data root as above or
the player's `build-package` action. The command refuses to reuse an existing
package directory when its manifest, executable, selected PRXs, or PSP header
hashes differ. The manifest remains unchanged. Make's `all` target still runs the generation and
compile phases separately so generated chunks are linked on a clean build.
A package build chunks generated C by size: `CHUNK_TARGET_BYTES` defaults to 4,000,000
bytes of C per translation unit (a value the caller sets wins, and the cache key names
it). Every function's C text, its order and the compile flags stay the same; only the
boundaries between translation units move (gcc inlines the runtime header's static inline
helpers into more functions of a smaller unit). Each compile job is then given memory in
proportion to the target (200 bytes per byte, at least 768 MiB, at most the 3 GiB that
count-based chunks need), so a host with little free memory still compiles several chunks
at once. The budget comes from measurement: one cc1 at `-O1` on 4 MB generated units peaked at
485 to 531 MiB (189 MiB for a 0.9 MB unit, #2284), so 768 MiB keeps a margin of 1.45 times. The
planner keeps 2 GiB for the player and the OS. When even one job does not fit beside that
reserve, the build still runs one job and its log says the compiler may fail; the compile
progress file names what bound the job count (`limit`: `memory`, `cpu`, `override`, `max`,
`background` for the Background speed's two-job cap, `unknown_memory` for the fixed two jobs
when the host gives no memory reading, and `unknown_cpus` for one job when it gives no CPU
count), and a one-job build tells the user in the player why it compiles one part at a time,
except for `unknown_cpus`, which names no cause.
The memory figures are GiB (1024^3 bytes), which is what Windows reports as GB.

Only a single body can make its unit larger than the target (#1730). Codegen gives every body
over `BODY_FALLBACK_BYTES` (4,000,000 bytes of C, the same in every build) a unit of its own
that compiles at `-O0`: the level most packages used before, at gcc's lowest time cost. The
level does not bound its memory, which grows with the body at every level (about 170 bytes per
byte of C, linear up to the 4 MB budget: a generated body chained to 4 MB compiles at `-O2` in
about a minute and 670 MB). The compile phase therefore budgets each job from the largest
generated unit at 256 bytes per byte (once it is over 125% of the target), without the 3 GiB cap, so a host runs fewer jobs
while such a unit is in the build. The generated `<game>_recomp_opt.mk` sets the unit's level
after `RECOMP_FLAGS`, so it is in the compile command and the object's cache key. It is a
required generated-code output, and any line in it other than a comment, a unit row or that
unit's level refuses the build before Make reads it. `build-report.json` lists each such unit
under `optimization.fallback_units`, with the level the rest compiled at. These are the only
compile flags that differ between units.
Current Make recipes cannot transport whitespace or shell-sensitive characters
in bound paths. Inputs at such paths are copied byte-for-byte into
`<output-dir>/staged-inputs/` (hashes are unchanged); staged guest PRX modules are
included. When the output directory contains spaces, packaging builds in a Make-safe
workspace (`NK_BUILD_ROOT` or Windows 8.3 short paths) and atomically promotes into the
destination; otherwise an unsupported path is rejected as `PACKAGE_UNSUPPORTED_PATH`.
Paths derived from that workspace are checked as written and never resolved again, because
resolving a short form expands it back to the spaced name (#2281).

An enhancement data root is read in place, not copied. The rule for it is the one for
every path Make cannot take: it is used in its short (8.3) form, or refused by name when
no short form exists, before Make runs. `NK_BUILD_ROOT` moves the build, not an input
directory, so it does not apply to the data root.

`package.json` is canonical JSON with this versioned shape:

| Field | Meaning |
| --- | --- |
| `format`, `schema_version` | `nakagawa-aot-package`, version `3` for new packages; the player continues to accept v2. |
| `title` | Manifest `id`, display name, kind, raw manifest SHA-256, and protected-semantics digest. |
| `inputs` | SHA-256 for the manifest, executable ELF, selected PRXs with load addresses, and optional PSP header. Absolute input paths are omitted. |
| `title_input_identity` | Versioned exact local inputs: manifest/profile and schema, DISC_ID/region/DISC_VERSION, selected PARAM.SFO facts, ISO container facts (format, volume ID, size, PVD sector and sector size; the other keys `inspect_iso` reports, such as the `format_warnings` notes and the `source_format` of a compressed image, are not part of the record), decrypted main executable name/hash, required guest PRX names/hashes, raw selected executable and required module hashes from their ISO members, and optional PSP-header hash/magic. Retail hashes remain only in per-user package data. |
| `runtime` | `CpuState` ABI version and header hash, resolved guest run entry, runtime contract, bindings, and required bindings. Packages built since [issue #2875](https://github.com/Jstar269/nakagawa-recomp/issues/2875) add `source_commit`, the runtime source commit (`{"commit", "origin", "reason"}`): from the install record `tools/stage_install.py` wrote beside an installed `source/` tree (`install-record`), else the checkout's `HEAD` (`git`), else `null` with origin `unknown` and the reason, never a guess. The player accepts packages without it; a malformed record is INCOMPATIBLE. A game's problem report names it on a `Runtime source:` line beside the player's commit; for a game that stopped, the report's `Package:` and `Runtime source:` lines name the package that launch validated, recorded when it started, never the library's current status ([issue #2998](https://github.com/Jstar269/nakagawa-recomp/issues/2998)). |
| `executable` | Relative native executable path and hash plus the guest ELF entry address. |
| `generated_objects` | Empty in v3 runtime packages. V2 packages retain their sorted object paths and hashes. Compiled objects stay in the bounded object cache; generated C stays in the AOT-stage cache for compatible rebuilds. |
| `required_local_assets` | Manifest title-data/resource roots, selected or optional guest PRXs, and host SDL3/SDL3_ttf/Vulkan runtime requirements. These are references; title assets are not copied into the package. |
| `build_report` | Relative path `build-report.json`. |
| `cache` | Versioned AOT/native cache key, codegen options, and runtime ABI compatibility decision. |
| `completion-manifest.json` | Completion marker containing the cache key, the exact title input identity, and SHA-256 for every file retained in the package; written last. |

The v3 player package contains the files needed to launch and validate the
title: its executable, runtime DLLs, generated guest image, declared guest
modules, runtime data/assets, notices, package/build reports, completion
manifest, and the small stub report used by the CLI diagnostics. Compiler
outputs, staged input copies, profile stamps/reports, and generated import/header
files are removed after a successful build, chosen only from the files the build
itself wrote: guest modules and assets staged beside them are never candidates,
whatever their names. The generated code is first kept in the AOT-stage cache,
recorded once when it is written; a stage the planner refused or that no longer
matches its record is replaced, never re-signed. The build report's
`artifacts.compiled_objects` still lists every object the build compiled and
linked. A v3 completion manifest is then written over the retained file set,
every guest module the package was built against must be present, and the
package is validated before promotion. This does not
weaken artifact checking: every retained file is still covered by the manifest
and hashed by the player. V2 packages continue to be checked with their original
manifest and generated-object rules.

The cache key includes the canonical `title_input_identity` digest. The same
record is stored under `<user data>/title-input-identities/<DISC_ID>/` and the
completion marker. Native package validation compares the package against that
current per-user record and the selected library entry's SFO revision before
launch. It reports changed classes such as `main executable`, a named guest
module, `SFO revision`, or `manifest/profile`; it never prints input hashes.
When a library entry names a source ISO, validation also hashes the selected
executable and each recorded required module directly from that ISO before a
cached result can be reused. This rejects changed source bytes even when
`DISC_ID` and `DISC_VERSION` remain the same, and launch repeats the check
against the selected ISO before starting the runtime ([issue #315](https://github.com/Jstar269/nakagawa-recomp/issues/315)).
Each check hashes a member at most once, even when the record names it twice.

A screen (the library list, the details, the picker's status) may instead use
`nk_title_manifest_validate_aot_package_for_display` or
`nk_title_manifest_identify_build_for_display`. These are a display shortcut,
not a security check. They may take a member's digest from an in-memory memo,
never written to disk, of digests the process computed in full. The memo is
keyed by the image's path, size, write, change and (Windows) creation times,
and its file ID or device and inode. A digest is stored only while those stay
the same across the hash and both times are at least 3 seconds old. Metadata
does not prove content: an image rewritten in place with its times restored,
which Windows allows, keeps its old answer on a screen. The launch path
(`nk_title_manifest_validate_aot_package` in prepare and at the pre-spawn gate)
never reads the memo and hashes every member in full. When a full hash
disagrees with the memo, the image's entry is dropped, so the next screen hashes
again and shows the new result.

Validation is memoized for the life of a process, so the memo is keyed on a
status identity that covers every input it must not go stale against: the
package metadata files, the selected executable and the runtime image derived
from it, the current per-user title input record, and every artifact the
completion marker publishes. Each of those files contributes its size, its
write timestamp, and the platform's change value (`ChangeTime` on Windows,
`ctime` on POSIX). When that value advances, it detects a same-size edit inside
one write-timestamp tick, where size and write time are unchanged ([issue #683](https://github.com/Jstar269/nakagawa-recomp/issues/683));
it only ever turns a would-be hit into a revalidation. These values are
filesystem timestamps, not generation counters: resolution and support vary,
and an edit inside both timestamp granularities can still produce a cache hit.
Where a filesystem reports no change value, or a file cannot be opened for its
metadata, the identity falls back to size and write timestamp. Guaranteed
byte-level detection would require hashing package contents on every lookup.

`build-report.json` has format `nakagawa-build-report`, schema version `1`, the
same `input_hashes`, Python/planner/analyzer/codegen/Make versions, the compiler
identity, runtime ABI, the same runtime source commit record as `runtime_source` (optional), and deterministic coverage counts (`analyzed_functions`,
`aot_entries`, callable/resume entries, fallbacks, and unsupported imports,
instructions, and regions). `coverage.static` is the total of the code generator's
`<game>_recomp_coverage.json`: owned, unowned, translated and interpreter-only words and
functions from the canonical CFG ([`DEBUGGING.md`](DEBUGGING.md#execution-share-and-static-coverage));
it is `null` only for a generation stage that predates the report. `unsupported.imports`, `unsupported.instructions`,
and `unsupported.regions` are always arrays. Each listed boundary includes a
reason, status `in the works`, and its tracking issue: import coverage uses #71;
unsupported Allegrex translation and AOT function regions use #118. Empty arrays
mean the analyzer/code generator did not report that boundary for these inputs;
they do not establish title acceptance. These records do not patch behavior:
generated unsupported functions retain the runtime's fail-closed path.

`build-report.json` is the one package artifact whose size grows with the title,
because it carries one record per AOT gap, unsupported instruction and analysis
diagnostic; a measured flagship report is about 1.3 MB. The package reader
therefore bounds it at 4194304 bytes and 262144 structural nodes instead of the
1 MiB ceiling that applies to `package.json`, the completion manifest and the
title input identity record, and the planner refuses to write a report larger
than that ceiling rather than leaving it to fail validation. The native runtime
validator still caps the same artifact at 262144 bytes, so a report between the
two sizes builds but is rejected there with `build-report.json is larger than
the supported JSON limit`; raising that native cap is a separate open gap.

This package command still requires the developer toolchain. It takes a plain
executable and does not unwrap a disc itself; `tools/nk_cli.py build-package
<disc_id>` performs the library-disc extraction and the player consumes the
resulting package. Checkout-independent run-directory provisioning landed
under #297, and the analyzer ownership and stack-balance gates under #291.

### Host runtime DLL staging (#421)

Every Windows route that ships or runs a player stages the same host runtime
DLLs with one mechanical step, `tools/stage_runtime_dlls.py`: `SDL3.dll` and
`SDL3_ttf.dll` (each overridable via `SDL3_DLL` / `SDL3_TTF_DLL`, otherwise
resolved from `C:/msys64/ucrt64/bin`), plus the SDL3_ttf dependency closure
resolved by walking PE import tables recursively and stopping at Windows system
DLLs. The step runs from the Makefile's `player` target (beside
`build/nakagawa_player.exe`) and from `tools/nk_cli.py build-package` (into each
built package). It never stages a partial closure: when an import is
unresolved, it names it. The player target then fails. A package build still
succeeds with its required SDL3, libiconv and Vulkan runtimes, and prints the
unresolved import as a warning. The prerequisite installer pins SDL3_ttf and its
closure (`assets/prereq_manifest.json`), so this happens only on a host without
them; that player's launcher cannot start, and it says why and exits with code 4. Licence texts for
every staged DLL are copied beside them as `THIRD_PARTY_NOTICES/` by `tools/package_notices.py`, driven by
`assets/third_party_components.json`. Because the closure sits beside the
executable, launching from Explorer or a plain `cmd.exe` without MSYS2 on `PATH`
uses the launcher's TTF fonts; there is no bitmap-font fallback, and the workspace doctor
reports `RUNTIME_SDL3_TTF`/`RUNTIME_SDL3_TTF_CLOSURE`.

## 6. Private content-addressed cache contract (#316)

A package is reusable only when every semantic input that can affect its output
is identified. The package layer, rather than a project release string, owns the
following key components:

| Key component | AOT generated C | Native objects/runtime package |
| --- | --- | --- |
| Exact executable bytes SHA-256 | required | required |
| Validated manifest/profile, exact title-input identity, and selected guest-module/PSP-header digests | required | required |
| Analyzer/codegen semantics epoch and analyzer/codegen content digests | required | required |
| Codegen options (profile, base/entry, spans, chunking, selected options, correctness modes) | required | required |
| Generated-code ABI epoch | required | required |
| Runtime ABI epoch | compatibility decision | required |
| Compiler identity (driver, version, `cc1`/`as`/`ld` content hashes) and target triple | not required | required |
| Runtime source/header digest (section 7) and output-affecting compile/link flags | not required | required |

The current package-layer epochs are `analyzer-codegen-v1`,
`GENERATED_CODE_ABI_EPOCH = 3`, and `RUNTIME_ABI_EPOCH = 2`. The generated-code
and runtime epoch values are compatibility contracts, not release versions. A
change to the generated C/runtime helper boundary must bump
`GENERATED_CODE_ABI_EPOCH`; a runtime-package ABI change must bump
`RUNTIME_ABI_EPOCH`. The CpuState layout (`SR_CPUSTATE_ABI_VERSION` in
`src/rt/recomp.h`) is part of both boundaries: generated C addresses CpuState
fields by offset, and the runtime package shares the struct with the generated
objects, so every CpuState version bumps both epochs. `CPUSTATE_ABI_EPOCHS` in
`tools/nk_core/package_cache.py` records the pairing (CpuState v2: epochs 1/1;
v3, which appended `llbit`: generated-code epoch 3, runtime epoch 2). The
generated-code epoch can also advance on its own, for example for the SHA-256
module identity (#969). The native validator carries the twins
`NK_AOT_GENERATED_CODE_ABI_EPOCH` and `NK_AOT_RUNTIME_ABI_EPOCH` in
`src/core/nk_title_manifest.h`, and `tools/test_package_cache.py` pins the header
version, the table, the constants and the twins to each other. A package whose
cache key names an older epoch is `aot-regenerate` for the tools and
`NK_RUNTIME_PACKAGE_INCOMPATIBLE` for the player (the same outcome the
codegen/runtime digests and the package `abi_version` already force; the epochs
make the contract explicit). The package layer documents the bump because the
codegen agent owns the implementation epoch source.

The cache decision has three levels:

* `reuse`: the complete AOT and native key matches. No AOT generation or native
  compilation is run.
* `native-recompile`: generated C is reusable, but a compiler, target, runtime
  source, flag, or explicitly compatible runtime ABI change requires native
  objects to be rebuilt and relinked.
* `aot-regenerate`: executable bytes, manifest/bindings, analyzer/codegen
semantics, codegen options, title-input identity, or an incompatible generated/runtime ABI requires
  fresh AOT output.

A runtime ABI change reuses generated C only when the package layer explicitly
maps the previous epoch to the current epoch as compatible. Unknown or missing
compatibility is never treated as compatible. The decision record names every
changed component (`aot:executable_sha256`, `native:compiler_identity`, and so
on) for the CLI, player preflight, and Doctor.

The private cache root is `<user data>/cache/packages/<DISC_ID>/`, never the
repository. Runtime packages are written to a content-addressed entry below that
root, assembled in a staging directory, and promoted with a directory rename.
On the same volume, the installed package hard-links its files to the cache
entry; a cross-volume promotion copies them. Cleanup protects every cache entry
named by an installed package variant and retains at most one unreferenced
previous entry per disc. AOT-stage directories share generated C by its
generated-code digest and keep the active stages plus at most one previous
stage. Cleanup never follows symlinks. The completion manifest contains the
canonical cache key and a SHA-256 record for every retained package file.
Because hard-linked package and cache files share bytes, damage to an installed
artifact also invalidates its cache copy. The next build verifies the AOT stage
and rebuilds from the current title inputs/object cache; it never promotes the
damaged entry.
Missing, incomplete, symlinked, or digest-mismatched completion entries are not
valid packages and cannot be launched. The player and launcher report the
failed component and the `build-package <DISC_ID>` action instead of falling
back to a possibly stale executable.

The cache contains private title-derived bytes and is intentionally outside the
public export and provenance tree. Publication policy remains default-deny for
`cache/` paths; a cache artifact accidentally proposed for publication is
rejected by the public-scope audit.

### Shared compiled objects (#1306)

Below the package entries, the compile phase shares objects between builds. Before
Make compiles, the planner keys every object it plans from the compiler identity
(driver hash and `gcc -v` report), the exact compile command with the build folder
written as a placeholder (so the optimization level and every flag count), and the
preprocessed source. A cached object with that key is copied into place, and the
compile writes the object's dependency file as Make would, so a later header change
still rebuilds it. An object that is compiled is stored only when its key, taken
again after the build, has not changed. The headers the build generates
(`make compile-inputs`) are written before any key is taken, and a key that still
cannot be taken is a miss, never a guess. Keys are taken on one worker per logical
CPU, with no fixed ceiling, not on the Make job count: a key only preprocesses its source, which
needs a small fraction of the memory a compile of that source needs, so a build that
memory limits to one compile job still keys its objects in parallel. The worker count keeps the
memory budget too: the free memory after the reserve, at 128 MiB a worker, bounds it, and
Background keeps to its own job count. No compile carries
the build folder: the runtime finds its own folder at run time for its exit
diagnostics. The runtime
therefore compiles once and serves every game, and a rebuild of an unchanged game
compiles nothing.

* `<user data>/cache/objects/` holds runtime and game objects.
  `NK_OBJECT_CACHE_MAX_BYTES` bounds it (default 2 GiB, least recently used first).
  `NK_OBJECT_CACHE_DIR=0` turns reuse off for one build.
* `<user data>/cache/shared-libraries/` holds the system libraries a disc carries.
  Codegen writes a runtime-placed module that the build carries once to its own
  `nk_lib_<fnv1a64>.c`, named by the module's bytes, with symbols `l<fnv>_<link>` and
  `sr_l<fnv>_base` and nothing that names the game. A module over the library budget
  (`LIBRARY_UNIT_TARGET_BYTES` of C or `LIBRARY_UNIT_MAX_FUNCS` bodies, fixed in codegen
  and independent of the build's own chunk settings) is cut into
  `nk_lib_<fnv1a64>_<n>.c` units instead, each with every declaration (#1730). Another game with the same module
  compiles the identical file, and the build log names each library it reused or
  compiled. Settings > BUILD TOOLS AND CACHE turns this sharing off
  (`build-package --no-shared-libraries`) and clears the folder. A module carried
  twice in one build, or a stack-census build, keeps its indexed `m<k>` names inside
  the game's chunks.

Neither folder is shipped, uploaded or shared: both stay in the player's data
folder like the package entries.

### Packages built by an older Nakagawa (v0.0.2)

The package format, the cache schema and both ABI epochs are 2 in v0.0.1 and
v0.0.2, so none of them tells a v0.0.1 package from one v0.0.2 builds. What
does differ is the part of the cache key that comes from Nakagawa itself. Every
package since v0.0.1 records it, and the native validator already requires it:

| Value | Where the package records it | What it covers |
| --- | --- | --- |
| `runtime_source_digest` | `cache.key.native.components` | `src/rt`, `src/core`, `src/ui` and the third-party headers they include (`third_party/clay`, `stb`, `portable-snippets`), the `Makefile` and `mk/*.mk`, `assets/vfpu` (`package_cache.source_tree_digest`) |
| `analyzer_sha256` | `cache.key.aot.components` | `tools/analyze.py` and `tools/analyze_cfg.py` (`package_cache.analyzer_sources_sha256`) |
| `codegen_sha256` | `cache.key.aot.components` | `tools/codegen.py` |
| `planner_sha256` | `cache.codegen_options` | `tools/title_codegen_plan.py` |

Measured on the tree: `source_tree_digest` gives `d1de9386...0beb` at the
v0.0.1 tag and a different value on main, and the three tool hashes changed
too. The player computes the same four values from the source tree a rebuild
would use (the folder above `tools/nk_cli.py`: a checkout, or a release's
`source/`), in C (`nk_title_manifest_source_build_identity`, byte for byte
the Python digest; `tests/native/test_player_state.c` and
`tests/native/test_player_ui.py` pin the two together). It reads a package's
values from `package.json` alone (`nk_title_manifest_aot_package_build_identity`).
A package whose values differ is *outdated*: rebuilding it compiles this
version's runtime, and regenerates its C when the translator changed. These
are exactly the components the build tools compare before they reuse a
package, so "outdated" means "a rebuild would change it".

An outdated package is not incompatible. Nothing in the format changed, the
validator accepts it, and it launches. A package that records no identity
(no planner digest, as hand-made fixtures have) is not judged at all.

**How a future release bumps it: it does not have to.** Any change to those
files moves the identity, so every release that changes the runtime or the
translator marks the packages the previous one built, and a release that
changes neither leaves them alone. A development checkout behaves the same
way: its packages read as outdated after a runtime edit, because they lack it.
Changes that must not trigger a rebuild stay out of those files.

What the player does (it suggests a rebuild and never forces one):

* At start it scans the library on a worker. With outdated games and a
  `settings.json` whose `rebuild_prompt_version` is not this player's version,
  it shows one prompt naming the count, with Rebuild all and Later. Either
  answer records the version, so the prompt shows once per update.
  `rebuild_prompt_version` is an optional key: a file without it has never
  shown the prompt.
* After Later, a library line ("N games were built by an older Nakagawa.")
  offers Rebuild all and Hide. Hide lasts for the session; the line comes back
  at the next start while outdated games remain.
* Each outdated card says "Built by an older Nakagawa. Rebuild for the latest
  fixes.", and the title page offers Rebuild beside PLAY. PLAY asks once a
  session (Rebuild now or Play anyway); Play anyway launches as before.
* Rebuild all queues the outdated games in the build queue, which builds one
  game at a time, behind whatever the user is doing, through the same package
  builder as BUILD PACKAGE. The library line counts ("Rebuilding 2 of 5", or
  "Building 2 of 5" once the queue holds a first build) and each card shows
  its state, its place in the queue and its compile progress. A game can be
  played until its own rebuild starts; while it runs, that game cannot launch
  and its card says why. A game whose disc image is missing is skipped and its
  card asks for the disc again; a failed or cancelled build is noted on its
  card and the queue moves on.
* The same queue takes first builds: a game with no package offers Add to
  queue beside BUILD PACKAGE on its page, and Remove from queue while it
  waits. The library line's Queue opens the queue: the game building first,
  then the ones that wait, each with Up, Down and Remove, and one Pause button
  that becomes Resume while the queue is paused. Pause lets the build that
  runs finish (a build cannot be suspended halfway; Cancel build stops one),
  then no next game starts until Resume; the screen's title, lead and hint say
  so, and Add to queue or Rebuild all add games without resuming it. The queue
  also pauses by itself, with Resume, when a build could not start for a reason
  outside its game. The build card says how many games follow.
* The queue survives a restart in `rebuild-queue.json` beside `settings.json`:
  `{"format": "nakagawa-rebuild-queue", "schema_version": 2, "titles":
  [{"disc_id": "...", "kind": "build" | "rebuild", "state": "pending" |
  "done" | "missing_disc" | "failed"}]}`, in queue order. A build that was
  running is saved as `pending` and starts again. A schema 1 file (no `kind`,
  every title a rebuild) is read as rebuilds and rewritten as schema 2. A file
  with another format or an unknown member is left in place and not resumed;
  a file with a newer `schema_version` is not resumed and is never replaced or
  removed. The file goes when nothing is pending.
* The person's Pause is the marker file `rebuild-queue-paused` beside it, there
  only while a queue is paused; its presence is the whole record and its bytes
  are never read. A paused queue's build that was running starts again only
  after Resume. The queue file keeps the shape every released player reads, so
  an older player resumes the queue and ignores the pause. A marker with no
  queue file beside it (an older player finished the queue) is removed at
  start.

A rebuild never removes the old package first. The build runs in the
content-addressed cache entry, is validated, copied next to the package,
validated again, and only then swapped in, with the old package moved back if
the swap fails (`_promote_cache_entry` in `tools/nk_cli.py`). A failed or
cancelled build therefore leaves the old package in place and launchable. The
one gap is a build stopped between the swap's two renames, which leaves the old
package in `cache/packages/<DISC_ID>/previous-<package folder>-<n>`: the next
build of that package moves it back before anything else
(`_restore_interrupted_swap`). Every package of a disc (the authentic one, an
enhanced one, a code-hook one) shares that cache folder, so a backup is named
for its own package folder and is only ever moved back into it.
Saves live outside the package and are not touched, and the cache keeps its
reuse: when only the runtime changed, the generated C is kept and only the
native objects are rebuilt.

### Generated-code stage reuse (bring-up)

`nk_cli.py bringup` generates each title's code once. Its codegen stage runs
inside the package build, after the package's preflight and cache-key
computation, as a planner call with `--aot-stage`: the same planner command,
inputs, title input identity and Make environment as the package build,
running only Make's generation phase (`pipeline`) into the work directory's
empty `codegen-stage/`. The stage writes the package completion manifest last,
holding the build's cache key, the title input identity and the SHA-256 of
every file it contains.

The package build then receives `--reuse-aot-stage codegen-stage/`. It compiles
the stage's output with `NK_AOT_PREGENERATED=1` (Make's `compile` phase) only
after the checks the native-only reuse of a package's generated C performs:
the completion record must be complete and every recorded artifact intact,
with no file added or missing, and the generated-code components of its cache
key must equal the build's own (`compare_cache_keys`, generated C reusable).
Each copied file is hashed as it is written and compared with the record, so
a stage that changes after its check is refused rather than compiled. Any
refusal regenerates the code from source with Make's `all` and is named:

| Reason | Cause |
| --- | --- |
| `AOT_STAGE_RECORD_MISSING` | The stage or its completion record is missing (an interrupted stage). |
| `AOT_STAGE_RECORD_INVALID` | The record is unreadable, incomplete, or inconsistent with its key. |
| `AOT_STAGE_ARTIFACT_MISMATCH` | A recorded file changed, is missing, or a file was added. |
| `AOT_STAGE_INCOMPLETE` | The record does not cover a required generation output. |
| `AOT_STAGE_INPUT_CHANGED` | Executable, manifest, module, PSP header or title input identity differ. |
| `AOT_STAGE_GENERATOR_CHANGED` | Analyzer or code generator content, or an epoch, differ. |
| `AOT_STAGE_OPTIONS_CHANGED` | Codegen options (profile, spans, chunking, user arguments) differ. |

The planner prints its decision as one `AOT_STAGE_REUSE:` line before Make
runs, and the sanitized bring-up report records it as `codegen_reuse`
(`REUSED`, `REGENERATED` with the reason, or `NOT_RUN` when a valid package
already held the key's generated C). The report's
`counts.unsupported_opcodes` is read from the stub report inside the built
package, so it describes the code that shipped; only a build that fails
before packaging falls back to the stage's stub report.

## 7. Per-user data formats and versions

A user who updates Nakagawa keeps every library entry, setting, mapping and
save, and rebuilds a package only when something the package was built from has
changed. The files Nakagawa writes for the user carry a version where Nakagawa
owns the format. `library.json`, `settings.json` and `input_profile.json` share
one writer rule (`src/core/nk_user_data_schema.h`); the runtime's
`registry/system.json` overlay is versioned separately and is the exception to
it:

* An **older** file is read and carried forward one version at a time
  (v1 to v2 to the current version). It is not rewritten on load; the first save
  first copies the original, byte for byte, to `<file>.v<version>.bak`, and does
  not write when that copy fails.
* A **newer** shared-writer file, written by a later player, is refused by
  name when its version is readable and at or below 1,000,000: the session runs
  on defaults or an empty library, the screen says why, and every direct save
  over it is refused with `NK_ERROR_NEWER_SCHEMA`. `library.json`'s normal
  player commit is stricter: it rereads the library and refuses any readable
  newer primary, including above 1,000,000. A version above 1,000,000 is outside
  the direct writer's probe, so that direct-save refusal and the versioned
  backup do not apply (see below). When the file is refused, returning to the
  newer player finds it as it left it.
* A file that is there but cannot be opened or read (an I/O failure; this is not
  the same as a version above the probe) is refused by the direct writer: its
  version is unknown, so the direct save is refused with `NK_ERROR_IO` and the
  file is left as it is. The Controls page's first save is the exception
  described below: it moves an unreadable primary to `.unreadable.bak` and then
  writes a replacement.
* A shared-writer file over the size bound the reader holds it to is first
  copied, byte for byte, to `<file>.unreadable.bak` (a later player may allow a larger file), and
  the save does not write when that copy fails.
* A shared-writer file with no readable version (empty, damaged, not JSON) is
  reported, and the reader falls back as before (`library.json` to its `.bak`, the others to defaults).
  A controls file that does not load for this or any other reason except a newer
  version is moved to `input_profile.json.unreadable.bak` by the first save, which
  does not write when that move fails, and the Controls page names the copy.

The direct writer's probe reads `schema_version` only when it is an integer
from 0 through 1,000,000 (`nk_user_data_file_version`,
`src/core/nk_user_data_schema.h:64-100`). A larger integer is
`NK_USER_DATA_VERSION_UNREADABLE` to the direct writer: it gets no versioned
backup, and `nk_user_data_prepare_replace` treats it like any other unreadable
version and lets the direct write proceed (`:151-163`). The three readers parse
the version themselves, so they can still refuse a larger value as newer; the
ceiling is a bound on the direct writer's probe, not a reader format limit. No
migration or versioned backup is defined for a version above it.

The folders are the per-user ones of [`SETUP.md`](SETUP.md) (on Windows
`%LOCALAPPDATA%\Nakagawa\data` and `...\config`, plus the saves folder).

| File | Version field and history | An older file | A newer file |
| --- | --- | --- | --- |
| `<data>/library.json` (`src/core/nk_library.c`; read too by `tools/nk_core/library.py` and `nk_cli`) | `schema_version` 1, the only version so far (v0.0.1 and later). `source_image_path`, added after v0.0.1, is an optional member v0.0.1 skips | (none exist) | Refused by the reader for any readable newer version, including above 1,000,000; the Library screen names it. The normal player commit (`library_commit`) rereads the library and refuses any readable newer primary, including above 1,000,000, before writing. The older `.bak` beside it is not loaded in its place. A direct `nk_library_save` is limited by the 1,000,000 writer probe, so it can replace a primary above that unless the `.bak` has a readable version above 1 and at or below 1,000,000; it makes no versioned backup |
| `<config>/settings.json` (`src/player/player_state.c`) | `schema_version` 1 (before v0.0.1), 2 (v0.0.1 and later) | 1 to 2: an `fps_cap` of 30 or 60 becomes PSP timing; `settings.json.v1.bak` on the first save | Refused by the reader for a readable version at or below 1,000,000; defaults, and every Settings page says changes are not saved. Above 1,000,000 the reader still sets the newer-format notice, but the writer's probe does not see that version, so the shared guard does not refuse a save |
| `<config>/input_profile.json` (`src/core/nk_input_profile.c`) | `schema_version` 1, 2 (per-title map, #520), 3 (keyboard keys, #1299; v0.0.1 and later), 4 (analog stick keys), 5 (mouse aiming, #1791), 6 (mouse buttons as keys, #1791) | 1 to 2: no per-title map; 2 to 3: default keys; 3 to 4: default stick keys (I, K, J, L), with a direction left unbound when its default key is already a button's key; 4 to 5: aiming off; 5 to 6: nothing changes. `.v1.bak` to `.v5.bak` on the first save | Refused by the reader for a readable version at or below 1,000,000; the Controls page names it and its changes do not replace it in that case. Above 1,000,000 the reader still calls it newer, but the writer's probe does not, so the save path can replace it |
| `<saves>/<DISC_ID>/` | The PSP's own Memory Stick layout (`PSP/SAVEDATA/...`), defined by the PSP rather than by Nakagawa; the root rule (`nk_launch.c`) is unchanged since v0.0.1 | n/a | n/a |
| `<data>/packages/<DISC_ID>/package.json`, `build-report.json`, `completion-manifest.json` | `nakagawa-aot-package` schema 3 (the player still reads v2), cache key and completion schema 2, and the ABI epochs of section 6 (the cache and completion schemas are unchanged since v0.0.1) | Built output, not user data: a package from another epoch is INCOMPATIBLE and its card offers REBUILD PACKAGE | INCOMPATIBLE with the card line "built by a newer Nakagawa (package format N; this player reads 3)". The player never changes it; only a rebuild the user asks for replaces it |
| `<data>/title-input-identities/<DISC_ID>/title-input-identity.json` | `schema_version` 2 | Rebuilt by the next package build | Read as absent; the package card asks for a rebuild |
| `<data>/fonts/v2/manifest.json` | `schema_version` 2, also named by the folder | n/a (v0.0.1 wrote `v2`) | A later layout gets a folder of its own and leaves `v2` as it is |
| `<data>/experimental/<DISC_ID>/profile.json` | `schema_version` 1 | n/a | Not accepted: the package check fails closed |
| `<data>/registry/system.json` (the PSP system settings a game changes; `src/rt/hle.c`, `SR_SYSTEM_REGISTRY` overrides the path) | `schema_version` 1, the only version so far (v0.0.1 and later) | (none exist) | Ignored: the game sees the modeled defaults, and the next registry flush (at start-up, when the launcher's host time zone is seeded) moves the file to `system.json.corrupt` (kept, not deleted) before writing a fresh one |

### The four files in detail

The table above is the summary. This one separates what each reader does for a
file that is older, newer, missing or invalid, and names the live constant and
reader. For the three shared-writer files, a **refusal** is an error the direct
writer will not replace: a readable newer version at or below 1,000,000, or a file it
cannot open or read. A version above 1,000,000 is not a refusal by the direct
writer; its probe treats it as unreadable and the direct save may replace it,
even though the reader can still call it newer. `library.json`'s normal player
commit is stricter: it rereads the library and refuses any readable newer
primary, including above 1,000,000. The Controls page's first save can move an
unreadable primary to `.unreadable.bak` and write a replacement. The registry
has no newer-file refusal: an invalid or newer overlay is moved aside at the
next flush and replaced. A
**fallback** is defaults in memory, after which a later save may replace the
file. A **backup** is a copy the writer makes before replacing it. The
`library.json` row is the player's C reader; `tools/nk_core/library.py` reads the
same schema version and refuses a newer file, but it has no `.bak` recovery and
makes no versioned backup.

| File and source | Version | An older file | A newer file | A missing file | An invalid file |
| --- | --- | --- | --- | --- | --- |
| `library.json` (`nk_library_load`, `library_load` and `nk_library_save` in `src/core/nk_library.c`; `NK_LIBRARY_SCHEMA_VERSION` in `src/core/nk_library.h`) | 1, the first and only version (v0.0.1 and later). `source_image_path`, `content_sha1`, `content_crc32` and `disc_verification` are optional members an older reader skips | No older version exists and there is no migration table. A version below 1 is not carried forward: it fails as invalid, so the `.bak` recovery in the last column applies | `NK_ERROR_NEWER_SCHEMA`, an empty library, and `newer_schema_version` set for any readable newer primary, including above 1,000,000. The normal player commit (`library_commit`) rereads the library and refuses it before writing. The `.bak` is not read in its place. A direct `nk_library_save` writes nothing while the primary or `.bak` has a readable version above 1 and at or below 1,000,000; above 1,000,000 its probe treats the primary as unreadable, so that direct save is not refused unless the `.bak` is itself a readable version above 1 and at or below 1,000,000, and no versioned backup is made | `NK_OK` with an empty library. When the primary is missing, a `.bak` beside it is read as the library | A parse failure, a missing `games` array, or a version below 1: the `.bak` is read; if that fails too, an error and an empty library. A file that is there but cannot be opened or read is not damaged: the `.bak` is not used instead, and nothing is written over it |
| `settings.json` (`player_app_load_settings` and `player_app_save_settings` in `src/player/player_state.c`; `NK_PLAYER_SETTINGS_SCHEMA_VERSION` in `src/player/player_state.h`) | 2 (v0.0.1 and later); 1 is the format before v0.0.1 | 1 to 2 in memory: an `fps_cap` of 30 or 60 becomes PSP timing (`-1`), and an explicit 0 stays uncapped. The file is not rewritten on load; the first save keeps it as `settings.json.v1.bak` | Defaults in memory, `settings_newer_schema` set, and a notice naming the format. The shared writer refuses the save with `NK_ERROR_NEWER_SCHEMA` when the version is readable at or below 1,000,000; above that it treats the version as unreadable and does not refuse the save | Defaults, no notice, `NK_OK` | A missing, non-integer or below-1 `schema_version`: defaults and "Unsupported settings schema version; defaults restored." A corrupt or non-object file: defaults and "Settings file is corrupt; defaults restored." A zero-byte or over-1 MiB file: defaults and "Settings file has invalid size; defaults restored." A file that cannot be opened or read: defaults, `settings_unreadable`, and no save over it in that session |
| `input_profile.json` (`nk_input_profile_file_load` and `nk_input_profile_file_save` in `src/core/nk_input_profile.c`; `NK_INPUT_PROFILE_SCHEMA_VERSION` and the per-member constants in `src/core/nk_input_profile.h`) | 6 (mouse buttons, #1791); 5 added `aim` (#1791), 4 stick keys, 3 keyboard keys (#1299), 2 the per-title map (#520), 1 the first global mapping | 1 to 6 load in memory, one step at a time: v1 gets no per-title map, v2 the default keyboard keys, v3 the default stick keys (a direction stays unbound when its default key is already one of that file's button keys), v4 aiming off, and v5 changes nothing. The file is not rewritten on load; the first save keeps it as `input_profile.json.v<n>.bak`. A version that carries a member introduced later, such as `per_title` in v1 or mouse buttons in v5, refuses the whole document | `NK_ERROR_NEWER_SCHEMA`, defaults, and a diagnostic that names the version and says the file is kept as it is. The save path refuses it when the version is readable at or below 1,000,000; above that the shared writer's probe treats it as unreadable and does not refuse the save | `NK_ERROR_FILE_NOT_FOUND`; the player treats that as no profile and uses the defaults | Missing, non-numeric or non-whole `schema_version`, a version below 1, invalid JSON, or any invalid entry: defaults and `NK_ERROR_GENERIC`. The Controls page's first save moves a file that exists but did not load for any reason except a newer version to `input_profile.json.unreadable.bak`, and does not save if that move fails. A zero-byte or over-1 MiB file, or one that cannot be opened or read, takes the same `.unreadable.bak` route |
| `registry/system.json` (the PSP system settings a game changes; `sreg_ensure_loaded`, `sreg_overlay_apply` and `sreg_persist` in `src/rt/hle.c`; `SREG_OVERLAY_VERSION` in `src/rt/hle.c`; `SR_SYSTEM_REGISTRY` overrides the path) | 1, with `"schema": "nakagawa.psp-system-registry-overlay"`; the only version so far (v0.0.1 and later) | No older version exists and there is no migration: any version other than 1 is invalid | Not preserved as a newer file: a later version fails the same `schema_version` check, so it is ignored like any invalid overlay and moved to `system.json.corrupt` on the next flush, then replaced | First run: the modeled defaults. The host time-zone seed may write a fresh overlay at start-up | Unparseable JSON, a wrong or missing `schema`, a version other than 1, a wrong shape, or a file over 1 MiB: reported, ignored, and the modeled defaults are served; the next flush (at start-up when the host time zone is seeded, or a game's `sceRegFlush*`) renames it to `system.json.corrupt` (kept, not deleted) before writing a fresh one; if the rename fails, it is not written over. A file that cannot be opened or read is reported and left in place, and no flush writes over it |

### Backups, and what is not one

The shared writer's versioned backup is made only when the file on disk carries
a readable integer version older than the build's. A readable version below 1
is not migrated, but it is still older to the writer: the first save keeps a
`settings.json` or `library.json` at `schema_version` 0 as `<file>.v0.bak`. A
missing, non-numeric or unreadable version gets no versioned backup.

`.unreadable.bak` is the shared writer's name for a file over the reader's size
bound, not for every file that fails to parse. `input_profile.json` adds a
second use: the Controls page moves any file it could not load, except a newer
one, to that name before its first save, so the mappings it holds are not lost
to one bad entry.

`library.json` has a second backup beside the versioned one: each save attempts
to rotate the previous primary to `library.json.bak` before promoting the new
file, but that rotation is best-effort. On Windows the copy's result is ignored
(`CopyFileW`, `src/core/nk_library.c:406-413`); on POSIX a failed or short copy
removes its temporary and still promotes the new primary (`:420-463`). A failed
rotation therefore does not fail the save; on POSIX it leaves the previous
`.bak` as it was, while on Windows its result is ignored, so a failed rotation can leave that file stale or incomplete.
`library.json.bak` is read when the primary is missing or invalid, not when it
is newer or cannot be read. This is not the shared writer's versioned backup: a
failed versioned backup stops the save (`src/core/nk_user_data_schema.h:115-162`).
`settings.json` and `input_profile.json` have no such fallback; when their
primary cannot be loaded, the player runs on defaults.

`registry/system.json` has neither a versioned backup nor an `.unreadable.bak`.
An invalid or newer overlay is moved to `system.json.corrupt` on the next flush;
one that cannot be read is left in place. The three shared-writer files are not
rewritten on load: a migration stays in memory until a save, and the original
stays on disk until then. The registry is the exception described next: its
startup seed persists immediately when the overlay is absent or invalid.

The registry's one startup write is the host time-zone seed. When the overlay
is absent (a first run) or invalid, `sreg_seed_host_time_zone`
(`src/rt/hle.c:1952-1984`) reads `SR_HOST_TIME_ZONE` (`<minutes>,<0|1>`), writes
`/CONFIG/DATE/time_zone_offset` and `summer_time`, and calls `sreg_persist`
immediately (`:2034-2039`). On a first run with a valid variable, that creates
the overlay; on an invalid one it moves the old file to `system.json.corrupt`
before writing the fresh overlay. A missing or malformed variable keeps the
measured seed (-300 minutes, daylight saving on) and does not perform the
startup write. A later host-zone change does not move the saved setting. An overlay that exists but cannot be read is neither seeded nor
written over, and registry load makes no other write.

### Flags and environment variables

The code table is authoritative: a player option exists only when it is in
`k_player_cli_options` (`src/player/main.c:4345-4384`); `--help` prints that
table, and `player_cli_normalize` checks the whole command line against it
before anything else reads it (`src/player/main.c:4406-4463`). The classes below
are a reading of that table at this commit; a new option must be added there
and classed here.

| Class | Options | Contract and source |
| --- | --- | --- |
| Stable user-facing, printed in `--help` | `--help`, `--version`, `--user-data-root`, `--iso`, `--stage`, `--stage-only`, `--launch-now`, `--launch-title`, `--launch-index`, `--runtime-root`, `--manifest-overlay`, `--experimental-60hz`, `--cheats`, `--cheats-file` | These are the flags a person or launcher may write. `--launch-title` is what Add to Steam writes (`docs/SETUP.md:321-324`), and `--user-data-root` moves the whole per-user folder (`docs/SETUP.md:868-872`). |
| Documented QA, capture and scripted-run flags (**Proposal**) | `--screenshot`, `--headless-launch`, `--headless-accept-presented-frames`, `--headless-timeout-ms` | `--screenshot` and `--headless-launch` are printed in `--help`; the two acceptance options have `help == NULL`. `docs/TITLE_TESTING.md:314-324`, `:343-347` and `docs/SETUP.md:908-909`, `:914-922` document them for QA/acceptance and headless runs, not as a stable user contract. This class is a proposal: it separates documented QA/capture routes from the stable CLI above, and is not yet ratified. |
| Hidden but accepted | `--runtime-bin`, `--view`, `--width`, `--height`, `--render-scale`, `--demo`, `--empty`, `--wizard` | `help == NULL` in the same table. `--runtime-bin` is the old spelling of `--runtime-root`; the others are harness or launcher internals, not a user contract. |
| Removed, kept as a tombstone | `--ui <ignored>` | Parsed and ignored so shortcuts made before its removal still pass `--ui=next` (`src/player/main.c:4374`). |
| Compile-gated test options | `--ui-test-events`, `--ui-test-screenshot`, `--ui-test-error-code`, `--ui-test-iso-path`, `--ui-test-wait-background`, `--ui-test-catalog-reload-path`, `--ui-test-catalog-reload-count` | Present only with `NK_PLAYER_UI_REGRESSION_TEST`; not in a release build (`src/player/main.c:4375-4383`). |

A command line that names an unknown option, leaves a stray argument, omits a
value, or gives a value to a switch is refused by name and exits 2; nothing is
ignored in silence (`src/player/main.c:4414-4462`; the exit code is stated at `:4331-4333`). A stable user-facing flag is the
contract; a QA/capture, hidden or test flag may change or disappear. The split
between the stable list and the QA/capture class is the **Proposal** above, not
a ratified promise. The current exceptions are the accepted legacy spellings:
`--runtime-bin` still names the runtime root, and `--ui=next` is still accepted
and ignored. Keeping a future removed spelling for a notice release is a
**Proposal** (see the release rule below), not a current guarantee.

The runtime's variables have the same single source: `src/rt/env_registry.h`
declares every name, its kind, its default, the file that reads it and its
purpose (`src/rt/env_registry.h:7-24`). The classes below are a reading of that
table and `docs/SETUP.md`. List each name explicitly so a new member of a prefix
group receives its own classification. `tools/test_env_registry.py` checks for
missing and malformed registry rows and checks the option and environment classes
below against the option table and the variables in `docs/SETUP.md`.

| Class | Variables | Contract and source |
| --- | --- | --- |
| Build and toolchain inputs | `BUILD_DIR`, `BUILD_ROOT`, `CC`, `GAME_NAME`, `MSYS_PATH`, `NK_BUILD_ROOT`, `NK_INSTALL_ROOT`, `NK_PACKAGE_JOBS`, `NK_POWERSHELL`, `PYTHON`, `SDL3_DIR`, `SDL3_DLL`, `SDL3_PATH`, `SDL3_TTF_DLL`, `TITLE_MANIFEST`, `VULKAN_SDK` | These inputs select compilers, interpreters, dependency paths, or build inputs and outputs. The Vulkan tool uses `VULKAN_SDK` after an explicit SDK path (`tools/vulkan_sdk.py`). SDL discovery consults `SDL3_DIR`, then `SDL3_PATH`, when no explicit path was given (`tools/nk_doctor_checks.py`). The staged SDL library overrides are `SDL3_DLL` and `SDL3_TTF_DLL`. |
| Documented runtime overrides | `SR_ACE`, `SR_CHEATS`, `SR_CHEATS_FILE`, `SR_FPS_CAP`, `SR_GPU_PIPELINE_CACHE`, `SR_MEMSTICK`, `SR_MOVIE_UPSCALE`, `SR_PRESENT_SCALING`, `SR_PRESENT_UPSCALE`, `SR_RECORD`, `SR_REPLAY`, `SR_SYSTEM_REGISTRY`, `SR_TEX_SCALE_BUDGET_MB`, `SR_USER_DATA_ROOT`, `SR_VIDEO`, `SR_VIDEOREC_DIR`, `SR_WATCHDOG_EXIT` | These names describe current runtime behavior. A registry row or a mention in setup or debugging does not confer a stability guarantee. The registry carries each name's kind and default. An invalid `SR_USER_DATA_ROOT` is refused by name. `SR_SYSTEM_REGISTRY` overrides the registry overlay path (`src/rt/hle_data.c:107-144`): an empty value falls back to the default, and a configured file that cannot be read leaves the change in memory only (`src/rt/hle.c:2190-2200`). |
| Launcher-owned, not user knobs | `NK_INPUT_PROFILE`, `PSP_ISO`, `SR_ADHOC_ENABLED`, `SR_ADHOC_OFF_FILE`, `SR_ADHOC_PORT_OFFSET`, `SR_ADHOC_SERVER`, `SR_CHEATS`, `SR_CHEATS_FILE`, `SR_DATAROOT`, `SR_DISC_ID`, `SR_GAME_TITLE`, `SR_MODULE_DIR`, `SR_TEX_REPLACE`, `SR_USER_DATA_ROOT` | For player starts, settings and validated launch paths determine the child values for cheats, online ad hoc, guest modules, texture packs and the data directories (`src/core/nk_launch.c`). `NK_INPUT_PROFILE` is the path the launcher exports for the child. When it has none, it exports the bare name so an inherited value is removed (`src/core/nk_launch.c:1869-1873`, `:1998-2006`). The direct-runtime uses of these variables do not change the player launch contract. |
| Inherited fallback, not a launcher export | `SR_INPUT_PROFILE` | The runtime reads it only when `NK_INPUT_PROFILE` is unset or empty (`src/core/nk_input_profile.c:2491-2493`; `src/rt/env_registry.h:173`). It is the older name and a fallback, not an alias the launcher writes; an inherited value can reach a launch whose `NK_INPUT_PROFILE` is unset. |
| Retired runtime overrides | `SR_GLYPHS`, `SR_SMART_2D`, `SR_TEX_SCALE` | Nonempty inherited values are masked on player starts. A directly started runtime refuses nonempty values by name. These controls now use keys in `SR_ACE` (`src/rt/ace.c`). |
| Diagnostic and developer-only | `SR_DEBUG`, `SR_GE_TIMING`, `SR_HEADLESS_ACCEPT_PRESENTED_FRAMES` | Diagnostic logging, the GE timing model and the frame-acceptance checkpoint are developer controls. `docs/DEBUGGING.md` describes other registry families, including log, flight recorder and snapshot controls. |
| Host and system | `APPDATA`, `HOME`, `LOCALAPPDATA`, `MSYS_NO_PATHCONV`, `PATH`, `SDL_VIDEO_DRIVER`, `SDL_VIDEODRIVER`, `TEMP`, `USERPROFILE`, `WINDIR`, `XDG_CACHE_HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME` | Set by the host and read for their value. The runtime registry excludes `APPDATA` and `LOCALAPPDATA`, which are launcher and tooling inputs, and `MSYS_NO_PATHCONV`, which controls Git Bash path conversion. |
| Test-only | `ATRAC3P_FIXTURE`, `CODEGEN_ORACLE`, `FUZZ_CONSTRAINT`, `FUZZ_SEED`, `FUZZ_TRIALS`, `LOCAL_COSIM_TRACE`, `MICROTEST_MODULE`, `MICROTEST_ORACLE`, `NK_VIDEOREC_FAKE_FFMPEG`, `PSP_HARDWARE_TRACE` | Inputs to self-tests and verification tools. The oracle, microtest and trace inputs belong to build and test commands outside the runtime registry. |

The classes overlap by design: the launcher sets documented variables
(`SR_CHEATS`, `SR_USER_DATA_ROOT`) as well as its own, and a hand-started
runtime reads them directly.
`NK_INPUT_PROFILE` and `SR_INPUT_PROFILE` are not aliases in the launch path:
the launcher writes the first (or its bare name to mask an inherited value), and
the runtime reads the second only as a fallback when the first is unset or
empty.

### Release notes, removal and version changes

The release notes are the announcement point: `docs/PREVIEW_RELEASE.md:3` names
the current release's notes as the description of what it holds, and
`docs/RELEASE_NOTES_v0.0.3.md:1-5` is the current example. The deprecation
promises below are a **Proposal** for
[#2745](https://github.com/Jstar269/nakagawa-recomp/issues/2745), not an
approved or code-enforced rule. The current legacy spellings and format refusals
are behavior; the notice period is not.

* **Proposal:** A user-facing flag, environment variable, format version or
  release-asset name is not removed or repurposed in the release that first
  stops using it. That release's notes name the replacement; the old spelling
  or version remains accepted for at least one published release, then may be
  refused.
* A format version is bumped when a writer starts writing a layout the previous
  reader cannot read. Runtime packages are the live example, with
  `NK_AOT_PACKAGE_SCHEMA_VERSION 3` and v2 still accepted
  (`src/core/nk_title_manifest.h:38-40`;
  `src/core/nk_title_manifest.c:4727-4737`). The package reader refuses a newer
  version by name and never rewrites it (`package_newer_format`,
  `src/core/nk_title_manifest.c:2984-2996`); it does not call the file damaged.
  **Proposal:** the previous version is kept readable for at least one release
  under the notice period above.
* **Proposal:** A removed player spelling is kept as a tombstone for that
  window, not silently dropped. The current legacy spellings are behavior, not
  evidence that the window is approved: `--runtime-bin` still names the runtime
  root, and `--ui=next` is still accepted and ignored
  (`src/player/main.c:4358`, `:4374`).
* A published release asset is never replaced or edited; a changed file is a
  new version (`docs/PREVIEW_RELEASE.md:518-520`). The version in every asset
  name is the same version (`docs/PREVIEW_RELEASE.md:494-521`).

This is a release-process proposal, not a code-enforced rule. Nothing in `src/`
reads these documents or counts published releases, and no test enforces the
notice period. The format refusals above are enforced; the notice period and
tombstone window are proposals, not code behavior.

### The remaining contracts in detail

The first table in this section is the user-data summary. This one covers the
formats and release contracts that are not one of the four shared-writer files.
As in the user-data tables, a **refusal** is an error that keeps the file or
package from being accepted or replaced; a **fallback** is defaults in memory
after which a later write may replace the file, and a **backup** is a copy made
before replacing it. A missing file is not a refusal:
each row says whether it falls back or is created.

| Contract and source | Version | An older or newer file | A missing file | An invalid file | Backup or repair |
| --- | --- | --- | --- | --- | --- |
| Built game package (`package.json`, `build-report.json`, `completion-manifest.json`) under `<data>/packages/<folder>/` (`src/core/nk_title_manifest.c:4727-4737`, `:5120-5131`) | package 3, cache 2, completion 2 (`src/core/nk_title_manifest.h:38-40`) | Older: v2 and v3 are read; v2 is not rewritten. Newer: refused by name, "built by a newer Nakagawa (package format N; this player reads 3)", status `INCOMPATIBLE`; never rewritten (`package_newer_format`, `:2984-2996`) | No folder or no `package.json`: `NK_RUNTIME_PACKAGE_MISSING` with a rebuild reason (`:5818-5823`, `:5834-5839`) | Unreadable JSON or a failed non-identity contract: `NK_RUNTIME_PACKAGE_INCOMPATIBLE`; an identity or completion mismatch: `NK_RUNTIME_PACKAGE_STALE` (`:5919-5976`) | None. A rebuild is the only repair; the player never edits a package (`package_newer_format` comment, `:2982-2988`) |
| `<data>/title-input-identities/<DISC_ID>/title-input-identity.json` (`package_validate_title_input_identity`, `src/core/nk_title_manifest.c:4061-4110`) | 2, with nested `manifest.schema_version` 1 (`:4089-4107`) | Only 2 is read; there is no migration, so any other value is invalid. A later version is refused by the same equality check, not treated as a newer file with a backup | Missing or unreadable: "Title input identity record is missing or unreadable; source media cannot be qualified." gives `NK_RUNTIME_PACKAGE_STALE` (`:5848-5851`) | A failed format, schema or member check: `STALE` with the reader's reason (`:5871-5884`) | None in the player. The next package build writes the file (`tools/nk_core/package_cache.py:356-375`) and the card asks for a rebuild |
| `<data>/fonts/v2/manifest.json` (`font_load_manifest`, `src/core/nk_font.c:515-575`) | 2 (`NK_FONT_CACHE_SCHEMA_VERSION`, `src/core/nk_font_slots.h:19`) | Only 2 is read; an older or newer number fails the exact check and reports "schema is not version 2" (`src/core/nk_font.c:557-561`). There is no migration and no versioned backup | Missing: `NK_FONT_STATUS_MISSING`, "No imported PSP font." (`:531-534`); the project font still serves the slots it has (`src/core/nk_font_slots.h:8-10`) | Malformed JSON, no `files` object, a bad entry, or a file over 1 MiB: `NK_FONT_STATUS_INVALID` with the reader's message; the user source is skipped, not partially served | None. Re-run `fonts import <folder>` to rewrite the cache |
| `<data>/experimental/<DISC_ID>/profile.json` (`nk_title_manifest_read_experimental_profile`, public wrapper `src/core/nk_title_manifest.c:5390-5397`; implementation `:5240-5388`) | 1 (`package_number(..., 1)`, `:5293`) | Only 1 is read; an older or newer number is invalid, with no migration | Missing or unreadable: the reader returns false (`:5275-5281`); title selection fails with `NK_LAUNCH_DATA_ROOT_INVALID` (`src/core/nk_launch.c:658-671`), and an experimental package validates as `STALE` (`src/core/nk_title_manifest.c:5993-6002`) | A failed profile or embedded-manifest check: false with the reader's message; the package check fails closed, never falling back to the normal title | None in the player. The writer `nk_title_manifest_write_experimental_profile` (`src/core/nk_title_manifest.h:122-133`) rewrites the profile when an experimental package is made |
| `<data>/manifests/*.json` (`nk_title_manifest_load_overlay_dir`, `src/core/nk_title_manifest.c:2198-2255`) | 1, the only manifest schema (`:635-640`) | Only 1 is read; any other number fails "only schema version 1 is supported". There is no migration and no backup | Missing folder: no overlays, no error (`:2200`). A missing individual file is simply not listed | Malformed or refused: skipped, with a report line naming the file and, when it declares one, the disc ID (`:2237-2251`). At most 8 manifests load; later names are skipped with a report line (`NK_TITLE_MAX_OVERLAYS`, `src/core/generated/nk_title_catalog.h:17`; `:2220-2224`) | None; the files are user-owned and never rewritten |
| Enhancement package under `<data>/packages/<DISC_ID>-enhanced-<variant>/` (`nk_title_manifest_package_dir_name`, `src/core/nk_title_manifest.c:2324-2358`) | Same package schema as the authentic package (3 written; v2 and v3 read); the variant is a name, not a version (`nk_title_manifest_enhancement_variant_valid`, `:2305-2322`) | Older: the same v2/v3 package rule. Newer: the same `package_newer_format` refusal. A package recorded for another variant is `STALE` (`package_validate_variant`, `:5629`, `:5945-5950`) | `NK_RUNTIME_PACKAGE_MISSING`, reason "The enhanced runtime package is missing." (`:5818-5824`) | An invalid variant id is `INCOMPATIBLE` before any path is formed (`:5784-5788`); a recorded variant that differs is `STALE` | None; rebuild the enhanced package. It is selected only by the library entry's `enhancement_package_id` |

| Contract and source | Version | An older or newer file | A missing file | An invalid file | Backup or repair |
| --- | --- | --- | --- | --- | --- |
| Cheat file, `<data>/cheats/<DISC_ID>.ini` or `--cheats-file` (`nk_cheats_load_file`, `src/core/nk_cheats.c:951-980`) | None. The file is CWCheat text with `_S` disc sections; it carries no version (`src/core/nk_cheats.h:45-49`) | Older/newer: n/a; the syntax is accepted as the reader parses it | No file, or `SR_CHEATS` not exactly 1: nothing is read and nothing is applied (`src/rt/cheats.c:236-275`) | A missing or unreadable file, or one over 64 MiB, makes `nk_cheats_load_file` return NULL; the runtime prints `CHEATS: on, but ...; nothing is applied` and the game still starts (`src/rt/cheats.c:273-276`; `NK_CHEATS_MAX_FILE_BYTES`, `src/core/nk_cheats.h:121`) | Past 4096 cheats are counted and not kept; a code line past 262144 is refused; a cheat past the per-frame write, byte or code limits is refused whole (`src/core/nk_cheats.h:130-141`; `src/core/nk_cheats.c:369-410`, `:907-930`). No backup; the file is user-owned. A package built before cheats is not launched with cheats on: the player refuses and asks for a rebuild (`src/core/nk_launch.c:1916-1922`) |
| Texture pack folder or ZIP with `textures.ini` (`src/rt/texrepl.c:1013-1017`; `src/rt/texrepl.h:34-43`) | `[options] version` 1; a higher number is read anyway with a diagnostic (`src/rt/texrepl.c:1013-1017`) | Older/newer: no migration; the option is informational. A folder with no `textures.ini` is allowed and images are matched by hash (`src/rt/texrepl.h:9-16`) | No pack selected: no replacement, the game draws its own textures | A malformed or unreadable pack is refused whole, with a message and nothing loaded (`src/rt/texrepl.h:34-39`); one bad image is skipped and its texture is drawn as the game draws it (`:40-43`) | No backup; the pack is user-owned. `textures.ini` is capped at 4 MiB and an image at 64 MiB (`src/rt/texrepl.h:65`) |
| `<saves>/<DISC_ID>/` (the PSP Memory Stick layout) (`src/core/nk_launch.c:1425-1496`) | None; the PSP defines the layout, not Nakagawa | Older/newer: n/a. A package rebuild or player update does not touch it (section 6; section 8) | The per-disc folder is the last candidate; it is used when it is writable | No writable root: `NK_ERROR_IO` and no launch ("No writable save location..."; `:1490-1495`). An explicit `SR_MEMSTICK` that is not writable is refused, with no fallback to the real save folder (`:1438-1458`) | None. Migration is #2744 and outside this slice; this section does not move or convert saves |
| The per-user folder layout (`src/core/nk_data_migrate.h:13-22`) | None on disk. The choice is made once per process and nothing is recorded | The rule is fixed, not a file version: an older install uses its old layout exactly as before #324; otherwise the new `Nakagawa` layout | Missing old layout and missing new: the new layout. An empty or dangling old folder counts as existing | Both exist: the old layout is used and the player shows one notice naming both; nothing is moved, copied, merged or deleted | No backup; a user who wants the new folder moves the old folders away (`src/core/nk_data_migrate.h:9-11`) |
| Release asset set (`docs/PREVIEW_RELEASE.md:494-534`; `--check-assets`, `tools/stage_install.py:960-1010`, `:1239`) | SemVer `<version>` in every name; one version across the set | The updater parses a tag as SemVer, accepts an optional leading `v` and ignores build metadata; pre-releases count and drafts or invalid tags are skipped (`src/player/update_check.h:66-77`, `:116-118`) | A missing player ZIP is `ASSET_PLAYER_MISSING`; a missing checksum is `ASSET_CHECKSUM_MISSING`; `--check-assets` exits 1 with the named problem (`docs/PREVIEW_RELEASE.md:522-529`) | A second archive for a platform, a mixed version, an orphan, an installer without its ZIP, or a name a filter drops is refused with its own `ASSET_*` name (`docs/PREVIEW_RELEASE.md:522-529`; `tools/stage_install.py:994-1002`) | No backup: a published asset is never replaced or edited; a changed file is a new version (`docs/PREVIEW_RELEASE.md:518-520`). Signing is scoped to the in-player Windows ZIP download, not the whole asset set: `src/player/update_check.c:379-416` looks only for `<zip>.sha256` and `<zip>.sha256.minisig`, and `docs/RELEASE_SIGNING.md:12-14` defines one signature over that checksum file; the ZIP itself is not signed. Missing key, missing signature, unknown key or invalid signature are `UPDATE_NO_SIGNING_KEY`, `UPDATE_SIGNATURE_MISSING`, `UPDATE_SIGNATURE_UNKNOWN_KEY`, `UPDATE_SIGNATURE_INVALID` (`src/player/update_check.h:139-150`; `src/player/update_check.c:557-590`; `src/core/nk_sig.c:143`, `nk_minisign_verify`). `--check-assets` checks names and versions only (`tools/stage_install.py:957-1010`) and verifies no signature; the installer, source archive and other assets are outside the in-player check. |

### Ace package files

For the 60 Hz timing package and the Ace cull hook, the player checks
`<data>/enhancements/<disc_id>-60hz.json` (or `-cull.json`) first and, only
when that file does not exist, the shipped
`assets/enhancements/<disc_id>-...json` (`player_read_title_package_file`,
`src/player/player_state.c:3389-3415`; `docs/ENHANCEMENT_CONTRACT.md:237`,
`:379-380`). The disc ID is lowercase in both names. `build-package
--experimental-60hz` and `--ace-cull` use the same order
(`tools/nk_cli.py:2836-2846`, `:2926-2936`). A user file that exists always
wins, even when it is unusable: the reader reports that file's error and does
not fall back to the shipped one, so a broken override blocks the package
rather than silently selecting the published package.

The Ace declaration has its own version fields, separate from the built runtime
package and the release. Its `schema_version` must be exactly 1
(`SUPPORTED_SCHEMA_VERSION`, `tools/enhancement_package.py:49`, checked at
`:480-488`; `docs/ENHANCEMENT_CONTRACT.md:247`, `:256`), and its `version` is
the declaration's SemVer (`docs/ENHANCEMENT_CONTRACT.md:247`). That
`schema_version` is not the built runtime package's package schema (3 written;
v2/v3 read, the table above), and that `version` is not the release asset set's
SemVer. The file name and `id` are names, not versions.

Logs, the update download (kept only when its SHA-256 matches) and the
content-addressed package cache (section 6) hold no state a user could lose.

`tests/fixtures/user_data/` holds each of these files as an earlier player's own writer
wrote it, and `tests/native/test_player_state.c`,
`tests/native/test_input_profile.c` and `tools/test_nk_core.py` load, migrate and
round-trip them, and cover the newer-file refusal for the versions they use.

**What a package is keyed on after an update.** The native key's
`compiler_identity` names the driver by its content hash, its version
(`-dumpfullversion`) and the content hash of each program the driver runs to build
the package (`cc1`, `as`, `ld`, as `-print-prog-name` resolves them), with no
paths: a binutils update replaces the assembler and linker without touching the
driver, and now recompiles the native package (generated C is reused), while an
unchanged toolchain at another path keeps the key. `runtime_source_digest` covers
what a package is compiled from: `src/rt`, `src/core`, the overlay's UI library
`src/ui`, the third-party headers those include (`third_party/clay`, `stb`,
`portable-snippets`), the `Makefile` and its `mk/*.mk` fragments. The player's own
sources (`src/player`) are not in a package and no longer rebuild one;
`tools/test_package_cache.py` checks that every third-party directory the runtime
includes is listed.

## 8. Portable layout, runtime identity and user state

This section records what the Windows portable ZIP holds today and where the player keeps
everything it writes ([#324](https://github.com/Jstar269/nakagawa-recomp/issues/324)). The
per-user `Setup.exe` that #324 also asks for carries the ZIP's exact files
([#2208](https://github.com/Jstar269/nakagawa-recomp/issues/2208), [`SETUP.md`](SETUP.md#windows-installer-setupexe)),
and the upgrade and uninstall rules for both are at the end of this section.

### The install image (read-only)

The release ZIP (`docs/PREVIEW_RELEASE.md`) and `tools/stage_install.py --dest` share one
layout. Nothing in it is written at run time, so it can sit in a read-only folder:

| Path | Contents |
| --- | --- |
| `bin/nakagawa_player.exe` | The player. Release builds also carry `bin/production_smoke.exe` and the showcase demos in `bin/demos/`. |
| `bin/*.dll` | `SDL3.dll`, `SDL3_ttf.dll` and their resolved closure (Section 5, #421). |
| `bin/runtime-manifest.json` | The runtime identity record (below). |
| `bin/assets/fonts/ui/`, `bin/font/nk*.pgf` | The project's own UI fonts and generated PGF fonts. No firmware font. |
| `bin/THIRD_PARTY_NOTICES/`, `SOURCE.txt`, `RELINK.md` | Licence texts for every bundled binary, the exact source commit, LGPL relinking. |
| `source/` | The public-safe source export BUILD PACKAGE runs from (`tools/nk_cli.py`, the Makefile, `assets/prereq_manifest.json`). |
| `prereqs/` | The pinned build prerequisites (#1285), checked by the player against `installed.json` and `bundle-inventory.txt` before BUILD PACKAGE uses them. |
| `docs/`, `README.md`, `LICENSE`, `NOTICE.md`, `START HERE.txt` | User documentation. |
| `SBOM.*`, `toolchain.json`, `SHA256SUMS.txt` | SPDX 2.3, SPDX 3 and CycloneDX SBOMs, which carry the SHA-256 `bin/runtime-manifest.json` records for each executable and DLL in `bin/` (`verify_sbom.py --package-dir` checks it), the attested toolchain record, and the SHA-256 of every other file. |

The image never holds a game, decrypted module, generated title code, save, key or firmware
font: the release script's forbidden-file and private-path scans refuse them.

### Runtime identity manifest

`bin/runtime-manifest.json` (format `nakagawa-runtime-manifest`, schema 1) lists every
`.exe` and `.dll` directly in `bin/` with its size, SHA-256 and the non-system DLLs it imports,
plus `host_libraries`, the imports the host provides and no route ships (today only the Vulkan
loader `vulkan-1.dll`, from `host_resolved_components` in `assets/third_party_components.json`).
`tools/stage_runtime_dlls.py --runtime-manifest` writes it after staging; writing fails with
`RUNTIME_CLOSURE_INCOMPLETE` when a binary imports a DLL that is neither beside it, a Windows
system DLL nor a host library, which is the loader failure a clean machine would otherwise show.
The attestation workflow writes and attests it with the binaries, and
`--verify-runtime-manifest` checks a staged or extracted `bin/`: every listed file present with
its size and SHA-256 (`RUNTIME_FILE_MISSING`, `RUNTIME_FILE_CHANGED`), no unlisted `.exe` or
`.dll` (`RUNTIME_FILE_UNLISTED`), imports unchanged (`RUNTIME_IMPORTS_CHANGED`) and a licence
record in `THIRD_PARTY_NOTICES/index.json` for every DLL (`RUNTIME_LICENCE_RECORD_MISSING`).
`tools/stage_install.py` writes the same file into an installed `bin/`, so portable and
installed layouts share one identity contract. The player does not read it yet; a start-time
check is open under #324.

### Reproducible portable ZIP

`tools/stage_install.py --portable-zip <zip> --zip-stage <folder> --artifact-reference <reference>` writes the ZIP from the
staged folder. The closed temporary ZIP must pass the [artifact content audit](PREVIEW_RELEASE.md#recreate-the-package)
against the independently approved reference before promotion. The writer rewrites `SHA256SUMS.txt` in ordinal path order, verifies
`bin/runtime-manifest.json`, and stores one entry per file and per empty folder, sorted by
UTF-8 path, every entry dated `SOURCE_DATE_EPOCH` (the commit time of `--revision` unless the
variable or `--source-date-epoch` sets it; times before 1980 become 1980-01-01), mode 0644 for
files and 0755 for folders, Deflate level 9, no extra fields and no comment. A link, junction,
special file, absolute or `..` name is refused. The same staged bytes then give the same ZIP
bytes with the same CPython zlib build, which the tool reports as `zlib`.

Measured on 2026-10-29 at `77a1a5e3`, before this change: two packages built from one commit
differed in every ZIP entry's timestamp (`ZipFile.CreateFromDirectory` stores each file's write
time, and the prerequisite tree is unpacked fresh each time), and the player executable differed
in its PE `TimeDateStamp` and checksum because its link line lacked `--no-insert-timestamp`.
Both are fixed; `tools/test_stage_install.py` builds one stage twice in different orders and
with different file times and compares the bytes.

Measured end to end on 2026-10-31 with the v0.0.1 candidate: the same commit, attested bundle, trusted
ledger and prerequisite pins, packaged twice into two folders with the whole recipe
([`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md#check-that-the-package-is-reproducible)). The prerequisite source
archive was identical. The portable ZIPs differed in five files under `source/.git` (the source export leaves
a one-commit repository whose commit time and index timestamps change on every run), and so in
`SHA256SUMS.txt`. After removing `source/.git` (the installed `source/` never had one; see the recipe notes), both ZIPs were byte-identical (9,215
files, 255,540,572 bytes). `--portable-zip` now refuses a stage that has a `.git` entry. Two clean builds of
`nakagawa_player.exe` from one commit and one checkout path were also byte-identical. Whether two runs of the attestation workflow
for one commit produce identical binaries was not measured; the check above reuses one bundle.

### Per-user state (writable)

Everything the player writes lives outside the install image, under the per-user root, so an
upgrade or a second extracted version finds the same library, saves and settings. On Windows
the root is `%LOCALAPPDATA%\Nakagawa` (then `FOLDERID_LocalAppData`, then `%APPDATA%`; never
`USERPROFILE` alone, `nk_platform_resolve_windows_data_base`). `--user-data-root=<folder>`
moves all of it into one folder (`nk_platform_set_app_data_dir_override`).

| Folder (Windows) | Holds | Owner |
| --- | --- | --- |
| `Nakagawa\config` | `settings.json` | player |
| `Nakagawa\data` (`<user data>`) | the library and title manifests | player, `nk_cli` |
| `<user data>\titles\<DISC_ID>\` | staged title inputs; `decrypted\` holds the player's decryption output or the user's own plain files | user's private inputs |
| `<user data>\title-input-identities\<DISC_ID>\` | the exact local input identity a package must match (Section 5) | `nk_cli` |
| `<user data>\cache\packages\<DISC_ID>\` | built AOT packages, content-addressed, promoted by rename (Section 6) | `nk_cli` |
| `<user data>\cache\objects\`, `cache\shared-libraries\` | shared compiled objects and disc libraries (#1306) | `nk_cli` |
| `<user data>\fonts\v2\` | fonts imported from the user's own PSP, with `manifest.json` | user |
| `<user data>\prerequisites\` | the consented per-user prerequisite download (absent when `prereqs/` is bundled) | player |
| `<user data>\downloads\` | a verified update ZIP and, outside a release folder, its unpacked copy | player |
| `Nakagawa\saves\<DISC_ID>\` | the title's Memory Stick root (`SR_MEMSTICK`) when the install is not writable | runtime |
| `Nakagawa\logs\` | `game-<DISC_ID>-last.txt`, the last game session's output; the player's build logs (`build_<DISC_ID>.log`, `build_<DISC_ID>_progress.jsonl`) and `prerequisite-fetch.log` | player |
| `Nakagawa\cache\` | the platform cache folder (`NK_PATH_CACHE`) | player |
| `Nakagawa\fonts\` | optional UI font overrides | user |

A fresh Linux or macOS install uses the same `Nakagawa` folder. Linux puts it in `$XDG_DATA_HOME`
(else `~/.local/share`) with `config`, `data` and `saves` inside it, and keeps the cache in
`$XDG_CACHE_HOME/nakagawa-recomp` (else `~/.cache/nakagawa-recomp`) and the logs in
`$XDG_STATE_HOME/nakagawa-recomp/logs` (else `~/.local/state/nakagawa-recomp/logs`); an XDG variable
that is not an absolute path is ignored, as the XDG specification says. macOS puts it in
`~/Library/Application Support`, with all five folders inside it. An install from before
[#324](https://github.com/Jstar269/nakagawa-recomp/issues/324) keeps its folders where they are:
Linux's `$XDG_DATA_HOME/nakagawa-recomp` with its settings in `$XDG_CONFIG_HOME/nakagawa-recomp`,
macOS's `~/Library/Application Support/NakagawaRecomp`. Each process chooses once, by one rule that
the player (`src/core/nk_platform_posix.c`) and the Python tools (`tools/nk_core/prereq_fetcher.py`)
share: the earlier folders when any of their settings, data or saves folders is present (by `lstat`,
so an empty folder or a dangling link counts; nothing inside is read), else the new folder. Nothing
is recorded, so the next start that sees an earlier folder uses it again. The rule and its cases are
in `src/core/nk_data_migrate.h`, and both test suites read the cases from there. When the earlier
folders are in use and the new one exists too, the library screen names both. Nothing is ever
moved, copied, merged or deleted between them, and moving an existing install is not offered; a user
who wants the new folder moves the earlier ones away. Each launch
still starts one child process per title (Section 2, Option C), with the #297 run-package and
save-root semantics unchanged.

Updates today: Check for updates (`src/player/update_check.c`) downloads a release ZIP into
`<user data>\downloads\`, keeps it only when its SHA-256 matches the published `.sha256`, and
INSTALL unpacks it into `<folder>.partial` beside the running version, checks every file
`SHA256SUMS.txt` lists, and renames it into place. It never replaces the running version and
never touches the per-user folders. Removing an extracted folder removes the install image
only; the per-user folders above stay until the user deletes them.

### Upgrade and uninstall rules

* **Upgrade is side by side, except a copy `Setup.exe` installed.** A new version is a new folder; the old one keeps running until the user
  deletes it, so an interrupted update leaves the old version whole and at worst a `.partial` folder
  that the next update replaces. In-place staging (`tools/stage_install.py --dest`) swaps `bin/` and
  `source/` by rename and removes the old copy only afterwards. The one in-place upgrade is a copy `Setup.exe`
  installed: the next release's Setup replaces its files in the same folder, then deletes each file the previous
  install's own `SHA256SUMS.txt` lists and the new one does not, only while it is unchanged since it was installed
  and no junction or link lies on its path, and removes the folders that leaves empty. Files the user added or
  changed, `bin/data`, `bin/portable.txt` and the per-user folders stay (`RealProbeTests` and the upgrade leg of
  `tools/verify_installer.py`).
* **A runtime or AOT epoch change forces a rebuild, never a stale launch.** Packages live in the
  per-user data folder and are keyed on the epochs of Section 6. A package built by another epoch
  is INCOMPATIBLE: the card offers REBUILD PACKAGE and the old code is never started. Saves and
  title inputs are not keyed on an epoch and survive.
* **Uninstall removes only what the install wrote.** `python tools/stage_install.py --uninstall --dest
  <folder>` removes `bin/`, `source/`, the icon, the install marker and the shortcuts and desktop entries the
  marker records, and nothing else: it never reads or deletes the per-user folders above, a file
  the user added beside the install survives, a recorded path with a name the tool does not write is skipped, and
  a folder with no marker is refused. In portable mode `bin/data` and `bin/portable.txt` stay. An unpacked ZIP is
  removed by deleting its folder. `tools/test_stage_install.py` (`UninstallTests`) covers each rule. The
  `Setup.exe` uninstaller follows the same rule: it removes what Setup installed and keeps the per-user folders
  unless the user ticks the box on its uninstall page and confirms ([`SETUP.md`](SETUP.md#windows-installer-setupexe)).
