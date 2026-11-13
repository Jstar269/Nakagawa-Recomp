# Architecture Overview

This page is a map of how Nakagawa Recomp turns a PSP game into a native program: what each major part
does, how data flows between them, and where to start looking when something behaves differently from a
PSP. When this page and the code disagree, the live source, Makefile and tests are right.

In short: the tools in `tools/` read the game's program files, find its functions, and translate its MIPS
machine code into C ahead of time (AOT). That C is compiled together with the native runtime in `src/rt/`,
which stands in for the PSP's system software, graphics, sound and input. If the running game reaches valid
code that wasn't translated, a small interpreter runs it, but only inside code the analyzer has claimed;
anything it can't handle stops the run with a named message instead of guessing. The player in
`src/player/` puts it all in front of the user.

New to the code? [`CODE_TOUR.md`](CODE_TOUR.md) follows one game through these parts, file by file, from the
disc image to the first frame on screen.

## Pipeline

```mermaid
flowchart LR
    INTAKE["ISO and EBOOT intake<br/>tools/nk_cli.py<br/>tools/nk_core/iso_inspect.py"]
    REFUSE["Encrypted input the player could not decrypt<br/>refused by name: ENCRYPTED_EXECUTABLE / ENCRYPTED_MODULE<br/>(the player decrypts the user's disc, src/core/nk_psp_*)"]
    LOAD["ISO / PRX loading and import mapping<br/>src/core/nk_iso.c<br/>src/player/iso_reader.c<br/>tools/prxload.py<br/>tools/imports.py"]
    ANALYZE["Function-boundary analysis<br/>tools/analyze.py"]
    CODEGEN["AOT code generation to C<br/>tools/codegen.py"]
    BUILD["Compile generated C with the native runtime<br/>Makefile<br/>src/rt/"]
    PLAYER["Player launches the native program<br/>src/player/"]

    subgraph RUNTIME["Runtime subsystems · src/rt/"]
        AOT["AOT dispatch<br/>src/rt/recomp.c"]
        FLOOR["Fail-closed interpreter floor<br/>src/rt/guest_interp.c<br/>analyzer-owned spans; unsupported forms stop"]
        SCHED["Scheduler<br/>src/rt/sched.c"]
        HLE["HLE services<br/>src/rt/hle.c<br/>src/rt/hle_*.c (one per subsystem)"]
        GE["GE rendering<br/>src/rt/ge.c<br/>src/rt/gpu_sdl3vk/ge_gpu.c"]
        VK["Vulkan presenter<br/>src/rt/gpu_sdl3vk/sdl3vk.c"]
        AUDIO["Audio output<br/>src/rt/hle_audio.c<br/>src/rt/audio_sdl3.c"]
    end

    INTAKE -->|"encrypted input with no decrypted copy"| REFUSE
    INTAKE -->|"plain or user-supplied decrypted EBOOT / PRX"| LOAD
    LOAD --> ANALYZE --> CODEGEN --> BUILD --> PLAYER --> AOT
    AOT -->|"analyzer-owned AOT gap"| FLOOR
    AOT --> SCHED
    AOT --> HLE
    AOT --> GE --> VK
    AOT --> AUDIO
```

The repository does not contain the retail game executable, ISO, decrypted game PRXs, or private
oracle traces. Those remain local inputs.

## PSP core, title profile, and backend boundary

The first wave of this work adds a small, versioned contract between the PSP core, a title's profile and
the host backends, without changing how Hot Shots Tennis (HST) is built today:

```text
validated ProgramImage (tools/prxload.py)
        -> canonical CFG / ownership observation (tools/analyze.py)
        -> psp-core-v1 semantic capabilities
        -> title profile (boot/resources/explicit HLE declarations/input labels)
        -> host backend contracts and optional enhancements
```

The core contract owns Allegrex/VFPU semantics, guest memory, scheduling,
interrupts/callbacks, generic HLE, GE/display/audio semantics, I/O capability
interfaces, backend contracts, and evidence schemas. A title profile may select
boot policy, public resource locators, input labels, and explicit HLE capability
dispositions. Unknown capabilities fail closed; a profile cannot add an implicit
PSP-semantic replacement, and enhancements are disabled by default in the public
profile-zero contract. HST remains an existing manager/build profile and is not
switched to the new adapter wholesale in this wave.

### ProgramImage v1 and CFG ownership observation v1

`ProgramImage` and `CanonicalCfgState` remain observation types, not replacements
for the production analyzer. The legacy `analyze()` path now exposes the
canonical CFG report through opt-in `analyze.py --cfg-report/--cfg-gate` and
`codegen.py --cfg-report/--cfg-gate`; ordinary codegen output and analysis are
unchanged when those flags are absent, and `--cfg-report` alone reports gate-mode
analysis without changing emitted code. `--cfg-gate` checks the primary image and
each supplied extra ELF before code emission. This wiring consumes the legacy
analyzer result. `--cfg-gate` rejects overlapping executable spans and prevents
direct-jump adjacency from seeding a callable. It does not claim equivalence
for, or route production through, `ProgramImage` or `CanonicalCfgState`.

**Precondition for any later production wiring.** Before either type may replace a
production path, it must first be shown *equivalent* to the path it replaces on
real inputs -- not merely self-consistent. `cfg_compatibility_findings()` exists
for exactly this: it reports differences against a legacy entry set and
deliberately does not pick a winner. Wiring either type in without that
equivalence evidence would convert an observation tool into an unverified
reimplementation of the analysis the pipeline already depends on.

`tools.prxload.load_program_image()` is the read-only Wave-1 adapter. It validates the
ELF32 envelope, checked load/file/guest spans, permissions, zero-fill extents, entry,
imports/exports, module metadata, and relocation records before allocating a flat image.
The immutable object carries source name/size/SHA-256, fixed-width guest spans, and
`bytes` payloads; `canonical_program_image_json()` serializes metadata deterministically
and omits raw payload bytes. It deliberately does not apply relocations or replace the
legacy `Prx` loader, so the current HST path remains authoritative while synthetic tests
compare both representations.

`tools.analyze.canonical_cfg_report()` emits schema version 1 as an observation-only
ownership report. Its instruction rows carry address, raw word, opcode identity,
delay-slot attachment, branch-likely annulment, owners and reasons. Edge rows distinguish
direct branches/jumps, calls, tail transfers, fallthrough, delay slots, and unresolved
computed transfers. The report also records interior entries, continuations, ownership
conflicts, jump-table candidates, data spans, padding, unowned executable words, partial/unreadable executable
spans, and explicitly unmapped entry candidates. `verify_canonical_cfg_report()` checks coverage and
structural consistency; `cfg_compatibility_findings()` reports differences from a legacy
entry set without silently selecting a winner. `canonical_cfg_json()` is stable for
fixtures and build-cache comparisons. The CFG gate rejects an owned in-range edge whose
target has no owner, plus ownership conflicts and unmapped entries. Non-padding words
outside known control-flow remain listed as unowned; this records uncertainty and does
not prove that no dynamic entry exists. Dynamic-entry ownership is not
modelled; the gate keeps such words visible as unowned instead of assuming the image
is complete. File adjacency after an unconditional transfer
does not make a word a callable. The opt-in `codegen.py --stack-census` flag
also requires the CFG gate and wraps generated callable entries to compare guest
`$sp` at entry and host return. Continuation entries share their callable's census;
flow exits are reported as excluded and keep the result `PARTIAL`, while an
ordinary unexplained stack delta reports `FAILED`. An unobserved workload reports
`NOT_OBSERVED`; a balanced workload that misses generated callable entries reports
`PARTIAL` with `unobserved=N`, so `COMPLETE` requires observing every expected entry.
These diagnostics are source-owned runtime evidence and make no physical PSP
correctness claim. The cosimulation harness classifies its source-owned
`spleak` cell as a positive control and passes only when that cell is the sole observed
mismatch, with its declared 32-byte delta; every other entry must balance. Neither
report is an optimizing IR or a production HST switch.

`assets/titles/synthetic.json` and `synthetic-title2.json` carry the source-owned
`psp-core-v1` / `profile-zero-v1` contract. Their shared PSPDEV fixture points to
`fixtures/profile_zero`; each acceptance case names `profile-zero-e2e` as its
proving gate and retains its declared evidence class. That gate drives both
manifests through ProgramImage, analyzer/AOT package generation, and the headless
production runtime. The fixtures cover the public source route only; broader
title intake is tracked in [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308).
Generated output from a source-owned profile may be public in principle, but retail
or private-input-derived AOT remains local, ignored, and outside publication.

### Analysis IR view (advisory)

`tools/analysis_ir.py` turns the CFG report that `codegen.py --cfg-report=<path>` writes into a
module-wide view (#305): basic blocks with their delay slots, callables, resume entries and
continuations from codegen's `emitted_ownership_map`, the call graph, cross-references (code edges
and jump-table candidates) and, per instruction, the guest registers it reads and writes and its
memory access. An encoding the register table does not name stays `gpr_known: false`; nothing is
guessed. The IR is a projection, never an input: codegen does not read it, building it leaves the
report unchanged, and `analysis_ir.py verify` fails unless every function, block and instruction
owner matches the report it came from (`ANALYSIS_IR_PARITY PASS`).

External annotations (`nakagawa-analysis-annotations-v1`: address, kind, source, evidence class
and confidence) from Ghidra, decomp.me, manual review, runtime traces or hardware are compared
with the IR by `analysis_ir.py annotate`, which lists agreements, advisories and disagreements and
counts disagreements backed by runtime or hardware evidence separately. An annotation never adds,
removes or renames an analyzer fact. `export-annotations` writes the analyzer's own entries in the
same format, and importing it back gives only agreements. `explain <address>` prints why a word is
owned, how control reaches it and what annotations say about it.

### Native title recognition projection

`python tools/title_manifest.py --print-public-catalog` emits an immutable C99
header from the explicitly included manifests in `assets/titles/`. The canonical
validator owns the schema; there is no second native title registry to maintain.
Excluded and unclassified files are not read, missing included files fail, and
symbolic-link/junction input paths are rejected. Run generation on a quiescent
checkout; these checks do not provide filesystem race isolation or provenance
attestation. Capture subprocess stdout as bytes to preserve the generated LF text.

The projection contains only title ID, display name, kind, canonical manifest
SHA-256, and explicit primary/compatible disc IDs. It preserves UTF-8 strings
through C compilation, rejects duplicate identities, and invents no disc IDs for
synthetic or homebrew titles. Native lookups use exact identifiers. The digest
also changes when non-projected manifest fields change; it identifies input
content and does not authorize it.

Recognition is not compatibility, preparation readiness, or permission to launch.
The existing manifest-to-plan and runtime-configuration generators retain those
separate responsibilities. Private overlays require their own explicit local
binding and collision policy in a future host consumer. The current player
prototype is not integrated by this projection. Source-owned tests generate,
compile, and execute the header; no retail input is required.

## Directory Layout

```text
NakagawaRecomp/
├── tools/              # Python offline compilation, verification, and audit tooling
├── src/
│   ├── core/           # Shared native library, title, and launch services
│   ├── player/         # Native player UI and setup staging
│   ├── rt/             # C native runtime
│   │   ├── gpu_sdl3vk/ # SDL3 + Vulkan backend
│   │   └── ...
│   └── ref/            # C++ reference interpreter / differential-test support
├── assets/vfpu/        # Pinned VFPU lookup-table assets with provenance
├── font/               # Pinned replacement PGF fonts with provenance
├── build/              # Generated build output (Git-ignored)
├── docs/               # Maintained documentation and dated investigation records
├── Makefile            # Build driver
├── nk_manager.ps1      # Canonical build/run/inspection orchestration
├── nk.ps1              # Simple build, doctor, and play entry point
└── README.md           # Project entry point
```

## `tools/` — Offline Compilation

These tools run on the development host before the native compiler.

| Script | Input | Output | Purpose |
| --- | --- | --- | --- |
| `prxload.py` | decrypted ELF/PRX | `*_image.bin` | Rebase modules, apply supported relocations, emit flat guest image |
| `imports.py` | ELF + base | `*_imports.toml` | Resolve PSP import NIDs for HLE/codegen |
| `analyze.py` | ELF | in-memory function map | Discover function boundaries/control-flow metadata |
| `codegen.py` | ELF + analysis/import data | `*_recomp.c`, `*_recomp_N.c`, headers/reports | Translate guest MIPS functions into C |

### Codegen internals

`codegen.py` is the translation core.

- **Function discovery:** consumes `analyze.py` results.
- **Non-code in an image without relocation evidence:** when the analyzer has no relocation
  evidence for an image (`relocated_words` is `None`), it cannot tell a word that holds a code
  address from any other. That is an overlay or an `ET_EXEC` executable loaded at a fixed address,
  a code capture, a PRX whose relocation map is empty or that is analyzed without a base (the
  pipeline never does the latter), and an `ET_REL` or relocation-bearing `ET_EXEC` loaded at a
  nonzero base, which the loader relocates without recording the words. Its seed sweeps (address
  pairs, `j`-shaped words, table stores, prologue words, the word after a terminator) and its
  data-section pointers can then start functions inside the data its code section holds. For such
  an image `analyze.py` judges every seed and call target the format does not name by the body
  codegen would emit for it (`_BodyWalk`: the rules of `codegen.function_flow`, every entry and
  every withdrawn address a boundary), and withdraws it when that body holds a word
  `reserved_encoding` judges no instruction (a reserved opcode, SPECIAL function or REGIMM `rt`,
  or a `blez`/`bgtz` form with `rt` set). The emitter and the interpreter refuse every such word,
  so codegen could not have translated that body: a withdrawal takes no translation away. The body
  judged is the split walk's (no edge enters another entry), which every form of the generated
  walk owns. A seed that a body which stays may own (the unsplit walk's reach of a kept entry or
  of a clean candidate, which follows a conditional edge into another entry) is never withdrawn,
  and neither are the
  entry point (and a prologue at the start of the executable segment), the exports and init
  array, the import stubs, and the addresses a run reached (capture entries and miss seeds,
  however they were first seeded); one of those whose body holds a reserved word stays. A valid
  instruction the translator does not support is no evidence, and neither is a transfer out of the
  image: a fixed-address overlay may call one loaded later. Every body that stays owns its unsplit
  reach: each candidate whose body holds no reserved word, an entry or not (a clean seed a data
  body's trace ran over first becomes an entry once that body is withdrawn), and each entry not
  withdrawn. What withdrawn bodies reach beyond that is their shadow: the sweeps skip it, so a
  seed whose evidence lies there is no evidence. The void is the part of the shadow from which
  every path runs into a reserved word (none returns, calls, since a callee may never return,
  jumps or branches out of the image, stops at `jr`, a syscall, `break` or `eret`, or enters an
  entry; a branch-likely's delay slot counts only on its taken path): proven never to run as
  working code. The rest of the shadow
  stays executable, as before the withdrawal; a withdrawn address there has no translated body
  and, if called, runs on the interpreter floor, which refuses at the reserved word if it gets
  there. Discovery runs again until a pass withdraws nothing.
  Every entry that is not withdrawn stays an entry in the next pass, so a pass only adds
  boundaries and no body that stays runs on into words it did not own; each pass that continues
  withdraws at least one more address, so the loop ends without a limit. One walk answers for every
  seed of a pass, so a word is stepped once however many bodies own it. A withdrawn address ends
  every discovery path and stays a dispatch boundary in the generated code. An image whose code
  holds no reserved word is not walked. The judging walk (`_BodyWalk` in `tools/analyze.py`) follows
  `codegen._walk_body`'s rules for one shared walk over every seed; `tools/test_analyze_non_code.py`
  runs both over a seeded corpus of random bodies and fails if a generated body leaves the judged
  reach, or a judged body is not inside the generated split body. The one difference is a
  transfer in a delay slot: the judging walk steps such a slot when a branch also lands on it,
  the generated walk does not, and the emitter refuses every body with one, so it is a stub in
  either case. In a runtime module the module's spans exclude the void,
  and the descriptor registers an `SrModuleNonCode` record (count of withdrawn seeds and void words)
  only for a module that has one, so other descriptors are unchanged; a dispatch that reaches a
  withdrawn span stops with `NON_CODE_WITHDRAWN` (see [`DEBUGGING.md`](DEBUGGING.md)). The runtime
  finds a module's span, and the executable's, by a binary search. A module's registration
  refuses spans out of address order; the executable's spans, which the main image and each
  `--extra-elf` image register in any order, are kept sorted.
  In the executable and an `--extra-elf` image the void leaves the registered executable spans, so
  a dispatch there stops as not executable, without that name. Codegen refuses the build
  (`NON_CODE_TRANSLATED`) if an entry, or a word it walked for any function it emits (translated
  or not), lies in the void: the analyzer and the generator would disagree about what is code.
  Images with relocation evidence are untouched.
- **Instruction translation:** emits C operating on the shared `CpuState` ABI.
- **Narrow compatibility translations:** address-specific/custom behavior is allowed only when
  evidence justifies it and should be tracked as semantic debt rather than treated as a general
  translation rule.
- **Chunking:** generated functions are split into `<game>_recomp_0.c` through
  `<game>_recomp_N.c`. The count is computed from the discovered functions and
  `FUNCS_PER_CHUNK`; it is not fixed to eight HST chunks.
- **LLVM IR backend (opt-in, #330):** with `--backend=llvm` (`CODEGEN_BACKEND=llvm`),
  `codegen_llvm.py` lowers each function it supports to LLVM IR in `<game>_recomp_<n>.ll`
  beside its C chunk, from the same ownership and transfer rules, and keeps the C body,
  with the reason recorded, for the rest. [`LLVM_BACKEND.md`](LLVM_BACKEND.md) is the design.
- **Local register promotion (opt-in, #284):** `codegen.py --promote-registers` keeps the guest
  GPRs that a run of simple instructions touches in C locals (`_gpr<n>`). A GPR is loaded from
  `s->r[]` before its first read in the run, and every written local is stored back before each
  label, control transfer, instruction emitted the legacy way and the end of the body, so
  dispatch, the interpreter floor, HLE, the scheduler and the stale-code redirect always see the
  architectural register file. An instruction is promoted only when its translated line touches no
  other guest state than its GPRs, HI and LO, calls only the guest memory accessors, and neither
  returns nor jumps (`promoted_line_is_safe`); LLE-guarded accesses, FPU, VFPU, COP0, LL/SC and
  unaligned accesses stay legacy. A trace build mirrors every promoted write at once
  (`SR_PROMO_SYNC` in `src/rt/recomp.h`), so `mingw32-make cosim-selftest-promoted` compares the
  whole cosimulation corpus channel for channel. The win comes from the guest stores: they call
  runtime code, so without promotion the host compiler reloads `s->r[]` after each one. Generated
  code built at `-O0` gains nothing, so the option only pays off at `-O1` or above. Codegen prints
  `CODEGEN_PROMOTION` with the promoted, span, load and write-back counts. Making it the default
  needs all of these: the promoted cosimulation and the production-smoke and profile-zero routes
  pass; HST and PAC-MAN CE reach the same route results as the legacy build; HST's guest CPU time
  per frame (`SR_PERF` `cpu_ms`) improves by at least 5% at its qualified `-O1`; and compile time
  and peak compiler memory grow by at most 10%. Measured on 2026-10-29 (#284): every route still
  passes, but on HST at `-O1` the guest time outside the GE fell by only about 1.3% (two runs per
  build). The software GE takes 80% of guest time on that route, so frames sped up by about 0.25%.
  Chunk objects grew by 0.03%, and PAC-MAN CE at `-O0` was about 11% slower. The option therefore
  stays off by default. It does not drop write-backs of dead locals.

### Verification tools

| Script | Purpose |
| --- | --- |
| `codegen_gate.py` | Generate/compile translated code using `$CC` (falling back to `gcc`) and compare the pre-HLE execution trace with a supplied oracle |
| `funcdiff_cmp.py` | Compare developer-supplied per-function traces |
| `ppmdiff.py` | Compare framebuffer snapshots, commonly software-vs-Vulkan A/B output |
| `gen_microtest.py` / `microtest_gate.py` | Build targeted instruction/function verification cases when the required external inputs are available |
| `verify_gates.py` | Orchestrate optional codegen/microtest gates and report missing oracle inputs as blocked |

In-repository A/B agreement is evidence of local consistency; it is not by itself an external
proof of PSP correctness.

## `src/rt/` — Native Runtime

The runtime executes generated guest functions and implements the host side of PSP services.

### Core

| File | Purpose |
| --- | --- |
| `recomp.h` / `recomp.c` | Shared `CpuState` ABI, guest-memory access helpers, dispatch support, instrumentation |
| `sched.c` | Cooperative PSP-thread scheduler and wait/lifecycle behavior |
| `sr_coro.c` / `sr_coro.h` | Host coroutine abstraction: Windows fibers on Windows and the POSIX/ucontext path where supported |
| `driver.c` | Runtime entry point, image setup, tracing/termination plumbing |
| `debug.c` / `debug.h` | Centralized debug categories and memory-watch support |
| `perf.c` / `perf.h` | Runtime performance counters/telemetry |
| `domain_mode.c` / `domain_mode.h` | LLE Phase 1 per-domain HLE/LLE mode table (every domain defaults to HLE), the NID-to-domain and export registries, and the `sr_import_call()` seam that generated import stubs use instead of calling `sr_syscall()` directly |
| `fbcap.c` / `fbcap.h` | Presenter-neutral frame-capture service: owns the capture state and the atomic P6 PPM publisher, so a capture means the same thing whichever presenter showed the frame |
| `fbcap_policy.c` / `fbcap_policy.h` | Deterministic policy for who owns the single frame-capture slot when both `SR_FBDUMP` and `SR_FBSNAP` are set |
| `videorec.c` / `videorec.h` | Run recorder (`SR_VIDEOREC_DIR`): one video frame per VBLANK from the presenter plus the guest's mixed audio on one guest timeline, encoded through FFmpeg or written raw |

### HLE

| File | Purpose |
| --- | --- |
| `hle.c` | PSP syscall/NID dispatch (`sr_syscall()`), the handler table and registration API, kernel object UIDs and the user-memory allocator, HLE code shared by two or more subsystems, and `sr_hle_init()`, which calls every subsystem's registrations in order |
| `hle_<subsystem>.c` | One file per subsystem in `tools/hle_registry_meta.py` (`hle_audio.c`, `hle_io.c`, `hle_kernel_thread.c`, ...): its handlers, their state and its registrations (#307) |
| `hle_power.c` / `hle_power.h` | `scePower` battery and power-source getters and the emulated boot and bus clocks, with values measured on a PSP-3000 |
| `hle_evidence.h` | Generated table of registered HLE NIDs whose evidence class is `unverified` (by `tools/verification_ledger.py`); `sr_syscall` logs the first call of each and nothing else reads it |
| `hle_host.c` / `hle_data.c` / `hle_route.c` | HLE infrastructure that registers no NIDs: host primitives and checked host paths; the data-root lookup and `/DATA/FONT`; acceptance routes and frame capture |
| `hle_internal.h` | Private header of the runtime HLE files: shared types, macros and small helpers, and the declarations one file defines and another uses. `tools/hle_sources.py` lists the files |
| `hle_thread_selftest.c` | Game-input-free Windows and Linux harness that executes selected production HLE handlers through registered NIDs against a synthetic scheduler world |
| `audio_sdl3.c` | Public-safe SDL3 host audio backend linked under `PUBLIC_SAFE=1`; fails closed with a diagnostic when no device is available. It was named `audio_unavailable.c` until 2026-11-01 but is a working backend, not a refusing stub; the lineage-sensitive `audio.c` backend was retired on 2026-10-25 and no build compiles it |
| `iso_public.c` / `iso.h` | Public-tree UMD/ISO backend linked under `PUBLIC_SAFE=1` using `nk_iso`; `iso_unavailable.c` is kept as a stub alternative; the private `iso.c` backend was retired on 2026-10-25 |
| `pgd_unavailable.c` | Public-tree PGD stub linked under `PUBLIC_SAFE=1`; the private installed-data backend `pgd.c` was retired on 2026-10-25 |
| `mpeg.c` | MPEG/SAS/Atrac-related behavior derived in part from PPSSPP lineage |
| `savedata.c` | Utility savedata mapped to host storage |
| `pgf_public.c` | Project-authored public PGF reader linked under `PUBLIC_SAFE=1`, written from `docs/cleanroom/PGF_SPEC.md` (#349); the lineage-sensitive private backend `pgf.c` was retired on 2026-10-25 |
| `h264_mf.c` | Windows Media Foundation video-decode integration |
| `h264_ffmpeg.c` / `h264_ffmpeg_sub/` | Portable H.264 backend: the pinned FFmpeg n4.4 decoder subset, the default off Windows (`H264_FFMPEG=1`, [#1263](https://github.com/Jstar269/nakagawa-recomp/pull/1263)) |
| `h264_null.c` | Shared H.264 front end (decoder instances, PSMF demux, backend selection) and the null backend, which fails closed with a named boundary when no decoder is built (`H264_FFMPEG=0` off Windows) |
| `osk_win.c` | Win32 on-screen keyboard input box; it blocks its caller until the person answers, so only the text-entry worker shows it |
| `osk_text_entry.c` / `osk_text_entry.h` | Project-authored, non-blocking keyboard text entry: answers a field in the game window when the presenter can draw the keyboard, otherwise shows the input box on a worker thread so the keyboard never stops guest time; closes whatever it shows when the title drops the keyboard, and opens nothing under the offscreen presenter |
| `osk_overlay.c` / `osk_overlay.h` | Project-authored in-window on-screen keyboard: the key grid, cursor, typing, the guest's length bound and inputtype, the UTF-16 answer, and edge-based pad input. Pure C with no host dependency; the session it keeps is the one the HLE keyboard polls and the vblank controller latch feeds |
| `osk_overlay_paint.c` / `osk_overlay_paint.h` | Draws the in-window keyboard over the host copy of the frame the SDL3/Vulkan presenter is about to show, with SDL's software renderer. Guest VRAM is never written, and the frame capture sees the same pixels |
| `gui.c` | Host GUI/input integration and fallback presentation plumbing |

Unknown/unregistered PSP operations are not intentionally converted into fabricated success in a
scheduled game run. Missing behavior should fail visibly so the HLE gap remains observable.

### GE / graphics

| File | Purpose |
| --- | --- |
| `ge.c` | Software GE rasterizer with PPSSPP-derived behavior; dedicated `-O2` build rule |
| `ge_shared.h` | Constants/data shared by renderer paths |
| `gpu_sdl3vk/sdl3vk.c` / `.h` | SDL3/Vulkan initialization, host window/input/presentation |
| `gpu_sdl3vk/ge_gpu.c` / `.h` | Vulkan GE command processing |
| `gpu_sdl3vk/shaders/` | GLSL sources and checked-in embedded shader data |

`SR_GPU_GE=1` selects the Vulkan GE path; `SR_GPU_GE=0` selects the software comparison path.
Neither path should be described as an external PSP oracle.

### VFPU

| File | Purpose |
| --- | --- |
| `vfpu_interp.c` | Single-instruction VFPU interpreter using the pinned lookup tables in `assets/vfpu/` |
| `vfpu_fuzz.c` | Differential harness for translated VFPU behavior versus runtime/reference behavior |

## `src/ref/` — Reference Interpreter

The separate C++ interpreter provides an independent execution implementation for verification
work. It is not linked into the normal game executable.

| File | Purpose |
| --- | --- |
| `cpu.h` | C++ mirror of the guest CPU state needed by the interpreter |
| `interp.cpp` / `interp.h` | Instruction-level reference execution |
| `run_elf.cpp` | Reference runner used by verification workflows |
| `selftest.cpp` | Reference-interpreter/runtime self-tests |

Trace format: `tools/TRACE_FORMAT.md`.

## `CpuState` ABI

`src/rt/recomp.h` defines the load-bearing state shared with generated code. The current structure
contains:

- `r[32]` — MIPS general-purpose registers;
- `hi`, `lo` — integer multiply/divide state;
- `pc` — current guest PC at maintained boundaries;
- `f[32]` / `fi[32]` — FPU register view;
- `fcr31`, `fpcond` — FPU control/condition state;
- `v[128]` / `vi[128]` — physical VFPU register file;
- `vfpuCtrl[16]` — VFPU control/prefix/condition state;
- `cop0[32]` — modeled COP0 register bank; COP0 status is `cop0[SR_CP0_STATUS]`;
- `next_pc`, `in_delay_slot` — branch/delay-slot bookkeeping;
- `flow_kind`, `flow_target` — runtime transfer metadata;
- `llbit` — the MIPS32 LLbit used by `ll`/`sc` (see "LL/SC link state" below).

The layout is versioned by `SR_CPUSTATE_ABI_VERSION` (currently `3u`; v3 appended `llbit`
at offset 996, size 1000) and is checked in both C and C++ at compile time.

There is **no separate `lr` member**. MIPS `$ra` is general register `r[31]`; similarly `$sp` is
`r[29]` and `$gp` is `r[28]`.

Changing this layout requires coordinated updates to every consumer and explicit ABI/offset
verification. The consumers are: the `_Static_assert`/`static_assert` blocks in `recomp.h`;
`ref::CpuState` in `src/ref/cpu.h`; `CPU_STATE_ABI_VERSION` in `tools/codegen.py`, which the
generated `generated_funcs.h` checks against the runtime; the offsets and version in
`tools/mem_debug.py` (live process view and `crash_dump.bin` header); the package-layer epochs
`GENERATED_CODE_ABI_EPOCH`/`RUNTIME_ABI_EPOCH` and the `CPUSTATE_ABI_EPOCHS` table in
`tools/nk_core/package_cache.py` with their native twins in `src/core/nk_title_manifest.h`
(every layout version bumps both; `tools/test_package_cache.py` pins them to this header); and
the AOT package's `runtime.abi_version`, which the player compares with its own
`SR_CPUSTATE_ABI_VERSION`, so packages built for an older ABI are refused until rebuilt. `recomp.h` is hashed into the runtime,
codegen and generated-code build profiles, so every object that includes it rebuilds.

### LL/SC link state

`ll` (opcode 0x30) loads a word and sets `llbit`. `sc` (opcode 0x38) stores `rt` and writes 1 to
`rt` only while `llbit` is set; otherwise it stores nothing and writes 0. `sc` leaves `llbit` as
it found it (the MIPS32 Release 2 operation). Both execution tiers implement this:
`tools/codegen.py` `_ll_sc_stmt()` and the production interpreter in `src/rt/guest_interp.c`;
the reference interpreter in `src/ref/interp.cpp` follows the same rule. A PSP-3000 measured
it (`PSP-LLSC-001`, [hardware oracle index](HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize)):
a second `sc` after a successful one, with no new `ll`, stores and writes 1, and an `sc` to an
address other than the one `ll` loaded also stores and writes 1, so `sc` keeps the link and
compares no address. `tests/native/cpu_lle_selftest.c` and `src/ref/selftest.cpp` run the probe's
three windows against those words.

The MIPS32 contract clears LLbit on an exception return (ERET). On the PSP every event that can
run other code between a thread's `ll` and its `sc` -- a thread switch, an interrupt, a callback,
a kernel syscall -- reaches the thread again through an exception return. This runtime models
those events at a fixed set of points, and `sr_cpu_link_clear()` runs at exactly these:

| Event | Where |
| --- | --- |
| exception return | `sr_cpu_eret()` (`src/rt/cpu_lle.c`), on a successful return only |
| thread switch-in | `sched_load_thread_context()` (`src/rt/sched.c`), the only place `sched_run()` resumes a thread, so every parking path (`sr_yield`, blocking HLE waits, preemption) is covered |
| interrupt return | `deliver_vblank()` and `scheduler_alarm_deliver()` after restoring the interrupted frame |
| callback / nested guest call return | `sr_callback_dispatch_one()` (`recomp.h`), `ge_call_guest*()` (`hle.c`), `call_guest3()` (`mpeg.c`) |
| HLE syscall return | `sr_syscall()` after the handler runs; a call linked to a started module's guest export is a plain jump on hardware and does not clear |

Nothing else writes `llbit`. In particular a `SR_YIELD` point whose `sr_yield()` neither switches
threads nor delivers an interrupt leaves the link set, and exception entry does not clear it (the
handler's `eret` does).

Forward progress. Interrupts are delivered and threads switched only from `sr_yield()` or from
inside an HLE call, and `sr_yield()` is reached only from `SR_YIELD`, which the generated code
places at function entry and on backward branches. An `ll .. sc` window that contains no call, no
backward branch and no syscall -- the shape of every retry loop `L: ll; ...; sc; beqz L` -- has
no clearing point inside it, so its `sc` succeeds on the first pass after any earlier failure.
The retry branch's own `SR_YIELD` runs after the failed `sc` and before the next `ll`, outside
the window. A window that does contain a yield point fails only when the time slice expires
inside it and another thread is runnable or an interrupt is pending; `sr_yield()` then grants a
fresh slice of `TIMESLICE` yield points, so the next pass succeeds unless the window itself holds
that many. A window that contains an HLE syscall fails on every pass, as it would on hardware for
a kernel call.

## Build System

### Important Make variables

| Variable | Default | Purpose |
| --- | --- | --- |
| `GAME_NAME` | `mygame` | Build/output identifier |
| `GAME_ELF` | `eboot.elf` | Decrypted ELF path; HST normally supplies its canonical private path through the manager |
| `GAME_BASE` | `0x08804000` | Generic rebased-ELF load base; HST requires `0` |
| `GAME_ENTRY` | `0x08804000` | Generic entry; HST requires `0` and runtime initialization resolves the actual entry |
| `VULKAN_SDK` | empty for direct Make; manager discovers explicit > environment > newest valid `C:/VulkanSDK/<version>` | SDK path used for Vulkan headers/import libraries; the manager validates capability before invoking Make |
| `GAME_EXTRA_ELFS` | empty generically; HST-specific inside its Make block | Additional decrypted modules |

### Two-phase build

The `all` target intentionally invokes Make twice:

1. `pipeline` generates/rebases the image, imports, and generated C;
2. a second `compile` invocation reparses the Makefile after generated chunk files exist.

`CHUNK_OBJS` is based on `$(wildcard ...)` at parse time, so collapsing the process into a single
`all: pipeline compile` dependency pass can omit generated chunks on a clean build.

### Source-owned production smoke

`mingw32-make production-smoke` generates a deterministic PSP-shaped ELF/PRX fixture under
`build/production-smoke/`, then enters the ordinary two-phase `all` target with `PUBLIC_SAFE=1`.
The fixture is a recipe in [`fixtures/production_smoke/`](../fixtures/production_smoke/); the PRX,
`~PSP` header, relocated image, generated C, objects, link map, executable, and run logs remain
ignored build outputs.

This gate covers two load segments, PSP-header BSS recovery, type-A relocation, import discovery,
entry/helper analysis, multiple generated chunks, the complete public-safe production link, the
real driver and registration table, scheduler startup, real NID dispatch in `hle.c` (one synthetic
import plus the sceImpose language setter/getter pair, whose guest-observed round-trip the driver
asserts), a checked guest-memory sentinel, and the absence of unimplemented-NID dispatch misses.
It is useful before bringing up another title because it catches generic
pipeline and composition failures without requiring an ISO: dropped production objects, stale or
missing chunks, entry discovery regressions, bad relocations/imports, broken scheduler startup,
guest-to-HLE dispatch failures, fake-success handler regressions on the covered NIDs, and
public-safe link drift.

It does **not** establish commercial-title compatibility or legality, PSP timing, rendering or
audio correctness, physical UMD behavior, or title-specific runtime bindings. Those remain separate
private-title, visual/audio, and hardware evidence domains.

#### AOT-gap dispatch seam

`mingw32-make production-smoke-gap` builds the same source-owned fixture in its `aot-gap` mode:
identical guest addresses (entry, helper at `0x08804068`, import stubs, out-slots, result slot,
sentinel), but
the mode's build-time codegen choice `--omit-aot=0x08804068` removes the helper from native
emission/registration only. The guest bytes stay complete in the image inside the ordinary
executable `.text` extent; region A's direct `jal` therefore compiles to the ordinary production
`dispatch(s, 0x08804068)` statement — the same mechanism real generated code uses when control
leaves its directly compiled destination set. Generated `sr_register_all()` records the analyzer's
exact executable ranges before registering native functions; mapped guest RAM outside those ranges
is never implicit code. The production interpreter executes the omitted helper's source-owned bytes
and its delay slot, then transfers to registered AOT region B at `0x08804098`. Region B commits the
interpreted `0x00001235` value before the real HLE path and production-driver assertion. Nothing
patches generated C after codegen and nothing substitutes host-side helpers.

The scalar interpreter floor also covers `lwl`, `lwr`, `swl` and `swr`. These
forms merge bytes within the aligned containing word, use the same runtime
accessors as generated code, and retain fail-closed bounds checks in the
interpreter.

#### AOT/interpreter cosimulation

`mingw32-make cosim-selftest` takes the same seam further: one pipeline run over a source-owned
synthetic guest ([`fixtures/cosim/`](../fixtures/cosim/)) produces both execution lanes for the same
guest bytes, and a comparator runs every cell twice — once through the generated `f_<addr>` body,
once through the production interpreter floor with that body absent from the dispatch table — then
reports the **first** difference. Selection is at run time rather than through `--omit-aot`, so one
build compares every cell.

Four independent channels are compared: the canonical per-instruction trace
([`tools/TRACE_FORMAT.md`](../tools/TRACE_FORMAT.md), which the interpreter now emits in the same
branch-before-delay-slot order as generated code), the ordered guest writes seen by
`sr_note_mem_write()`, the guest memory window, and the full architectural state vector.
`mingw32-make cosim-mutants` proves the comparator is load-bearing by rebuilding it against a
mutated copy of the interpreter — or of `tools/codegen.py`, so both sides of the differential are
covered — and requiring each defect class to fail the gate.

Three lane `MIXED` cells (`xcall`, `xtail`, `xnest`) install every native body except one, which is
the seam the AOT-gap floor actually crosses: native caller, dispatch miss, interpreter, back. Beside the
comparison the harness runs interpreter-tier assertions the two-lane shape cannot express — a
fail-closed negative corpus, a sweep of every control encoding in a delay slot, the `jalr`
link-register shape, and a census that probes the production decoder and requires the set it
decodes to equal the set the cells execute.

`CpuState.pc` is not compared directly, because it is not a shared architectural field: generated
code does not maintain a **per-instruction** architectural PC (each instruction's address is a
literal in its `sr_begin()` call), while the interpreter advances `pc` per instruction and leaves
the handoff destination there. Generated code is not silent on `pc` in general — it assigns
`s->pc` at a VFPU interpreter fallback, at a conditional branch leaving the current function, and
in the profile stubs — so the per-lane assertion is exact for the fixture's cells rather than
universal. Both lanes are instead normalized to one **handoff target** and required to
agree, and each lane's own PC behavior is asserted so a change fails the gate rather than silently
redefining the field. `next_pc` and `in_delay_slot` are asserted to remain unclaimed by both lanes,
keeping a future COP0 BD/EPC model free of an existing consumer. The full contract, the cell list
and the limits of the evidence are in [`fixtures/cosim/README.md`](../fixtures/cosim/README.md).

### Compile flags

The live Makefile currently uses:

- **Generated translation units:** direct Make defaults to `-O0`. A validated private title
  adapter may request measured profile-specific values.
- **General runtime objects:** `$(CFLAGS)`, whose direct-Make default begins with `-O0`, plus
  `fno-strict-aliasing`, include paths, feature defines, and warnings.
- **`ge.c`:** a dedicated `-O2 -fno-math-errno` compile rule for software-rasterizer speed.
- **Portable-core objects:** a separate host-neutral `PORTABLE_CORE_CFLAGS` set, currently `-O0`.

The opt-in guest-PC profiler uses an explicit occupied bit, so guest PC `0x00000000` is a valid
profile key for zero-based images. Its bounded 64-probe lookup reports `lookup_drops` in every
profile dump; a nonzero value means the hotspot ranking is incomplete and must not be treated as
authoritative. `make profiler-selftest` covers the zero-PC and saturated-probe cases without game
inputs.

A package build compiles the runtime at `-O2` and generated code at `-O1` for every title
(`PACKAGE_RUNTIME_OPT` and `PACKAGE_RECOMP_OPT` in
[`tools/nk_core/package_cache.py`](../tools/nk_core/package_cache.py)); direct Make stays at
`-O0/-O0`, and an explicit `RUNTIME_OPT` or `RECOMP_OPT` wins. The level was measured for #1730
on HST and two other titles: `-O1` ran generated guest code 11-33% faster than `-O0` outside the
GE, and `-O2` was no faster than `-O1` beyond run-to-run noise while taking up to 3.3 times the
build CPU time. A body too large for its unit compiles alone at `-O0`
([packaging](RUNTIME_PACKAGING_ARCHITECTURE.md)).
Runtime, generated-code, and codegen profile changes have separate content-addressed invalidation
stamps. C objects emit `-MMD -MP` dependency files so transitive headers participate in freshness.
The [`Makefile`](../Makefile) is the source of truth for the current optimization split and
generated chunk policy.

## Runtime Execution Model

### Dispatch

Generated functions use the shared `CpuState`, direct translated calls where emitted, and runtime
dispatch support for computed/dynamic transfers. When tracing is enabled, maintained boundaries
can be compared with reference/oracle traces.

A guest CALL (`jal`, `jalr rd`, or since #968 a linking branch `bal`/`bltzal`/`bgezal` to a
callable target) is a nested native call: the callee's frame lives on the guest thread's host fiber
until its `jr $ra` returns, so host stack use follows guest call depth. A linking branch whose
target is its own link (`bal .+8`, which reads the PC) or a frame-releasing continuation is a jump
that also writes `$ra`, not a call; the analyzer, the frame walk and codegen share that one rule. The
dispatcher's host-stack preflight stops a dispatched transfer at a named
`host-fiber-stack-exhausted` boundary before the fiber is exhausted. A direct `jal` to a known
body does not pass through the dispatcher, so every generated body that makes such a call opens
with `SR_HOST_STACK_CHECK` (#855): one test of its frame address against
`sr_host_stack_window`, the cached range of the running fiber's stack that still leaves the
dispatcher's reserve. A frame outside the window asks the runtime, which takes the dispatcher's
own measurement and either stops at the same named boundary with the same report or refills the
window for the running stack. Every native recursion of generated code passes through a direct
call or the dispatcher, so leaves and dispatch-only bodies carry no check. The window spans one
stack, so after a fiber switch the first checked entry refills it, and the scheduler clears it
when it creates a guest stack that may reuse a released range. Behind the check, each POSIX
coroutine stack has a `PROT_NONE` guard band below it, as Windows fiber stacks have a guard page,
so an overrun that escapes the check faults instead of writing into the next guest thread's
stack.

A guest TAIL transfer (`j`, `jr` on a register other than `$ra`, `jalr $zero`, a non-calling
branch out of the body, or a fall-through into another entry) does not nest (#855). The body records the
transfer in one runtime slot (`sr_tail_record`) and returns, and whoever invoked that body
drains it:

- the generated CALL site (`SR_TAIL_DRAIN`);
- the dispatcher, right after the body returns (`dispatch_call_aot`);
- `sr_call_body`, for runtime direct calls;
- the stack-census and title profile wrappers, for the bodies they wrap.

Each drained body may record the next transfer, so a loop of tail transfers runs in constant host
stack. A transfer to a computed target is one dispatch (`dispatch_tail_step`). That dispatch runs
the direct transfers that follow it, which used to nest inside it, so exception and ERET flow
they raise is still handled there. It hands only the next computed transfer back to the
enclosing drain.

A callable body's entry-SP restore travels with its transfer, and each drain applies the first
one recorded in its chain: the value the nested returns used to leave. A computed target still
runs at the live SP. An interpreted body entered by a computed step leaves the same way
(`sr_guest_interp_run_tail_step`): its handoff to native code is a recorded transfer, with the
entry-SP restore a native body entered there would record, so both tiers give the chain the same
restore (#1935). A body whose `$sp` load left its frame, a context switch or a longjmp, may have
moved the guest out of frames entered before it: its record releases them (`SR_TAIL_RELEASES`),
bounded by the `$sp` it loaded. A load below where `$sp` was leaves every frame. A load at or
above the body's entry `$sp` leaves only the frames whose restore is at or below that `$sp`, so an
alloca free in a piece keeps the restore of the body that handed it the frame. Once a restore is
void, the first restore recorded after the release is the chain's. A body that leaves that way by
returning tells the drain that ran it (`sr_tail_note_exit`), and so does a computed step's chain,
for the enclosing drain and for an interpreted caller's continuation.

The slot is never live across a yield, syscall, dispatch entry or unwind; each of those refuses a
live transfer with a named `TAIL_TRANSFER_FAULT`.

**What a body owns.** A body owns exactly the words its entry can run. `j`, `jr`, `jalr $zero` and
the `b` idiom (`beq $zero, $zero`, which the analyzer's CFG also gives no fall-through) end a
straight line after their delay slot, so the words after it belong to the body only when another
edge reaches them (#1730). Walking on past `b` used to copy the next function into every body that
reached it: on one compilation disc a module of about 100,000 instructions became 2 GB of C. A
branch may also land on the delay slot of such a transfer; on that path the slot is an ordinary
instruction, so the body owns the word after it too, whichever the walk meets first. The emitter
refuses a body whose slot label has no owned successor. `cosim-selftest` covers both (`bjoin`,
`bslot`, `jslot`, `bself`).

**Entry splitting.** A body owns the code it reaches from its entry. Straight-line fall-through and
`j` into another entry already leave through TAIL transfers, but a conditional branch, taken or not
taken, used to carry the body into the other entry's code and translate a copy of it. In a large
branch-connected region every entry then holds a copy of the whole region (one title: 6,539
entries in a region of 1.33 million instructions). A body whose walk exceeds
`CodegenOptions.region_split_threshold` instructions (`tools/codegen.py`, `--region-split-threshold=N|off`)
instead stops at every other entry. A conditional branch into one records a TAIL transfer to that
entry's own body after the delay slot (annulled on the not-taken path of a branch-likely), and a
not-taken successor that is one is a continuation, exactly as straight-line fall-through is. A
branch back to the body's own start stays a loop edge. Every reader of a body's ownership (the
emitter, the stale-code records, the LLVM backend's lowering plan) gets the same split answer, and
the LLVM backend lowers a crossing as the same TAIL transfer. Bodies within the threshold are
byte-identical to the unsplit output. `mingw32-make cosim-selftest-split` runs the cosimulation
with every body split (`xing_*` cells), and the split-generator mutants show it catches a crossing
that drops or misplaces its delay slot, ignores its condition or loses its fall-through.

Some paths still nest:

- Interpreter-to-AOT handoffs go through `dispatch()` and add one dispatch per crossing.
- Stack-census builds keep one nested drain per transfer, because each census wrapper drains its
  own body.
- Per-function profile time for a computed target still includes its direct successors, but no
  longer the work after the next computed transfer.

### Resume at a PC (direct save states)

A direct save state (#2172) has to continue a guest thread from data. The thread's registers are
data (`CpuState`), but a parked thread also sits inside host frames: one generated C function per
guest call it is in, and under them the dispatcher. Those frames cannot be saved. This section is
the contract for rebuilding them. It is separate from the interpreter floor above and does not
extend it.

**Native re-entry, not interpretation.** A restored thread goes back into the same generated
bodies, at the statement where each one stopped, and from there runs the code an uninterrupted
run runs. Running the restored frames on the interpreter floor instead was rejected, because the
two tiers differ in state a save state and a replay checkpoint both hold:

- `CpuState.pc`: generated code writes it at a yield and a few other sites, the interpreter per
  instruction, and a checkpoint hashes the whole `CpuState` (`nk_ckpt_add_cpu`).
- yield points: every `SR_YIELD` counts down the slice and, under `SR_DETERMINISTIC_CLOCK=1`, each
  `sr_yield` charges guest time. Where they sit (a body's entry, a taken backward branch, the
  dispatcher) is decided by `tools/codegen.py`; the interpreter has none.
- `$sp` at a return: a callable body ends with `s->r[29] = _sp_entry`, where an interpreted
  `jr $ra` leaves `$sp` as the instructions set it (the `spleak` cell).
- coverage: the interpreter decodes only the forms a cosimulation cell executes. This one decides
  alone: a restored frame with any other instruction stops.
- the way back: the interpreter hands over to generated code only at a body's entry, so a restored
  frame stays interpreted until it returns, and a thread's main loop never does.

Copying host stacks was rejected too, for a state file and for in-process rewind alike. A file
cannot hold them: they are absolute host addresses, so a copy can be neither checked nor loaded
into another process. In one process the addresses stay valid, but the copy still cannot be
checked, and it holds pointers into runtime state that a rewind would have to put back in place.

**When a thread is parked.** A state-resumable body names the point it continues at before it
enters `sr_yield`. That does not park it. `sr_yield` then advances guest time, latches and
services interrupts and callbacks, and only afterwards decides whether to switch; most often it
returns without switching. A thread is parked once `sr_yield`'s own switch has switched it out.
The scheduler marks the thread at exactly that switch, until it runs again, so a capture
describes threads from the scheduler's side, `sched_resume_describe`, and the running fiber is
never described, not even inside its yield: a frame list taken there would skip the rest of the
yield. After that switch `sr_yield` does nothing more, so the point's label is the whole
continuation.

**What a thread is, as data.** A frame list, outermost frame first (`SrResumeFrame` in
`src/rt/recomp.h`). Each frame names:

| Field | Meaning |
| --- | --- |
| `body` | The entry address of the generated body the frame runs in. One guest PC can sit in several bodies, so the PC alone does not name it. |
| `pc`, `kind` | The resume point. `SR_RESUME_POINT_CALL`: the frame is inside a direct call and continues at the call's return PC once the frame above returns. `SR_RESUME_POINT_YIELD`: the body is parked at a loop edge's yield and continues at the edge's target. `SR_RESUME_POINT_ENTRY`: it is parked at its entry yield and runs from its first instruction. One PC can be two points with two continuations (a loop head that is also a call's return PC), so the kind is part of the key. |
| `sp` | The body's entry `$sp`, its `_sp_entry`: the one C local a callable body keeps across statements. |

Every frame but the innermost is a call point; the innermost is a parked point. The live `$ra` is
not part of it, as at a `dispatch_call`.

A frame list means something only to the generated code that wrote it: it names bodies and
points in them, and another build has other bodies. `sr_resume_validate` checks structure, not
origin, so the binding is the state file's. An `nkstate` file has a field for the SHA-256 of the
generated code and refuses another build as `executable-identity` before any loader runs. No
capture fills that field for a state yet; the one that does has to hash the generated code that
is running. The frame lists
travel only in that file, in its `FRMS` segment (`nkstate_frames_save`, `nkstate_frames_load`),
whose version (`NKSTATE_FRAMES_VERSION`) changes with what a frame holds. `nkstate-selftest` loads
a state with that segment under its own identity and requires `executable-identity` under another,
with the frames loader not reached.

What a describable thread implies about the per-thread state the scheduler keeps outside its
registers: it is in no import (`hle_depth` 0, no import active), which `sched_resume_describe`
checks; it runs generated code; and interrupts and thread dispatch are enabled, or `sr_yield`
would not have switched, unless a stop was requested, when `sr_yield` switches with dispatch
suspended. Its outermost frame is a thread's own entry (`coro_body`): a callback or
interrupt entry opens a scope and is refused. Whoever resumes the thread therefore owes what
`coro_body` does after `dispatch_from_host` returns.

**Resume points exist only where codegen writes them.** `codegen.py --state-resume` is opt-in and
the default output is byte-identical. It is not a default build: its cost (a test of
`sr_resume_request` per body entry, four more stores per direct call, a `switch` per body, and a
second entry into every loop that contains a direct call) has not been measured on a title, and
builds with promoted registers, the LLVM backend or the LLE CPU have no direct states at all.
With it, `state_resume_pass` rewrites three statement shapes of a callable body and nothing else:

- the body's first statement becomes `SR_STATE_RESUME_TEST`, which sends a resume to the body's
  `switch` before the host-stack check and the entry yield, so entering a body to resume it counts
  no yield point; the entry yield becomes `SR_STATE_ENTRY`, which names the entry point;
- a loop edge's `SR_YIELD(...); goto L_<target>;` becomes `SR_YIELD_PARK`, which counts and sets
  `s->pc` exactly as `SR_YIELD` does and, around `sr_yield`, records the body, the target and the
  body's entry `$sp` on the call record the body runs under;
- a direct call's boundary is begun with `sr_aot_return_boundary_begin_at`, which also records the
  callee, the calling body and its entry `$sp`, and the call gets a label between the callee and
  the call's own tail drain and end check.

A point's label is entered with only the entry `$sp` restored, so no other C local may be
live there and what follows has to be straight-line code. The pass reads the body's block
structure before it writes anything (`_StateResumeScan`). A call gets a point only when its line
closes every block it sits in and none of them belongs to a loop or a `switch`, so an
enhancement site that reads a local of its own after the call keeps its text; a loop edge gets
one only when its target label is at function scope. A body in which the scan finds any other
function-scope local, or which it cannot read with certainty (a line comment, a preprocessor
line, a statement that may be a declaration it cannot name), is left as it is.

The body ends with one `switch` over `SR_RESUME_KEY(kind, pc)`: a call point runs
`sr_resume_inner` (the frame above) and jumps to the call's label, so the drain and the end check
run after a resumed callee exactly as after a called one; a parked point jumps to its label. That
switch is the only list of a body's points. A body with guest-owned `$sp` exits, a live-frame
resume entry (`r_<addr>`), a runtime-placed module's body, and any statement of another shape have
no point. `--state-resume` refuses `--stack-census`, `--promote-registers` and `--backend=llvm`,
each of which keeps state a point does not restore, and `--lle-cpu` and `--lle-import-seam`,
under which every direct call is followed by a flow check and would get no point.

**Where the points are written.** Today a pass over the emitted lines writes them. That is the
form this contract was proved in, not the form it keeps. The pass has to read back C it did not
write, and there are things it cannot do at all: flush a promoted register at a point, reach an
LLVM body, or give a call a point when the emitter put a flow check after it. The emitters know
each of these by construction. The points therefore move into the emitters, for the entry, the
loop edge and the direct call and in both backends, before any new park shape is added (#2172).
The frame list, the refusals and the test below do not change with that move.

**Describing a thread** (`sched_resume_describe`, then `sr_resume_describe`) reads the
switched-out thread's return-boundary records, which follow its fiber, and refuses unless the
frame list is exact. No refusal is a guess in either direction:

| Refusal | The thread is not described because |
| --- | --- |
| `no-frames` | it is not inside a host entry |
| `callback-or-interrupt-live` | a callback or interrupt scope is open on its fiber: a nested host-to-guest entry is on its stack, above its call records or, when its own code has begun none yet, in their place |
| `tail-chain-live` | a tail drain is running a body: the chain's owed restore and release are on the host stack |
| `foreign-cpu-state` | a call record belongs to another `CpuState`. Under the scheduler every thread's records name the one shared `CpuState`, so this separates threads only where each has its own, as in the cosimulation harness |
| `root-not-host-entry` | its outermost call is not a host entry's first dispatch of a body: it does not return to `SR_HOST_RETURN_PC`, or the interpreter ran first and handed the frame on, or the call was dispatched again for exception or ERET flow its code raised. In the last two the host frames below still hold what the call owes |
| `return-in-flight` | a call record holds a captured return target or an unwind |
| `frame-not-described` | a live call above the outermost was not begun by a direct call site of a state-resumable body: a dispatched call (`jalr`), a call from the interpreter, a runtime call, or a body with no points |
| `frame-chain` | a call's calling body is not the body the call below entered: a tail transfer changed the body |
| `frame-sp` | a body's entry `$sp` is not the `$sp` of the call that entered it |
| `not-parked` | the thread is running (the fiber that asks, or one inside its own `sr_yield`), or was switched out inside a call and not by `sr_yield`'s own switch, or is inside an import, or its innermost body named no resume point |
| `too-many-frames` | the list does not fit |

**Resuming** (`dispatch_resume_from_host`) is `dispatch_from_host` for a restored thread. It first
validates (`sr_resume_validate`), and a refusal comes before any guest or runtime state changes:
`bad-frames`, `resume-point-kind` (a call point innermost, or a parked point below a frame),
`body-not-registered`, `body-stale`, `body-not-resumable` (the function registered for the address
has no points; it is not called), `no-resume-point` (the body is asked; its switch has no such
key), `frame-chain` (a call point's callee is not the body of the frame above) and `resume-busy`
(the fiber already holds a call record or a transfer). Asking a body runs its first statement and
its `switch` only, so validation may be called from any host context. `body-stale` is a refusal
at load only: describing a thread does not look at staleness, so a state can be written that the
same build then refuses, where the uninterrupted run would have gone on in its native frame. Then it rebuilds the call records with the
values the host entry and each call site left, enters the outermost body at its point, and runs
the rest of the host entry's `dispatch_call` when that body returns. It does not pass through the
dispatcher's own yield point.

**What is claimed.** After a resume the thread is in the generated bodies, with the host frames, the
call records, the registers and the slice count an uninterrupted run has at that point, so what
follows is the same code on the same state. `mingw32-make cosim-selftest-resume` checks it: the
cosimulation corpus translated with `--state-resume`, where every yield point of the resume
cases parks in turn, the thread is described, and a run that starts from the parked registers, the
parked memory and the frame list, with a rebuilt dispatch table and an empty call-record stack, is
compared with the uninterrupted run on the whole `CpuState` (`pc` included), the memory window,
the guest writes and the instruction trace after the park, and the number of yield points counted.
`mingw32-make aot-return-boundary-thread-selftest` checks the definition of parked with the real
scheduler: a thread whose yield services an alarm and then switches to a peer is refused when it
is described from the alarm handler and is its one frame when it is described from the peer.
The same lane refuses each frame list and each set of call records above by name and checks that
nothing changed, except `body-stale`: stale-code detection is an environment switch the fixture
runs without. Every other cosimulation lane runs the same cases on ordinary output and requires
every thread to be refused. The cells, the mutants and the limits of the evidence are in
[`fixtures/cosim/README.md`](../fixtures/cosim/README.md).

**What is not claimed, and is refused today.** This is the generated-code half of a direct state.
Not built yet, each a separate change under #2172:

- a thread parked anywhere else: in a blocking import (its continuation is "return this value
  from the import", data the wait must latch), at the dispatcher's own yield point, under a
  dispatched call, under a tail drain (the chain's state has to become data on the call record),
  or in a body with a checked `$sp` load;
- the scheduler's side of a restore: making a thread from its saved registers and frame list,
  starting its fiber in `dispatch_resume_from_host`, and the state the scheduler holds for it
  outside both (the slice counter, the wait and wake fields);
- every subsystem's own segment, and the quiet point at which a capture is allowed. Nothing
  writes a state with a `FRMS` segment yet; the segment and its binding exist.

A capture that meets any of them refuses and names it. A resumed thread that parks again is
described from its rebuilt records like any other.

### HLE boundary

At a PSP import/HLE boundary:

1. the runtime identifies the NID/registered handler;
2. the host implementation executes;
3. the PSP-visible result is returned through `$v0` (`r[2]`);
4. scheduler/HLE-specific state transitions occur according to that operation's semantics.

Do not infer PSP correctness merely because a handler returns zero or because a route advances.
Behavioral side effects, waits, wakeups, callbacks, outputs, and error values are part of the ABI.

The source-owned PSP DMAC matrix also keeps the copy boundary explicit. Fully valid RAM/VRAM
requests are copied at their requested size through 1 MiB; the measured `0xC000` prefix is an
allocator-boundary observation, not an API-wide size cap. A source or destination span that runs
past the end of the guest arena, or wraps, is refused whole with nothing copied; that is a runtime
safety boundary, since the PSP's answer past the end of RAM is unmeasured. A guest thread
that starts a channel transfer stays blocked in the call for the transfer's measured duration
(a timing table by size and by main-RAM/VRAM source and destination, with surcharges when source
and destination differ modulo 16); meanwhile another thread's `sceDmacTryMemcpy` returns BUSY
(`0x80000021`) and its blocking `sceDmacMemcpy` queues, and queued callers get the channel in
arrival order. A body of at most 0x100 bytes whose source and destination share their phase
modulo 16 bypasses the channel. A copy into VRAM tells the GPU about every byte it writes: one dirty
notification covers the whole destination range, whatever the size (the selftest pins this for
RAM-to-VRAM copies of `0xC001`, `0x10000` and `0x100000` bytes; other directions are pinned at 64
bytes). A VRAM-source copy that crosses the end of VRAM goes through a 4 KiB bounce buffer and
dirties its destination one piece at a time instead. All of this was measured on a PSP-3000 (#303,
`PSP-DMAC-001` and `PSP-DMAC-002`); the unmeasured choices are listed above the DMAC handlers in
`src/rt/hle_cache_dma.c`. `make dmac-mutants` shows the selftest is load-bearing: it builds
`hle-thread-selftest` against copies of that file with one defect each (a `0xC000` ceiling, a clamped
dirty range, a wrong BUSY copy, ...) and requires every one to fail the assertion it names.

### Scheduler / coroutines

`sched.c` models PSP threads cooperatively. Host execution context is provided by `sr_coro` rather
than being intrinsically tied to one platform API. Windows uses fibers; the repository also keeps
a POSIX/ucontext compile path for host-neutral verification/portability work.

Scheduler correctness includes priority selection, lifecycle, waits/timeouts, callback-aware waits,
and wakeup semantics—not only context switching.

Message Pipes pilot scheduler-owned semantic wait invocations (`sched_wait_*`). A nonrecycled
numeric handle identifies one unfinished call independently of its thread and object. The record
owns its thread association, callback/active-block state, deadline identity, terminal result and
object-detach hook; `SrCoro` still owns executable host-stack preservation. Only the innermost
block attaches to the TCB. Its result is captured before callback dispatch can install a child
wait; callback-parked parents retain their own outcomes, including object deletion.

Message Pipe queues retain only the handle and transfer-specific direction, size and mode.
Notifications make a thread runnable but do not reserve bytes or grant a transfer. Every normal
or abandoned invocation detaches through one object hook, which reconsiders satisfiable successor
requests. Owner teardown invalidates all its invocations before detaching any; hooks never
preempt, and composite terminate/delete completes before replacement execution. Other wait
families remain on their existing APIs pending focused migration and semantic tests. This pilot
does not establish speculative callback timeout policy or Message Pipe argument-five semantics.

### Package-local program replacement (`sceKernelLoadExec`)

`sceKernelLoadExec` accepts only a `ms0:/` guest path that exactly matches a required,
runtime-placed guest program in the running package's validated module manifest. The package's
module directory supplies the corresponding project-built image; the loader does not resolve an
arbitrary host path. The parameter block's argument bytes are copied before the current guest
thread exits. The driver then resets guest RAM, executable-span ownership, scheduler threads and
waits, module/export records, callback and kernel-object tables, partition allocations, file
descriptors, utility-dialog state, and guest audio/SAS/media-player state. Package configuration,
registry and savedata files, and host presentation services persist across the replacement. The
new module starts at its `module_start` with its own `$gp` and the copied `arglen`/`argp` on the
new root stack.

This is a synthetic program-replacement contract. Unsupported target classes and arguments fail
closed with a named refusal; no network route participates in loading or validation.

### Clocks

`sched.c` owns one authoritative monotonic microsecond timeline (`s_vtime_us`). Every guest-visible
time value derives from it; reading a time API never advances it. The clock advances only at
scheduler/emulation progression boundaries (`sr_hle_advance_time`, yield/idle steps, vblank source
delivery).

Clock ownership:

- **System time** — `sceKernelGetSystemTime[Low/Wide]` and `sceKernelLibcClock` read `s_vtime_us`
  directly; the libc clock is elapsed guest time, not Unix time.
- **RTC calendar** — `sceRtcGetCurrentTick`/current-clock map the same timeline through a one-time
  epoch offset (`s_rtc_epoch_tick`), sampled from the host wall clock at first RTC use and anchored
  at guest time zero. After that, RTC reads are pure guest-time arithmetic at the same 1 us/us rate
  as system time. Host wall time never enters an ordinary read path.
- **Scheduler waits** — delay deadlines, timeout deadlines, and remaining-time computations all use
  `s_vtime_us` via the shared `sched_vtime_refresh`/`sched_vtime_deadline_after`/
  `sched_block_on_timeout` plumbing. No wait object or interrupt/precedence semantics are duplicated
  in the clock layer.
- **Display domain** — VCOUNT/HCOUNT/VBLANK phase are a separate display timeline derived from
  `s_vtime_us` through the rational 60000/1001 Hz model (286 scanlines/frame). Display reads never
  deliver vblanks or move the counters. Guest-visible VCOUNT is modelled as an interrupt-gated
  display-source counter. It advances only when the scheduler observes and latches an elapsed source
  period, so it is not a strictly free-running register and is not a count of serviced episodes:
  - **CPU interrupts enabled.** The scheduler source latch
    (`scheduler_latch_due_events` -> `sr_display_advance_vcount`) advances VCOUNT by the number of
    elapsed display periods even when VBLANK *service* is starved, while the serviced episode
    (`deliver_vblank` -> `sr_vblank_tick`) stays coalesced to one and performs
    framebuffer/interrupt/callback work without re-incrementing VCOUNT.
  - **CPU interrupts masked** (`sceKernelCpuSuspendIntr`). VCOUNT stops, and VBLANK delivery stops
    with it; elapsed periods are consumed into the single coalesced pending bit rather than replayed.
    Resume delivers exactly one episode and credits VCOUNT **exactly one** — never `N`, and nothing
    at all when no period became pending.
  - **Clearing the interrupt bit is itself a display-timeline boundary.** Source periods are
    discovered lazily at scheduler boundaries, so `sched_suspend_interrupts()` consumes everything
    already due *before* clearing the bit. Without that step a period that elapsed with interrupts
    enabled stays undiscovered until some later latch, and the most frequent later latch is the one
    `sched_resume_interrupts()` performs before restoring the bit — which would classify it as masked
    and drop it. This is a discovery-time question, not a residency one: private route measurement
    found every dropped period had a boundary predating the mask that later discovered it, while the
    mask itself was held for a negligible fraction of wall time. Per-title rate figures are run
    evidence and belong with the run that produced them, not here.

  The masked-window behavior is `HARDWARE_MEASURED`. The original interrupt-conformance probe
  (historical tracker item #88) found system time advancing while VCOUNT and VBLANK handler
  calls stayed frozen, followed by one coalesced delivery on resume, but it never sampled
  VCOUNT immediately after `CpuResumeIntr`. The source-owned
  `display-mask-vcount` probe (PSP-3001 / 6.61-ARK, 12 trials at each of 4 / 16.7 / 30 / 50 ms) took
  that sample and settled it: a mask crossing no source period credits `+0`, and a mask crossing one
  or more credits `+1` — measured across durations from 0.24 to 3.00 display periods, which crossed
  0, 1, 1 and 2 source boundaries respectively. No trial showed an N-period catch-up. Guest-visible
  VCOUNT is therefore a count of *delivered* VBLANKs, and the observed behavior is consistent with a
  single coalesced pending VBLANK delivery. The probe observes the `+0`/`+1` result, not the
  interrupt controller's internal state, so the coalescing is the model that fits the measurement
  rather than a claim about hardware internals.

  **The display source and the delivered counter are different quantities.** The same probe measured
  `sceDisplayGetAccumulatedHcount` running straight through every mask at the full display rate
  (+69 scanlines over 4 ms, +857 over 50 ms, matching 286 lines per period), so the display
  controller never stops — only the interrupt-gated counter the guest reads does. That probe also
  calibrated the device's own period at 16 682 850 ns (59.9418 Hz), which is 0.003% from the
  60000/1001 model the runtime uses; the rate is now measured rather than assumed. The
  enabled/service-starved multi-period behavior remains `CORROBORATIVE_ONLY`, and the checked-in
  production-path regression is `HOST_TESTED`.

  A companion probe, `display-ge-mask`, established that the GE is a separate hardware domain: a
  stall-gated list released *while interrupts were masked* completed 1 MiB of block transfers in
  12/12 trials at the same speed as with interrupts enabled, while its interrupt-context finish
  handler stayed pending until resume (0/12 during, 12/12 immediately after). The CPU interrupt mask
  blocks interrupt delivery; it does not stop GE execution, and a guest polling GE-written memory
  under a mask is supposed to observe progress.

  Controller sample timestamps and audio pacing use the same vblank counter,
  matching the PSP's vblank-unit pad timestamps.
- **Player pause** — a host-only stall at the scheduler boundary after the stop hook and before
  the next guest thread is selected. No guest fiber runs, so guest clocks, waits, alarms,
  VBLANK delivery, display counters, controller sampling, and guest-driven utility state stay
  fixed. On resume, the scheduler shifts `s_clock_epoch_ns`, `s_vbl_next_ns`,
  `s_last_vblank_ns`, and `s_vbl_latch_ns` by the measured pause interval; this keeps guest
  time continuous and preserves the VBLANK phase. The RTC epoch is not re-anchored, so guest
  wall time lags host wall time by the accumulated pauses for the rest of the session. A pause
  is not PSP suspend and raises no power callback. Losing focus or minimizing the window does
  not pause the guest.

  In the SDL3/Vulkan window, `Esc`, `F1` or the controller's Guide button asks for the pause,
  and so does losing the open controller (`src/rt/gpu_sdl3vk/sdl3vk.c`, `src/rt/gui.c`). The
  request opens the pause at the scheduler's next switch point. It is refused while the
  on-screen keyboard is open, once the presenter can no longer present, without a window
  presenter (headless, offscreen or GDI runs) and under automation; `SR_PAUSE: refused` names
  the reason once per reason. While paused the window shows the frame the guest last had
  displayed under the overlay scrim, redrawn at the display rate and waiting on events while
  minimized; these presents are never guest presents, captures or headless-acceptance frames.
  The overlay owns all input: the guest reads a released pad and a centred stick, and a button
  or stick still held as the overlay closes stays hidden from the guest until it is let go, so
  the press that resumed never reaches the game as a fresh press (`NkInputGate`,
  `src/core/nk_input_profile.h`). Guide is the pause button only; a profile binding to it has no
  effect in game. The runtime draws the overlay's pages (resume, settings, controls, quit to
  library) with nk_ui (`src/rt/overlay/`,
  [#1159](https://github.com/Jstar269/nakagawa-recomp/issues/1159)); v0.0.1 had no pages, and `Esc`, `F1`,
  Guide or the controller's back button (East) only resumed.
- **libc time/gettimeofday** — seconds/usec since the standard Unix epoch, converted from the RTC
  tick. The PSP timezone is a console setting, not the host process timezone. It lives in the
  virtual system registry (`/CONFIG/DATE/time_zone_offset` and `summer_time`), the same keys
  `sceUtilityGetSystemParamInt` reports as TIMEZONE and DAYLIGHTSAVINGS, so
  `sceRtcGetCurrentClockLocalTime`, the UTC/local conversions and the `gettimeofday` timezone
  record follow a setting a game writes through `sceReg` and can never disagree with the getter
  ([#1382](https://github.com/Jstar269/nakagawa-recomp/issues/1382)). On the first run, when no
  per-user registry overlay exists yet, the setting starts from the host's local time zone, which
  the launcher passes as `SR_HOST_TIME_ZONE`, and is saved to the overlay at once; from then on it
  is retained the way a console keeps its setting, and a later host change does not move it.
  Without that variable the seed is the oracle console's measured setting (-300 minutes, daylight
  saving on), not a factory default. The 60-minute daylight-saving shift is the setting's meaning,
  not a measurement. The
  explicit-offset `sceRtcGetCurrentClock` path takes its offset from the caller.
- **Media** — the PSMF timestamp model (`mpeg.c`) and H.264 PES timestamps are stream-relative media
  domains and are not wall time. The `scePsmfPlayer*` handlers sit above a bounded project-authored
  PSMF producer (`psmf_producer.c`): a normalized access unit carries the stream's own presentation
  time, and a picture whose PES packet carried none is extrapolated by one codec frame step from the
  last known time instead of being invented before the first timestamp arrives.

Host behavior: in paced mode (default) `s_vtime_us` tracks SDL's monotonic clock at scheduler
boundaries — a host stall or sleep advances guest time by the stall, except for the player pause
above. Slow frames are caught up by skipping missed vblank slots while preserving the rational
phase carry. The update is forward-only (`t > s_vtime_us`), so host wall-clock corrections/rollback
cannot move guest time backward or jump it. In turbo mode (`SR_NOVBPACE=1`) time advances
deterministically at scheduler boundaries and is never host-dependent. SDL monotonic time is used
for pacing/profiling only; the one host wall-clock read is the RTC epoch init.

**Deterministic clock** ([#2037](https://github.com/Jstar269/nakagawa-recomp/issues/2037)). Paced
guest time includes whatever the host spent between two scheduler boundaries, so two identical
scripted runs saw different guest times (one run's disc-index walk took 5.4 s, the other's 2.2 s,
and the guest's first display flip landed on VBLANK 8 in one and 6 in the other) and, from there,
different frames. Turbo is host-independent but charges a flat 10 ms per scheduler boundary, about
a hundred times what the yield points just executed stand for, so a title that computes between
VBLANKs is outrun by the display and never gets to present. Turbo also jumps the clock to the
nearest sleeper's deadline at a running thread's yield, so a main thread that computes while the
audio thread sleeps 23 ms is charged 23 ms for every slice it runs (Hot Shots Tennis never finished
its start-up this way; explicit `SR_NOVBPACE=1` keeps that jump). The deterministic clock is the
turbo virtual clock charged at the rate of the guest's work: a scheduler boundary adds
`SR_DETERMINISTIC_SLICE_US` (default 100 µs, 1000 yield points at 10 million yield points per
second; an estimate of about 33 cycles per yield point, not a measurement, and it decides guest
results), a sleeper's deadline is reached by that work, an idle scheduler (no thread can run)
charges its slice, runs a sleeper that charge reaches (or serves an alarm, GE completion, VTimer or
transfer window it reaches, while some thread waits), and otherwise goes to the earliest deadline
still ahead when it comes before the next VBLANK or advances one display period when it does not,
as turbo's does, so a sleeper whose deadline is at or past the next VBLANK can wake up to 16.7 ms
late but identically on every run, and no vblank watchdog reads the host clock. The
RTC starts at 2012-01-01 00:00:00 UTC instead of the host's date, and the audio queue the guest
paces against (`sceAudioOutput*Blocking`, `sceAudioGetChannelRestLength`, the PSMF player's audio
lead) is a model that plays 44100 frames a second of guest time from the first push, not the host
mixer's depth, which drains in host time and so would make the length of a blocking output depend
on the host. It is on only when the caller sets `SR_DETERMINISTIC_CLOCK=1` (the picture gate
does, and a video recording requires it); a pad script or a frame-capture window alone keeps the
clock a run always had, because recorded routes and the showcase smoke are qualified under paced
timing. A route qualified that way may not replay the same way on the deterministic clock, so it is
re-qualified under `SR_DETERMINISTIC_CLOCK=1` before it is recorded, and a recorded run is not
real-time, so it cannot be played live
([Recording a video of a run](DEBUGGING.md#recording-a-video-of-a-run)). Guest time and the RTC
are then a function of the guest's own work, so two runs with the same inputs give the same
frames. Audio (with or without a host device, whatever the mixer holds), the Memory Stick's
size and free space, and a fresh profile's console MAC and PSID no longer come from the host:
[Where `SR_EXIT_AT_VBLANK` actually stops](DEBUGGING.md#where-sr_exit_at_vblank-actually-stops)
lists what the guest reads instead.

**Runtime host-thread audit.** Guest threads, HLE handlers, the window's event pump and the
pause loop all run on the runtime thread. Work that runs elsewhere, and why a paused guest
cannot observe it ([#1159](https://github.com/Jstar269/nakagawa-recomp/issues/1159)):

| Host work | Where it runs | What reaches the guest, and when |
| --- | --- | --- |
| On-screen keyboard text entry (`src/rt/osk_text_entry.c`) | A native worker thread, Windows input-box route only | It publishes an answer and writes no guest memory. The guest takes the answer only from its own `sceUtilityOsk*` update calls, so a paused guest takes nothing. The pause is also refused while the in-window keyboard is open. |
| Diagnostic stream drain (`src/rt/debug.c`, [#1296](https://github.com/Jstar269/nakagawa-recomp/issues/1296)) | A native thread, Windows only, started when stdout or stderr is redirected | Every 50 ms it writes the C runtime's buffered stdout and stderr text to the log. It reads no guest memory and writes nothing the guest reads, so a paused guest observes nothing. |
| Android output pump (`src/core/nk_platform_posix.c`, [#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)) | A native thread, Android only, started by `nk_platform_host_startup` | An app process's stdout and stderr go nowhere on Android, so it copies their text, line by line, to the system log and to the run log. It reads no guest memory and writes nothing the guest reads, so a paused guest observes nothing. |
| Texture-scaling workers (`src/rt/gpu_sdl3vk/tex_scale.c`, [#1273](https://github.com/Jstar269/nakagawa-recomp/issues/1273)) | Two native threads, started at the first scaling job of an Ace Mode run with `SR_TEX_SCALE` on | They scale a host copy of texels the renderer already decoded and write only a host buffer. The runtime thread takes a finished copy at a later texture lookup, so nothing reaches guest memory except through the GE rendering row below, and a paused guest observes nothing. |
| Preemption timer (`src/rt/sched.c`, [#1296](https://github.com/Jstar269/nakagawa-recomp/issues/1296)) | A native thread, paced runs started by the driver | It sleeps until the earliest timed wait of a thread stronger than the running one runs out, then ends the running slice so the next yield point enters the scheduler. It reads no guest memory and writes only the slice counter, which the runtime thread resets at every switch, so a paused guest observes nothing. The timer's thread and the runtime thread that starts it ask the host for high priority. On Windows that needs no privilege; on Linux it means nice -10, which an ordinary session refuses (and which threads the runtime thread creates later inherit when it is granted). A refusal leaves normal priority and is logged once, with its cause. |
| Host worker (`src/rt/sched.c` `sched_host_work_*`, [#2361](https://github.com/Jstar269/nakagawa-recomp/issues/2361)) | A native thread, started at the first item of a paced run | It makes a host call that would block the runtime thread: today a `sceIoRead`'s read of the disc image (`io_disc_fetch_run` in `src/rt/hle_io.c`), into a host buffer. It reads and writes no guest memory. The guest thread that asked waits in the scheduler and copies the bytes itself when its wait ends, so a paused guest, whose threads do not run, takes nothing; the read's results, refusals and position are the same call's as without the worker. Under the turbo and deterministic clocks nothing is handed over. |
| SDL audio output (`src/rt/audio_sdl3.c`) | SDL's audio device thread consumes the bound channel streams | The guest reads only the queue depth, on its own calls. A pause stashes each queue and puts back exactly that depth on resume. If the stash fails, the device is paused, so nothing is consumed (`audio-selftest`). The device, and with it SDL's device thread, is opened on the runtime thread by the driver once, before the scheduler runs the guest's first instruction ([#2361](https://github.com/Jstar269/nakagawa-recomp/issues/2361)): the open takes 45 to 92 ms, and no guest call waits on it. In a paced run it follows the scheduler thread's priority raise, so the device thread inherits it where the host passes priority on (Linux); the turbo and deterministic clocks raise nothing. The paced clock starts no later than that raise, so the open's time passes on it before the guest's first instruction: guest time, the VBLANK count and the VBLANK episodes owed start 45 to 92 ms in (3 to 5 VBLANKs), where they started near zero and the same time passed at the first audio call. Under the turbo and deterministic clocks no host time reaches the guest. `SR_AUDIO_DISABLED=1`, `SR_AUDIO_BACKEND=null` (or `none`, `0`, `off`), or `SR_HEADLESS=1` without `SR_AUDIO_BACKEND` opens no device. A run without the scheduler opens the device at its first audio call. |
| Audio dump (`SR_AUDIODUMP`, postmix callback) | SDL's audio device thread | It writes the mixed output to a host WAV file and reads no guest memory. |
| Audio pull reports (each channel stream's get-callback) | SDL's audio device thread | It reads its stream's queue depth and records the size of the device's pulls and whether the channel has been played dry. It reads no guest memory; its results are host atomics, which the guest's audio calls read as they read the queue depth, and a paused guest makes none. |
| Asynchronous file I/O (`sceIoReadAsync` and related calls) | The runtime thread | A request completes in `io_async_dispatch_tick`, which runs at guest syscall entry and counts syscalls, not host time. A paused guest makes no syscalls. |
| Media decode (ATRAC3+, H.264, MP3) | The runtime thread, inside the guest's decode call | Nothing runs between calls. |
| GE rendering and readback (`src/rt/gpu_sdl3vk/ge_gpu.c`) | The host GPU | Results reach guest memory only through the read barrier the runtime thread takes before a guest read or a present. Its timing changes when bytes are copied, never what the guest reads. |

Two source guards in `tools/test_sched_invariants.py` keep this table complete. One fails when a
native thread creator appears outside the keyboard, diagnostic-stream, Android output-pump, texture-scaling and scheduler files, or a third one in the scheduler's (it has the preemption timer and the host worker). The other fails when a host callback
entry appears that is not listed above: SDL audio, timer, event-watch and dialog callbacks, OS
thread pools, waits, timers, completion routines, and Media Foundation work queues. Before new
host work is added, its row must say how a paused guest is kept from observing it.

## Environment Variables

The runtime has many diagnostic and behavior switches. This table is intentionally a selected
architecture-level subset; `docs/DEBUGGING.md`, `nk_manager.ps1`, and the implementing source are
the maintained references for exact behavior.

| Variable | Values | Purpose |
| --- | --- | --- |
| `SR_NOVBPACE` | unset, empty, `0` / `1` | Vblank pacing mode: unset, empty, or `0` = paced (default); `1` = turbo. Other values fail closed |
| `SR_DETERMINISTIC_CLOCK` | unset, empty, `0` / `1` | Deterministic guest clock: `1` = on; unset, empty or `0` = off (paced, or turbo with `SR_NOVBPACE=1`). `1` with `SR_NOVBPACE=0` and other values fail closed |
| `SR_DETERMINISTIC_SLICE_US` | `1`..`100000` | Guest microseconds the deterministic clock charges per scheduler boundary (default 100). Other values fail closed |
| `SR_PAUSE_SCRIPT` | `<vcount>:<host_ms>[,...]` | Test-only scheduler pause source for showcase checks. Append `:user` to exercise an interactive refusal; malformed values fail closed |
| `SR_GPU_GE` | `0` / `1` | Software vs Vulkan GE path |
| `SR_GPU_TNL` | unset / `1` | With the Vulkan GE path, transform, skinning, fog and diffuse lighting of eligible transform-mode triangle draws run in the Vulkan vertex shader, with the same bits as ge.c's T&L ([#318](https://github.com/Jstar269/nakagawa-recomp/issues/318)), on a device that keeps fp32 subnormals in shaders (`shaderDenormPreserveFloat32`; elsewhere ge.c keeps T&L); any other value keeps ge.c's T&L |
| `SR_GPU_LOG` | present/unset | GPU diagnostics where implemented |
| `SR_VIDEO` | e.g. `gdi` | Select host fallback video path |
| `SR_PRESENT_SCALING` | `sharp` (default) / `integer` / `bilinear` | How the Vulkan presenter scales game frames into the window: sharp bilinear (nearest to the next integer multiple, then linear), the largest integer multiple letterboxed, or plain linear; an exact integer fit is a nearest blit in every mode ([#1129](https://github.com/Jstar269/nakagawa-recomp/issues/1129)). The player's Settings set it |
| `SR_PRESENT_UPSCALE` | `off` (default) / `fsr1` | The presentation upscaler: with `fsr1` the Vulkan presenter scales the shown picture to its place in the window with AMD FidelityFX Super Resolution 1 (EASU, then RCAS sharpening; `src/rt/gpu_sdl3vk/fsr1_pass.c`, the FidelityFX SDK v2.4.0 MIT headers in `third_party/fidelityfx_sdk/`) in place of `SR_PRESENT_SCALING`'s filter, on every frame the presenter shows except CPU-presented frames while `SR_MOVIE_UPSCALE` is on. Presentation only: the guest's frame, guest VRAM and captures are untouched. Used only when the picture is enlarged |
| `SR_MOVIE_UPSCALE` | `off` (default) / `fsr1` / `anime4k` | The movie upscaler, for frames the CPU presents (decoded movies; every frame when the software GE draws): `fsr1` scales them with FSR 1 as `SR_PRESENT_UPSCALE` does, and `anime4k` first doubles them once or twice with Anime4K's CNN x2 (M) upscaler (`src/rt/gpu_sdl3vk/anime4k_pass.c`, the MIT shader in `third_party/anime4k/`), then takes them to the window's size with FSR 1 unless the doubled picture already is that size. It overrides `SR_PRESENT_UPSCALE` for those frames. Presentation only: the decoded movie, the guest's frame, guest VRAM and captures are untouched. Used only when the picture is enlarged |
| `SR_LAUNCHER_DISPLAY_BOUNDS` | `<x>,<y>,<w>,<h>` or empty | Desktop bounds of the display that holds the launcher window, set by the launcher. The game window, windowed or fullscreen, opens on the display that contains the centre of that rectangle; empty, malformed or off-screen values use the primary display, and the last two are reported on stderr |
| `SR_FBSNAP` | positive integer | Rotating PPM snapshot interval |
| `SR_HLELOG` | present/unset | HLE dispatch diagnostics |
| `SR_HLE_DIAGNOSTICS` | present/unset | Retained title-scoped HLE diagnostic reads; requires the validated HST code-generation profile |
| `SR_SYSLOG` | present/unset | System-call diagnostics |
| `SR_THLOG` | present/unset | Scheduler/thread diagnostics |
| `SR_BLOCKLOG` | present/unset | Blocking/basic diagnostic output where consumed |
| `SR_IOLOG` | present/unset | Filesystem/I/O diagnostics |
| `SR_AUDIOSTAT` | present/unset | Audio queue and drift telemetry (`AUDIOSTAT_LEAD`) |
| `SR_AUDIO_DISABLED` | `1` | Use the null audio backend; `SR_AUDIO_BACKEND=null\|none\|0\|off` does the same, `dummy` selects SDL's dummy driver |
| `SR_MSGLOG` | present/unset | Bounded message-pipe diagnostics |
| `PSP_VFPU_TABLES` | path | Override VFPU lookup-table directory |
| `PSP_ISO` | path | Private ISO path where a route consumes it |
| `SR_MEMSTICK` | path | Canonical host Memory Stick root shared by ordinary `sceIo*` `ms0:` I/O and savedata (default `memstick/`) |
| `SR_FSDIR` | path | Legacy flat `fs/` source for one-time read-open import into the unified Memory Stick root (default `fs/`) |
| `SR_HOST_TIME_ZONE` | `<minutes>,<0\|1>` | The host's standard offset from UTC and whether daylight saving is on now, set by the launcher; adopted as the console's time zone only on the first run, when the registry overlay does not exist yet (#1382) |
| `SR_SYSTEM_REGISTRY` | path | Per-user overlay file of the virtual PSP system registry that `sceRegFlush*` writes (default `registry/system.json` in the per-user data directory) |

Many legacy Boolean diagnostics are enabled by **presence**, so setting them to the literal string
`"0"` may still enable them. Remove/unset such variables to disable them. Value-parsed switches
such as `SR_GPU_GE`, numeric settings such as `SR_FBSNAP`, and the `SR_DEBUG` bitmask are separate
cases. The manager profiles clear stale variables before launching and are safer than accumulating
manual environment state.

## Debug Framework

`src/rt/debug.h` / `debug.c` provide the central `SR_DEBUG` bitmask categories:

| Bit | Hex | Category | Description |
| --- | --- | --- | --- |
| 0 | `0x01` | `SR_DBG_MEM` | Memory access/watch diagnostics |
| 1 | `0x02` | `SR_DBG_HLE` | HLE diagnostics |
| 2 | `0x04` | `SR_DBG_SCHED` | Scheduler/thread diagnostics |
| 3 | `0x08` | `SR_DBG_GE` | GE/graphics diagnostics |
| 4 | `0x10` | `SR_DBG_INPUT` | Input diagnostics |
| 5 | `0x20` | `SR_DBG_FS` | Filesystem/I/O diagnostics |
| 6 | `0x40` | `SR_DBG_VIDEO` | Display/framebuffer/vblank diagnostics |
| 7 | `0x80` | `SR_DBG_MISC` | Miscellaneous subsystem diagnostics |

Example:

```powershell
$env:SR_DEBUG = "0x03"  # memory + HLE
```

The memory-watch and crash-reporting facilities are described in `docs/DEBUGGING.md`.

## Where to Look When Something Breaks

| Symptom | Start here |
| --- | --- |
| Pipeline/codegen failure | `tools/prxload.py`, `tools/imports.py`, `tools/analyze.py`, `tools/codegen.py` and the first failing command |
| Native compile/link failure | Makefile output, exact compiler/linker command, UCRT64/SDL3/Vulkan SDK availability |
| Unknown NID | `src/rt/hle.c`, import manifest/audit tools, `SR_HLELOG` diagnostics |
| Translation mismatch | `tools/codegen_gate.py`, `tools/funcdiff_cmp.py`, reference interpreter |
| Renderer mismatch | software/Vulkan A/B snapshots plus `tools/ppmdiff.py`; use an external oracle before claiming PSP correctness |
| VFPU mismatch | `src/rt/vfpu_interp.c`, generated VFPU tests, pinned tables, reference behavior |
| Scheduler deadlock/wait issue | `src/rt/sched.c`, callback/wait tests, `SR_THLOG`/targeted diagnostics |
| Repeated/infinite guest execution | identify owning guest thread and PC first; repair the missing semantic/control-flow cause rather than adding a loop cap |
| No presented frame | inspect GE/presentation diagnostics and the first upstream failure that prevents valid draw/present work |
