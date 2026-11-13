# Enhancement Contract: Authentic Baseline, Capability Isolation, and Mod Boundaries

> **Status: CURRENT — maintained architectural contract.** Issue reference: #327. Build cache coordination coordinates with #316, and high-refresh presentation coordinates with #328.

Sections 1–7 define the authentic baseline, category boundaries, fail-closed package validation (`tools/enhancement_package.py`), and AOT cache epoch separation (`tools/nk_core/package_cache.py`, `tools/codegen.py`). Declarative frame-rate rescale packages (60 Hz logic rescaling for 30 Hz titles) are implemented on the AOT codegen and package-builder path; host runtime asset replacement and overlay loaders are not built. Section 10 covers the project-authored library-wide mode, and section 11 the class of its items that change what the guest can observe (#2055).

## 1. Core Principle: Authentic Mode is the Untouched Baseline

Nakagawa Recomp's primary architectural foundation is verifiable, measurable, and bug-compatible PlayStation Portable execution on native 64-bit Windows. The authentic/fidelity execution mode is the project's authoritative ground truth.

To prevent runtime contamination, title-specific regressions, and untracked behavior:

1. **Authentic mode is authoritative:** All canonical correctness suites, hardware-oracle measurements, public acceptance tests, and private fidelity routes execute strictly with all host enhancements disabled.
2. **Untouched baseline:** In authentic mode, the runtime and recompiled code execute with zero enhancement overhead, zero mutation of guest state, and zero host presentation deviations. The instruction trace and hardware-observable behavior remain bit-for-bit identical to standard recompilation.
3. **Opt-in layering:** Host enhancements (such as HD texture replacement, high-refresh pacing, free cameras, or debug overlays) are explicit, isolated capabilities layered strictly above the authentic baseline.
4. **No hidden emulator hacks:** Enhancements are never implemented as ad hoc bypasses, magic memory writes, or hardcoded addresses inside the generic runtime core.

---

## 2. Enhancement Categories and Observation/Mutation Boundaries

Every enhancement package must declare exactly one primary category. Each category defines a rigid observation and mutation contract that determines what guest and host state the enhancement is permitted to inspect or modify. Speculative category payload objects (such as asset replacement tables, presentation parameters, timing multipliers, camera overrides, or save-data configurations) are deliberately not defined in the base declaration schema: each future capability defines its payload specification when that capability lands. The categories below define the observation and mutation boundaries, and the descriptions illustrate future capability examples.

| Category | Primary Target | Observation Boundary | Mutation Boundary | Strict Prohibitions |
| --- | --- | --- | --- | --- |
| `asset_replacement` | Textures, audio streams, fonts, UI icons | Asset identifiers, file read requests, texture descriptors | Host GPU texture allocations, host audio stream playback buffers | Cannot read or mutate guest registers, guest execution flow, or game logic |
| `presentation_only` | Host overlays, FPS counters, aspect-ratio framing, post-shaders | Host swapchain state, frame timings, compositor layout | Host presentation pass, render target compositing, debug overlay draw lists | Zero mutation of guest memory, guest registers, or PSP display list structures |
| `guest_memory_code_hook` | Intro skipping, symbolic routine interception, gameplay patches | Symbolic function entry/exit, exported milestone events, guest memory | Guest memory values, guest function return registers (`$v0`/`$v1`) at symbolic boundaries | Cannot use raw host or guest numeric address literals; must target symbolic identifiers only |
| `save_data_modification` | Save file converters, save-state slot remapping, progression unlocks | Save serialization calls, savedata header structures | On-disk save payloads during load/save serialization | Cannot modify live guest state during gameplay; strictly restricted to I/O serialization seams |
| `camera_projection_override` | Ultrawide FOV adjustment, projection aspect ratio, free camera | GE transform matrices, view matrices, camera update calls | Host projection matrices submitted to the graphics backend | Cannot modify guest physics or collision boundaries unless paired with a declared guest hook |
| `timing_frame_presentation` | High-refresh display (#328), frame interpolation, VBLANK multiplier | VBLANK interrupts, display buffer swap requests, frame intervals | Host presentation swap cadence, presentation interval throttling | Must preserve guest internal game tick rate; cannot silently accelerate gameplay logic |

### 2.1. Asset Replacement (`asset_replacement`)

Asset replacement operates at the resource loading boundary. When the guest title requests a texture or audio track via standard PSP interfaces, the host loader matches the resource identifier (or cryptographic hash of the original asset) against declared replacement tables.

- **Allowed:** Replacing a 128x128 8-bit paletted texture with a host 4K BC7 texture in the GPU pipeline; routing audio playback through a high-definition FLAC/WAV stream.
- **Forbidden:** Altering memory layouts visible to guest CPU code or modifying game data structures in guest RAM.

### 2.2. Presentation-Only (`presentation_only`)

Presentation enhancements operate strictly after or alongside the host graphics presentation step.

- **Allowed:** Drawing an ImGui-based diagnostic HUD or FPS counter; applying CRT or CAS post-processing shaders to the final swapchain render target; letterboxing 16:9 PSP output into ultrawide display windows.
- **Upscaling:** `SR_PRESENT_UPSCALE=off|fsr1` (default `off`) enlarges the shown picture to the
  window with AMD FidelityFX Super Resolution 1 (EASU, then RCAS) in place of the bilinear or
  nearest filter. It reads the shown image and writes private images only; captures still hold
  the guest's frame (`gpu-capture-selftest`, run by hosted CI on lavapipe, presents through the
  real pass and checks both). A run recording, which holds the picture the window shows, is
  enlarged by its own FSR 1 pass at the recording's size and names the scaling it used in `record.json`
  (the same selftest compares it with the pass's output, byte for byte), and guest VRAM and every
  guest flight event match with it off (`display-smoke-fsr1`, which needs a Vulkan device and a
  display, so it is a local gate). In the player it is the Enhanced part "Sharper enlarging"
  (`"upscale"`, off for a new library) on Settings > Enhanced and each title's page; the Medium, Ultra and
  Handheld graphics presets turn it on and Low and High off (section 10.1). It is not part of
  Authentic: the launcher exports `SR_PRESENT_UPSCALE=off` for an authentic session whatever the
  setting or the inherited environment (section 10.1), and a run with it on says
  `ACE_MODE: present upscale fsr1`, emits a `phase=present_upscale` boot event and carries
  `"present_upscale": "fsr1"` in its flight bundle's `runtime.enhancements`, so neither
  `flight_diff.py` nor `boot_gate.py` takes it as authentic evidence. `picture_gate.py` and
  `library_sweep.py` run the runtime themselves, so they set the key `off` (a picture-gate route
  may ask for it) and record the value they used. GPU cost on an RTX 3080 (`fsr1-selftest`): 0.40 ms
  per present for 1920x1088 to 3840x2160, and 0.06 to 0.20 ms at the other preset sizes (`docs/SETUP.md`).
- **Movie upscaling:** `SR_MOVIE_UPSCALE=off|fsr1|anime4k` (default `off`) gives CPU-presented
  frames (decoded movies, and every frame when the software GE draws) FSR 1, or Anime4K's CNN x2 (M)
  network, then FSR 1 to the exact size unless the doubled picture already is that size (as at the
  default 960x544), in the window and in a recording, and overrides `SR_PRESENT_UPSCALE` for those
  frames. It reads the shown image and writes private images only; captures still hold
  the guest's frame, and the decoded movie, guest VRAM and every guest flight event match with it
  off (`display-smoke-anime4k`, which needs a Vulkan device and a display, so it is a local gate).
  A run with it on is stamped as `SR_PRESENT_UPSCALE` is (`ACE_MODE: present upscale
  movie-anime4k`, the `phase=present_upscale` boot event and `runtime.enhancements`), so neither
  `flight_diff.py` nor `boot_gate.py` takes it as authentic evidence. It reaches only frames the CPU
  presents: a movie the GE presents (drawn as a texture, or decoded into a GE target) is a GE frame
  and is not upscaled by it. An Authentic launcher session, the picture gate and the library sweep
  run with it `off`, whatever the environment says.
  GPU cost at 4K: 0.75 ms per present for a 480x272 frame to 3811x2160 on an RTX 3080
  (`anime4k-selftest`).
- **Forbidden:** Mutating GE display list commands, intercepting guest draw calls to alter gameplay geometry, or feeding host input back into the guest controller state.

### 2.3. Guest Memory and Code Hooks (`guest_memory_code_hook`)

Guest code hooks represent the highest-risk category because they directly alter guest architectural state.

- **Allowed:** Intercepting a symbolic function call (e.g., `title_skip_intro_movie`) to set return register `$v0 = 1` and bypass playback; adjusting game progression flags at verified symbolic lifecycle points.
- **Forbidden:** Specifying raw hex address literals (e.g., `0x08804000`) in hook symbols, capability names or site function names; embedding commercial title binaries, game bytes or patched MIPS machine code inside the package; executing arbitrary unverified host shellcode. A published package may name its sites' addresses in its site map, under the rules of section 5.1.

### 2.4. Save-Data Modification (`save_data_modification`)

Save-data enhancements interface exclusively with the virtual filesystem (`ms0:/PSP/SAVEDATA/`) and the PSP utility savedata subsystem (`sceUtilitySavedata`).

- **Allowed:** Remapping save slots to host profile folders; importing external savedata with checksum recomputation; converting regional save formats.
- **Forbidden:** Hooking active memory during combat or gameplay to simulate infinite inventory or health; altering save data during authentic hardware-oracle test executions.

### 2.5. Camera and Projection Override (`camera_projection_override`)

Camera overrides operate at the 3D projection interface, transforming guest camera matrices before graphics submission.

- **Allowed:** Calculating 21:9 or 32:9 projection matrices from guest 16:9 aspect ratios; providing host mouse/gamepad camera offsets to view matrices.
- **Forbidden:** Altering collision meshes, modifying culling logic without declaring a guest hook, or de-syncing game physics from camera motion.

### 2.6. Timing and Frame Presentation (`timing_frame_presentation`)

PSP titles commonly lock physics and animation loops to 30 Hz or 60 Hz VBLANK ticks. Enhancing presentation or rescaling frame-counted game logic to run at a true 60 Hz requires strict separation from real-time clocks (see #328):

- **Allowed:** Interpolating host rendering between consecutive guest frames; adjusting presentation swap intervals; multiplying display updates while keeping guest timer tick counts authentic; applying the declarative frame-rate rescale transforms of section 9.2 to verified symbolic sites during an explicit AOT enhancement epoch.
- **Forbidden:** Speeding up the guest real-time clock (`sceKernelGetSystemTime*`, `sceRtc*`, audio hardware sample clocks) to force higher frame rates, which breaks real-time clocks, physics, and audio synchronization. Only frame-counted logic is rescaled; the game's real-time clock stays 100% authentic.
- **Guest memory:** A timing transform writes guest memory only where it replaces, at the same place, a value the stock logic itself stores there, with that value's 60 Hz image (section 9.2, "Guest memory written by a timing transform"). Any other write belongs to section 2.3.
- **RNG Policy for 60 Hz Rescaling:** Frame-rate rescale packages must declare an explicit `rng_policy` (`rescale_probability` or `subtick_30hz`) with its rationale. Under `rescale_probability`, stateless per-update probability thresholds use `probability_per_update_x0.5` ($p_{60} = 0.5\,p_{30}$) so expected events per second remain invariant at 60 Hz, while stateful multi-roll PRNG sequences or coupled physics/particle steps use `gate_30hz` to step on alternate 60 Hz ticks (30 Hz cadence) without altering PRNG state evolution.

---

## 3. Metadata, Flight Recorder, and Evidence Tracking

Captures, traces, and performance logs must never be mistaken for authentic execution evidence. When enhancement loading lands, every enabled enhancement must be stamped deterministically across all diagnostic channels:

1. **Process Exit and Diagnostics:** The native player and CLI must print visible status indicating that enhanced mode is active, naming the loaded package ID and capabilities.
2. **Flight Recorder (`flight_recorder_schema.json`):** Active enhancement package identifiers and their requested capabilities must be stamped into the recorder manifest header, and any event log recorded under enhanced execution must be tagged as non-authentic. The comparator (`tools/flight_diff.py`) prints MATCH only for two comparable, fully retained captures: differing build identities are reported, never compared, while differing schema/runtime/event-class/trigger coverage or any dropped history is refused as INCOMPARABLE (exit 3) instead of matching (#320).
3. **Bring-Up Reports (`bringup_report.schema.json`):** Public bring-up and stage evidence reports must reject runs performed with enhancements enabled unless explicitly categorized under a separate non-canonical evidence suite.
4. **Crash Dumps and Logs:** Crash reports must include full enhancement provenance, identifying whether the failure occurred within authentic core execution or inside an enhancement hook boundary.

> **Rule:** No test run or performance capture conducted with enhancements enabled may be cited as evidence of authentic PSP hardware conformance or fidelity acceptance.

---

## 4. AOT and Build Cache Invariants (Coordination with #316)

Issue #316 establishes content-addressed private AOT build caches with explicit compatibility epochs (`AotPackageManifest`). Reusing compiled native C chunks safely requires that cache identity reflect all semantic inputs that affect code generation.

Enhancements are governed by the following cache rules:

- **Presentation, asset, and timing enhancements do not invalidate AOT cache:** Because presentation overlays, texture replacements, and swapchain pacing operate entirely at the host runtime layer, they require zero modifications to generated C code. Authentic AOT packages are shared without recompilation.
- **No silent AOT mutation:** An enhancement package can never silently alter the generated C output of a title.
- **Explicit AOT instrumentation epoch:** If an advanced guest hook requires ahead-of-time code generation hooks or stub wrappers, that hook dependency must be explicitly included in the `AotPackageManifest` cache key. An instrumented AOT package produces a distinct cache digest and cannot collide with or replace the authentic baseline package.

---

## 5. Symbolic Capabilities vs. Raw Addresses

A critical design requirement of Nakagawa Recomp is the complete absence of title-specific guest addresses and title-keyed behaviour from generic code (`TITLE_PROFILE_ARCHITECTURE.md`).

Enhancements must follow the same strict rule:

- **Symbolic hook targets and capability names only:** Enhancement packages declare hook points using symbolic function names (e.g., `symbol: "title_init_display"`, `symbol: "stage_load_complete"`) and request capabilities via symbolic names (e.g., `capabilities: ["hud_overlay"]`).
- **Prohibition of raw address literals:** Enhancement package manifests must not contain raw numerical or hexadecimal addresses in hook symbols or capability names. The validator enforces a uniform rejection rule across all forms: 0x-hex (e.g. `0x08804000`), decompiler-style stems (`sub_`, `loc_`, `func_`), bare 6-8 digit hex strings (including all-letter strings like `deadbeef`), and numeric addresses.
- **Resolution via title profile or site map:** Symbolic hook names are resolved to guest addresses through the title's private profile and symbol registry, or through a published package's site map (section 5.1). The generic enhancement runtime operates strictly on resolved symbols; generic code (`src/`, `tools/`) never holds a title's addresses.

### 5.1. Published Site Data

I publish a title's packages as site data, so that a user who builds from source gets the same enhancements as a user of my own builds. A published package is data about one exact executable, not a piece of it. It holds only:

- the disc ID, the disc version and the SHA-256 of the decrypted executable the title profile selects (usually `EBOOT.BIN`) it was made for;
- a site map: each symbolic function name the package uses and its address in that executable;
- each site's change: one of the declared transforms of section 9.2, never code or values;
- for each byte range a site reads or replaces, a digest of the stock bytes, keyed by the executable (below).

It holds no game bytes: no instruction words, data, tables, strings or values copied from the disc, and no patched code. Every symbolic name is project-authored (from a decompilation or named by the project), never taken from the executable's own symbol table. A disc file path and a CSV column name appear only as locators, naming where a data transform applies. A store site that checks the values the game's loader stores publishes only keyed digests of them (`stock_value_digests`, never `stock_values`): when the game stores a value, the runtime digests it, compares, and scales the value it observed, and on a mismatch it stops the run, naming the site. Where a transform's result depends on the stock value (such as twice it), the package publishes the rule, never the result. The same holds for a transform whose rule is built from other stock values (the site references of section 9.2): the package names the sites that pin them, each with its keyed digest, and the values and every factor computed from them are read and computed from the user's executable when the package is built.

**Keyed digests.** A plain SHA-256 of one 32-bit instruction word can be inverted by trying all $2^{32}$ words, so it would publish the word. Every stock digest in a published package is therefore an HMAC-SHA-256 whose key is derived from the whole executable (a different hash from the published executable digest, so that digest does not give the key away). Only someone who holds the executable can compute or check it, and they already have the bytes.

**Exact match, fail closed.** A published package applies only to the executable it names. Before any site is read, build-package refuses it by name when the disc ID differs (`other_disc`: another region or another game), when the disc version differs (`other_revision`), or when the disc ID and version match but the executable digest does not (`modified_executable`: a patched or re-encoded image). A site whose keyed digest does not match refuses the whole package (`site_mismatch`). The build stays authentic, as section 6 requires.

**Why this is publishable.** It is the same practice as decompilation projects that publish symbol maps (function names and addresses for one ROM checksum) and emulator patch and cheat databases that publish addresses and their own replacement values for a named game ID. In each case the game comes only from the user's own copy, and the published file is useless without it.

**Where they live.** Published packages are separate data files in `assets/enhancements/`, one per disc and package, and pass the publication audit's no-game-bytes check before they enter the public tree. That check sees only the file. It refuses any property outside the format, and any string holding an address, an instruction-word-shaped or long hex run, a base64-like run or an escaped byte. It cannot tell a keyed digest from 32 raw bytes, and no pattern check can stop free text encoded on purpose; those are its stated limits. The export command holds the executable and computes every digest from it, so it is what makes each digest a digest, and review covers the text.

**How a player gets one.** The source tree, which every release carries beside the player, ships these files, so a published package needs no copying. The player and `build-package --experimental-60hz` look for `<user data>/enhancements/<disc_id>-60hz.json` first and, only when that file does not exist, for the same name in `assets/enhancements/` (section 9.4). A file in the user's data folder always comes first, even one that cannot be used, so a package author can try a new declaration; build-package refuses a shipped file that is not in the published form.

---

## 6. Fail-Closed Validation and Incompatible Packages

Enhancement packages are declared as versioned JSON manifests adhering to `assets/enhancement_package.schema.json` and validated by the normative hand-written validator in `tools/enhancement_package.py`.

The declaration schema is kept lean and focused on core metadata and capability requests:

- **Required root properties:** `schema_version` (must be 1), `id` (package identifier), `display_name`, `version` (semver), `category` (one of the 6 recognized categories), `target_title_id` (title ID or `*`), and `capabilities` (non-empty array of symbolic capability names).
- **Optional root properties:** `experimental` (`true` or `false`; a package without it is experimental, section 9.10), `min_runtime_version` (semver), `hooks` (array of hook objects with `symbol` and `type` only), and—for declarative frame-rate rescale packages (category `timing_frame_presentation`, and the `game_version`, `executable_sha256` and `sites` of a `guest_memory_code_hook` package, section 11.3.6)—`game_version` (exact title version string), `executable_sha256` (64-char lowercase hex SHA-256 of the stock guest ELF), `rng_policy` (`mode` and `reason`), and `sites` (non-empty array of symbolic instruction-site transform descriptors).
- **Symbolic site descriptors (`sites`):** Each site entry declares `id` (site identifier), `function` (symbolic function name, never a raw hex address or `FUN_`/`sub_`/`loc_`/`func_` stem), `insn_offset` (non-negative 4-byte-aligned byte offset inside the function), `stock_digest` (64-char lowercase hex SHA-256 of the expected little-endian stock MIPS instruction word or words), optional `word_count` (an integer from 1 to 16, default `1`, the number of words `stock_digest` covers; only `1` has a defined rewrite, so a larger value refuses the package), and `transform` (one of the site transforms of section 9.2). A `gate_30hz_alt` site also declares `alternates`, a list of 1 to 4 distinct function symbols; every other transform refuses that field. A `clip_x_widen` site (category `guest_memory_code_hook`, section 11.3.6) also declares `clip_register` (`v0` or `v1`) and `clip_offsets` (1 to 4 distinct 16-byte-aligned byte offsets from 0 to 65536); no other transform takes them, and a `guest_memory_code_hook` package takes no other transform and carries only `game_version`, `executable_sha256` and `sites` of the timing fields.
- **Published packages (section 5.1):** a published package adds `published_package` (must be 1), `disc_id`, `disc_version`, `digest_scheme` (`hmac-sha256-executable-key-v1`) and `site_map` (each symbolic name the package uses and its `0x%08x` address, and no other), and requires `executable_sha256`. Its stock digests (`stock_digest`, `low_stock_digest`, a data site's `stock_digest`, a data file's `sha256`) are HMAC-SHA-256 under `published_digest_key()`, SHA-256 of a fixed tag and the whole executable. It may not declare `stock_values`: a store site declares `stock_value_digests` instead, the keyed digests (`keyed_stock_value_digest()`) of the tag `stock value`, the site id and each stock value's 32 bits. Codegen embeds the key in the user's own build, and the store-site check (`nk_enh_store`) digests each value the game stores, compares it, and scales the observed value as the builder scales a stock value; a mismatch, or a result outside the f32 or s32 range, stops the run with `ENHANCEMENT_DATA_MISMATCH`, naming the site. `python tools/enhancement_package.py --publish <package> --elf <selected.elf> --base <hex> --disc-id <ID> --disc-version <n.nn> --out <disc_id>-<kind>.json` writes one from a private package and its `.symbols.json`, after checking that it resolves back to the same declaration. Its names come only from that symbol map, and it refuses any name the executable's own symbol table holds. A function without a decompilation name may carry a project-assigned name ending in its own address (`hst_fn_0001ea64`); a name whose 8-hex suffix differs from its site-map address is refused. A published package's `version` has no build metadata and a pre-release tag of at most 16 characters, and its `rng_policy.reason` is at most 1024 characters. Codegen resolves a published package only through `resolve_published_package()`, which refuses `modified_executable` and `site_mismatch`; build-package refuses `other_disc`, `other_revision` and `modified_executable` first, from the disc's PARAM.SFO. `tools/publish_audit.py` (`check_enhancement_packages`) refuses any package outside `assets/enhancements/`, any `.symbols.json`, and any file there, whatever its extension, that is not a valid published package named `<disc id, lowercase>-<kind>.json`. It also refuses a package in which any string or key, outside the digest slots and the site map's addresses, holds an address literal, an 8-hex-digit token delimited by anything but a letter or digit (an instruction word, or an address; a site-map name's own-address suffix is the one exception), a run of more than 8 hex digits, a base64-like run of 32 or more characters, an escaped byte or a control character. A regular-expression gate cannot stop text encoded on purpose: shorter base64 runs, or hex in chunks of 7 digits or fewer, pass it. Bounded fields (`rng_policy.reason` at 1024 characters) limit that, and review covers the rest. The audit checks only a digest's shape: it cannot tell a keyed digest from 32 raw bytes without the executable, so the export command, which computes every digest from the executable, is what makes it a digest.
- **No speculative payloads:** Unimplemented category payloads (`assets`, `presentation`, `timing`, `camera`, `save_data`) remain forbidden in the schema until their respective runtime loaders land.

The package loader and AOT codegen hook (`tools/enhancement_package.py` and `tools/codegen.py`) strictly adhere to fail-closed principles:

1. **Unknown Category or Transform:** Any package specifying an unrecognized category string or site transform is immediately rejected.
2. **Incompatible Schema Version:** Packages with `schema_version != 1` are rejected with an explicit version error.
3. **Address Literals:** Any hook symbol, capability name, or site `function` containing address literal patterns (`0x...`, `FUN_...`, `sub_...`, `loc_...`, `func_...`, bare 6–8 digit hex strings, or integer addresses) fails validation.
4. **Stock Instruction Digest Verification:** Before any AOT transform is emitted, `verify_and_resolve_sites()` checks `executable_sha256` against the guest ELF and verifies that the stock instruction word(s) at `function + insn_offset` match `stock_digest`. If any site's stock word differs, or if a symbolic function cannot be resolved, the entire package is refused and the build stays authentic. Codegen also refuses the package, before it writes any output, when a verified site is not emitted with its rewrite (for example, its function became an untranslatable stub).
5. **Undeclared Properties:** Extra, unknown, or malformed JSON properties cause validation failure (`additionalProperties: false`).
6. **Fallback to Authentic Baseline:** When an enhancement package fails to load or validate, the runtime and package builder do not crash or attempt a "best guess" partial patch. They refuse the package with a clear diagnostic and preserve 100% authentic execution.

---

## 7. Project API vs. Third-Party Script Separation

The enhancement architecture enforces a clean boundary between the compiled host runtime and external mod packages:

- **Project-Authored Enhancement API:** Nakagawa provides a structured, type-safe C interface (`src/rt/`) for presentation hooks, texture substitution callbacks, and frame pacing. This API is versioned, reviewed, and compiled as part of the native player.
- **Declarative Mod Packages:** Third-party enhancements are distributed as declarative JSON packages containing metadata, replacement asset files, and symbolic hook points.
- **No Untrusted Scripting Core:** The runtime does not embed a general-purpose scripting engine (such as Lua, Python, or JavaScript) in the core execution path. Untrusted external scripts cannot run arbitrary host code or compromise memory safety.

---

## 8. Implementation Status and First-Proof Fixture

The enhancement subsystem was built in phases, each keeping fidelity intact:

1. **Phase 1: Contract, Schema, and Package Validator (built; #327):**
   - Architectural contract defined in `docs/ENHANCEMENT_CONTRACT.md`.
   - Machine-readable package schema in `assets/enhancement_package.schema.json`.
   - Standalone package validation tooling in `tools/enhancement_package.py`.
   - Comprehensive unit test suite in `tools/test_enhancement_package.py`.
2. **Phase 2: First-Proof Synthetic Presentation Fixture (retired; #1069):**
   - Host presentation ships as Enhanced and Ace *parts* (section 10): project-authored code in the player and
     runtime, with no package and no loader. No native presentation-package loader is planned, so the
     `presentation` payload stays forbidden in the schema.
   - Authentic mode, the default, keeps the guarantee this phase was meant to prove: with every part off, the
     player runs the authentic path (section 10).
3. **Phase 3: Asset Replacement Engine (not built; section 10.12 is a development-only texture replacement):**
   - Texture dumping and replacement interface at the host GPU backend.
   - Streaming replacement audio handler.
4. **Phase 4: High-Refresh Frame Pacing and Declarative 60 Hz Logic Rescaling (built; #328):**
   - Declarative frame-rate rescale package format (`game_version`, `executable_sha256`, `rng_policy`, symbolic `sites` with `stock_digest` and the section 9.2 transforms).
   - Fail-closed loader and stock-word digest verifier in `tools/enhancement_package.py` and `tools/codegen.py`.
   - Separate AOT instrumentation epoch (`aot-enhancement-package-v1`) in `tools/nk_core/package_cache.py`, `tools/title_codegen_plan.py`, and `tools/nk_cli.py`, plus the run-time opt-in (`SR_EXPERIMENTAL_60HZ`, which the launcher sets only for a session with an enhancement package).
   - VBLANK multiplier and display list presentation decoupling.
5. **Phase 5: Symbolic Guest Hook Dispatch and Cache Epochs (#316, closed):**
   - Symbolic hook resolution via private title profiles and instrumented AOT cache keys.

---

## 9. Declarative Frame-Rate Rescale Packages (60 Hz Logic)

### 9.1. Purpose and Invariants

Many PSP titles run a 30 Hz game-logic loop by waiting two VBLANK intervals per update and tune frame-counted durations, steps, decays and per-update probabilities for $\Delta t = 1/30\text{ s}$. A frame-rate rescale package lists the instruction sites that make the loop wait two VBLANKs and the sites whose constants assume $1/30\text{ s}$, so the same logic can run once per VBLANK.

1. **Authentic mode is untouched.** Without `--enhancement-package` (the default), `tools/codegen.py` output and the codegen options in the AOT cache key are byte-identical to a build without this feature. A package build records `enhancement_package_epoch` (`aot-enhancement-package-v1`), the package id and digest in its codegen options, and `nk_cli.py build-package` writes it to its own directory (`packages/<disc_id>-enhanced-<pkg_id>`), never over the authentic package. `build-package` applies a package only when its command names one; it refuses to build while the environment names one on its own (`ENHANCEMENT_PACKAGE`, also through `MAKEFLAGS`, or `--enhancement-package` in `CODEGEN_USER_ARGS`), because the Makefile would apply it.
2. **Off at run time unless opted in.** Every rewritten site is emitted twice: the stock statement and its rewrite. The rewrite runs only when the process starts with `SR_EXPERIMENTAL_60HZ=1`; any other value runs the stock statements, so a package build with the toggle off executes the authentic instruction stream. The player sets it per title: "60 FPS game logic" on the title page, shown only for a title with a usable enhancement package, is off by default and saved as the list `enhancement_60hz_discs` in the player's settings file (no other key turns it on). A title with it on launches its enhanced package with `SR_EXPERIMENTAL_60HZ=1`; with it off, the player launches the authentic `packages/<disc_id>` with `SR_EXPERIMENTAL_60HZ=0`, even when an enhanced package exists. Started with `--experimental-60hz`, the player turns it on for every title that has an enhancement package and does not resolve to Authentic (section 10.5), for that session only; the flag is never written to the settings file.
3. **No faster guest clock.** System time, RTC and audio sample clocks are untouched. Only the sites the package lists change.

### 9.2. Site Transforms

Each `sites` entry names a symbolic `function`, a 4-byte-aligned `insn_offset`, the SHA-256 `stock_digest` of the stock little-endian instruction word, and a `transform`. `plan_site_rewrite()` in `tools/enhancement_package.py` gives each (transform, instruction) pair exactly one rewrite, chosen from the transform and the instruction encoding alone; a pair without one refuses the whole package. A site is one word, or two words (`word_count: 2`) that build a float constant: `lui rt, hi` followed by `ori rt, rt, lo` of the same register. A pair takes one of the scaling transforms below, applied to the whole 32-bit value and rounded to nearest, and both words are rewritten (`plan_float_pair_rewrite()`).

| Transform | Instructions with a defined rewrite | Rewrite |
| --- | --- | --- |
| `vblank_wait_2_to_1` | `addi`/`addiu`/`slti`/`sltiu`/`ori` with immediate 2 | immediate 2 becomes 1 |
| `gate_30hz` | `jal`; accumulating `addiu rt, rt, imm`, `addu`/`subu`/`add`/`sub` with the destination as a source, `add.s`/`sub.s` likewise | the statement runs only on even logic updates (§9.5), so every instance runs on the same alternate updates |
| `gate_30hz_hold` | `jal` | the call runs only on even logic updates, as `gate_30hz`; on a skipped update `f0` takes the value of `f12`. It is for a helper that returns its first float argument advanced by one update (an ease toward a target): a skipped update holds the value, so each 30 Hz value shows for two updates. The resolver refuses the site unless the callee, read from its entry to its `jr $ra` and that delay slot, makes no call, jump or trap, writes no memory, `$ra`, `$sp` or FPU control register, branches only to words up to that `jr $ra`, reads `f12` before it writes it, and writes `f0`: skipping it then drops nothing but its result. The check reads the body in address order, not along each path, and does not look at VFPU registers. The site's evidence shows the rest: that the result is stored back as the value the call advances, and that nothing reads the helper's other outputs |
| `gate_30hz_alt` | `jal`, with `alternates` naming 1 to 4 functions | the gated call keeps its stock instants, and on the updates it skips the named functions run in its place, so work the gated function does every update (a model's pose tick) runs at 60 Hz while the rest of it keeps its 30 Hz cadence. The site keeps its own phase: it makes its alternates only in an update right after one in which it made the stock call, and otherwise the stock call, so the gated function always runs first when it is reached again. On an alternate update each alternate is called in list order with `$a0` as it was at the site after its delay slot and `$ra` set to the site + 8; nothing else is set up. The resolver refuses the site unless: the gated function calls each alternate directly, exactly once on every path that returns, in list order, and passes it its own entry `$a0` (read along every path of its graph; a table switch, `sltiu`, `beq`/`beql` to the out-of-range case, then a `lui`/`addiu` table address loaded with `lw` and `jr`, is followed, and any other jump through a register refuses); after the site, no path of the caller reads `$v0`, `$v1`, `f0`-`f3`, HI or LO before writing it, up to its next call, and a return counts as reading `$v0`, `$v1` and `f0`; no alternate reads `$a1`-`$a3`, `$t0`-`$t3`, `f12`-`f15` or the FPU condition flag as its caller left them, read along every path of the alternate and of each function it calls while one of them may still hold that value, three calls deep, and an alternate that calls through a register while one may refuses; and no function an alternate reaches through direct calls is the gated function, the caller, or a function that holds another gated site. Limits of the check: below the alternate's own body, a call through a register, and a call more than three deep, are taken to read only the arguments their caller set (the PSP ABI); the re-entry check does not follow calls through a register; VFPU registers are not tracked. The site's evidence shows the rest: that the alternates touch no state the skipped work reads or writes in a way that depends on the order (no random rolls, no shared counters), and that the state they read is what the gated function left for them |
| `gate_30hz_store` | `sb`, `sh`, `sw`, `swc1` (word_count 1) whose base is not `$sp` | the store runs only on even logic updates (§9.5); on an odd update the store does not happen, so a field the code advances by one update from what it read (an inline ease, a step toward a target) takes the 30 Hz values, each for two updates, as long as no other writer stores to it in that update. It is for a step with no exact per-update form at 60 Hz, where gating the arithmetic would store a wrong value (a gated `addu` whose destination is the step, not the field). A store through `$sp` writes a stack slot, which does not keep its value from one update to the next, so holding it would expose what another call left there; the resolver refuses it. Limits of the check: it reads the base register only, so a frame store through `$fp` or through a register that holds an `$sp` address is not seen, and it cannot see where the store writes. The site's evidence shows the rest: that the store writes a logic field in main RAM or the scratchpad, not VRAM, a hardware register or a display list; that the stored value is the next value of the same field, computed from what the code read from it this update; that every later reader in the update reads the field back from memory (or is itself gated); and that nothing needs the unheld value. Fields written together (a position and the copy derived from it) are gated together |
| `count_x2` | positive `addi`/`addiu`/`slti`/`sltiu` immediate; `andi` mask $2^n-1$; `lui` of a normal float; `sll`; integer loads; `lwc1`/`swc1`; `sw` (only with `stock_values`, §9.6); `lv.s` (the loaded VFPU register is scaled); single-precision arithmetic and `cvt.s.w`; a unit countdown `addiu rt, rt, -1` | the produced value is doubled (a countdown is gated) |
| `count_x2_even` | `slti`/`sltiu rt, rs, n` with $0 < n \le$ `0x3FFF` and `rt` neither `$zero` nor `rs` | a bound on a counter of logic updates since an event: true while `rs` $< 2n$ and `rs` is even, so the $n$ events of the stock window still happen, one every second update, at the stock instants (`count_x2` would make $2n$ of them, twice as dense). Under `slti` a negative `rs` stays true, as in stock, so an inactive value such as $-1$ keeps its meaning; under `sltiu` it is a large unsigned value, false in both forms. The resolver refuses the site unless `rs` is loaded from memory (`lb`, `lbu`, `lh`, `lhu` or `lw`) within the 16 instructions before the compare, with no branch, jump or call in between, and not in the delay slot of a jump, a call or a branch-likely. The check reads backward in address order and does not see a branch from elsewhere into those instructions. The site's evidence shows the rest: that the stored count advances by one every update |
| `step_x0.5`, `probability_per_update_x0.5` | as `count_x2` except integer loads, plus accumulating `+1`/`-1`/`addu`/`subu` updates | the produced value is halved (exactly, or by gating a unit or register step); `add.s fd, fd, ft` halves the addend `ft` |
| `decay_k_to_1-sqrt(1-k)` | `lui` of a float $k \in (0, 1)$; `lwc1` or `lv.s` of the factor; `mov.s` that copies the factor (section 11.3.2.7) | $k \to 1 - \sqrt{1-k}$, evaluated as $k / (1 + \sqrt{1-k})$ in double so that a small $k$ does not cancel to 0, and rounded to the upper half; after an `lwc1` or `lv.s` the loaded register is mapped at run time, after a `mov.s` only the copy, and a value outside $(0, 1)$ keeps its stock value and is reported once per site (`ENHANCEMENT_FACTOR_KEPT`) |
| `retention_m_to_sqrt(m)` | `lui` of a float $m \in (0, 1)$; `lwc1` or `lv.s` of the factor; `add.s`, `sub.s`, `mul.s` or `div.s` whose result is the factor (such as $1 - cL$); `mov.s` that copies the factor (section 11.3.2.7) | $m \to \sqrt{m}$, rounded to the upper half; after an `lwc1` or `lv.s` as for `decay_k_to_1-sqrt(1-k)`, and likewise after the arithmetic instruction, whose result register is mapped at run time; after a `mov.s` only the copy |
| `accel_x0.25` | `lui` of a float (or a `lui`/`ori` pair) | the per-update acceleration is divided by 4 (two exponent steps on a lone `lui`) |
| `launch_velocity_x0.5` | a `lui`/`ori` float pair with `acceleration_site` | the launch velocity $v_0 \to v_0/2 + g/8$, where $g$ is the stock value of the named `accel_x0.25` site |
| `offset_x2` | `addi`/`addiu` with a nonzero immediate, also when the destination is a source | an additive offset counted in updates (a lookahead, a delay added to a base) doubles: $n \to 2n$ |
| `window_start_x2` | `addi`/`addiu` with an immediate of $-2$ or less | the start of a window that ends one before a per-update counter: $k \to 2k+1$, so the window spans twice as many updates |
| `argument_x2` | the instruction at a function's entry (`insn_offset` 0), with `arguments` naming the registers (`a0`-`a3`, `t0`-`t3`) | a helper that takes durations in updates from every caller doubles them once per call, after its stock entry instruction, which may not write them; a function that branches back to its entry is refused |
| `value_reference` | `lwc1` or `lv.s` of read-only data; a `lui`/`ori` float pair (`word_count: 2`, or a lone `lui` with `low_insn_offset`) | none: the instruction keeps its one stock form. The site pins one stock float, digest-checked like every site, for the site references below. Every such site is read when the package is resolved, referenced or not, and one that pins nothing refuses the package. It takes no `requires` and no `stock_values` |
| `gate_30hz_phase` | `jal`; a single-precision compare (`c.cond.s`) with `odd_phase_condition`; `lwc1` or `lv.s` of a value; each with `phase_time` (a `register`, `s0`-`s7`, and a 4-byte-aligned `offset`) and `step_site` | the site keeps the stock instants of one state, counted on that state's own time: the float $t$ at `offset` from the register, read through the guest accessors when the site runs (for a call, after its delay slot). The state's time is the guest's own float32 sum: zero when the state is entered, then $t \leftarrow \mathrm{fl}(t + h/2)$ once per 60 Hz update, $h$ being the stock step. The resolver builds the values of that sum exactly, a few rows for each binade, up to one update before the sum stops advancing (or $2^{31}-1$ updates), and the generated code finds $n$, the number of updates since zero, by matching $t$ against them bit for bit. The rounding error of the sum is therefore part of the model: it cannot move a phase, however long the state lasts. After an even $n$ the stock instruction runs. After an odd $n$ a call is skipped (its delay slot still runs); a compare runs and then the FPU condition flag takes `odd_phase_condition`; a load runs and then its register reads $+0.0$. A $t$ that is not a value of the sum (the time was set or advanced some other way) runs the stock instruction, and the site reports once (`ENHANCEMENT_PHASE_KEPT`); while the time stays off the sum, a gated call or load acts on every update. The value at which the sum stops advancing (after 6 days of state time for a step of 1/60 s) is treated the same way: it repeats on every update from then on, so no count can be read off it. A report says that state's time left its sum, so the phases the site computes for that state afterwards are not to be trusted either until the state is entered again; a tool that accepts a 60 Hz run checks for the line, as for `ENHANCEMENT_DATA_SITE_NOT_REACHED` (§9.6). One case cannot be reported: a time that starts at a non-zero value which happens to lie on the sum is, in every later bit, the time of a state that has run that many updates, so its parity can be the wrong one with no sign of it. Only the evidence that the time is zero when the state is entered excludes it. The resolver refuses the site unless: of the instructions that can run before it, along every path of its function, exactly one writes the register, and no path from the entry reaches the site without running that write (the graph is read as for `gate_30hz_alt`, and calls are taken to preserve `s0`-`s7`); a gated call's delay slot does not write the register; for a call, no path of the caller reads `$v0`, `$v1`, `f0`-`f3`, HI or LO before writing it, up to its next call, a return counting as a read of `$v0`, `$v1` and `f0`, as for `gate_30hz_alt`; for a compare, it is not in a delay slot, and a `bc1f` or `bc1t` (or their likely forms) follows within 4 instructions, with no call, jump, other branch, other compare or `ctc1` in between. A gated load needs neither of those, because the call it feeds always runs; it is refused when it is the load of the state's own time (the gate would zero what it counts; the time load is matched by its base register, so a load of the time through a copy of that register is the site's evidence), and when the title stores the gated register before overwriting it: read forward from the load to the first branch, jump or call and its delay slot, 16 instructions at most, a `swc1` of a gated FPU register, an `sv.s` of a gated VFPU register, or a store of a general register that an `mfc1` or `mfv` copied it to refuses, since that store would write a zero the stock code never stores. Copies through other FPU or VFPU registers and an `sv.q` are not followed, and VFPU registers are not otherwise tracked. The site's evidence shows the rest: that the field is the state's time, zero when the state is entered and advanced by the step once per update, after the site has run in that update (a state whose time advances before the site reads the other parity, and this transform does not fit it); that the step is fixed; for a call, that it is a per-update effect with no step in it; for a load, that the value only scales such an effect, so that zero makes it nothing ($v + 0 \cdot k$ is $v$ in every bit, except that a $-0.0$ becomes $+0.0$); for a compare, that `odd_phase_condition` is its no-event direction, and that its threshold is not within float32 rounding of a whole number of stock steps (the 60 Hz sum and the stock sum differ in their last bits, so such a threshold could pass one stock step apart) |
| `velocity_source_x_kappa` | `lwc1`; `lv.s`; `lv.q`; `swc1` (which may declare `stock_values`, §9.6); each with `damping_site` and `damping_step_site` | a source of velocity change (an acceleration, the size of an impulse) is multiplied by $\kappa$ (below), in double, and rounded to single precision once: after an `lwc1` or `lv.s` the loaded register, after an `lv.q` each of the four loaded lanes; a `swc1` stores the scaled copy. An `lv.q` site in a delay slot is refused. The site's evidence shows that every lane an `lv.q` loads is such a source or zero |
| `launch_from_rest_60hz` | `jal`, with `velocity_offset` (4-byte-aligned), `gravity_axis` (0 or 1), `gravity_site`, `damping_site`, `damping_step_site` and `step_site`, in a package that names the capability `launch_velocity_write` | the stock call runs unchanged. Before it, `$a0` as the site passes it (after the delay slot) and the float of the other axis are taken. After it, of the two floats at `velocity_offset` and `velocity_offset + 4` from that `$a0`: the one `gravity_axis` names becomes $\kappa v + q g$ (below), the image of a velocity set from rest; the other becomes $b + \kappa (v - b)$, $b$ being its value before the call, so only what the call changed is scaled, a velocity the body already carried is never scaled, and a float the call left unchanged is not written. Each is computed in double as one fused multiply-add and rounded to single precision once. They are read and written through the guest accessors, so a bad pointer faults as the guest's own access would. The resolver refuses the site unless the function it calls stores both floats itself, at those offsets from its entry `$a0`, on every path that returns (copies of `$a0` are followed as for `gate_30hz_alt`; a store made by a function the callee calls does not count). The runtime counts the calls made and the maps applied; when a call is not followed by its map (the callee did not return in the same host frame, or a guest access of the map faulted) the velocity keeps what the call left, and the end of the run names the site (`ENHANCEMENT_LAUNCH_NOT_MAPPED`). The site's evidence shows the rest: that `$a0` is the body and the offset its velocity; that gravity acts on that axis only and the body takes the damping the sites name; that the body is at rest along gravity when the site runs, so that its velocity there is its support's and not an image (a launch in flight that is already mapped would add the offset twice); that on the other axis the callee adds to the velocity and what the body carried there is zero or already an image (a carried velocity the title sets directly each update, or a callee that overwrites that float, leaves it off by $(1 - \kappa)$ of the carried value until something sets it again); and that the callee cannot block. A tool that accepts a 60 Hz run checks for `ENHANCEMENT_LAUNCH_NOT_MAPPED` |

**Damped motion: the two-update image.** A stock update that runs $v \leftarrow (v + g h + I)\,m$, then $x \leftarrow x + v h$, with retention $m = 1 - cL$, has an exact image in two updates of half a step, $w \leftarrow (w + a + I')\,\mu$, then $x \leftarrow x + w h/2$. Writing $v^* = g h m / (1 - m)$ for stock's terminal velocity, the two reach the same $x$ at the end of every pair exactly when $\mu = \sqrt{m}$, $a = \kappa\, g\, h/2$ and $w - v^* = \kappa\,(v - v^*)$, with $\kappa = 2\mu / (1 + \mu)$. Each rule follows. `retention_m_to_sqrt(m)` gives $\mu$. `velocity_source_x_kappa` on the gravity the integrator loads gives $a$, with the step site halved as usual. An impulse in flight scales by $\kappa$ and goes wholly into the first update of its pair, which is what `velocity_source_x_kappa` on its size and `gate_30hz_phase` on its call, or on the load of its size, declare. A velocity that is set from rest, where the map does not hold, becomes $\kappa V + q g$ with $q = h \mu^2 / (1 + \mu)^2$, which is `launch_from_rest_60hz`. The parts go together: without any one of them the pair ends differ by far more than rounding. At the end of a pair the velocity is the image $w$, not stock's $v$, so code that reads the body's velocity in flight sees that.

**When the image is exact, and what "exact" means.** The image holds, for the position and for the velocity as its image, at the end of every pair, when all of the following are true of the title's own integrator, and each site's evidence shows them:

- $m$, $g$ and $h$ are constant through the flight: one damping for the body (not one per material or medium), one gravity, a fixed step;
- each update applies the force, then the damping, then moves by the new velocity;
- a launch, and every impulse, is applied before the integrator, in the first update of a pair;
- nothing clamps the velocity and nothing collides between two pair ends.

"Exact" is what float32 code gives, not what the algebra gives. Every guest operation and every emitted statement rounds to single precision, so at a pair end the 60 Hz position agrees with stock's to rounding, in most bits and not in all. `tools/test_enhancement_package.py` runs the emitted statements through a 90-update flight with a launch and timed impulses, in float32 and bit for bit against a float32 model of these rules. On that model the worst pair-end difference is $3.2 \times 10^{-7}$ of the arc's height along gravity (5 of the 90 pair ends bit-identical) and $4.6 \times 10^{-7}$ of the distance covered on the other axis (6 of 90), and a test that needs no compiler bounds both at $10^{-6}$. Those are that flight's figures. The difference is rounding accumulated over the flight, at most 6 float32 steps of the largest position it reaches, so it is a larger share of a small arc than of a large one. A double-precision model of the same flight agrees to $10^{-12}$; that figure is the algebra's, not the code's.

Where a condition does not hold, the same constants are a first-order approximation, and what a player sees is an arc that is slightly off, not a failure. In a model with a damping of 1.6 % per update: damping applied before the force, 1.0 % of the arc's height; moving by the old velocity (explicit Euler), 1.3 %; an impulse in the second update of its pair, 0.09 %; a clamp on the fall speed, the image is lost at the clamp, 4.9 % over 90 updates. The errors scale with the damping per update. A title whose integrator has another form needs other constants, which these transforms do not declare.

**Landing is not moved to the stock update.** A decision, not an oversight. The flight is stock's at every pair end, but the title tests contact on every 60 Hz update, so a body can touch down on the odd update of a pair, half a stock step (1/60 s) before the update on which stock would have seen it: in the model, for 4 of 8 launch speeds. The landing update, the position along the ground at landing (by up to the horizontal velocity times $h/2$) and the velocity the collision response reads (the image, at a mid-pair instant) then differ from stock's. The samples between pair ends are also points stock never computes: the highest of them can lie up to 0.24 % above stock's highest. No transform here gates contact to pair ends; a title for which the landing update matters needs its own site. For a player: the character can be grounded one 60 Hz update (1/60 s) before stock would have grounded it, so the next jump input can be accepted one update sooner, the landing point shifts by up to the horizontal speed times 1/60 s, and a chain of jumps on a held button runs with a period up to 1/60 s shorter per landing.

**Site references.** $g$, $c$, $L$ and $h$ are stock values, named by `gravity_site`, `damping_site`, `damping_step_site` and `step_site`: each holds the id of another site of the package whose instruction pins one float, as `acceleration_site` does for `launch_velocity_x0.5`.

- **What may be named.** $g$, $c$ and $L$ are named through `value_reference` sites only: a value another transform rescales is not the value the title then uses (with the halved step as $L$, the title would compute $1 - cL/2$ and the retention site would take the root of that). `step_site` may also name the `step_x0.5` site that halves the step, since that site is what makes the 60 Hz step $h/2$. One site pins one quantity: a site named for two of them, in one site or across the package, refuses it.
- **How the value is read.** The site is a `lui`/`ori` pair (or a lone `lui` with `low_insn_offset`), or an `lwc1` or `lv.s` whose base register is a constant: within the 16 instructions before the load, a `lui` of that register and at most one `addiu` or `ori` of it. Delay slots are read as the code they are. A load that is itself a delay slot runs after everything before its branch, jump or call. A conditional branch that is not a likely form is passed, because on the path that falls through to the load its delay slot and the code before it both ran, so a `lui` in that slot counts. Any other transfer ends the scan: after a jump, a call, a branch-likely or a branch that is always taken, the code that follows is not reached with that slot run. The taken path of a branch is checked where the scan can see it: a branch or jump among those 16 instructions whose target lies at or before the load, and whose taken path skips the `lui` or the `addiu`/`ori` (they lie after its delay slot and before its target), would reach the load around the code that builds its address, and refuses. A branch with the `lui` in its own delay slot, or one taken past code that does not build the address, builds it on both paths and is accepted. The check reads backward in address order and does not see a branch from farther away into those instructions, nor a backward branch from after the load into them.
- **Only data the title cannot change.** The word is read from an allocated `PROGBITS` section that is neither writable nor executable. A value in writable data is refused: the title could change it while it runs, and $\kappa$ and $q$ are fixed when the package is built.
- **Nor the package.** A data site (§9.6) whose span covers a word pinned for $g$, $c$ or $L$, or by a `value_reference` no site names, is refused, for the same reason a rewritten site may not be named. The word of the step may be rescaled by a data site, as it may be by its `step_x0.5` site.
- **What is refused.** Any referenced value that is not finite, $cL$ outside $(0, 1)$, and a step whose half is not a positive float32. $\kappa$ and $q$ are computed in double from the stock single-precision values. A package holds none of these numbers, published or not: it holds the rule and the sites.

Nothing ties $\kappa$ and $q$ to the retention the title computes at run time beyond these checks: the retention site maps whatever value in $(0, 1)$ it finds. That the body's retention is always $1 - cL$ with the pinned $c$ and $L$ is the first condition above, and part of the evidence.

**Guest memory written by a timing transform.** Decision of 2026-11-10 (#2366): a transform of this section writes guest memory only where it replaces, at the same place, a value the stock logic itself stores there, with that value's 60 Hz image. Two forms do. A `swc1` or `sw` store site makes the title's own store instruction store the scaled value. `launch_from_rest_60hz` writes after a call, so the resolver proves that the called function stores both floats itself, the package must name the capability `launch_velocity_write` (the validator refuses the site without it), and the transform's section 11.3.2 entry lists every field written. Any other write to guest memory needs a `guest_memory_code_hook` package (section 2.3), as `clip_x_widen` does, and the validator refuses such a transform in a timing package. A transform that only reads guest memory to decide what a site does (`gate_30hz_phase`) is not a write; its entry says what it reads. Its gated load changes a register, not memory, but the title's own instructions could carry that zero into memory: the resolver refuses the store it can see (the gated register, or a general-register copy of it, stored in the straight-line code after the load), and every other use of the gated value is the site's evidence.

**Host arithmetic.** The scale by $\kappa$, the launch map and `sqrtf` run in the host's round-to-nearest, whatever rounding mode the guest has selected, and set no cause or flag bit in FCR31. The launch map's fused multiply-add is one correctly rounded operation where the C library's `fma` is; the rounding to single precision hides nearly every difference.

A semi-implicit Euler motion (`v += g`, then `x += v`, once per update) reaches $x_n = n v_0 + g\,n(n+1)/2$. With $g/4$ per 60 Hz update and $v_0/2 + g/8$ at launch it reaches the same $x$ at every second update, which is what `accel_x0.25` and `launch_velocity_x0.5` declare together. The acceleration site must pin its whole float: a two-word site, or `low_insn_offset` and `low_stock_digest` naming the `ori` that completes a lone `lui` (read, never rewritten). A site with `operand: f64_high` is the `lui` of a double's upper word: `count_x2`, `step_x0.5`, `probability_per_update_x0.5` and `accel_x0.25` step its 11-bit exponent exactly, and other transforms refuse it.

The transforms scale the operand the site produces; they do not guess its role. A constant that multiplies or bounds a random roll (`rand() % 100`, `rand * 100.0f < p`) halves the per-update probability when it is doubled, so its site declares `count_x2`, not `probability_per_update_x0.5`. A `swc1` or `sw` site stores a scaled copy of its register and never writes the register, so a store that faults under `--lle-cpu` leaves it unchanged. A `sw` site is `count_x2` only, must declare `stock_values`, and may not store `$zero` or its own base register.

### 9.3. Random-Number Generator (RNG) Policy

Every frame-rate rescale package declares `rng_policy`. Under `rescale_probability`, single-roll per-update checks keep running every update with their probability halved; stateful multi-roll consumers are gated with `gate_30hz` so they keep their 30 Hz roll order. Under `subtick_30hz`, every roll site is gated.

### 9.4. Building and Running

1. Draft a package for a new title: `python tools/enhancement_package.py --draft <selected.elf> --title-id <library title_id> --out <disc_id>-60hz.json` reads the decrypted executable from the title's package cache and recognises its frame-pacing wait: a VBLANK counter that one routine increments by 1 (it reports whether that routine is registered as a VBLANK sub-interrupt handler, also through one wrapper), that the wait compares with `slti`/`sltiu rX, 2` and clears with `sw zero`; or `sceDisplayWaitVblankStartMulti(2)` and its CB form. Each becomes a `vblank_wait_2_to_1` site, with a `<package>.symbols.json` sidecar that names the routines `draft_fn_<address>`. The routines that call a pacing wait are printed as update-boundary candidates. The draft sets `update_boundary` only when there is exactly one, and that routine must still be confirmed to run once per logic update. Other VBLANK waits, for example two plain `sceDisplayWaitVblankStart` calls per frame, are printed for review, because no transform rewrites them. The draft validates but contains none of the title's frame-counted constants: those still come from the title's own code (sections 9.2 and 9.6).
2. Validate a package: `python tools/enhancement_package.py path/to/pkg-60hz.json`.
3. Build a package build: `python tools/nk_cli.py build-package <DISC_ID> --enhancement-package path/to/pkg-60hz.json` (or `--experimental-60hz` with the package at `<user data>/enhancements/<disc_id>-60hz.json`, the disc ID in lowercase, for example `ulus12345-60hz.json`, else the published package of that name in `assets/enhancements/`, section 5.1), or pass `ENHANCEMENT_PACKAGE=path/to/pkg-60hz.json` to `make`. Codegen resolves function names from the ELF symbol table or a `<package>.symbols.json` sidecar.
4. Run the built executable with `SR_EXPERIMENTAL_60HZ=1` for 60 Hz logic, or without it for the authentic 30 Hz game. The player selects the package from the title's setting (section 9.1, item 2). A title has an enhancement package when `<user data>/enhancements/<disc_id>-60hz.json` (lowercase disc ID), or when that file does not exist the shipped `assets/enhancements/<disc_id>-60hz.json` of the source tree beside the player (the tree it finds `tools/nk_cli.py` in), parses, its `id` matches `^[a-z0-9][a-z0-9._-]{0,63}$` and its `target_title_id` is the title's own; the player reads only those fields and `experimental` (section 9.10), and never forms a path from an id that fails the pattern. With the setting on, the player validates and launches `packages/<disc_id>-enhanced-<pkg_id>` with the same manifest, ABI and identity checks as the authentic package, and the package must record that enhancement package id in its codegen options; the authentic folder must record none. Each package has its own status: when the enhanced package is missing or stale, the title's primary action builds it (`build-package --experimental-60hz`) and nothing launches the authentic package in its place. When the setting is on but the enhancement package is gone, the title neither builds nor plays until it is turned off or the package is restored.

### 9.5. Update Boundary: Gating Counts Logic Updates

A gated statement (a `gate_30hz`, `gate_30hz_hold` or `gate_30hz_store` site, and the unit steps and countdowns whose rewrite is a gate) runs when the number of logic updates begun so far is even. A `gate_30hz_alt` site keeps its own phase on the same count instead (section 9.2): it makes its alternates only in an update right after one in which it made the stock call, and otherwise makes the stock call (a second run in the same update repeats that update's choice), so a function reached again after a scene change runs before its alternates. The package says where an update begins with `update_boundary`: a symbolic `function`, a 4-byte-aligned `insn_offset` and the `stock_digest` of one instruction that runs exactly once per logic update, usually the first instruction of the title's per-frame body. While `SR_EXPERIMENTAL_60HZ=1`, that instruction first advances the counter `nk_enhancement_updates` and then runs its stock statement unchanged; with the toggle off nothing is counted. A `gate_30hz_phase` site does not use this count and needs no `update_boundary`: its phase is the number of updates its own state has run, read off that state's time (section 9.2), which starts with the state and stops while the state does, whatever the parity of the update the state was entered on.

1. **Required when anything gates.** The validator refuses a package with a `gate_30hz`, `gate_30hz_hold`, `gate_30hz_alt` or `gate_30hz_store` site and no `update_boundary`, and codegen refuses one whose planned rewrites include any gate without it.
2. **Verified like a site.** The boundary's function resolves and its stock word is checked against `stock_digest` the same way, fail-closed. It may not be a control transfer or a rewritten site, and a package whose boundary is never emitted is refused like an unemitted site.
3. **Correct under frame drops.** The count advances once per update however many display VBLANKs that update takes, so gated statements keep alternating when an update overruns a VBLANK or a stock two-VBLANK wait still runs. `tools/test_enhancement_package.py` compiles the generated gate and runs updates of one to three VBLANKs to check it.
4. **Slowdown is reported.** An update that takes longer than one VBLANK of host time is followed by the title's own wait for the next VBLANK, so the logic runs below 60 updates per second while every per-update step stays halved: the game plays slower than the original. The generated start-up hands `nk_enhancement_updates` to the runtime (`sr_enhancement_register_updates()`), which compares it once a second. Logic that runs but stays under 57 updates per second for three seconds in a row is logged as `ENHANCEMENT_LOGIC_SLOWED` and shown in the game window as a red `60 FPS logic: <n>/s - SLOWED` line, with or without the F3 HUD. A second with no update at all (a load or a pause) clears the count.
5. **Why it is declared, not inferred.** A `vblank_wait_2_to_1` site is the compare inside a title's wait loop, which can run several times in one update, so it does not mark where an update begins. The package names the boundary explicitly.

This package format is opt-in; whether a package is experimental is section 9.10.

### 9.6. Data Sites and Data Files

Some frame-counted values are data, not instructions: tables in the executable's initialised data, and parameter files the title reads from its disc. A package rescales them once, when they are loaded, and only while `SR_EXPERIMENTAL_60HZ=1`. The scaling transforms of §9.2 apply (`count_x2`, `step_x0.5`, `probability_per_update_x0.5`, `decay_k_to_1-sqrt(1-k)`, `retention_m_to_sqrt(m)`); an integer value with no exact result, a float outside the range its transform needs, or an overflow refuses the package.

- **`data_sites`** name a symbolic data label (`symbol`, resolved like a site's function), a byte `offset` aligned to the element `type` (`u8`, `s8`, `u16`, `s16`, `u32`, `s32` or `f32`), an element `count`, the SHA-256 `stock_digest` of those bytes and a `transform`. Codegen reads the bytes from an allocated, non-executable `PROGBITS` section (never code or zero-fill), checks the digest and computes the rescaled bytes. The generated start-up (`nk_enhancement_apply_data()`, run from `sr_register_all()` after the executable is loaded and before it runs) compares the loaded bytes with the stock bytes and writes the rescaled ones; any difference stops the run with `ENHANCEMENT_DATA_MISMATCH`.
- **`data_files`** name a disc `path` (plain names separated by `/`, compared without case), the file's `sha256`, a `format` (`csv`), the zero-based `header_row` that names the columns, the `first_data_row`, and `cells`: a `column` by its header name, or a zero-based `column_index` where the header line has no ASCII name for it, optional zero-based data `rows` (all data rows when left out) and a `transform`. Several `cells` entries may name one column when their `rows` do not overlap, so no value is rescaled twice. A cell is a plain decimal number. Its optional `type` (a `data_sites` type) says how the title parses it; without one, a cell with a decimal point is a float and any other an integer. An integer cell stays an integer and must rescale exactly; when the cell gives a `type`, both the stock value and the rescaled value must fit that type, and a cell without a `type` has no range check; a float cell must stay within the float32 range, before and after the rescale; a float cell is read as a float, rescaled, and written as the shortest decimal that reads back to the same float. Build-package passes the title's extracted files (`--enhancement-data-root`); codegen finds the file by path and digest, rewrites it, and embeds the result. At run time `sr_enhancement_file_register()` holds it, and opening that path for reading reads the stock bytes first: matching bytes are served rewritten, with the rewritten size; any other bytes at a registered path stop the run with `ENHANCEMENT_DATA_MISMATCH`. Files on disc are never modified. Only an open by path is served: a title that reads its data through raw UMD sectors or its own archive reader never opens these paths, and their rewrites do not apply. With the other end-of-process reports, however the run ends (their order relative to `COMPAT_PATCH_REPORT` is not fixed), `ENHANCEMENT_DATA_REPORT: served=K/N` counts the registered files that were opened and served, and `ENHANCEMENT_DATA_NOT_SERVED` names each one that never was.

Both are covered by the package digest, so a change to either is a different enhanced build.

#### Load-time store sites

Some titles parse a table from disc at load time and store each field into RAM with its own instruction. Rescaling that store makes every later reader see the rescaled value, once per load, and it stays correct when the title loads again, because each load scales the value being stored. The store is the anchor: declare it as a `swc1` site (`count_x2`, `step_x0.5` or `probability_per_update_x0.5`) or as a `sw` site (`count_x2`), and give it `stock_values`.

- **`stock_values`** is a non-empty list of numbers, the values the loader is expected to store at that instruction. The package refuses a duplicate (after conversion to `f32` for a `swc1` site), a value that is not finite or whose scaled result is not finite, and, for a `sw` site, a value that is not an integer or whose double is outside the `s32` range. A `sw` site without `stock_values` is refused, because exactness and range are proven from the declared values. It is allowed only on `swc1` and `sw` sites.
- **Run time, toggle on.** The generated statement compares the 32-bit pattern of the stored register with the stock values. A value outside the list prints `ENHANCEMENT_DATA_MISMATCH: site <id> at 0x<pc> stores <value> (0x<bits>), not one of the package's stock values; the run stops here` to stderr and exits with status 1. A listed value is stored scaled and counts as one application: the first 16 applications of a site print `ENHANCEMENT_DATA_APPLIED: site <id> at 0x<pc>: <stock> -> <scaled>`, then one line `ENHANCEMENT_DATA_APPLIED: site <id>: later applications are not logged` follows. With the toggle off the statement is the stock one, with no check and no output.
- **A store site that never runs.** With the toggle on, when the process ends, each site with `stock_values` that stored no value prints `ENHANCEMENT_DATA_SITE_NOT_REACHED: site <id> at 0x<pc> stored no value in this run; whatever it should have rescaled stayed stock`: the title filled the field on another path, and its readers saw stock values. A run stopped by `ENHANCEMENT_DATA_MISMATCH` prints only the mismatch. The line is printed at an ordinary exit and on the runtime's `_Exit` paths (the headless end of run, a route failure, a capture exit, an unsupported import, a guest fault), through `sr_at_process_end()` in `src/rt/perf.h`. It is a report and leaves the exit status as it is: those statuses already mean something (a route result, a capture code), and the site may legitimately not have run yet when the run ends early. A tool that accepts a 60 Hz run checks for the line.
- **Choosing the store.** The site must be the last store of the value: after any clamp or range check the title applies to the field, or a scaled value the title clamps afterwards is lost without a line. If a later store cannot be ruled out, keep the scaled values inside the clamp.
- **A scaled value that is also a stock value.** The check compares the stored value with the stock list, so it cannot tell a stock value from an already scaled value that equals another stock value (stock values 30 and 60 with `count_x2`: a re-stored 60 that was a scaled 30 is scaled again to 120). The package is not refused, because one table can hold both v and 2v, but codegen prints `warning: enhancement site <id>: stock value <v> scales to <w>, which is also a stock value; ...` once per such value. The package author checks that no path stores an already scaled value again through that instruction.
- **`requires`** is a non-empty list of site ids, each another site of the package that declares `stock_values`. The site runs its rewrite only after every required site has applied at least once in the run; until then it runs its stock statement, and the first time that happens it prints `ENHANCEMENT_SITE_GATED: site <id> at 0x<pc> runs stock: <required id> has not applied` once. It works for every rewrite kind; on a gated call (`gate_30hz`, `gate_30hz_hold`, `gate_30hz_alt` or `gate_30hz_phase` on a `jal`) the stock statement is the call made on every update, with no alternates, and on a `launch_from_rest_60hz` site it is the call with no map after it. The package refuses an unknown id, the site's own id, a duplicate, an id that names a data site or data file, and a required site that itself declares `requires`.
- **"In the run" means the process.** Application counts and the gate are kept for the life of the process, including after an in-process `LoadExec`: a site that applied before it still counts as applied.

### 9.7. Side-by-Side Timing Check

A package is right when the enhanced game does in each second what the authentic game does in that second. The check compares two runs of the same input route, sampled with `SR_SAMPLE_SPEC` ([sampling guest state per vblank](DEBUGGING.md#sampling-guest-state-per-vblank)): one of the authentic build, one of the enhanced build with `SR_EXPERIMENTAL_60HZ=1`. The spec must sample the title's logic-update counter; it may also sample a word that names the game phase (a mode or screen) and the values whose changes are the events to compare.

```text
python tools/enhancement_package.py --compare-rates stock/samples.csv enhanced/samples.csv \
    --phase mode --event count:points --event change:ai_choice --event lcg:seed:<A>:<C>=2 --json report.json
```

1. **Game time, not host time.** Each run's game time is its logic updates divided by its nominal rate (`--stock-hz 30`, `--enhanced-hz 60`). An enhanced run that falls below 60 updates per second still plays every update in half the game time, so its event rates stay comparable on a loaded host. The wall-clock rate is reported per phase as well, and a phase under 57 updates per second is marked `SLOWED` (the threshold of §9.5, item 4).
2. **Events.** `count:COL` sums the increases of a counter; `change:COL` counts the samples whose value differs from the one before; `lcg:COL:A:C[:BITS]` counts the steps a linear congruential generator $x' = (Ax + C) \bmod 2^{\text{BITS}}$ took between two samples, searching at most `--lcg-limit` steps (4096) and reporting a pair it cannot join as unresolved. `=RATIO` gives the expected enhanced/stock rate per game second, 1 unless given: under `rescale_probability` (§9.3) a generator rolled once per update advances twice as often, so its expected ratio is 2, while the events its rolls decide keep the ratio 1.
3. **A verdict from the counts.** For each event, the enhanced share of the two runs' events is binomial for a given true ratio; the 99 % Wilson interval of that share gives an interval for the ratio. `PASS` means the expected ratio lies inside it, `FAIL` that it does not. With fewer than 20 events in the two runs together the event is `NOT_JUDGED`: a rare event is not a pass. The interval assumes independent events; scripted events that come at fixed times vary less than that, so for them it is wider than it needs to be and can pass a small difference it could have caught.
4. **Per phase.** With `--phase`, each phase is judged as well as the whole run, and a `FAIL` in any phase fails the run: a phase whose rate is wrong must not be hidden by the totals. A phase with too few events to judge adds them to the whole run and is listed as not judged. A phase that runs logic in one run only means the runs did not follow the same route, and makes the result `INCOMPLETE`; a phase that runs no logic (a loading screen) has no game time and may appear in one run only.
5. **The captures.** The sampler can write one vblank twice: a row that repeats the previous row exactly is skipped and counted. Anything else that would lose time refuses the file (exit 1): a vblank that is not an integer, does not increase, or repeats with different values; a row without the updates counter; a cell that is not an integer in a counted column. An empty event cell (a value the spec could not read, such as a pointer chain before its object exists) leaves that step out of the event's count and out of its game time, and the number of such steps is reported. The shorter run must cover at least `--min-coverage` (0.9) of the longer run's game time, or the result is `INCOMPLETE`: a truncated capture is not a pass.
6. **Exit status.** 1 when an event fails in the whole run or in any phase, an LCG step is unresolved, or an input is refused (with a `rates: FAIL:` line naming it); 2 (`INCOMPLETE`) when an event is not judged over the whole run, no event is given, a phase is `SLOWED` or in one run only, or the coverage is too low; 0 otherwise. Each phase's events are also printed for diagnosis, and `--json` writes the measurements and the report.

Sample specs and routes name guest addresses, so they stay private with the package; the check itself is generic.

### 9.8. Ace Parts (`ace_parts`)

A timing package may also carry Ace parts: items for its one title that belong to the Ace tier (section 10) and
that a player turns on or off apart from the 60 FPS game logic. The field is an object of named parts:

```json
"ace_parts": {"load": {"sites": [{"id": "...", "function": "...", "insn_offset": 0, "stock_digest": "...",
                                  "transform": "vblank_wait_skip"}]}}
```

- **Names.** The part names are a closed list: today only `load` (section 11.3.10). Any other name refuses the
  package, as does an empty `ace_parts` or a part with no sites.
- **Sites.** Each site has an `id`, a symbolic `function`, a 4-byte-aligned `insn_offset`, the `stock_digest` of one
  stock instruction word, an optional `word_count` that may only be 1, and a `transform` from the parts' own list
  (below). A site resolves and is verified like a `sites` entry (section 9.2), fail-closed, and in a published
  package (section 5.1) its function is in the site map and its digest is keyed, as for `sites`.
- **One namespace.** Site ids and instruction words are shared by `sites`, every part's sites and the
  `update_boundary`. The validator refuses a duplicate id, an Ace part site on a word a `sites` entry (any of its
  `word_count` words), another part site or the `update_boundary` already covers, an Ace part site that declares
  `requires`, and any `requires` that names an Ace part site. It compares words by `function` and `insn_offset` in a
  private declaration, and again by site-map address in a published package (section 5.1), whose site map may give
  one address two names; the publish gate runs the same check. Codegen compares the resolved addresses, whatever
  the names: it refuses a part site whose word is a 60 Hz site's word or the boundary's.
- **Their own transforms.** The parts' transforms are a set of their own, disjoint from the transforms of section
  9.2: a 60 Hz transform in `ace_parts`, or a part transform in `sites`, is an unknown transform and refuses the
  package.
- **Their own switch.** A part site is gated at run time only by its part's `SR_ACE` key (section 10.1), never by
  `SR_EXPERIMENTAL_60HZ` or the update count. With the key off the stock statement runs. A package without
  `ace_parts` generates exactly the code it generated before; with them, the generated C differs only at each part
  site's statement, at the `update_boundary`'s statement, which also calls `nk_ace_load_update()` for the `load`
  part's pacing guard (section 11.3.10), and in the declaration digest, which covers `ace_parts` like every other
  field, so the AOT cache key changes with it.

| Transform | Instructions with a defined rewrite | Rewrite |
| --- | --- | --- |
| `vblank_wait_skip` | `jal` whose target is the executable's import stub of `sceDisplayWaitVblankStart` (NID `0x984C27E7`); any other target refuses the package | the delay slot runs as stock; the call runs unless the runtime skips it (`nk_ace_load_skip_wait()`), which it does only while the run's `load=fast` is in effect and waits are permitted, that is, where the stock call would block. A skipped call still blocks the thread, for 1 µs, the shortest delay wait the scheduler has (that of `sceKernelDelayThread(1)`), so ready threads of any priority run as during the stock wait, and it leaves `$v0` = 0, the stock function's return there. Where waits are not permitted the stock call runs and returns `CAN_NOT_WAIT`. A package with a `load` part must declare an `update_boundary`, and the site may not be in its function, compared by name, by site-map address in a published package, and by resolved address in codegen. Nothing else about where the site sits is checked: a wait in a caller or a callee of the boundary's function is accepted, and the site's own evidence must show it is reached only while loading (section 11.3.10) |

### 9.9. A Code-Hook Package Beside the Timing Package

A build carries one enhancement package and, beside it, at most one `guest_memory_code_hook` package (section 2.3),
so a title's Ace items that change guest memory build together with its 60 FPS game logic. The code-hook package
stays a package of its own: the rule of section 9.2 holds as it is, and a timing package still refuses a code-hook
transform such as `clip_x_widen` (the decision of 2026-11-10, #2366, under "Guest memory written by a timing
transform"). A timing transform writes guest memory only where it replaces a value the title's own logic stores
there; `clip_x_widen` writes the x row of matrices the title built, a value no stock store of that site writes, so
it is a code hook and keeps that category's declaration and checks. The two are joined only in the build, where
each keeps its own category, digest and checks.

- **Naming it.** `build-package` takes `--code-hook-package <path>`, or `--ace-cull`, which reads
  `<user data>/enhancements/<disc_id>-cull.json` (lowercase disc ID), else a published package of that name in
  `assets/enhancements/` (section 5.1). Make takes `ENHANCEMENT_CODE_HOOK_PACKAGE=<path>` beside
  `ENHANCEMENT_PACKAGE`, and codegen `--code-hook-package` beside `--enhancement-package`. An environment that
  names one on its own is refused, as for the enhancement package.
- **Checks.** The code-hook package is validated, matched to the disc, title and executable, and its sites
  resolved and verified as the enhancement package's are. Each of these refuses the build by name, and nothing is
  merged: a second package whose category is not `guest_memory_code_hook`; an enhancement package that is a
  code-hook package too; a code-hook package for another title; a site id both packages use; a code-hook site at
  a word the enhancement package rewrites (a 60 Hz site, an Ace part site or the `update_boundary`) or at the delay
  slot of a rewritten call; a code-hook call whose delay slot is such a word, inside the span the enhancement
  package rewrites; a code-hook site whose rewrite keeps run-time state; a code-hook call to a function that a
  `gate_30hz_alt` site of the enhancement package lists as an alternate (on an alternate update that site calls the
  alternate itself, so the hook would act only on the other updates); and a code-hook package with no sites. Codegen
  refuses `--code-hook-package` without `--enhancement-package`.
- **The build.** The code-hook sites join the site table and are emitted as they would be in a build of that
  package alone; the generated code gains one header line naming the code-hook package's id, version, site count
  and digest; the package's cache key gains its id, its digest and, when it has one, its symbols sidecar's digest.
  A build of a timing package with a code-hook package is `packages/<disc_id>-enhanced-<timing id>+<hook id>` (no
  package id holds a `+`, and the joined name may have at most 64 characters); a code-hook package with no timing
  package is the build's one package, `packages/<disc_id>-enhanced-<hook id>`.
- **Without one** the generated output, the package directory and the cache key are byte-identical to a build
  before this section.

### 9.10. Experimental or Released

A package is experimental unless it declares `"experimental": false`; the validator refuses any value other than
`true` or `false`. The player treats a missing or non-boolean value, and a package it cannot use, as experimental.
The flag changes one thing: the title page's ENHANCEMENTS card tags the title's 60 FPS game logic switch
EXPERIMENTAL for an experimental package and OPTIONAL for a released one. The switch keeps its tier (Ace), its
default (off, section 9.1, item 2) and what it runs. The flag is part of the declaration, so it is in the
declaration digest and the AOT cache key like every other field (section 9.8): the next build of the enhanced
package misses the cache once. An enhanced package already built keeps launching, because the player checks only the
enhancement package id a build records, not its declaration.

Only the maintainer releases a package the project ships in `assets/enhancements/` (section 5.1), by a decision
recorded in an entry for that package under this section, with what was measured and the limits known when it was
released (the entries below). The player honours `"experimental": false` only in a
package it finds in the source tree's `assets/enhancements/` (section 5.1), never in one from the user's data
folder: a package there, which comes first, still loads and plays but is always shown as experimental, whatever it
declares.

#### 9.10.1. *Hot Shots Tennis* (UCUS98701): `hst.timing.native_60hz`

- **Status.** Released from package 14.0.0-pre1 by the owner's decision of 2026-11-12 (#1732), with the known
  limits below. The release carries to package 15.0.0-pre1 (#2547), which adds four sites to 14.0.0-pre1 and
  changes no other; the coordinator recorded this on 2026-11-13. It is short of the goal #1732 sets, true 60 logic
  updates a second in menus, the title screen and matches.
- **Measured, 14.0.0-pre1** (the side-by-side check of section 9.7, recorded in #2362). Package 14.0.0-pre1
  against the authentic game on a private full-match route (title screen, menus and one full match): 57.2 logic
  updates per game second in the menus (with the boot), 59.9 on the title screen and 57.1 in a match. In that run
  77 % of the match's seconds after its entry load held 57 or more, with short dips (the lowest second 33), so the
  `SLOWED` line of section 9.5, item 4 (three seconds in a row under 57) can appear in a match. No compared event
  differed from the authentic game: the point winner, the ball's side and the ball's owner match, and both players'
  random-number seeds advance 1.98 and 2.03 times as often, as `rescale_probability` (section 9.3) expects.
  Shot-type changes were too few to judge, and the tool's overall verdict reads `FAIL` only for reseeds it cannot
  resolve as generator steps, which the authentic run has about as many of (35 against 38). The host was never
  fully idle: the measured run had 43 % mean processor load from other work, and an earlier run at 53 % fell to
  52.7 in the match (`SLOWED`).
- **Measured, 15.0.0-pre1** (recorded in #2547). The package adds four of the eight `gate_30hz_store` sites of
  #2360: the menu cursor ease (S0375) and the float ease of a ticker line (S0386). On private title runs (the
  maintainer's copy), each ease timed from its stored values and the logic-update count: the cursor takes its 7
  values in 12 updates, 0.200 s, as the authentic game does in 6 updates (14.0.0-pre1 took 0.100 s), and a second
  cursor takes 0.167 s in both; the ticker's ease-in takes its 13 values in 24 updates, 0.400 s, in each of 8
  eases, as the authentic game does (14.0.0-pre1 took 0.200 s). A side-by-side check (section 9.7) of a private
  menus-and-match route found no compared event differing from the authentic game (authentic against 60 Hz
  counts: points 18 and 18, point winners 33 and 33, ball side changes 69 and 62, a ratio of 1.13 per game second
  inside its 99 % range of 0.72 to 1.77; both opponents' random-number generators at 1.97 and 2.06 against the
  expected 2), but by the tool's own rules it is not a pass: it covers 79 % of the authentic run's game time,
  under `--min-coverage` (0.9), and logic ran at 46.8 updates a second on a loaded host (`SLOWED`), each of which
  makes it `INCOMPLETE`, and 73 generator steps are unresolved, which makes it exit 1 (`FAIL`). It ran on an
  earlier build of the package that held all eight `gate_30hz_store` sites, not on 15.0.0-pre1; on that private
  route the function of the four sites not shipped never ran.
- **Known limits.**
  1. Further sites belong in the next package update: the four `gate_30hz_store` sites of #2360 that 15.0.0-pre1
     does not carry (S0332, a step toward a target, which runs twice as fast at 60 Hz until then; no private route
     reaches the screen that uses it, so its game time is not measured), and five triage rows still open (#2362).
  2. At 60 Hz the step from the main menu into single player takes 3.94 s longer than in the authentic game (288
     vertical blanks against 52). A 60 Hz run with an earlier package shows the same, so it is older than
     14.0.0-pre1. Its cause is not traced yet (#1732).

  Also recorded in #2362: when two bounces come 33 ms apart or less, the bounce-effect sites free their emitters
  up to two 60 Hz updates early.
- **Not run.** A run on a fully idle host; a side-by-side check of 15.0.0-pre1 itself; the step into single
  player (limit 2) with 15.0.0-pre1; and a measurement of whether 15.0.0-pre1 costs more per update than
  13.0.0-pre1.

## 10. Play Modes: Authentic, Enhanced and Ace

The player offers three play modes per game (#2055):

- **Authentic** is the standard: the game as released, recreating the PSP as closely as the project can. It is the
  default for a new library.
- **Enhanced** turns on the project-authored improvements that work for any game: texture filtering, crisp system
  text, the speed readout, the game badge, widescreen, smooth motion, clean pitched sound, finer patches, sharper
  enlarging and full 32-bit colour (section 10.11). A new library starts with texture filtering, crisp system text
  and the game badge on and the rest off, and a graphics preset sets crisp system text and sharper enlarging
  (section 10.1); finer patches and full 32-bit colour are in no preset. Each one is a *part* that can be turned
  off for the library or for a single game.
- **Ace** is Enhanced plus the improvements made for that game alone. Today that is the title's 60 FPS game logic
  package (section 9). The player offers Ace only for a title with a usable package; its own parts can be turned off
  for that game too.

The parts are project-authored code in the player and the runtime (section 7), not a package: no loader, no AOT change,
no guest hook. Ace's per-title package is the one exception, and section 9 governs it. The runtime keeps the
development name: `src/rt/ace.h` serves both Enhanced and Ace runs, and the in-game UI is `src/rt/gpu_sdl3vk/ace_ui.h`.
Code identifiers that say `ace` mean "Enhanced or Ace"; every name a player reads says Enhanced or Ace.

### 10.1. The Launch Line

The launcher always exports `SR_ACE`, so an inherited value cannot opt a launch in: `off` for Authentic, or
`v1;tier=enhanced;af=16;text=on;readout=off;notice=on;wide=off;smooth=x2;sas=off;tess=off;twin=off`. `tier` (`enhanced` or `ace`) only names the
run, in the game badge, the pause menu and the stamps; a line without it is an Enhanced run. `stick` (the title's
stick dead zone, 1 to 126, section 11.3.11), `load` (`fast` or `off`, section 11.3.10), `depth` (`32` or `off`,
section 10.20), `ssaa` (`2`, `4` or `off`, section 10.21), `peak` (`limit` or `off`, section 10.22), `glyphs` (`on`
or `off`, sections 10.13 and 11.3.5) and `cull` (`on` or `off`, section 11.3.6) are the other keys a line may leave
out: a missing `stick`, `load`, `depth`, `ssaa`, `peak`, `glyphs` or `cull` is off, `stick=<counts>`, `load=fast`
and `cull=on` need `tier=ace` (`depth=32`, `ssaa=2|4`, `peak=limit` and `glyphs=on` do not), the launcher writes
`;load=fast` and `;cull=on` only for an Ace launch with that part on and `;ssaa=2` or `;ssaa=4` for an Enhanced or
Ace launch with supersampling on, and no launcher writes `depth` (section 11.3.12), `peak` (section 10.22) or
`glyphs` (section 11.3.5) yet: a line that carries one is written by hand. A line writes them only when on, in that
order, `cull` last, so a line with them off is the line without them, byte for byte. Every
other key is required, in any order, each once; a value for a feature this build does not carry (`smooth=match`)
refuses the line. So does anything else: the run is Authentic and one `ACE_MODE: refused (<reason>)` line says why.
Unset is off. With Enhanced or Ace on, the launcher also exports `SR_ACE_REPORT`, the per-run report under
`<user data>/ace/`. Ace's 60 FPS game logic is not on this line: it selects the title's enhanced package variant
(section 9.4). Nor is sharper enlarging (`"upscale"`, #2122): the launcher exports it as `SR_PRESENT_UPSCALE`, `fsr1`
for an Enhanced or Ace run with the part on, and `off` otherwise, and always `off` for Authentic (section 2.2). The
part's value is the title's own, else the graphics preset's where it sets the part (Medium, Ultra and Handheld on,
Low and High off), else the library's; crisp text (`text`) is resolved the same way, and every preset turns it on.
The launcher also exports `SR_MOVIE_UPSCALE=off` for an Authentic session; an Enhanced or Ace session
keeps the inherited value, as the movie upscaler has no setting.

### 10.2. Authentic Is Untouched

With `SR_ACE=off` (the default: players opt in) every Ace branch sits behind one switch that stays false: the Vulkan
device is created with no Ace feature (its one other feature setting is the destination-read path below), every
sampler and the texture descriptor pool are as before, the presenter registers no UI pass, and the observation
counters do not run. Nothing is printed, except one `TEX_SCALE: off` line when the environment asks for texture
scaling or the smart 2D filter, which names the request and does not obey it (section 10.10), one `TWIN: off` or
`TWIN: refused` line when `SR_GPU_TWIN_COLOR` is set, which changes nothing else (section 10.11), one line that
names an unknown or refused `SR_GPU_DST_READ` value, which changes nothing else, and the
`sdl3vk: destination reads: interlock|snapshot` line that every run prints (below). Finer patches (section 10.14)
have no environment variable, so an authentic run prints nothing for them. Nor has the host depth buffer (section
10.20): an authentic run makes the 16-bit depth buffer it always made and prints nothing for it.
`gpu-coherence-selftest` checks the samplers and the pool; `display-smoke` keeps its image hash.

One device feature setting outside Ace applies to every run, Authentic included, because it is a correctness fix and not an
enhancement: the destination-read path of shader blends and the stencil test (#2109,
[`src/rt/gpu_sdl3vk/README.md`](../src/rt/gpu_sdl3vk/README.md)). On a device with
`VK_EXT_fragment_shader_interlock`, `fragmentShaderPixelInterlock`, `fragmentStoresAndAtomics` and RGBA8 storage
images, the device enables that extension and those two features, unless `SR_GPU_DST_READ=snapshot` forces the
snapshot path, so primitives of one batch blend in drawing order, as on the PSP and in `ge.c`. On the snapshot path
the device is created with no feature outside Ace.

### 10.3. What Ace Changes, and What It Never Changes

- **Never, for a host-only feature:** guest logic, the instruction trace, the generated code and the package key
  (`SR_ACE` is not a codegen option), the guest timing model (VBLANK schedule, GE timing, input sampling, media
  clocks). Every feature in this section is host-only except texture filtering (the "Pixels" bullet), which section
  11.1 names as already guest-visible; the other items are host-only unless section 11 admits them.
- **Host work:** the detector and the readout count flips, draws and host waits; their cost runs on the scheduler
  thread like any renderer cost.
- **Pixels:** texture filtering (anisotropic, for transformed draws with linear minification) changes framebuffer
  pixels, and every present commits the frame to guest VRAM, so a game that copies its screen (a save picture) keeps
  the filtered picture: the same class as a render scale above 1x, and the launcher says so. The launch notice, the
  speed readout and the crisp keyboard draw only on the swapchain image, never on the frame a capture reads, and so
  do Smooth motion's in-between frames (section 10.6).
- **Supersampling:** `ssaa=2` or `ssaa=4` changes only the image the presenter shows; the frames a present commits
  to guest VRAM are those of a run without it (section 10.21).
- **Sound:** `sas=sinc` writes band-limited sample values into the guest's SAS output buffer, so it is a
  guest-visible item. Its entry is section 11.3.1. `peak=limit` changes only what the host plays: the guest's SAS
  buffer keeps the PSP's saturated bytes (section 10.22).
- **Patches:** `tess=2` to `tess=4` draws every triangle patch with finer triangles, which changes the frames a
  present commits to guest VRAM, and under `SR_GE_TIMING=1` it alters the GE timing model's clocks. It is a
  guest-visible item. Its entry is section 11.3.7.
- **Input:** `stick=<counts>` (Ace only) changes the left-stick values of the live host samples the guest latches,
  never when they are latched. It is a guest-visible item. Its entry is section 11.3.11.
- **Loads:** `load=fast` (Ace only) shortens the display waits a title's own loading screen paces itself with, so
  it changes guest timing. It is a guest-visible item, not yet admitted. Its entry is section 11.3.10.
- **Depth:** `depth=32` gives the Vulkan renderer a 32-bit float depth buffer, which changes which surface wins
  where two lie within one 16-bit depth step, in the frames a present commits to guest VRAM. The depth the guest
  reads stays 16-bit and unchanged. It is a guest-visible item, not yet admitted. Its entry is section 11.3.12.

### 10.4. Stamps and Evidence

An Enhanced or Ace run prints `ACE_MODE: on (tier=..., ...)` at start and `ACE_MODE: exit (...)` at an ordinary exit,
and its flight recorder bundle carries `runtime.enhancements` (`assets/flight_recorder_schema.json`); an Authentic
bundle's bytes are unchanged. `tools/flight_diff.py` refuses to compare any bundle that carries the block,
`tools/boot_gate.py` refuses an Enhanced or Ace run's log, the acceptance checkpoint refuses both
(`reason=ace_mode`), and the launcher never starts either for it.

### 10.5. Per-Title Choice and Precedence

The library's default is Authentic or Enhanced. Ace is never a library default: it depends on a package only some titles
have, and a player turns it on title by title where they want it. Each title follows the library or chooses one of the
three modes for itself (its page's Play mode button, or Settings > Enhanced opened from its page). What a title launches
with resolves in this order:

1. **The mode.** The title's own choice, else the library's default. Authentic turns everything off, its 60 FPS game
   logic included, and the line is `off`.
2. **Each Enhanced part, for Enhanced and Ace alike.** The title's own value where it has one, else the library's. So
   Ace includes Enhanced, with the title's own choices first.
3. **Ace's parts.** The title's 60 FPS game logic is on unless the title turned it off. A title whose package has gone
   is blocked, not quietly played without it, until the package is back or the part or Ace is turned off.

A title whose published package ships in `assets/enhancements/` (section 5.1) is offered Ace with nothing to install:
choosing Ace for it runs its 60 FPS game logic, as item 3 says. Ace stays a per-title choice; shipping a package never
changes the library's default or a title that follows it. The player offers Ace from the package's title alone; the
build then checks the disc's revision and its executable against the package, and a disc they do not match is
refused there, with nothing built and nothing launched in its place.

A title's 60 FPS game logic and smooth motion may be on together (owner decision of 2026-11-04, replacing the earlier
rule that refused the pair, #1272): the package keeps the game's logic at 60 Hz, and interpolation presents
in-between frames above that rate. The two are separate switches. Smooth motion keeps its own key (`smooth`) and its
own library and title settings, is off by default, and stays off by default until the "nothing worse" evidence of
section 11.2, item 4 exists for the 60 FPS package together with the 60 Hz interpolator. The 60 FPS
package's own switch does not turn it on or off.

What stays refused is a second interpolator: one interpolation mode runs at a time, so `smooth=match` and `smooth=x2`
are never both on, and a launch line that names a mode this build does not carry is refused whole (section 10.1).

This build does not interpolate above 30 Hz. Smooth motion acts only on a title the detector holds at 30 (section
10.6); a title running 60 FPS game logic is presented frame by frame as it comes, as without the feature. The
interpolator for a 60 Hz source is its own item, with its own key, entry and evidence: a host-only feature under
section 10.6 unless it changes something section 11.1 lists, in which case it is a section 11 item. The
player turns smooth motion off for a launch of the enhanced 60 FPS build, and the title page says so. That is a
limit of this build, not a rule of the contract.

The session flag `--experimental-60hz` makes every title with a usable package that resolves to Enhanced or Ace run as
Ace for that session, with its 60 FPS game logic and `tier=ace`; a title that resolves to Authentic stays Authentic.

A part is one row of the player's part table (`src/player/player_state.c`): its key, its values (each part has an off
value, 0) and the words a row shows. The settings file, the launch line and both settings pages walk that table, so a
new part is one more row and no new switch. Widescreen (`wide=fill`, the Fill mode of section 10.8) is one such row,
off by default.

### 10.6. Smooth Motion

With `smooth=x2`, a title the 30 Hz detector holds steady at 30 is presented at the VBLANK rate: every second
picture is an in-between made from the two frames around it. That doubles the pictures, not always the motion.
Content the search cannot follow is shown as in the old frame and still moves 30 times a second ("What it follows"
below). The code is `src/rt/gpu_sdl3vk/ace_smooth.c`, its shaders and the pure pacing rule in `src/rt/ace.c`.

- **How a frame is made.** Block-matched motion between two private copies of consecutive presented frames, in a
  compute pass on the present's command buffer. It does not need #318, and it treats a GPU target and a CPU frame (a
  movie) alike.
- **What it reads of the GE.** For a GPU target, two host-only inputs (`src/rt/gpu_sdl3vk/ace_hud.c`):
  - **A HUD mask.** The boxes of the frame's through-mode (2D) draws onto that target, as one bit per guest pixel.
    A box that covers a quarter of the screen or more, or that samples a render target, is a backdrop or a post
    effect and is left out. A frame with more than 1024 boxes, or boxes covering more than two screens, has no mask.
    Masked pixels never steer the motion search. Beside the HUD, where the scene's own vector puts most of a block
    under it, the block keeps the cost the coarser search level measured for that vector. A vector whose end lands
    on the HUD in one frame takes only the other frame's side, judged by its block's cost, so the HUD is never
    dragged into the scene. A changed pixel on the HUD keeps the old frame's side.
  - **Depth.** A private R16 copy of the target's depth, taken only when a batch wrote that depth image since it
    was last handed on. A clear that clears depth counts as a write. Freshness belongs to the depth image, not to
    the colour target: when two display buffers share one depth buffer and a frame writes no depth, the depth still
    holds the other buffer's frame and is withheld. A frame without fresh depth has none, so its pair matches on
    colour alone. A vector whose two ends land at different depths fits worse: it joins two surfaces, not one that
    moved. The GE's depth image is only read and keeps its layout.

  Both are used only when both frames of a pair have them. Each present hands its inputs to the image it presents
  and to no other, and the show pass takes them once: a CPU frame (a movie) never gets a GPU frame's mask or depth,
  and inputs no show pass took are dropped and counted. `SR_ACE_SMOOTH_HUD=0` and `SR_ACE_SMOOTH_DEPTH=0` turn each
  off. There is no per-title switch yet. With both off, the in-betweens are the same bytes as without these inputs.
  The boxes are noted in the GPU GE's draw hooks after the draw is recorded, and the depth copy is recorded in the
  present's command buffer. So neither changes what the guest, the authentic renderer or a capture sees.

  Nothing is dropped silently. The report's `smooth_aux` block counts the pairs that used each input (`pairs`,
  `pairs_mask`, `pairs_depth`) and the depth copies refused (`depth_refused`). It counts the boxes kept, merged or
  left out, by reason (`boxes`: `kept`, `merged`, `large`, `target`, `outside`, `full`). It also counts frames:
  those with a mask, those that overflowed it, and those with depth, with unwritten depth or with no depth buffer.
  The last count is inputs no show pass took (`frames`: `mask`, `overflow`, `depth`, `depth_unwritten`,
  `depth_none`, `unshown`). The run's exit prints the same counts on one `ACE_MODE: smooth inputs` line. The first
  frame to overflow the mask prints a warning.
- **When.** The flip of frame B shows the in-between of A and B; the next VBLANK shows B. Nothing is shown before the
  guest made it, so a frame is never predicted. The cost is one VBLANK of added delay (about 17 ms), reported in the
  run's report as `added_delay_ms`. A new flip that arrives before B's present replaces it, counted in
  `presents.superseded`: one VBLANK of delay does not fit a frame that is on screen for one VBLANK (a title leaving
  30 FPS), and holding each frame back instead would keep every later frame one VBLANK late.
- **A busy display never costs a real frame** outside fast forward (see "Only at 30" below). While the guest is
  paced to real time, a present does not wait for the display (`src/rt/gpu_sdl3vk/README.md`): when the display
  cannot take a frame, the present returns at once and shows nothing. When that happens to B's present, B stays
  owed. It is tried again at each later VBLANK until a present shows it: one with no flip, or one whose flip's own
  present found the display busy too. The first flip whose present the display takes shows B in place of its
  in-between, then its own frame at its VBLANK, however many flips the display missed in between. Frames whose
  presents the display never took are not shown, as without smooth motion; every frame whose present it took is
  shown, in order. So a frame whose in-between was shown is never dropped for a made-up picture, and a display that
  takes only every second present shows the real frames, with `SMOOTH OFF` on the readout. A title that flips each
  frame twice is no different: when the flips in between found the display busy, the second flip of a newer frame
  is that frame's first present, so it shows B and holds its own frame, owed in turn, for the next present. What a
  present shows changes the pace only once that present has reached the screen. When the present that shows B fails
  after its submission (an out-of-date window, say), nothing reached the screen, as with a busy display: B stays
  owed, the pass drops the frame it recorded with it, and the CPU present that takes over shows B and holds C. When
  that one fails too, B's VBLANK shows it and C is lost, as a failed present loses a frame without smooth motion.
  B is given up when a present fails before its submission, when its VBLANK's own present fails (a failure that
  repeats, such as a minimised window, never leaves an old frame waiting to be shown after newer ones), when the
  window closes, or when the renderer can present nothing any more. `presents.busy` counts the held presents the
  display could not take or that failed and were tried again, and `presents.superseded` every held frame that never
  reached the screen.
- **Only consecutive frames.** An in-between is made of a frame and the one flipped just before it. When that one
  never reached a present (the presenter's slot rule dropped its flip on a busy PC, or the present failed), the
  pass still holds an older frame, and an in-between of the two would put a fast-moving object in the wrong place
  or lose it. The new frame is then shown as it is and kept, and the report counts it in `presents.unpaired`.
- **Only at 30.** Every other detector class, a pause, a static picture and fast forward (section 10.15) show each
  frame as it comes, as without the feature. Fast forward keeps each frame it shows, so the first in-between after it
  is made of two frames shown one after the other. A frame still held or owed when fast forward starts is
  superseded by the first new frame (`presents.superseded`), which is shown as it comes. A title that runs 60 FPS game logic is such a class in this build (section 10.5). The detector enters
  30 Hz after a second of agreement and leaves after half a second of disagreement.
- **Never off without saying so, and never more than it moved.** The setting's description says it smooths
  steady 30 FPS scenes only where it can follow the motion. With the speed readout on, its text ends with
  `SMOOTH n`, or with `SMOOTH OFF` for a second in which no in-between was shown: another frame rate, a PC that
  cannot keep up, or no Vulkan device for it. `n` counts what moved, not what was presented: the real pictures
  that reached the screen in that second (the held frames and any new frame shown as it is; a present the
  presenter dropped is not counted), plus each in-between by its measured weight (next item). A scene whose
  in-betweens repeat the old frame reads `SMOOTH 30` at sixty presents a second.
- **What an in-between moved.** Every in-between is measured against its two frames in its own command buffer
  (`shaders/ace_measure.comp`), on a 480 x 272 grid of the picture, by the largest channel difference in 1/255.
  A pixel is moving when the two real frames differ there by more than 40. Over the moving pixels the pass sums
  the in-between's distance to the old frame and to the new one. The phase is the old sum over both: 0 for a
  repeat of the old frame, one half when the changing content sits half-way, 1 for the new frame. The weight is
  twice the smaller sum over both: 0 at either end, 1 at one half, and 0 when nothing differs. A plain blend of
  the two frames also weighs 1, so the weight says the content is between the frames, not that it is sharp. The
  counts of moving pixels within 6 of the old frame and of the new one tell the two apart: a carried object
  leaves its old place showing the new frame, a blend shows neither. The pass also counts the pixels the two
  frames agree on (within 40) that the in-between shows more than 64 from both. That is an object drawn in mid
  flight or a hole in something that stood still, and the count cannot tell which. Only an in-between that
  reached the screen is counted. Its numbers are read once the presenter has seen the submission that carried
  its commands finish, a few presents later. One that cannot be read then is counted, never guessed, and the last
  few of a run are not read. A device that cannot build the measure pass has no smooth motion for the run, and the
  report says it is unavailable. The report's `smooth_motion` block holds the run's totals (`measured`,
  `unread`, `moving_px`, `old_px`, `new_px`, `novel_px`), the pooled `phase` and the mean `weight`.
- **Recording.** A run recording (`SR_VIDEOREC_DIR`) holds what the window shows, in-betweens included: the recorder
  keeps each in-between for its own VBLANK and writes the held real frame at the next one. Its `frames.csv` and
  `logic.srt` still count only the game's own flips.
- **UI.** A pixel that did not change between the two frames is copied, so a HUD stays exact while the scene moves
  behind it.
- **What it follows.** The search matches blocks of two finished pictures. It finds motion of the picture as a
  whole or in large parts (a scroll, a camera move) and shows that half-way. It does not find a figure whose pose
  changes a lot between two frames, or a small object that travels further than its own size (a ball). Those keep
  the old frame ("Where the search guesses" below), so they still move 30 times a second, one VBLANK late.
  Measured on 2026-11-09 on a deterministic recording of a private route through a *Hot Shots Tennis* rally, with
  a package built after #2320 landed: a pooled phase of 0.019 and a mean weight of 0.085. (A run before #2320
  measured a phase of 0.03, with 92.5% of the moving pixels shown as the old frame.) For such a scene the feature
  adds presents, not motion, and the readout says so.
- **Which titles use what.** A title with a 60 FPS game logic package (section 9; *Hot Shots Tennis* has one)
  gets every frame from the game itself. That is its path to 60, and the player turns smooth motion off for that
  launch (section 10.5). A 30 FPS title without a package has only smooth motion, with the limit above.
  In-betweens drawn from the game's own draws with interpolated positions, which would follow figures and small
  objects exactly, are not built yet (#1272).
- **Cuts.** When a third of the blocks fit nothing or disagree with their neighbours, the two frames are different
  pictures: the in-between is the old frame, and the new one follows on its VBLANK.
- **Where the search guesses.** In a block whose two frames differ, a vector is a guess when moving it two samples
  fits the colours no worse (a plain floor), or when, at half or more of the changed samples, it joins content that
  looks alike but differs and that neither it nor the coarser level's 3x3 vectors carry unchanged (a textured
  floor). Every changed pixel within two blocks of a guess keeps the old frame. So a figure whose pose changes too
  much for any vector (a player serving on a plain court) shows whole, as in the old frame, and never partly as
  floor. Limits: in such a scene most moving pixels repeat the real frame instead of moving halfway; a small fast
  object near a guess (a ball in hand) freezes where it was in the old frame; and on strongly blocky textured floors
  a few pixels can still be wrong (4 to 12 in a probe).
- **What it never touches.** The in-between and the held frame are private images blitted to the swapchain. The
  flip's own present still commits the real frame to guest VRAM and gives any capture the real frame. The VBLANK's
  present latches no input and serves no capture.
- **Stamps.** The report adds `presents` (real, between, held, superseded, unpaired, busy), `smooth_motion` and
  `present_interval_ms`. Section 10.4 applies: the run is refused as evidence.

### 10.7. Band-Limited SAS Resampling

`sas=sinc` (#2054) resamples SAS VAG voices through a band-limited filter on the host. The mixed values land in guest
memory, so it is a guest-visible item, and its entry is section 11.3.1.

### 10.8. Widescreen Fill: the Renderer Layer

Fill shows more of the 3D scene to the left and right of the PSP's 480x272 picture, at the game's own vertical field of
view and pixel scale, and keeps 2D at its native shape in the centre. The GE has no x or y clip planes, so the
transformed positions of geometry past the screen edges already exist; the guest's scissor is what drops them. Fill
draws them again onto host-only images instead of widening any guest matrix. The code is in `src/rt/ge.c` (which
primitives reach the wings, and the software rasterizer's wings), `src/rt/gpu_sdl3vk/ge_gpu.c` (the Vulkan wings and
the Vulkan picture), `src/rt/gui.c` (the setting, the wing width and the software picture), `src/rt/gpu_sdl3vk/sdl3vk.c`
(the window fit) and `ge_wide_*` in `src/rt/ge_shared.h`.

- **The picture itself never changes.** Guest VRAM, depth, readbacks, captures, the snapshot path and GE time are those
  of Fill off. Each display framebuffer has a host-only wing canvas holding both wings, which no guest-memory path
  reaches, and a wing pass charges no GE time and counts in no guest or frame statistic. One route differs: the opt-in
  GPU T&L route (`SR_GPU_TNL=1`, off by default) declines while Fill is on, because the wings need ge.c's screen-space
  triangles, so with that switch set the picture is drawn by CPU T&L as it is without the switch.
- **What reaches the wings.** A transform-mode triangle or sprite that leaves the screen while it draws into a display
  framebuffer under a scissor as wide as the screen, shifted by whole pixels so it rasterizes as it would on a wider
  screen: a triangle after the 28.4 snap, a sprite after flooring its corners, so a corner at x = -10.5 lands in column
  D - 11. Of the through-mode (2D) draws, only clear-mode sprites that reach a screen edge (`sceGuClear`'s strips) and
  untextured whole-screen sprites (fades) are stretched across the wings; every other 2D draw stays in the centre. CPU
  writes (movies), block transfers, lines and points do not reach the wings. On Vulkan a primitive whose state the
  backend draws in software is dropped from the wings, counted, and named once on stderr (`gegpu: Fill dropped a wing
  primitive`).
- **Never a stale wing.** The first triangle or sprite drawn into a framebuffer after it was shown (`ge_wide_shown`)
  begins a new frame there and clears its canvas, so a canvas holds only the wings of the frame in its framebuffer,
  whatever the game clears with, and a frame presented again with no draw in between (a 30 FPS frame on a 60 Hz VBLANK)
  keeps its wings. `ge_wide_drawn` is true only when a wing primitive was really drawn since that clear; the
  presenter shows black wings otherwise.
- **Presentation.** With `wide=fill` (Settings > Enhanced > Widescreen, off by default and never in Authentic), each
  present sets the wing width from the window's shape and shows one (480 + 2D) x 272 picture: the left wing, the frame
  and the right wing. The Vulkan renderer composes it on the GPU (`gegpu_present`), the software renderer in
  `src/rt/gui.c`, and the window fits it by its own shape, so the upscaler and window captures see one picture. Only
  the SDL3/Vulkan presenter shows wings; the headless sink shows the frame alone. In this build Fill and Smooth motion
  (section 10.6) are exclusive: Smooth motion's HUD mask and depth are sized to the 480-wide frame, so with both on,
  Fill reports itself unavailable (`ACE_MODE: wide unavailable (...)`) and the run has Smooth motion only.
- **Any window aspect wider than the PSP's.** The wing width comes from the window's shape at the PSP's 272 lines,
  rounded down to a multiple of 4: 21:9 (about 635x272) has 76-pixel wings and 32:9 (about 967x272) has 240-pixel
  wings. The PSP's 480x272 is 30:17, about 16:9; a 16:9 window widens it by under 4 pixels, so 16:9 has none. One canvas
  holds both wings in a 512-column image, the GE's widest framebuffer, so the Vulkan canvas shares every target's size,
  viewport and readback. That caps a wing at 256 pixels, a picture of 992x272 (about 32.8:9); a wider window (three
  screens) gets the widest Fill and black beyond it.
- **Off is the default.** With no wing width set, no canvas exists and no wing is drawn or submitted, and the window
  shows the frame alone, fitted as before. What Off still costs is a test per primitive in `src/rt/ge.c` (`s_wide_d`),
  tests of an always-NULL pointer in the software rasterizer's pixel accessors (`s_wing`) and the Vulkan backend's
  batch path (`s_wing_dst`), and a test of the wing width per present.
- **Tests.** `gpu-coherence-selftest` (case `wide-fill`, also at render scales 2 and 4) and
  `ge-cpu-profile-validate-selftest` run one list at 16:9, 21:9 and 32:9. Shapes with sub-pixel corners in and across
  each edge (flat and gouraud triangles, transform-mode sprites) are drawn a second time wholly on screen, and each wing
  pixel must equal, exactly and at every scale, the pixel of the same renderer's frame that a wider screen would show
  there. At render scale 1 the software and Vulkan wings must also agree with each other: byte for byte, except on a
  gouraud shape's rows, where each channel may differ by one step. The tests also check that all of guest VRAM is
  byte-identical with Fill on and off, that the GE clock is unchanged, that a narrower scissor reaches no wing, that a
  shown canvas holds only the next frame's wings, that a frame whose every wing primitive the Vulkan backend drops
  leaves the canvas undrawn and black, and that the wing hooks receive the guest's vertices snapped and shifted. The
  Vulkan present cells check the composed picture: its wings are the canvas's and its centre is the frame, a frame
  presented twice keeps its wings, a new frame that draws no wing shows black ones, and Fill off makes no picture.
  `gpu-capture-selftest` checks that the window fits every 480k x 272k source as before, to the pixel, and a Fill
  picture by its own shape; `ace-selftest` and the player's tests check the `wide` key from the settings file to the
  runtime's parser.

**Next layer.** Layer 2 is per title: a game that skips objects it considers off screen never submits them, so they
are missing from the wings or pop in at their edges. A declared symbolic `guest_memory_code_hook` (section 2.3) that
widens that title's culling extents together with the view, resolved through its title profile and never by a raw
address in a package (section 5), fixes that title, and the title's HUD stays inside the PSP's picture at every
aspect. That hook changes what the guest computes, so it is a section 11 item (11.3.6). The Widescreen row's help
text says that objects can pop in at the edges. The `clip_x_widen` site of section 9.2 is the mechanism for a title
that builds one clip-space matrix per camera and tests object bounds against it.

### 10.9. Settings

`settings.json` keeps the library's choice in `"enhanced"` and a sparse `"title_play"` map, every value by name:

```json
"enhanced": {"default": "enhanced", "af": "16", "text": "on", "readout": "off", "notice": "on", "wide": "off",
            "smooth": "off", "sas": "off", "tess": "off", "cull": "off", "upscale": "off", "stick": "off",
            "load": "off", "twin": "off"},
"title_play": {"ULUS10000": {"mode": "ace", "af": "8"}, "ULES00000": {"mode": "library", "logic_60": "off"}}
```

A title's `mode` is `library`, `authentic`, `enhanced` or `ace`; a part it does not name follows the library; its
`"logic_60": "off"` turns Ace's 60 FPS game logic off. An entry that follows the library everywhere is not written.
Reading never makes a choice more permissive: in an `"enhanced"` block, a part it does not name or a word this build
does not know is off, and a default other than `enhanced` is Authentic; a title's unknown mode follows the library,
and an unknown part value is off. A file with no `"enhanced"` block starts from the fresh defaults: Authentic, with
16x filtering, crisp text and the game badge set for when the player turns Enhanced on.

Development builds after v0.0.1 saved `"ace"`, `"ace_titles"` and `"enhancement_60hz_discs"`. The player reads them only
when `"enhanced"` is absent, maps them as below, and writes the new keys from then on:

- `"ace"`: `"enabled": true` is an Enhanced library default, `false` Authentic; its feature values become the library's
  parts (a value those builds did not know keeps the fresh default, as they did).
- `"ace_titles"`: mode `ace` is Enhanced, `authentic` Authentic, anything else follows the library; the title's
  feature values become its parts.
- `"enhancement_60hz_discs"`: each disc becomes plain Ace: every Enhanced part as the library has it (the documented
  defaults unless the old `"ace"` block changed them), any value of its own from `"ace_titles"` kept, plus its 60 FPS
  game logic. Its parts can be turned off one by one afterwards. A disc chosen Authentic stays Authentic: its 60 FPS
  game logic was not in effect while Authentic was chosen.

`tests/native/test_player_state.c` (`test_play_mode_settings`, `test_play_mode_settings_mapping`) checks the names,
the precedence, the off switches at both levels and this mapping; `test_play_mode_upscale_settings` checks sharper
enlarging (`"upscale"`: `fsr1` or `off`) and the upscaler it sets for the launch; `test_play_mode_ssaa_settings`
checks supersampling (`"ssaa"`: `2`, `4` or `off`) and its place on the launch line.

### 10.10. Texture Scaling and the Smart 2D Filter

Two features in the Vulkan GE renderer (#1273). Both are guest-visible (section 11.1) and are not admitted under
section 11.2: section 11.3.3 is their entry and says what the guest can observe, and this section says how they draw.
The code is `src/rt/gpu_sdl3vk/tex_scale.c` and its hook in `ge_gpu.c`'s texture cache; the scaler is xBRZ 1.9
(`third_party/xbrz/`, GPL-3.0).

- **Settings.** `SR_TEX_SCALE=off|2|3|4|5|6` (the xBRZ factor), `SR_SMART_2D=off|on` and `SR_TEX_SCALE_BUDGET_MB`
  (1 to 8192, default 512: the GPU memory the scaled copies may hold), read when the renderer starts. Both features
  take effect only in Ace Mode: in an authentic run they stay off whatever the environment says, and a request
  prints one `TEX_SCALE: off (texture scaling and the smart 2D filter need Ace Mode)` line. A value outside the list
  prints `TEX_SCALE: refused (<reason>)` and leaves both off. There is no launch-line key, library setting or
  per-title setting yet: they come in the launch-line pull request that follows the Ace settings work (#2074), and
  only that pull request admits the features (section 11.3.3).
- **What is scaled.** A texture the texture cache decodes, with one level (no mipmaps), after its palette lookup.
  The factor drops for a texture whose scaled side would pass 2048 texels, and a texture that would pass 2048 at 2x
  is not scaled. The image is padded by two texels, repeated or clamped by the texture's wrap mode, so a repeating
  texture tiles without a seam.
- **Never scaled.** A render target sampled as a texture: bound directly, it never enters the cache; read back
  through the cache, it is decoded unscaled; and a texture whose VRAM range meets any live render target's is not
  scaled.
- **Never stale.** A scaled copy records the cache entry's upload and the content hash it was made from, and is
  bound only while both still match. A texture the guest rewrites, or a CLUT texture drawn with another palette (a
  different cache key), is drawn unscaled until its own copy is ready, and never with the old one.
- **Never waited on.** Two worker threads scale; the renderer only queues the decoded texels and stores finished
  copies at a later lookup. The jobs, waiting, running or finished, are at most 64 and hold at most 128 MiB of host
  memory together (one job is always accepted, so the largest copy, 16 MiB, can be made). Storing a copy is an upload
  on the renderer thread, so at most 8 MiB of copies are stored per present, the first copy of a present always;
  the rest wait in the job table for the next present. Until its copy is stored, or with the job table full, a
  texture is drawn unscaled. Nothing is hidden or skipped.
- **Budget.** The copies hold at most `SR_TEX_SCALE_BUDGET_MB` of GPU memory. A copy that would not fit first frees
  copies that no draw has bound for 60 presents (one to two seconds), the longest idle first, until a quarter of the
  budget is spare besides the new copy; that drains the GE once, as a cache eviction does. A copy bound in those 60
  presents is never freed, so when the copies in use fill the budget, the new texture is drawn unscaled. It is
  offered again on a later draw, at most once per present and only when it could fit, and so is a texture whose
  copy was freed.
- **Sampling.** A copy is sampled on its own texel lattice: the fragment shader's PSP sample-point rule (nearest at
  the texel centre, linear on the 1/16 grid) runs at the copy's resolution, from the factor the draw carries in its
  flags.
- **One texel per pixel.** A draw that maps one texel to one pixel (the smart 2D test below, whatever the filter)
  binds the unscaled texture even when a copy is ready. xBRZ adds no detail at that size, and because it scales a
  whole atlas (a font page), a copy carries a neighbouring cell into a cell's edge: the marks LittleBigPlanet's text
  showed. Magnified and 3D draws keep the copy.
- **Smart 2D.** A textured draw that maps one texel to one pixel on each axis (either direction), axis-aligned,
  without perspective and with its edge on a texel boundary, samples nearest: a HUD, a font or a sprite stays crisp
  at a render scale above 1x. The map may differ from 1:1 by 1/64 texel over the draw's whole span, and the
  coordinate every pixel centre samples must lie on a texel centre or at most 1/32 texel above it, never more than
  1/65536 below (the test's own float rounding). The PSP's linear rule floors to a 1/16 step, so a centre even
  slightly below takes part of the texel before, while above it up to the next step reads the texel alone; the
  shader's rounding to 1/4096 is left whole for the host's interpolation error. A triangle must carry the same 1/w
  at all three vertices, bit for bit: the shader divides by the interpolated 1/w at each pixel, so any difference
  bends the map between the vertices. At 1x every draw the test accepts reads the same whole texel with either
  filter, so the frame is unchanged; `tex-scale-selftest` checks that against the shader's two rules, with its
  per-pixel division, over offsets and drifts on both sides of the lattice, and refuses the draws just below it.
  Every other draw, a 3D one included, keeps the game's filter.
- **Guest memory.** Neither feature writes guest memory, the CLUT or a texture source; texture readbacks and
  render-to-texture results are unchanged. What a present commits to guest VRAM does change, which makes both
  features guest-visible: section 11.3.3 is their entry, and this section covers only their presentation.
- **Stamps.** `TEX_SCALE: on (xbrz=<factor>, smart2d=on|off, budget_mb=<n>)` at start; at exit,
  `TEX_SCALE: draws (scaled_binds=, onetoone_unscaled=, scaled_uploads=, stale=, smart2d=, reclaimed=,
  budget_short=, upload_capped=)` and
  `TEX_SCALE: exit (scaled=, queue_full=, discarded=, budget_peak_mb=, worker_ms=)`.
  A run with either feature on also prints, at start, the line section 11.3.3 describes.
  Section 10.4 applies: the run is an Ace run.
- **A title it breaks.** A title whose fonts or pixel art suffer gets its off switch in its own package, never as
  per-title code in a generic file.
- **Hot Shots Tennis and LittleBigPlanet keep smart 2D off.** Both draw text from an anti-aliased glyph page, one
  texel to one pixel. Nearest sampling turns each glyph texel into a hard block at a render scale above 1x, so the
  filter makes their text worse, not crisper. The evidence is the before-and-after runs at render scale 4 recorded in
  #2092. `SR_SMART_2D` is off by default for every title, so nothing in the runtime names them. Their per-title
  setting comes with the launch-line pull request that follows #2074 (section 11.3.3). Their text is what section 10.13 rebuilds.
- **Tests.** `tex-scale-selftest` checks the C port against goldens from the upstream release, the borders, the job
  queue, the settings (an authentic start with both variables set keeps both off) and the classifier, with refused
  draws at the edge of its tolerance. `tex-scale-gate-mutant` builds the selftest without the Ace gate and passes only
  when the settings cell fails for that reason. `gpu-texscale-selftest` runs the production path on a Vulkan device
  at render scales 1, 4 and 2, the budget's reclamation included; `gpu-texscale-mutant` builds a cache that binds a
  copy without its content check (state, serial and hash) and passes only when the rewrite cell fails for that
  reason.

### 10.11. Full 32-Bit Colour (the Host Twin)

The game's 16-bit render targets are presented in full 32-bit colour without dither (#2050), in both GE renderers.
It is a presentation-only change (section 2.2): it changes only the image the host shows, never what the guest
draws, reads or waits for. The code is the host twin in `src/rt/gpu_sdl3vk/ge_gpu.c` (its header and the comment
at `s_twin`) for the Vulkan renderer, and in `src/rt/ge.c` (the comment at `s_twin_mode`) for the software renderer.

- **How.** Every render target and depth buffer gets a host-only twin at the user's render scale. Every write to the
  authentic image reaches its twin, in order: each render op is drawn a second time into the twin without dither or
  the 16-bit colour snap, and uploads, CPU-write patches, depth uploads, block transfers and destination snapshots
  are repeated on it. Twin passes sample twins, so a render-to-texture chain stays full colour. The presenter shows
  a twin only while it holds every write its image took; otherwise it shows the authentic image and re-seeds the twin.
- **The software renderer.** The same switch turns on a twin in `ge.c`, decided on the renderer's first colour write
  and inert while the Vulkan renderer draws. It keeps one host slot per 2-byte unit of the 2 MiB of VRAM (an 8888
  pixel uses the slot of its first unit; 9 MiB of host memory): the twin's 8-bit colour, and a witness, the exact
  value the authentic write left in VRAM. Each colour write is computed a second time from the same source colour,
  with the same blend and logic op but no dither and no 16-bit colour snap, against the twin's colour where the slot
  is valid and against the authentic pixel where it is not. A slot is valid only while VRAM still holds its witness
  in the same format, so a write the twin does not take (a CPU store, DMA, the fast clear path) shows, and is
  blended against, its authentic value from that pixel on. The twin's alpha is the alpha the authentic write left
  (the stencil, in the target's format, as the Vulkan twin snaps it); a stencil-only write updates it, and a write
  mask over colour bits leaves the pixel authentic. Block transfers within VRAM copy the slots with the bytes. The
  CPU presenter shows the slots (`ge_twin_present_row`), each pixel without a valid slot as the CPU presenter converts it with the twin off.
  `gpu-coherence-selftest` twin-soft checks it on twin-scene's list: guest VRAM equals the twin-off run byte for
  byte, the shown gradients are smooth, and T1 and T3 are within one step per colour channel of the Vulkan twin.
- **Render-to-texture in the software renderer.** Its texture fetches read guest VRAM, so a target drawn from a
  render target's texture takes that texture's 16-bit texels into its twin: twin-scene's T2 shows T1's dithered
  5551 texels in the software renderer, and T1's full colour twin in the Vulkan renderer. Its own blends and its
  presentation are still full colour.
- **Guest memory.** The authentic chain keeps the user's render scale, and nothing reads a twin back. Guest VRAM,
  every readback, every block transfer into RAM and the frame each present commits to guest VRAM are the twin-off
  run's at the same scale, byte for byte (`gpu-coherence-selftest` twin-scene, at scales 1, 2 and 4). Present-source
  captures follow the shown image, so they hold the twin's picture; `SR_GPU_TWIN_CAPTURE=authentic` shows and
  captures the authentic image instead.
- **What the guest can observe (section 11.1).** Each of the four items is unchanged:
  - VRAM bytes and other guest memory: unchanged, byte for byte (twin-scene and twin-soft, above).
  - Readbacks: unchanged. GE and display readbacks, block transfers into RAM and the committed frame read the
    authentic image, as with the twin off (twin-scene).
  - Timing the guest can measure: unchanged. The guest timing model (VBLANK schedule, GE timing, media clocks) does
    not follow host GPU work (section 10.3). The second draw of each render op, and the drain when a twin allocation
    fails, are host waits.
  - I/O speed: unchanged. The twin performs no guest I/O.
- **Default.** Off. The player's part "Full 32-bit colour" (the launch line's `twin=on`) or `SR_GPU_TWIN_COLOR`
  turns it on (see Setting). Turning it on by default needs section 11.2 item 4's "nothing worse" runs: picture-gate
  or route runs on *Hot Shots Tennis* and *LittleBigPlanet*, and a library sweep, each with the twin on against the
  same run with it off (#2050). The launch line carries its key (`twin`, below), which follows section 11.2 item 2:
  a library switch and a per-title switch, each off by default.
- **Setting.** The launch line's `twin=off|on` (section 10.1), a required key: `twin=on` asks for full colour. The
  player writes it from the part "Full 32-bit colour" (`"twin"`, `on` or `off`) on the library's Settings > Enhanced
  page and on each title's own page (sections 10.5 and 10.9); it is off unless the player turns it on.
  `SR_GPU_TWIN_COLOR=1` (full colour) or `native` (the twin with the authentic colour, which the identity self-tests
  use), read when the renderer starts (the software renderer: at its first colour write), stays as the developer
  override and wins whenever it is set: unset or empty leaves the choice to the launch line, `0` and `off` are off,
  and any other value prints `TWIN: refused (...)` and leaves it off. When the variable turns off a twin the line
  asked for, the run names `twin` unavailable ("SR_GPU_TWIN_COLOR overrides it") in the report. `gpu-coherence-selftest`
  twin-key turns the twin on through `twin=on` alone, in both renderers, and checks the report, with the variable
  unset and set. A twin that the line or the variable turned on is reported on (`effective.twin` true, and in the
  pause caption), so `SR_GPU_TWIN_COLOR=1` with the key off reports it too; `twin-key --twin-key off --twin 1` checks that. The twin takes effect only in Ace Mode: in an
  authentic run it stays off whatever the environment says, and a request prints one `TWIN: off (the host twin
  needs Ace Mode)` line. When the key asked for it and it does not come up (the `TWIN: unavailable` and `TWIN: off
  for the rest of the run` cases below), the run names `twin` unavailable on stderr and in the report; a twin that
  ran is confirmed, so a run where the renderer never started it is reported as not run (section 10.4).
  Supersampling (section 10.21) also runs the twin, with the authentic colour unless the twin was asked
  for in full colour; a twin that runs only for supersampling is not reported (`effective.twin` stays false,
  and `twin` is not named unavailable); `twin-key` with `--ssaa 2` checks that.
- **Cost.** Each render op is drawn twice at the render scale, and the twins use as much GPU memory as the targets.
  In the software renderer each colour write is computed twice, and the slots take 9 MiB of host memory.
- **Fail-closed.** A GPU without nearest blits of RGBA8 and D16 prints `TWIN: unavailable (...)` and runs without it.
  If any twin allocation fails during the run, one `TWIN: off for the rest of the run (...)` line is printed, every
  twin is freed, and the run continues exactly as a twin-off run at the user's scale (`twin-alloc-fail`). A software
  renderer that cannot allocate its slots prints `TWIN: unavailable (the software renderer could not allocate its
  twin)` and runs without it.
- **Stamps.** `TWIN: on (colour=full|native, scale=<n>x)` at start, and `TWIN: exit (ops=, copies=, reseeds=,
  authentic_presents=, alloc_failures=)` at exit; the software renderer's are `TWIN: on (software renderer,
  colour=full|native)` and `TWIN: exit (software renderer, pixels=, units_copied=)`. Section 10.4 applies: the run
  is an Ace run, refused as evidence.

### 10.12. Texture Replacement

A user-supplied texture pack replaces guest textures in the Vulkan GE renderer (#1880), the way PPSSPP's texture
packs do. It is guest-visible (section 11.1) and not admitted under section 11.2: section 11.3.4 is its entry, and
this section says how it draws. The code is `src/rt/texrepl.c` (the pack reader and the hashes: no GPU, no guest
memory, no network; ZIP packs through the project's own `src/core/nk_zip.c` and its inflater
`src/core/nk_psp_inflate.c`) and its hook in `ge_gpu.c`'s scaled-copy path (`tex_xs_offer`, section 10.10).

- **Settings.** `SR_TEX_REPLACE=<folder>` names the pack and `SR_TEX_REPLACE_BUDGET_MB` (1 to 8192, default 256) bounds
  the host memory its decoded images may hold, both read when the renderer starts. It takes effect only in Ace Mode:
  in an authentic run the pack is not read whatever the environment says, and a request prints one
  `TEX_REPLACE: off (texture replacement needs Ace Mode)` line. A pack whose folder, ZIP or `textures.ini` cannot
  be read or is malformed (a line that is no comment, section or `key = value`, no `hash` type, an unknown one, a
  `textures.ini` over 4 MiB or holding a NUL byte, which no text file holds and which would hide the text after
  it, a damaged or unsafe ZIP) prints
  `TEX_REPLACE: refused (<reason>); the game runs without the pack` and is refused whole; the reason names the pack
  by its last path component only, and every control character in a reason or note (C0, DEL and the C1 range,
  raw or in UTF-8), which a pack could use to send its own escape sequences to the terminal, is printed as `?`;
  the run goes on unreplaced without waiting on the pack (a stored pack that
  no longer reads, its folder gone or its ini broken, is refused the same way at the next start). One image that
  cannot be used (missing, gone since the pack opened, not a regular file, outside the pack folder, over 64 MiB, a
  bad PNG, the wrong size for its level, over the budget) is skipped: its texture is drawn as the game made it, a
  note says why, and the exit line's `skipped=` counts it once. That is the whole rule: the pack is used whole or not
  at all only for what is read at open (the folder or ZIP listing and `textures.ini`); images are read later, one
  at a time, and each fails alone. There is no launch-line key, library setting or per-title setting yet; they come
  with the launch-line pull request that follows #2074, and only that pull request admits the feature
  (section 11.3.4).
- **The pack.** The project ships none and never downloads one: it is a folder or a ZIP file the user made or took
  from a community, kept locally. `SR_TEX_REPLACE` names either.
  - *A folder* is read as it is, unless it holds a `textures.zip`: then that ZIP is the pack and the folder's own
    files are not read, as in PPSSPP (a `TEX_REPLACE: note:` line says so).
  - *A ZIP file* (or that `textures.zip`) is read in place: nothing is extracted or written. Its root is the
    archive's top when `textures.ini` is there; otherwise, when every entry sits in one top-level folder (entries
    starting with `.` and the `__MACOSX` folder macOS adds are not counted) and `textures.ini` is in that folder,
    that folder is the root, the way packs are often zipped with their folder. A ZIP with no `textures.ini` in
    either place is refused, as PPSSPP refuses a ZIP pack without one. Names in a ZIP are matched with case
    ignored, as PPSSPP's ZIP lookups ignore it. The archive's whole structure is checked when the pack opens, by
    `nk_zip.c`'s rules (APPNOTE layout, every name safe, no two names equal when case is ignored, no ZIP64, no
    encryption); stored and DEFLATE entries are read, with data descriptors. A damaged structure or an unsafe name
    refuses the pack whole with the reader's code (`TEX_REPLACE: refused (texture pack ZIP refused (ZIP_...)...)`).
    Each file's CRC-32 is checked when it is first read: a damaged image is noted and that texture drawn as the
    game made it, like a damaged PNG in a folder.
  - *`textures.ini`* (optional in a folder; a folder with none reads as `quick`). The line rules are PPSSPP's:
    `;` or `#` starts a comment line, `#` starts a comment after a value unless the value is in double quotes,
    `\#` is a `#`, the blanks around a key or value and one pair of quotes around a value are removed, a section
    header starts its line and any text after its `]` is a comment, and only the first section of each name is
    read (a later one is noted). `[options]` takes `hash = quick | xxh32 | xxh64` (required), `ignoreAddress`,
    `reduceHash`, `skipLastDXT1Blocks128x64`, `skipLastDXT1Blocks128x128`, `ignoreMipmap` (which PPSSPP uses only
    when dumping) and `version`; the first line of each counts, and a value that is not a bool or a number leaves
    the option at its default, as PPSSPP's reader does, with a note. `[hashranges]` (`addr,w,h = newW,newH`; a
    range larger than its texture or empty is not read) and `[reducehashranges]` (`w,h = fraction`; 0 or a value
    that is not a finite number is not read) are read as PPSSPP reads them, a later line for the same texture
    replacing an earlier one. The lines are sorted once at open and looked up by binary search, so any number the
    4 MiB `textures.ini` holds is read.
  - *`[hashes]` keys* are read as PPSSPP reads them, with the C library's `sscanf("%16llx%8x_%d")` and at least
    its first field matched: up to 16 hex digits of key, up to 8 of data hash, then `_<level>`; a field not
    matched is 0. So every form PPSSPP takes is taken: the full `<16><8>` name, `<16><8>_<level>`, a 16-digit
    key alone (any data hash), a shorter key, a name with blanks between the two fields, a 25-digit name (the
    25th digit is no level). A level past an `int`'s range, where `sscanf`'s `%d` is undefined, is read as the
    largest or smallest `int` (so `_99999999999` is level 2,147,483,647, a chain no image reaches). A key whose
    part is zero is a wildcard (see **Identity**). A value is a file under
    the pack's root; an empty value marks a texture to leave alone; a value that leaves the root is not read.
  - *Hash-named files* in the root need no ini line: a name of 24 characters, or 26 to 27 with `_` as the 25th,
    before `.png`, `.dds`, `.ktx2` or `.zim`, read with the same `sscanf`. DDS, KTX2 and ZIM files are listed
    (so a texture they name matches and no wildcard is tried for it) but not decoded: one note counts them, and
    the texture is drawn as the game made it. A `[hashes]` line for the same key and level replaces a file.
  - *Mip levels.* The lines and files of one key make its mip chain as PPSSPP's alias map does: the last entry
    for each level counts; levels are taken from 0 up while they follow one another (a gap ends the chain, with
    a note); a value holding `|` names several levels, since PPSSPP joins and splits the names with `|`; a blank
    name ends the chain; at most 12 levels. A key whose entries do not start at level 0 (a negative level sorts
    first), or whose level 0 is blank, has no chain: the texture is left alone. A level counts only when every
    level below it is a readable PNG and level *n* is level 0's size halved *n* times (at least 1), as PPSSPP
    loads a chain. `nk_texrepl_lookup_level` gives each level to the renderer; the renderer draws level 0 only.
  - *Reported, not applied:* `[filtering]` (the game's own filtering is kept), `[games]` (per-game ini
    overrides), `saveIndexed`, and any key or section this reader does not know, one `TEX_REPLACE: note:` line
    each. A texture `[indexed]` marks (`true`, `1` or `gpu`, looked up with the same wildcards) is not
    replaced: its image holds palette indices for the game's palette to colour, which this build does not draw.
- **Identity.** A texture is identified as in PPSSPP: its address (30 bits) and size code in the high half of the
  key, its quick (or xxh) hash of the guest bytes in the low half, and for a palette format the palette's XXH32
  hash (seed 0xC0108888) folded into the key together with the CLUTFORMAT command word, opcode byte 0xC5
  included. The bytes hashed are PPSSPP's: the last CLUT load's size plus the format's index start position,
  capped at the largest load seen. A run in which the game has loaded no palette yet hashes the 16 or 256
  entries the texture can index, which a replayed capture is the only case of. Lookup tries, in PPSSPP's order,
  the exact key, the palette alone, the key with no data hash, the data hash with no address, the address and
  data hash without the palette, the address alone, and last the data hash alone; with `ignoreAddress` the
  address is zeroed first and the steps that keep it are skipped.
- **Which textures.** One level (no mip chain), not a render target, whose guest bytes the renderer can read
  directly. The image must be a whole factor from 2 to 6 of the texture, the same on both axes, and no side may
  pass 2048 texels; any other image is counted (`bad_factor`) and the texture is drawn as the game made it. Only PNG
  is decoded, and the renderer draws only level 0.
- **Where an emulator pack cannot match, and why.** Each of these was checked in this build's code; a pack that
  depends on one replaces less here than in PPSSPP, or is refused.
  - *512-high textures.* PPSSPP hashes a texture 512 texels high only down to the tallest row the game has drawn
    from it (`maxSeenV`) when no `[hashranges]` line covers it; the renderer does not track that row and passes 0,
    so such a texture is hashed whole and its pack name does not match unless a `[hashranges]` line gives the
    height.
  - *`[hashranges]` images.* The range changes the hash as in PPSSPP, but the image must still be a whole factor of
    the whole texture (`nk_texrepl_factor` on the texture's own size): an image drawn for the range only, which
    PPSSPP stretches over the range, is counted `bad_factor` and not drawn.
  - *Image size.* PPSSPP draws an image of any size; here it must be 2 to 6 times the texture on both axes and at
    most 2048 texels a side. Same-size images (factor 1), larger factors and uneven ones are not drawn.
  - *Mipmapped textures.* A guest texture with more than one level is never replaced, even by a pack that lists
    its levels: the reader gives the levels, but the renderer does not use them yet.
  - *Image formats.* DDS, KTX2 and ZIM files match but are not decoded, so their textures are drawn as the game
    made them; PPSSPP uploads them.
  - *`[indexed]` and `[filtering]`.* An indexed texture is not replaced (above); a `[filtering]` line's filter
    is not applied.
  - *`[games]` overrides.* A pack that keeps per-game settings in a second ini named by `[games]` is read with its
    main `textures.ini` only.
  - *The address in the key.* Without `ignoreAddress` (which needs `xxh32` or `xxh64`), a pack names textures
    by the guest address they were loaded at in the run it was made from. They match here only when the game
    loads them at the same address, so on the same disc version and with the same memory layout; this project's
    HLE allocator has not been checked against PPSSPP's placement, so a pack made in PPSSPP without
    `ignoreAddress` may miss textures here that PPSSPP finds.
  - *Render targets and unreadable sources.* A texture the game renders to, or whose bytes the renderer cannot
    read directly (`tex_source_ptr`), is never hashed or replaced.
  - *Case in a folder.* PPSSPP's folder reader ignores case on every system; this reader opens a folder pack's
    files by their exact name, so on a case-sensitive file system (Linux, Android) a `[hashes]` value whose case
    differs from the file's does not load. ZIP packs ignore case everywhere.
  - *Refused here, loaded there.* Lines PPSSPP keeps as comments refuse the pack here: a line with no `=` or
    nothing before it, a `#` before the `=`, a line before any section, an indented section header, a `[` with no `]`. A
    `textures.zip` that is not a valid ZIP refuses the pack (PPSSPP falls back to the folder). A ZIP with ZIP64
    (more than 65,534 entries or 4 GiB), encryption, a method other than stored or DEFLATE, or a name
    `nk_zip.c` refuses (a Windows-reserved name or character, a `.` or `..` folder, two names equal when case is
    ignored) is refused; PPSSPP's ZIP library may read it. So is a DEFLATE entry, `textures.ini` included, packed
    in more than its declared size plus an eighth plus 64 bytes: no archiver we measured (Python `zipfile`, 7-Zip,
    Windows `tar.exe` and `Compress-Archive`, at every level) comes near that (stored blocks cost 5 bytes in
    65,535), so the rest is padding a reader would have to work through. An encoder that flushes after every few
    bytes can pass it, and such an entry is refused. A `textures.ini` over
    4 MiB refuses the pack. Images
    PPSSPP may still read are skipped here: a folder file that is a link or junction leading out of the pack, a
    device or FIFO, a name the ZIP reader refuses (`CON.png`, `CON .png`, `a.png.`), a file over 64 MiB, a PNG with
    any chunk's CRC wrong (libpng, which PPSSPP uses, by default only warns about a bad CRC in an ancillary chunk),
    a PNG whose image data unpacks to more or less than its `IHDR` implies or whose Adler-32 is wrong (libpng
    ignores extra data and, by default, a bad Adler-32), bytes after the Adler-32 inside the image data, and a grey
    or RGB `tRNS` naming a sample the image's depth cannot hold. A second `PLTE` is refused and a `PLTE` in a grey
    image ignored, as libpng does. A palette longer than its depth can index (more than 16 entries in a 4-bit image)
    is kept whole, as stb_image keeps it, and so is a `tRNS` of that length, whose alpha values then apply to the
    entries the pixels name; libpng cuts such a palette to the entries the depth can index and drops that `tRNS`
    whole, so the same image is opaque there.
  - *The skip-last-DXT1 options* (`skipLastDXT1Blocks128x64`, `skipLastDXT1Blocks128x128`) are applied as
    PPSSPP applies them, so they are not a difference.
- **How it draws.** The image is stored as the entry's scaled copy: an image `factor` times the guest texture,
  sampled on its own texel lattice (section 10.10, "Sampling"). It is finished work, so no worker is involved, and
  it is normally stored in the draw that finds it. Storing goes through the collect step scaled copies use, which
  takes the oldest finished job first and stops at the per-present upload cap (section 10.10), so with xBRZ also on,
  or with several large images first drawn in one scene, a later image waits for a later present; until it is
  stored the texture is drawn as the game made it, never stale. A replacement is tried first; xBRZ scales a texture the
  pack leaves alone. A draw that maps one texel to one pixel keeps the replacement: the rule that binds the
  unscaled texture for xBRZ copies never applies to a pack's image, which the pack's author drew.
- **Never stale.** The copy records the cache entry's upload and the content hash it was made from, as scaled copies
  do. A texture the guest rewrites is looked up again: its new content is replaced by the image the pack lists for
  it, or scaled or drawn as made, and the old image is never drawn. Replaced content that returns is replaced again.
- **Budget.** Decoded images stay in host memory up to `SR_TEX_REPLACE_BUDGET_MB`, the least recently used dropped
  first (taken from the tail of a list, so making room never scans the images) and decoded again when next
  needed; an image the budget cannot hold is counted (`over_budget`) and its texture drawn as made. An image
  dropped and wanted again is decoded again at once the first time; after its kth decode (k from 2) its next
  2^(k-1) - 1 lookups, at most 65,535, draw the texture as made and are counted (`deferred`), and a note says it
  was evicted and decoded again. Two images taking turns in a budget for one thus cost about 2 log2 n decodes
  over n lookups, not n. Each file is held once, however many keys or mip levels name it. While every held image
  is in use, an image whose size is known and does not fit is not read again until one is released; one larger
  than the whole budget is never read again. The copies share the GPU budget of `SR_TEX_SCALE_BUDGET_MB` (512 MiB by default) and its
  reclamation (section 10.10).
- **Guest memory and files.** Texture bytes are read through the same host view the cache's content hash reads;
  nothing is written to guest memory, the CLUT or a texture source. Files are read only from inside the pack's root,
  the folder or the ZIP (a name or `[hashes]` value that leaves it is refused); a ZIP is never extracted, a
  DEFLATE entry being unpacked in memory straight into a buffer of its declared size; nothing is uploaded and no network
  connection is made. A folder's files are checked on the handle opened, not the name: one is read only when it is
  a regular disk file (Windows: `GetFileType` is `FILE_TYPE_DISK` and it is no folder; POSIX: `fstat` says
  `S_ISREG`, the file opened without blocking so a FIFO is refused at once) whose final path, links and junctions
  resolved (`GetFinalPathNameByHandleW`; `/proc/self/fd` or `realpath`), is inside the folder's own final path.
  On Windows every file is opened with `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`, so a link to a named
  pipe gives the pipe's server no more than the user's identity, never the right to act as the user, before the
  handle check refuses it. A name the ZIP reader refuses (a device name such as `CON.png`, `COM1.png` or `CON .png`, blanks before the
  extension dropped as Windows drops them; a trailing dot or space; a character Windows refuses) is not opened at
  all. That rule is a first line only; the handle check above is the guard. Reads are capped: `textures.ini` at 4 MiB, an image at 64 MiB,
  in a folder or a ZIP alike; a ZIP image's data is unpacked only up to the size it declares, and a ZIP whose
  entries declare more than 8 GiB unpacked in all (`NK_TEXREPL_MAX_PACK_BYTES`) or hold more than 65,534 entries is
  refused whole. What a pack holds in memory is its index (about 80 bytes an entry, from the bench below) and the
  decoded images, which the budget bounds. A folder's `textures.zip`, and a ZIP named as the pack, is opened only when it is a
  regular file (the folder's inside the folder); the ZIP reader then opens it again by name, on every platform,
  so a file swapped in that moment is not caught by the handle check, though the ZIP's own checks still apply. A
  PNG is checked before any pixel memory is allocated: the signature, `IHDR` first with 1 to 2048 texels a side
  and a pixel format the PNG standard defines, every chunk whole with its CRC right, `PLTE` and `tRNS` where and as
  long as the format allows, the `IDAT` chunks one after another, and `IEND`; its pixels (width x height x 4 bytes)
  must fit the budget, and the image data, all `IDAT` chunks together, may be at most R + R/8 + 70 bytes, R being
  the size `IHDR` implies (each row's filter byte and bytes, each Adam7 pass's rows): stored blocks need R plus 5
  bytes for each 65,535, and an image whose data is longer is refused by name ("its image data is N bytes, more
  than R can need"), so a small image cannot carry megabytes of empty blocks for the reader to work through. The
  reader decodes PNG itself: the image data is unpacked by the project's inflater (`nk_psp_inflate.c`) straight
  into a buffer of exactly R bytes, and data that would unpack past it stops there and refuses the image, so a
  small PNG cannot make the reader allocate or work beyond what its `IHDR` declares; the Adler-32 must follow the
  stream's last block at once. The inflater builds the fixed Huffman tables once per stream, not once per block,
  and the ZIP reader's CRC-32 table is a constant. The memory one decode takes, at most the file (with a DEFLATE
  ZIP entry, its packed bytes too, while they are unpacked), then the joined image data and the R-byte buffer,
  then that buffer and the pixels, is recorded in the pack's statistics (`decode_peak`); for the largest image,
  2048 x 2048 at 16 bits a channel, R is 32 MiB and the joined data at most 36 MiB, so a 64 MiB file with its
  data in several `IDAT` chunks peaks at 132 MiB (one `IDAT` chunk is unpacked where it lies, with nothing
  joined). Reading such a file from a DEFLATE ZIP entry, whose packed size may be 9/8 of the file and 64 bytes
  more, peaks at 136 MiB and 64 bytes while it is unpacked, before the decode starts. Every DEFLATE stream the
  shared inflater unpacks (a PNG's, a ZIP entry's, a CSO block's, a module's gzip payload) may have at most 64
  dynamic Huffman blocks and one more for every 64 bytes it has written (`NK_INFLATE_DYNAMIC_FREE`,
  `src/core/nk_psp_inflate.h`), so the code tables it costs follow its output. 64 is half zlib's smallest block
  (127 symbols at memLevel 1), so no stream zlib writes without flushing can be refused; its smallest blocks
  and a flush every 513 bytes used at most 0.25 of it, measured. A PNG refused by it says so in its note. `nk_texrepl_decode_png` is the same check and decode, for a caller with bytes of its own.
  One file is read, checked, decoded, noted and counted against the budget once, however many `[hashes]` lines
  or names reach it: names are first grouped as written, and before a file's bytes are read its identity (the
  ZIP entry a name resolves to, case ignored; a folder file's volume and 128-bit file ID, or device and inode) is
  looked up among the files already read or found damaged, so another case, an 8.3 name or a link shares the
  first name's image, or its refusal. A file whose system gives no file ID has no identity and is read under each
  name, as distinct files would be. A file refused before it is read (over 64 MiB, not a regular file, outside
  the pack) is noted once for each name that reaches it, which costs no read. Identities are kept while the pack
  is open, so a file deleted and replaced meanwhile may share the old file's image under a name not yet read;
  the table, outside `index_bytes`, takes 36 bytes a slot and is kept at most half full: at least 64 slots, and
  under four for each file read or found damaged just after it doubles.
- **Sorting.** The index, the hash ranges and the ZIP reader's name and offset checks sort with one project heapsort
  (`nk_zip_sort`, at most about 2n log2 n comparisons whatever the order), not the C library's `qsort`, which
  some C libraries make quadratic for a crafted order.
- **Stamps.** `TEX_REPLACE: on (hash=, entries=, notes=, budget_mb=)` at start, followed by the line section 11.3.4
  describes and one `TEX_REPLACE: note:` line for each thing read and not applied (a file that cannot be read or decoded is
  noted at its first use, when it is found, not only counted), and at exit `TEX_REPLACE: exit
  (lookups=, hits=, misses=, left_alone=, queued=, bound_draws=, loaded=, load_failed=, skipped=, over_budget=, evicted=,
  bad_factor=, cache_peak_mb=, decode_ms=)`. Section 10.4 applies: the run is an Ace run.
- **Not yet.** Decoding a PNG (and unpacking it from a ZIP) runs on the renderer thread the first time an image is
  needed (a worker thread, as scaling has, and DDS or KTX2 files come after); the tallest row a game draws is not
  tracked, so a 512-high texture is hashed whole; the renderer's use of mip levels, same-size, larger or uneven
  images and a `[hashranges]` image drawn over its range, `[filtering]`, `[indexed]` images, `[games]` overrides,
  a dump switch and a player toggle or per-title pack folder are not done.
- **Tests.** `texrepl-selftest` checks the hashes against values taken from PPSSPP's own code and published xxHash
  vectors, the ini line and file-name rules, every `[hashes]` key form above, the mip chains (from ini lines and
  from files, gaps, `|`, a negative or missing level 0, a level of the wrong size), the lookup order with its last
  wildcard, palette identity (against PPSSPP's formula worked out separately, with a pack file named the way PPSSPP
  names it), `[indexed]`, malformed packs, path escapes and the budget, on synthetic packs; and ZIP packs written by
  the test: `textures.ini` at the top (DEFLATE-compressed by zlib itself) or in one top-level folder, stored,
  DEFLATE and data-descriptor entries, names in other case, a folder's `textures.zip`, a damaged image, and
  ini-less, two-folder, cut, `..`-named, malformed and bzip2 archives refused whole; a small PNG cut at every
  length and with every byte flipped (all refused), its image-data bytes flipped with the CRC made right (each
  decodes at its size or is refused), `IHDR` sizes of 0, 4096 and 2^31 and a 2048x2048 one over a 1 MiB cap, and
  bad images in a pack skipped, noted and counted once; device names, a trailing dot, a junction or link out of
  the pack (a file link in it still read), a FIFO image and a FIFO `textures.zip` (POSIX), a `textures.ini` over
  4 MiB, and a missing pack's message carrying no folder above it; every `IHDR` pixel format over the same data,
  and a seeded pass of 30,000 random mutations (1 to 8 bytes, CRCs made right) over three images, each decoded
  within its `IHDR` size or refused; images deleted after the pack opened, skipped at once; and, through
  `nk_texrepl_open`, every ZIP refusal: absolute, drive-letter, backslash, `..` and device names, duplicate names
  and names equal but for case, ZIP64, encryption, a local header that disagrees with the central directory,
  overlapping entries, a central directory outside the file, the archive cut at every length, entries declaring
  12 GiB, and an image declaring 100 MiB or unpacking past its declared size (skipped, the rest of the pack used).
  The selftest also writes PNGs of its own (stored and fixed-Huffman DEFLATE, one or several `IDAT` chunks, every
  filter, Adam7, palette and `tRNS`, 1- to 16-bit) and decodes each with this reader and with stb_image
  (`third_party/stb`, used only by the test), which must agree to the byte; image data longer than the image is
  refused, and 60,000 seeded mutations of those seeds must be refused by both or decoded alike. PNGs with their
  CRCs and Adler-32 right meet each of the decoder's own checks (a palette index past `PLTE`, filter type 5, a
  wrong Adler-32, a `PLTE` of 4 bytes or 257 entries, an RGB `tRNS` of 4 bytes, a second `PLTE`, a chunk between two `IDAT`s) and
  are refused for that reason, and a 4-bit palette image with 256 `PLTE` entries and a grey image with a `PLTE`
  decode as stb_image decodes them. A 1x1 PNG whose
  data unpacks to 64 MiB, listed under 81 wildcard keys, is skipped 81 times within the time bound, nothing cached,
  `decode_peak` within the file's size and 4 KiB, the file read and refused once (its data, over R + R/8 + 70
  bytes, refused before it is unpacked), and a 1024 x 1024 one unpacking past its size refused within 5 s; a ZIP
  image and a `textures.ini` behind 1 MiB of empty DEFLATE blocks are refused at open. The three shapes of the
  security review's slow decode each have a cell: a 1x1 PNG whose 64 MiB of data is all empty fixed-Huffman
  blocks, and one all empty dynamic blocks, refused within 2 s by the input bound; 5.6 million empty ancillary
  chunks before a good 1x1 image, decoded within 2 s (each chunk's CRC-32 from the constant table); 1 MiB of
  empty fixed blocks building the Huffman tables exactly twice, counted in the selftest's build
  (`NK_INFLATE_COUNT_BUILDS`), where tables built per block came to about 1.7 million; the Adler-32 one byte late refused;
  and the largest image the bound lets through, 2048 x 2048 at 16 bits a channel holding 36 MiB of empty dynamic
  blocks, refused within 2 s by the dynamic-block budget (77 ms measured, 3.1 to 3.4 s before it), and the same image with
  the most empty dynamic blocks the budget allows (64, then one after every 64 bytes it writes) decoded within 5 s
  (722 ms measured, 2.05 s under AddressSanitizer); 64 empty
  dynamic blocks unpack, a 65th is refused after 64 blocks' tables, and 2 KiB written first earns 32 more, not 33;
  400 KiB in dynamic blocks of 300 bytes, and in blocks of 513 bytes each followed by a flush's empty stored block,
  unpack. Two images taking turns in a budget for one, 1,000 rounds, are decoded 16 to 24
  times, every lookup either served or `deferred`; 40,000 held images make room for one 200 x 200 image within
  0.25 s, all 40,000 evicted; a DEFLATE ZIP image's `decode_peak` counts its packed bytes beside the file. A
  `textures.ini` key holding ESC, a UTF-8 CSI, a raw CSI byte and a cut UTF-8 sequence, and a refused line holding an OSC sequence, reach the notes and
  the refusal with `?` for each control character; a level of `_99999999999` is level 2,147,483,647; a NUL byte
  refuses the pack, naming its line; the sort is checked on every order up to 7 items, against its comparison bound on
  McIlroy's adversary and on the order that costs the C library's `qsort` most; then 16,384 `[hashes]` lines,
  16,384 `[indexed]` lines, 16,384 lines each of `[hashranges]` and `[reducehashranges]`, and a 16,384-entry ZIP (names and local headers) in that order open within a bound on
  the comparisons their sorts make, counted at the comparison functions in the selftest's build
  (`NK_SORT_COUNT_COMPARES`), so a sort that went back to `qsort` fails where `qsort` has such an order. A good
  image and a refused one, each listed under 81 keys, are read once (`reads`), the good one decoded and held once,
  and so is one file named 81 ways (case spellings in a ZIP and in a Windows folder, links in a POSIX folder), and
  a damaged ZIP entry named 81 ways is unpacked and refused once.
  With the budget full of pinned images a third is read once, not on each lookup, and served once they are
  released; a mip level whose image is the wrong size is not decoded, nor read again when that size is already
  known; a chain whose level 0 image proved unusable through another key serves none of its higher levels;
  `[hashranges]` lines are checked for the last line winning, a non-finite fraction noted, and 175,000 lines in
  reverse order (4.0 MB) read within 5 s and the comparison bound. These cells are fixed and seeded mutations, not coverage-guided fuzzing; the selftest also runs under
  AddressSanitizer and UndefinedBehaviorSanitizer on Linux. A file link needs Developer Mode on Windows, so that
  check is skipped there, and said, without it. `test_nk_zip` checks the
  reader's options: DEFLATE read only when asked, its level bits, damaged, short and long streams, a stream that
  stops at its declared size before a bad block, empty-block padding at and past its limit, data descriptors, device
  names with blanks before the extension, and the caps. `texrepl_selftest --bench N` measures an N-texture synthetic pack as a folder and as
  a ZIP (open, lookup and first-use times, index size and process memory). `gpu-texscale-selftest`'s `replace` cell runs the production path on a Vulkan device: a
  listed texture drawn from the pack's image from the draw that finds it, even with the 1:1 rule on, with
  `SR_TEX_REPLACE` alone and with xBRZ also on; a rewrite to unlisted
  content never drawn from the old image; the content returning replaced again; an image of no whole factor counted
  and not used; an authentic start reading no pack; and an Ace start whose stored pack no longer reads (its folder
  gone, its `textures.ini` malformed) refused, the renderer going on without it.

### 10.13. Sharp Game Text

At a render scale above 1x a game's text looks soft or blocky: it draws small rectangles cut from a glyph page that
holds 1x anti-aliased coverage, one texel per screen pixel, so every texel is stretched over several device pixels.
Texture scaling cannot help (section 10.10): it skips a 1:1 draw, and scaling a whole page carries a neighbouring cell
into a glyph's edge. This feature (#2110) rebuilds the rectangle a draw samples, and only that rectangle, at the
render scale. It needs no font outline and no hook in the game's code, so it works whichever code drew the page, a
font the user supplied from their own console included, and it works for *Hot Shots Tennis*, which draws through its
own font library, and for *LittleBigPlanet*, which draws from pre-built atlases. It is guest-visible (section 11.1)
and not yet admitted under section 11.2: section 11.3.5 is its entry, and this section says how it draws. The code is
`src/rt/gpu_sdl3vk/glyph_recon.c` (the setting, the refusals, the rebuild and its check: no GPU, no guest memory) and
`glyph_gpu.inc`, included by `ge_gpu.c`.

- **Settings.** The launch line's `glyphs` key (section 10.1): `glyphs=on`, or `glyphs=off` or no key for off. It
  is optional and an Enhanced part, so an Enhanced or an Ace line may carry it; any other value refuses the whole
  line (`ACE_MODE: refused (bad value)`), and the run is Authentic. The renderer takes it once when it starts
  (`nk_glyph_init`). An authentic run has no valid line with Ace on, so nothing can turn the feature on there,
  and the unit's own Ace gate keeps it off even if asked (the `glyph-recon-selftest` settings cell). It needs a render scale of 2 to 4, the
  factor the rebuild is made at (4 is the renderer's largest scale; the rebuild itself is tested up to 6); at 1x
  there is nothing to sharpen, a run prints `GLYPHS: idle (render scale <n> leaves nothing to sharpen; it takes 2
  to 4)`, and the report names `glyphs` unavailable with that reason. A run on the software GE, which has no
  rebuild, names it unavailable at exit (`did not run: its renderer or presenter did not start`). It is not the
  launch line's `text` key, which only sets the on-screen keyboard to the window's resolution; neither reads or
  implies the other. It has no library or per-title setting yet; section 11.3.5 says what admission needs. The
  `SR_GLYPHS` environment variable that switched it before the key is retired: set to any value, the run refuses to
  start with one line that names `glyphs=on` and exits 1, as an invalid `SR_NOVBPACE` does, so a script that set it
  is told rather than silently losing the part.
- **Which draws.** A sprite primitive that maps one texel to one pixel (the smart 2D test of section 10.10, either
  direction) over a rectangle of whole texels, each side 4 to 64 texels, inside a cached texture that has one level,
  is not a render target and lies in main RAM, in 4444, 8888 or an indexed 4-bit or 8-bit format. Text drawn as
  triangles, magnified or minified, from a page in VRAM, with a mip chain, or in 5650, 5551 or a block-compressed
  format keeps the game's own picture, byte for byte. The refusals are named: `vram`, `format`, `mip`, `scale`,
  `size`, `empty`, `colour`, `hard_edge`, `downsample` and `memory`, and two that guard callers other than the GE
  path, which has no such formats: `reversed` (a 4-bit buffer with its nibbles swapped, as sceFont keeps one) and
  `rgb24` (a 24-bit buffer).
- **A page that looks like text.** Every inked texel (alpha above 0) of the rectangle has the same colour, so the
  alpha channel is coverage and the colour comes from the draw; at least two texels are partly covered (alpha neither
  0 nor 255) and at least one in eight of the inked texels is. A rectangle with two inked colours is art, and a
  hard-edged bitmap font has no coverage to refine; both are drawn as the game drew them. A single-colour soft shape
  (a shadow, a glow, a fade bar) passes this test; whether the rebuild treats a rectangle as soft art is decided
  below (next bullet).
- **The rebuild.** For a rectangle of coverage `c` at factor `f`: the coverage is enlarged with a Catmull-Rom bicubic,
  replicating the rectangle's own edge texels past its border, so a neighbouring cell of the page is never read. Then
  for every texel a threshold is fitted (a bisection) so that the `f x f` block of pixels, each set to
  `clamp(0.5 + f * (field - threshold), 0, 1)`, averages to that texel's coverage. The ramp is about one device
  pixel wide at any factor, so the edge is sharp and still anti-aliased at the render resolution, and every block
  keeps the guest's coverage by construction. That applies where the coverage steps: the largest difference between
  two neighbouring texels in the 3 x 3 around a texel decides how much of its block is a ramp (nothing below 0.35,
  all of it above 0.55, a mix between). An edge that falls inside one texel leaves a step of at least 0.5 to one side,
  and a gradient whose steps stay under 0.35 is at least three texels wide; the two numbers are a choice by those
  cases, not a tuned optimum. Elsewhere the block is the bicubic field itself, shifted so its mean is the texel's
  coverage. A ramp that steep over a slow gradient would start over at every block and rib it. A soft ramp more
  than a texel wide has steps of 0.5 and more, and its texels have the same neighbourhoods as a slanted edge's, so
  the step cannot tell them apart; the rectangle decides. Its partly covered texels (alpha 16/256 to 240/256 of the
  rectangle's highest alpha) are counted and divided by its total variation of coverage, in steps from blank to that
  alpha. An edge box filtered by the texel grid that reaches solid ink crosses as many texels as it changes steps, at
  any angle (ratio 1), and a ramp w texels wide crosses w times as many. A stroke about a texel wide that never reaches
  the rectangle's solid level has two partly covered texels over a variation of about twice its peak, so its ratio is
  about 1 over its peak. Box-filtered synthetic text measured 0.54 to 1.44, a 6-texel E 1.71, filtered text up to 2.1,
  and the soft art the rule has to catch starts at 1.23 (a Gaussian disk, sigma 0.4 texels; a linear glow with a
  falloff of 1.5 texels is 1.30). The two populations overlap, and no single ratio separates them: 1.2 is a choice, and
  the soft art on both sides of it is within 0.05. Above 1.2 the whole rectangle is rebuilt smooth (the bicubic field
  shifted to keep each texel's coverage), whether it is a shadow, a glow or a fade, or text this measure takes for
  one. On a source that does not run back, a smooth rectangle has no run-back beyond the Catmull-Rom undershoot (at
  most 15 of 255 on the undithered soft art measured, 12 to 44 on a 4x4-dithered 4-bit shadow whose source runs
  back by up to 17). The cost to text, measured at 4x as the mean error against a supersampled truth (of 255, rebuild
  smooth against rebuilt as text): of 1,716 box-filtered synthetic variants 45 go smooth (an E from 2.89 to 3.37, a
  plus 3.05 to 3.26, a thin E 9.55 to 10.39; A, O and diagonal bars never do); a 6-texel E goes from 23.0 to 27.7
  (21 of 32 smooth) and an 8-texel E from 12.4 to 13.3; a dense grid of bars at 12 texels from 26.7 to 37.2 (29 of 36
  smooth) and at 16 texels from 18.9 to 24.7; text prefiltered by a tent or a Gaussian goes smooth in 1,104 of 3,432
  variants (+0.6 to +5.2); text with a same-colour drop shadow goes smooth in every variant measured (76 of 76, +1.0
  to +2.7). A rectangle that goes smooth is drawn as a smooth enlargement of the game's texels, not refused. Every
  pixel with any coverage takes the rectangle's one colour. The arithmetic is float in a fixed order, so a run is
  repeatable.
- **Checked.** Before a tile is used it is compared with the guest's bytes: each texel of the rectangle must equal
  the mean of its block of the quantized tile within 2 of 255, and every inked pixel must have the rectangle's colour
  (`nk_glyph_verify`). A tile that fails is refused (`downsample`) and its draw is as the game drew it. The check
  does not look at the shape of the pixels: a tile that keeps every mean and the colour passes it, a ribbed or
  mirrored one included. No test of the pixels separates the kink where two edge blocks meet in a glyph from a rib in
  a ramp (a seam check at 24 of 255 refused half the plain synthetic E at 3x), so the only guard against ribs is the
  soft-art rule of the rebuild and its tests, and that rule has the limits listed below.
- **Reading.** The page is decoded from main RAM by the decoder the texture cache uses; a page in VRAM is refused
  before any decode, so the feature never causes a renderer readback. Nothing is written to guest memory, the CLUT or
  a texture source.
- **How it draws.** The first rectangle of a page that passes every check makes a host-only image beside the cache
  entry's own, the page's size times the render scale (a side times the scale may not pass 2048 texels; a larger page
  is drawn as the game made it), transparent, and each tile goes to its own place in it. A page whose rectangles are
  all refused makes no image, takes no descriptor set and does no synchronous clear; it keeps a table of
  tiles (about 28 KiB of host memory) recording its refusals. The texture descriptor pool has a set for the image (one more per entry when glyphs are on). It shares the
  GPU budget of `SR_TEX_SCALE_BUDGET_MB` (512 MiB by default); a page that does not fit is counted (`budget_short`) and drawn as
  made. The draw binds that image with the scaled-copy flag, so the shader samples it on its own texel lattice
  (section 10.10, "Sampling"): at render scale `f` one rebuilt texel lies under each device pixel centre. The draw
  samples nearest, whatever filter the game set: each pixel centre is a texel centre, and the shader's linear weight
  (snapped down to 1/16) would leave a pixel up to 1/16 of a neighbour off at a factor that is not a power of two.
  A draw uses rebuilt texels only when its rectangle's tile is valid; otherwise the draw is exactly the game's own picture.
  Rectangles that overlap are separate tiles, each a rebuild of the same texels, so the one drawn last may overwrite
  part of the other with an equally valid version.
- **Never stale, never waited on.** A tile belongs to the cache entry's content generation it was checked against.
  The cache hands out a new generation, never reused by another entry, each time it decodes and uploads an entry's
  texels; its content hash samples part of a page (16 bytes in every `size / 64`), so a rewrite can leave the hash
  unchanged, and the cache finds such a rewrite through its shadow copy. When the guest rewrites the page the entry's
  generation changes; the next draw decodes the page once (every other new rectangle of
  that content reuses the decode), compares each tile's texels
  with the ones it was built from, keeps the tiles whose texels are unchanged and rebuilds the rest when they are
  drawn, so the old tile is never drawn for new content. The rebuild runs on the renderer thread, not on a worker, so
  the same draws rebuild the same tiles whatever the host's speed. The work is bounded by a count: at most 48
  rectangles are checked or rebuilt per present, and a draw past the count is drawn as the game made it and offered
  again by the next draw. No time budget exists.
- **Stamps.** `ACE_MODE: glyphs on (...)` after the `ACE_MODE: on` line, `GLYPHS: on (per_present=48,
  tolerance=2)` when the renderer starts, followed by the line section 11.3.5 describes, the report's `glyphs`
  (`true`, or `false` with its reason under `unavailable`) and the feature words' `sharp game text`, and
  at exit `GLYPHS: draws (bound=, tiles_built=, tiles_kept=, tiles_stale=, page_decodes=, images=, capped=,
  budget_short=, tiles_full=)` and `GLYPHS: exit (refused: <name>=<count>, ...)`. Section 10.4 applies: the run is an Ace run.
- **Limits.** One page holds at most 768 tiles; a draw past that is drawn as made (`tiles_full`). Text drawn as
  triangles is not rebuilt. A stroke about one texel wide that straddles two texels (a common width in small fonts)
  is sharpened less than one that falls in a single texel: on synthetic strokes the mean error against a supersampled
  truth at 4x is 4.9 of 255 for a 1.0-texel stroke over nine offsets (the tenth is a hard edge and is refused), and
  0.9 to 2.1 for strokes of 1.2 texels and more. Small, dense or filtered text can be drawn smooth by the soft-art
  rule (the figures are in "The rebuild"). Soft art whose rectangle ratio is 1.2 or less is rebuilt as text and can show
  doubled edges: a 2-texel fade with a 2-level 4-bit dither (ratio 1.14) runs back by 237 of 255 at 4x (a 1-level
  dither, the common 4444 case, is clean), a glow with a 1.25-texel falloff (1.02 to 1.18) by 64 to 69, and a soft shape
  sharing a rectangle with text (an H and a glow, 1.02 to 1.12) by up to 100. A crisp anti-aliased box or disk at a
  sub-texel offset also runs back at its corners (19, 49, 81 and 159 of 255 for a box at factors 2, 3, 4 and 6), in
  the edge rebuild and not only in soft art. The feature is on the launch line and not on the settings pages. It is off unless the
  line carries `glyphs=on`; turning it on by default or putting it in a preset needs the owner's decision, to which these costs apply.
- **Tests.** `glyph-recon-selftest` checks the setting and its Ace gate, every named refusal, the classifier, and the
  rebuild at every factor for 4444, 8888, indexed 4-bit and indexed 8-bit pages (block means, colour, repeatability,
  fewer soft pixels than a bilinear enlargement, no dependence on neighbouring cells), the downsample check
  against tampered and stale tiles, and soft single-colour art at every factor, whose rows and columns must not
  run back where the source does not: fade bars of 64, 6, 4 and 2 texels, glows of 1.5, 2, 3 and 5 texels, a blurred
  drop shadow, a round shadow, and in two dimensions box shadows and disk glows with a falloff of 1.5 to 3 texels at
  two sub-texel offsets; a bold "A" at 36 stroke widths and offsets and a thick-stroke plus at ink 237 that must be rebuilt, and an
  "E" at four stroke widths, sixteen offsets and ink 255 and 237 (256 draws at factors 3 and 4) of which at least 230
  must be twice as sharp as a bilinear enlargement (the rule's measured 240; a threshold of 1.0 gives 152), all on
  synthetic glyphs. `glyph-recon-mutant` builds the selftest thirteen times, each without one guard (the Ace gate, VRAM,
  render scale 1, the mip chain, the two buffer formats, the downsample check, the colour check, the hard-edge check,
  the size check, the smooth blocks, the soft-rectangle rule, a soft-art threshold of 1.0, and a rebuild that fits no
  threshold), and passes only when each fails the cell that names it. `gpu-glyph-selftest` runs the production
  path on a Vulkan device at render scales 2, 3 and 4, and fails if the renderer runs at another scale than asked:
  a 1:1 sprite draws the rebuilt tile, device pixel for pixel, with and without other content beside
  the cell, and without the feature it does not; a page in VRAM, a hard-edged page, a two-colour page, a magnified
  sprite, a rectangle too small and one past the page draw exactly as without the feature and make no image; fade
  bars of one colour (shallow, steep, four texels and two) and a box shadow draw without a ribbed row or column; the descriptor pool holds a set
  for every image an entry can have; a rewritten page draws its new tile and never the old, also when the rewrite
  misses the bytes the cache's content hash samples, and an unchanged cell keeps its tile; the 48-per-present count; guest memory and
  the CLUT unchanged; the setting idle at 1x; and the key off byte for byte: one scene drawn in an authentic run, in an
  Ace run whose line has no `glyphs` key and in one with `glyphs=off` rebuilds and decodes nothing and has one picture
  hash (the device image and the frame the guest keeps), and with `glyphs=on` it draws rebuilt glyphs and another
  hash. `ace-selftest` checks the key: its values, that `glyphs=off` formats as the line without it, its place after
  `ssaa` and `peak` and before `cull`, the stamp's line, the feature words, and the report when the renderer confirms it, names
  it unavailable or never starts; and it starts Ace Mode as a run does with `SR_GLYPHS=on` and checks that the
  process stops with exit 1 and the line that names `glyphs=on`. `gpu-glyph-mutant` removes the content check of a bound tile, keys the tiles on the
  cache's sampled content hash instead of its generation, removes the per-present count, and the VRAM, hard-edge and
  colour guards, and builds a renderer that ignores the key, each of which must fail its own cell.

### 10.14. Finer Bezier and Spline Patches

The GE draws Bezier and spline patches by cutting them into triangles at the division the game sets with
`PATCHDIVISION` (#2053). Games keep it low, so curved surfaces show facets, the more so above 1x. This feature
multiplies that division on the host for every game, with no per-title data. It is guest-visible (section 11.1) and
alters the GE timing model's clocks under `SR_GE_TIMING=1`: section 11.3.7 is its entry and says what the guest can
observe; this section says how it draws. The code is `draw_patch` and `ge_patch_tess_init` in `src/rt/ge.c`, which
both renderers share, so the software renderer and the Vulkan renderer draw the same finer triangles.

- **Setting.** The `tess` key of the `SR_ACE` launch line (section 10.1): `tess=off`, or the factor `tess=2`,
  `tess=3` or `tess=4`, a required key like every key but `tier`. In the player it is the part "Finer patches"
  (`"tess"`: `2`, `3`, `4` or `off`) on the library's Settings > Enhanced page and on each game's own page
  (section 10.9). It is off in a fresh settings file, is in no graphics preset and is not turned on by choosing
  Enhanced or Ace: a player turns it on, for the library or for one game. It takes effect only in Ace Mode: an
  authentic run has no launch line, so its factor is never read, and a line with any other value is refused whole
  (`ACE_MODE: refused (bad value)`). The runtime reads the factor once when it starts, and there is no environment
  variable for it.
- **Only triangle patches.** A line or point patch (`PATCHPRIMITIVE` 1 or 2) keeps the PSP's division: a finer
  wireframe or point grid is a denser picture, not a smoother surface.
- **Cap.** While the finer grid would have more than 262144 vertices, F drops along a divisor chain (4, 2, 1 or
  3, 1), so every edge vertex of a lowered draw's grid lies on its neighbours' finer grid. The cap is per draw: where
  a lowered draw meets a neighbour drawn at the full factor, the neighbour's extra edge vertices lie on the curve and
  the lowered draw's edge between them is straight, which can leave a crack up to a pixel or so wide. The PSP's own
  cap does the same for neighbours whose divisions differ. Each lowered draw is counted, and the run's exit line
  reports the count.
- **Guest memory.** The vertex and index data in guest memory and the GE state the game reads back are unchanged.
  What a present commits to guest VRAM does change, which makes the feature guest-visible (section 11.3.7).
- **GE timing.** With the GE timing model off (the default) the extra triangles cost host time only. With
  `SR_GE_TIMING=1` the model charges a patch's control points and then the rows and spans of every triangle drawn,
  the finer ones included, so factor F on a division d costs exactly the GE time of the division d x F, which is not
  the time of the PSP's division d. On 8888 targets without depth test, for untextured spans and textures that run
  at fill rate, each added triangle adds a fixed row charge on spans under 48 pixels, so a small patch costs
  more and a patch whose wide spans the finer grid cuts below 48 pixels can cost less. Elsewhere (depth-tested,
  16-bit targets, most textured spans) a span is charged per pixel, and the cost changes only with the coverage
  of the finer surface, in either direction. List completion, `sceGeDrawSync`,
  `sceGeListSync` and the finish interrupt move with it (section 11.3.7).
- **Stamps.** `PATCH_TESS: on (factor=<F>, at most 262144 vertices a patch draw)` at start, followed by the line
  section 11.3.7 describes, and `PATCH_TESS: exit (quads=<n>, capped_draws=<n>)` when the run ends, through
  `sr_at_process_end()`. `ACE_MODE: on (...)` carries `tess=`, the flight recorder's `runtime.enhancements` carries
  the launch line, and the report's `effective` block has `"tess"`. Section 10.4 applies: the run is an Ace run.
- **Tests.** `ge-patch-tess-selftest` runs the production `ge.c` software renderer:
  - the factor the `tess` key carries (`ace-selftest` pins the key, its values and its Ace gate);
  - factor F on a division d against the PSP's own division d x F, for a Bezier and a four-patch spline surface with
    a colour per control point, at factors 2 to 4, and against division d to show it is finer;
  - F x F times the quads;
  - factor 1 against the default run;
  - two Bezier patches that share a curved edge, and a four-patch spline, with no background pixel between their
    outer edges at factors 1 to 4, and a control that draws the two halves at factors 4 and 1 and requires the check
    to find the crack;
  - line and point patches unchanged;
  - the vertex cap and its divisor chain;
  - a draw the PSP's own 65535-vertex cap lowers, which is lowered first and multiplied after;
  - with the GE timing model on, factor F on a division d costing exactly the GE clocks of the division d x F: more
    than d for a small patch, and different from d for a large one;
  - the exit line, with the quads drawn and the capped draws.

### 10.15. Fast Forward

A speed control for Enhanced and Ace runs, the way emulators offer one. It is not a part on the launch line: the game
window's keys turn it on, and it never runs in an Authentic run.

- **Keys.** `Tab` held, unless the input profile binds `Tab` to a PSP button, and `F9` to turn it on or off. Neither
  key counts while the on-screen keyboard has the keys. The scheduler applies a key's change at its next VBLANK
  (`src/rt/sched.c`, `sched_fast_forward_*` in `src/rt/recomp.h`); the pause stops fast forward at once, and a toggle
  still on starts it again at the first VBLANK after the pause. The pause menu's controls page lists both keys.
- **What runs fast.** The paced clock samples guest time from the host monotonic clock run at
  `SCHED_FAST_FORWARD_SPEED` (3) times its rate, re-anchored at each change so guest time is continuous. VBLANKs, timed
  waits and the RTC all follow that one clock, so the guest sees the events it always sees, in the same order and at
  the same guest times, only sooner in wall time. A host that cannot keep up skips VBLANK slots, as it does at normal
  speed.
- **What the guest sees differently.** Its clock runs ahead of wall time by twice the time fast forward was on (two
  extra seconds of guest time for each second at 3x). The host audio device is given no audio while it runs. The
  blocking audio outputs and the queue depth the guest reads come from the same guest-time model as under the
  deterministic clock, seeded with the host queue's depth when fast forward starts, so the game paces its sound as it
  would on a PSP. When it stops, the guest reads the host queue again, which drained while nothing was pushed.
- **The speed readout and the Ace report.** Their window is a second of wall time, so time at the fast-forward speed is
  never judged: the readout hides while fast forward runs, skipped slots are not counted, no second counts as PC
  LIMITED in the report, and the first full second at normal speed brings the readout back (`ace_note_fast_forward`
  in `src/rt/ace.h`). Smooth motion makes no in-betweens while it runs (section 10.6), so the report's
  `smooth_motion` block and in-between count hold only pictures made at normal speed.
- **Limit.** With **Frame pacing** set to no extra cap and vsync on, each present waits for the display, so fast
  forward runs no faster than the display's refresh allows: about 1x on a 60 Hz display. Under the default PSP
  timing, presents beyond the scanout rate are dropped and the guest never waits on them.
- **Never.** It is off in an Authentic run, under the deterministic clock (`SR_DETERMINISTIC_CLOCK`, and with it
  recordings and replays), on the turbo clock (`SR_NOVBPACE=1`) and during the pause. Until it first starts, the paced
  clock is the host clock exactly. Each start and stop prints one `FAST_FORWARD: on` or `FAST_FORWARD: off` line.
- **Tests.** `sched-selftest` pins the rate, the continuity, the VBLANK wait, the pause, and the refusals in an
  Authentic run, on the turbo clock and by the gate's deterministic term (a deterministic run has no paced clock, so
  its VBLANK returns where the turbo clock's does). `hle-thread-selftest` pins the audio: no host push from either
  output path, the guest queue seeded from the host's depth once per run and draining in guest time, and the host
  queue and pushes back after it stops. `ace-selftest` pins the readout and the report.

### 10.20. Higher-Precision Host Depth (`depth=32`)

The PSP's depth buffer holds 16 bits, and the Vulkan renderer keeps that format (`D16_UNORM`). Games set their near
and far planes for the PSP's screen, so surfaces that lie less than one 16-bit step apart, often far away, flicker
against each other (z-fighting), and the flicker is easier to see above 1x. This feature gives the Vulkan renderer a
32-bit float depth buffer for every game, with no per-title data (#2051). It is guest-visible (section 11.1):
section 11.3.12 is its entry and says what the guest can observe; this section says how it tests. The code is in
`src/rt/gpu_sdl3vk/ge_gpu.c` (`s_depth_fmt`, `depth_unorm`, `depth_texel`, `PipeKey.zbias`).

- **Setting.** The `depth` key of the `SR_ACE` launch line (section 10.1): `depth=32`, or `depth=off`, the same as no
  key. It is an Enhanced part and needs no `tier=ace`. The renderer reads it once when it starts; there is no
  environment variable for it. The player has no setting for it yet, so no launcher writes the key (section
  11.3.12).
- **The buffer.** The depth attachment is `D32_SFLOAT` on the scale the 16-bit buffer uses: a draw at 16-bit depth z
  stores z / 65535 (`psp.vert`, `psp_tnl.vert`), now without rounding it to a step. A device that cannot render to and
  copy that format keeps `D16_UNORM`, and one `ACE_MODE: depth unavailable (...)` line says so. The host twin (section
  10.11) keeps a depth twin of the same format, and Smooth motion's depth copy (section 10.6) is `R32_SFLOAT` instead
  of `R16_UNORM`, which its passes sample as the same 0 to 1 values.
- **How a test decides.** The fixed-function test compares the unrounded values. Rounding is monotonic, so where the
  two values round to different 16-bit steps the answer is the PSP's. Only where they round to the same step can it
  differ:
  - LESS and GREATER, and LEQUAL and GEQUAL with depth writes on: the nearer surface by its unrounded depth wins,
    where the PSP's test sees two equal values (its strict tests fail them, its or-equal tests pass them). This is
    what removes z-fighting.
  - LEQUAL and GEQUAL with depth writes off (decals, shadows and many effect passes): the fragment's depth leans
    toward passing by a depth bias of 64 units of the primitive's float exponent (`DEPTH32_TIE_BIAS`), at most a
    quarter of a 16-bit step. A surface drawn on the plane it lies on, from other vertices, then passes as it does at
    16 bits instead of failing on float rounding, and as the draw writes no depth, no stored value moves. The same
    lean also passes such a draw where its surface lies less than the bias behind the stored one, across a step
    boundary too, where the PSP fails it.
  - EQUAL and NOTEQUAL compare unrounded values. A pass drawn again from the same vertices (a second texture or a
    lighting pass) has the same depth and gets the PSP's answer. A different surface that rounds to the same step
    without lying exactly on the stored one fails EQUAL, where the PSP passes it.
  - A surface with depth writes on, drawn with LEQUAL or GEQUAL on the plane of another from other vertices, is
    decided by float rounding pixel by pixel where the PSP passes every pixel, so it can speckle. That, and the EQUAL
    case above, are why the feature is off by default.
  - Through-mode draws (2D overlays) and every line, 3D lines included, carry whole-step depths (a line's depth is
    cut to its step), while a 3D surface stores its depth with its fraction. Such a draw at the step a surface
    rounds to, with LEQUAL or GEQUAL, meets the cases above at each pixel: it fails where it lies on the losing side
    of the stored value by more than the lean, or by any amount when it writes depth, where the PSP passes it. A
    cut line can lie almost a whole step from the surface it was drawn on, so lines show this most.

  ALWAYS, NEVER and clear mode are unchanged, and so is the minz/maxz range test, which rounds the fragment's depth
  to a step in `psp.frag`, except that on a draw with the lean a device may give the shader the leaned depth, which
  can move a pixel within a quarter step of the range's edge to the other side.
- **What the guest reads.** Depth crosses to and from guest memory at 16 bits. An upload of guest depth stores step /
  65535, the value a draw at that step stores (`depth_unorm`), so a draw at that step passes EQUAL against it. A
  readback rounds each value to the nearest step (`depth_texel`), as the 16-bit attachment's conversion does. The
  step stored at a pixel is the same with the key on and off: a test whose answer the key changes compares two
  values of one step, and the value it keeps lies in that step either way, while a draw the lean passes writes no
  depth. So a depth readback, depth read as a texture through guest memory, and the software renderer when a draw
  falls back to it see the same 16-bit depth, except for a value near half way between two steps: Vulkan leaves the
  precision of a 16-bit attachment's conversion to the driver, so such a value may land in the other step with the
  key off (lavapipe stores 17/32 of a step as the lower step), while `depth_texel` rounds it to the nearest. The
  colour is what changes.
- **The software renderer** (`src/rt/ge.c`) keeps its 16-bit depth. It is the PSP's model and the renderer of the
  fallback paths, so a frame it draws z-fights as on the PSP; this feature is a property of the Vulkan renderer's host
  buffer.
- **Cost.** Each depth buffer takes 4 bytes a device pixel instead of 2: at 4x (2048 x 1088 device pixels) about 8.9
  MB instead of 4.5 MB, and as much again for its host twin. A depth readback or upload moves twice the bytes. A
  draw that writes no depth with LEQUAL or GEQUAL uses a pipeline with depth bias, so such a state can take one more
  pipeline.
- **Stamps.** `ACE_MODE: on (...)` is followed by `ACE_MODE: depth 32 (...)`; the renderer prints
  `HOST_DEPTH: on (D32_SFLOAT; ...)` when it makes the buffer; the report has `"depth": 32`, or 16 when the device
  refused it; the badge's words name "32-bit depth"; the flight recorder's `runtime.enhancements` keeps the whole
  line (its field holds `ACE_SPEC_MAX` characters; at 95 it cut an Enhanced line with `depth=32` to `depth=3`).
  Section 10.4 applies: the run is an Enhanced or Ace run.
- **Tests.** `ace-selftest` pins the key, its values, its place in the line, the stamp's line and the report.
  `gpu-coherence-selftest` runs `host-depth` with the key off and on (`--depth 32`) at scales 1 and 4:
  - z-fight: a surface drawn after a nearer one with GEQUAL and depth writes, behind it by one step closing to a
    small fraction of one (1/256 at the last device pixel at scale 1, 1/1024 at scale 4): with the key off the far surface shows where the two round to one step, which is the defect; with the
    key on the near surface shows at every pixel;
  - decal: a quad drawn on its base's plane from other vertices with GEQUAL, and with LEQUAL, and depth writes off,
    covers every pixel it spans, with the key on and off;
  - depth-equal: the base's own vertices drawn again with EQUAL cover every pixel; one step nearer, none;
  - upload: every 16-bit value written by the CPU reads back from the device unchanged, and a quad at a written
    step passes EQUAL against it while one a step away fails, from 0 to 65535;
  - readback: a scene's depth committed to guest VRAM hashes to the same value with the key on as the key-off run
    at the same scale (`--depth-ref`). The scene is a z-fight the key changes (the far surface shows at some
    pixels with the key off and at none with it on, which the test checks), drawn so that every pixel stores the
    near surface's step either way and that step is whole at each device pixel centre, away from the half steps
    where a 16-bit conversion's rounding is the driver's.

  The whole suite also runs once with `--depth 32`, and its cells that read the device's depth image read it at
  either format.

### 10.21. Supersampling (`ssaa`)

An Enhanced part (#2073), in the Vulkan GE renderer: each frame is drawn at 2 or 4 times the render scale on each
axis, and the presenter shows the average of the samples that fall in each pixel. It smooths the edges of
triangles, alpha-tested cut-outs and shader-made edges, which a higher render scale alone leaves stepped. It is a
presentation-only change (section 2.2), in the render-scale class: it changes the image the host shows, never
what the guest draws into, reads or waits for. The code is the comment at `s_ssaa` in
`src/rt/gpu_sdl3vk/ge_gpu.c`.

- **Setting.** Settings > Enhanced offers **Supersampling** (2x, 4x or Off), for the library or for one game
  (section 10.5), and the launch line carries it as `ssaa=2` or `ssaa=4` for an Enhanced or Ace launch with it
  on (section 10.1); a missing key or `ssaa=off` is off. It needs no `tier=ace`. It is off in a fresh settings
  file and in no graphics preset, because of its cost (below): a preset leaves it as it is. It has no other switch.
- **How.** It runs the host twin (section 10.11) at the render scale times the factor; the twin keeps the authentic
  colour unless full colour was asked for. At each present the twin is averaged down to the render scale by
  halving blits with linear filtering: one at 2x, two at 4x. Each halving step weighs four samples equally and
  stores 8 bits a channel, so at 2x each shown pixel is the mean of the 4 samples it covers, rounded once, and at
  4x the mean of the 16 it covers within two such roundings. Dither stays keyed to guest pixels, as at any render
  scale. At render scale 1 the twin draws from its own copy of the vertices, with the above-1x texture fixes
  (the render-scale item's edge box and nearest-sprite alignment, section 11.1) applied at the twin's scale; the
  authentic chain keeps the 1x vertices unchanged. The presenter, the speed readout, Smooth motion and Widescreen
  Fill take that image at the render scale, as without supersampling, so Fill's wings are drawn as usual.
- **Guest memory.** The authentic chain keeps the render scale, and nothing reads the twin or its average back,
  so guest VRAM, every readback, every block transfer into RAM and the frame each present commits to guest VRAM
  are those of a run without supersampling at the same render scale. The tests: ssaa-edge hashes guest VRAM after
  its scene is read back and requires the hash of a run without supersampling, at 2x and 4x and render scales 1
  and 2; twin-scene and the whole suite run with it on at 2x and 4x, so every readback and block-transfer case
  meets its own expected bytes with it on. Section 11.1's four items are unchanged, as for the twin: the extra GPU
  work is a host wait, never in the guest timing model.
- **What the picture shows.** Without it every pixel of a black-and-white scene is black or white; with it, pixels
  on an edge take the shades between, only within a pixel and a half of the edge, and every other pixel keeps its
  value (ssaa-edge, which reads the image the production present step, `target_prepare_present`, hands the
  presenter). 4x gives more shades than 2x can. A nearest-filtered sprite drawn one texel per guest pixel shows
  the authentic image at every pixel, since each guest pixel's samples hold the same texel (ssaa-sprite, at
  render scales 1 and 2). Present-source captures follow the shown image, so they hold the averaged picture.
- **Size check.** At start the renderer checks the twin's images against the GPU: each side at most the device's
  largest image, framebuffer and viewport (`maxImageDimension2D`, `maxFramebufferWidth` and `Height`,
  `maxViewportDimensions`), and an estimate of the memory the factor holds within half of the largest
  device-local memory heap. The estimate counts four render targets (two display buffers and two others), each
  with its twin and depth twin at the twin's scale (2 bytes a depth sample, 4 with `depth=32`, section 10.20) and
  its average at the render scale, the twin snapshot, and at 4x the halving step. If 4x does not fit and 2x does, the run uses 2x and says so in one
  `ACE_MODE: ssaa 2 (asked for 4: <reason>)` line; if nothing fits, supersampling is unavailable for the run
  (`SSAA: unavailable (<reason>)`, the reason naming each factor tried; the Ace report and stamps say so, the
  report's reason cut to its 95 characters). It never falls back to 1x without a
  message. A GPU without linear blits of RGBA8, or one that refuses the twin, makes it unavailable the same way.
  The estimate is not a promise: a title with more live render targets uses more, and a twin allocation that fails
  later turns the twin off for the rest of the run with one `TWIN: off ...` line and one
  `SSAA: off for the rest of the run with the twin` line (section 10.11), and the Ace report then gives
  `"ssaa": 1` with the factor asked for (twin-alloc-fail with `--ssaa 2`). At render scale 1 a failed allocation of the twin's vertices makes it
  unavailable the same way. `ssaa-limit` forces small limits and checks the factor chosen, the reason given and
  the Ace report.
- **Cost.** Every render op is drawn a second time, into the twin, at factor x factor times the render scale's
  pixels: 4 times at 2x and 16 times at 4x, plus one or two blits per present. The estimate above, with 16-bit depth, is
  17 MB at render scale 1 and 2x, 64 MB at 1x and 4x, 68 MB and 255 MB at 2x render scale, and 272 MB and 1020 MB at 4x
  render scale (render scale 4 with 4x makes 8192 x 4352 images).
- **Stamps.** `ACE_MODE: ssaa <n>` at start; `SSAA: on (<n>x: the twin renders <w>x<h>, ...)` when the twin starts;
  the Ace report's `"ssaa"` (the factor in effect, 1 when unavailable) and `"ssaa_requested"` when the size check
  lowered it; and "2x supersampling" or "4x supersampling" in the pause menu's list of parts. Section 10.4
  applies.
- **Tests.** `gpu-coherence-selftest` runs ssaa-edge without supersampling to write a reference at render scales 1
  and 2 and at 2x and 4x against it, twin-scene, ssaa-sprite at 2x and 4x (render scale 1) and at 4x (render
  scale 2), the whole suite at 2x and at 4x, twin-gate with it on, and ssaa-limit with a forced memory budget and
  a forced largest side. `ace-selftest` checks the key's parsing,
  order, report and words; `test_player_state` the setting, its presets and its launch line.

### 10.22. Unclipped SAS Mixes Through a True-Peak Limiter (`peak=limit`)

The PSP's SAS core adds a grain's voices, and with `__sceSasCoreWithMix` the caller's own PCM, then stores the sum
as int16, saturated. Some games clip their own sound this way: #2135 counted 126 full-scale samples in a 4 s burst
at the Hot Shots Tennis (UCUS98701) title logo on a PSP-3000. Authentic keeps doing exactly that. `peak=limit`
(#2137) plays the same mix without the hardware's clipping: the unsaturated sum, through a true-peak limiter at the
host's output.

- **The line.** `peak=limit` sits after `ssaa` and before `glyphs` and `cull` and is written only when on;
  a missing `peak` and `peak=off` are off. It is an Enhanced part and needs no `tier=ace`. A build without it
  (`ACE_HAS_PEAK`) refuses the line.
- **What the guest sees: nothing.** The guest's SAS buffer gets the PSP's saturated bytes in every mode, because a
  game may read it back; `hle-thread-selftest` checks them byte for byte against a run with the part off. The
  limiter keeps a channel's queue depth: a push of n frames queues n frames.
- **Where the wider mix comes from: provenance, checked by bytes.** The guest reads SAS output from guest memory, so
  the sum can reach the host only through the buffer the game hands to `sceAudioOutput*`. The other design, a host
  mix fed beside the guest's in place of that output, would have to guess which output carries which grain and
  would drop whatever the game does to the buffer after the mix (its own volume, its own music, a copy). So the
  SAS mixer (`src/rt/hle_audio.c`) keeps, for the host only, the last eight stereo grains whose sum it saturated:
  the address, the bytes it stored and the sum before saturation, with the same voices, envelopes, resampler
  (`sas=sinc` included) and input gain as the stored grain. A new mix over a kept grain's span forgets it. An
  output (`sceAudioOutputBlocking`, `sceAudioOutputPannedBlocking`, `sceAudioOutput2OutputBlocking`, and
  `sceAudioSRCOutputBlocking` at 44.1 kHz) whose span holds a kept grain's bytes unchanged gives the host that
  grain's sum for those frames; a span the game changed after the mix, a copy of the grain elsewhere, a mono
  output, a 48 kHz SRC output and the raw four-plane output mode play the guest's bytes. Where the store did not
  saturate, the sum is the stored value, so nothing is kept.
- **The limiter (`src/rt/audio_sdl3.c`).** With the part on, every push of every channel passes through one limiter
  per channel in place of the per-sample clamp, at the channel volume and master volume as before. Each frame's peak
  is its own sample and the inter-sample peaks on either side, read at 4x through a 16-tap-a-phase windowed-sinc
  interpolator. The gain a frame needs is the ceiling over its peak; the gain applied is a minimum over the next 64
  frames, released exponentially (50 ms), then averaged over 64 frames, so it falls over 1.45 ms before a peak and
  is at most what the peak needs when it arrives. The ceiling is -0.3 dBTP as that detector reads it; read at 16x,
  the overflow fixture's hard 62000-count step rings 0.13 dB past it, still under full scale. The output is 71
  frames (1.61 ms) late. A frame below the ceiling comes out bit-exact unless a peak's attack or release reaches it
  (the release is back within half a count of unity well inside a second), so the level is the PSP's wherever the
  PSP did not clip; only the overs are rounded off instead of cut. A stream that stops leaves its last 71 frames in the
  limiter until that channel's next push or hold of silence (`sr_audio_push_silence`, which passes through the
  limiter too), which plays them first, in order. A push or hold that finds the channel played dry (a device pull
  found nothing) drops them instead (after the gap they would be a 1.6 ms fragment ahead of the new sound); a hold
  also drops the converter's kept-back frames, a push leaves them in place. Nothing is invented. The video
  recorder (`sr_videorec_audio`) and the counters keep the guest's copy.
- **Off.** Without `peak=limit` neither side runs: the SAS mixer keeps nothing, `sr_audio_push` is the plain push,
  and `sr_audio_push_wide` is the plain push of the guest's copy (`audio-selftest` checks the bytes). Fast forward
  pushes nothing, so nothing is limited while it runs.
- **Stamps and the report.** `ACE_MODE: peak limit (...)` at start; the badge's words say "unclipped sound"; the
  report's `"peak"` block gives `on`, `pushes_wide` (pushes that carried a kept sum), `frames_wide` (their frames
  where the sum and the guest's copy differ), `frames_limited` (output frames whose bytes the gain changed) and
  `min_gain`. The part
  is confirmed once the SAS mixer and the audio backend both take the switch (`driver.c`); a link without either,
  the headless gate, reports it as never run. Section 10.4 applies.
- **Tests.** `hle-thread-selftest` runs the overflow fixture (twenty full-scale voices over caller PCM through
  `__sceSasCoreWithMix`): the guest's bytes are identical with the part on and off; the output of that buffer
  carries the sum, past 16 bits exactly where the store saturated and the guest's value elsewhere; a buffer changed
  after the mix and a copy play the guest's bytes. `audio-selftest` checks that the part off is the plain push; that
  quiet audio comes out bit-exact and 71 frames late with an unchanged queue depth; that a hold of silence keeps
  the order; and that a sine reaching 6.6 dB
  past 16 bits leaves no sample past the ceiling and a true peak (read at 16x) under full scale, a gain that falls
  before the overflow arrives, the
  passage before it bit-exact and the one after it at the input's level within 0.1 dB.
- **Not yet.** The player has no part for it yet: a launch line names it by hand. The Hot Shots Tennis title-logo
  check (no full-scale sample, and the non-clipping music at the PSP's -15.6 dBFS) waits for the #2135 console read.
- **Tier:** Enhanced. **Default:** off, and in no graphics preset.

## 11. Guest-Visible Items

Sections 10.3, 10.6, 10.8, 10.11 and 10.22 describe host-only features: what the guest reads, in its registers, memory and timing, is the
same with them on or off. The owner's decision of 2026-11-04 (#2055) is that Enhanced does not have to stay fully
Authentic: a change that improves a game without making anything worse is allowed, even when the guest can observe
it. This section is the class for those changes. It adds to the classes above and removes none of them.

"Enhanced" here is the library-wide tier and "Ace" the per-title tier of #2055. In builds before that rename the
library-wide tier is the Ace Mode of section 10, and the identifiers keep `ace`. Enhanced applies to every game and
may offer switches to turn parts off; Ace is Enhanced plus extras for one title, overrides the library's Enhanced
choice when selected, and may also offer switches.

### 11.1. What Makes an Item Guest-Visible

An item is guest-visible when, with it on, any of the following differs from the same run with it off:

- the bytes of guest VRAM or any other guest memory the item writes;
- what a readback returns: a GE or display readback, a save picture, a screenshot the title takes of its own screen;
- timing the guest can measure: when a VBLANK, a DMA, a GE list or a media clock completes relative to the guest;
- the speed of I/O the guest performs: disc, Memory Stick or savedata reads and writes.

The item's contract entry (section 11.2) names each of these that applies. An item that changes none of them is
host-only and follows sections 10.2 to 10.6, 10.8 and 10.11, not this section. One existing feature already sits on this line:
texture filtering changes the frame that every present commits to guest VRAM, so a game that copies its screen keeps
the filtered picture (the "Pixels" bullet of section 10.3). It keeps that entry, and is not held to items 2 to 5 of
section 11.2 retroactively; a change to it is. A new item is not admitted by analogy with it. Render scale, below,
is the other existing feature on this line, with its own rule for changes.

A render scale above 1x (the player's Internal Render Resolution, not an Enhanced or Ace item) sits on the same line.
Its off switch is 1x, where the picture and every readback are the PSP's values. A change to how the renderer draws
above 1x is part of this render-scale item, not a new item or a new class, when it keeps 1x byte-identical and above
1x moves the picture toward the 1x picture. That is a claim about the picture, not about every pixel: the change's
pull request traces each device pixel that ends farther from its own 1x value and says why it does. Items 2 to 5 of
section 11.2 do not apply to such a change; its pull request still carries a test that fails without it and the
comparison runs that show it. A readback keeps the
top-left device pixel of each guest pixel, so the guest reads what the device drew there:

- Where the device image holds the PSP's own value, the readback returns it. That covers a target uploaded from
  guest memory, and a sprite sampled nearest at one texel per pixel in either direction, whatever its origin. Every
  device pixel of such a sprite shows the PSP's texel (`sprite-scaled` in `gpu-coherence-selftest`).
- Every other draw samples at each device pixel's own position. The readback returns the top-left device sample,
  which sits 0.5 - 0.5 / scale of a pixel before the PSP's pixel centre. At 4x that is 0.375 of a pixel: for a 1:1
  linear sprite or triangle the value moves by up to 0.375 of the step between neighbouring texels, and a minified
  nearest sprite can return a different texel. The exception is the left and top edge pixels of a clamped primitive
  (next bullet): there the top-left device sample lies in the clamped band, so the readback returns the PSP's value.
- At the edges of an axis-aligned through-mode primitive (a sprite, or a strip or quad drawn as screen-aligned
  rectangles, flipped or not), no device pixel samples outside the texture range the primitive's 1x pixel centres
  cover. Those centres follow the rasteriser's top-left rule. A device pixel in the outer part of an edge pixel shows
  that pixel's own sample, as the PSP does, instead of reaching past it (with repeat wrapping, into the texture's
  opposite edge), and is filtered with the primitive's own footprint. This change is part of the render-scale item
  above: 1x stays byte-identical, and above 1x it removes the edge bleed, moving the picture toward the 1x picture
  (`uv-edge-clamp` in `gpu-coherence-selftest`). A pixel that ends farther from its own 1x value comes from
  interpolation on the axis the clamp leaves free, or from content beneath the primitive; #2104 traces each one on
  its captures.
  What it does not do:
  - Rotated and sheared primitives, a strip half without both a horizontal and a vertical leg (a trapezoid), and
    transform-mode (3D) geometry are not clamped.
  - A sprite drawn from a rebuilt glyph tile (section 10.13) is not clamped: the tile holds only its own rectangle, so
    no neighbouring cell can reach an edge sample, and each device pixel samples its own rebuilt texel.
  - Where two clamped primitives meet with continuous texture coordinates, each keeps its edge pixel's sample over
    its outer band, so the seam shows the 1x step instead of a smooth upscaled blend across it.
  - A sprite that samples a render target at device resolution (a screen copy) is clamped the same way, so the copy's
    one-pixel border shows the guest-pixel sample instead of the upscaled detail.

Render targets keep the render scale when a later draw reads them as textures, when the read is one the renderer can
bind directly: the same pixel format as the target, not swizzled, mipmapping not active, the target's own buffer width
(or a buffer width of 0) and a pixel-aligned address. Then the draw samples the target's image at the render scale, on
the PSP's sample lattice in guest texels: linear filtering, in sixteenths of a guest texel, blends the device texels
there, so an effect drawn into a smaller buffer and sampled back with linear filtering (a bloom or glow pass) keeps
its detail (`rtt-scale` in `gpu-coherence-selftest`). Nearest filtering takes each guest texel's centre, as the PSP
does, so it shows one device texel per guest texel. The texture's own width and height bound the read: each texel index
is clamped or wrapped at them, as the GE does, not at the target image's edge, so a texture inside a larger target never
shows the target's pixels beyond it (`rtt-edge`), and a texture at an offset inside a target scales the same way
(`rtt-scale-offset`).
Every other read of a render target, including a read in another format, a swizzled or mipmapped read and another
buffer width, decodes the target from guest memory at 1x. That is the GE's own picture of it, so such an effect shows
1x detail at any render scale; it never shows a picture the PSP would not draw. A CPU read of a target a draw sampled
returns its top-left device samples in its own format (the readback rule above, `rtt-scale`). A render-target read is
found by address: depth read as a texture decodes from guest memory at 1x when its address lies outside every colour
target's span. When it lies inside a colour target's span too, the read takes whichever was written last: depth
written after the target's image last changed makes it a depth read, decoded from guest memory at 1x after the depth is
committed there (`rtt-depth-alias`); otherwise the colour target's rules apply. The cases, the depth rule and the debug
switch are in `src/rt/gpu_sdl3vk/README.md` (#2052).

At 1x the readback is the PSP's value.

The render-target reads that decode at 1x (#2279; the README lists each with its reason) fall into two groups:

- no scaled form exists, because the texture's texels are not the target's pixels: 8888 read as 16-bit or CLUT16,
  16-bit as 8888 or CLUT32, CLUT4, CLUT8, DXT, an address off the target's pixel boundaries, and depth read in a
  format whose texel is not one depth value. The 1x decode is the read, swizzled, mipmapped or at another width too;
- a scaled form could exist but is not implemented, so these read at 1x and #2279 stays open for them, each in a
  format whose texel is one of the buffer's values: a 16-bit target read as another 16-bit format, CLUT16 and CLUT32
  at the target's pixel size, a swizzled read, a read with another non-zero buffer width, a mipmapped read, and a
  depth read in a 16-bit format (outside every colour target, or inside one whose depth was written last).

`rtt-1x-reads` in `gpu-coherence-selftest` runs thirteen such reads at 1x, 2x and 4x (the README lists them): none
binds a target's image, each depth read commits the GPU's depth first, and the reading target's guest VRAM equals
the software renderer's.

### 11.2. What an Item Must Have

1. **Authentic stays byte-identical.** With the library choice or the title's choice at Authentic, a build behaves
   as section 10.2 describes: the item sits behind the one switch that stays false, the generated code, the package
   key, the guest's memory, its instruction trace and its timing are what they are without the item. The test that
   pins the item also runs with Authentic and compares against the item-less result byte for byte.
2. **Its own key and off switch.** The item has a key on the launch line (section 10.1), a library setting and a
   per-title setting, and each can turn the item off alone, without turning off the rest of Enhanced or Ace. A
   missing or unknown value reads as off, never as more permissive. A launch line that carries a key this build does
   not know is refused, as section 10.1 says.
3. **A contract entry that says what the guest can observe.** Each item is listed in this document, in its own
   subsection, with: its key; what it changes; for each of VRAM bytes, readbacks, timing and I/O speed, either
   "unchanged" or exactly what differs and by how much; the tier it belongs to (Enhanced for every game, or Ace for
   one title); and its default. An Ace-tier item is on only for a title whose Ace is selected, and Ace keeps every
   Enhanced off switch. An item without an entry is not admitted.
4. **"Nothing worse" is shown before it is on by default.** Until the evidence below exists, the item is off by
   default and a player may turn it on. To be on by default in Enhanced or Ace it needs all of:
   - a test that pins the new behaviour, in this repository, that fails without the item and passes with it;
   - private route or picture-gate runs (`tools/picture_gate.py` or the title's private route) on *Hot Shots Tennis*
     and on *LittleBigPlanet*, each compared with the same build and route with the item off, with no new failure;
   - for an item that applies to every game, a library sweep (`tools/library_sweep.py`) with the item on against the
     same sweep with it off, with no title that passed turning into a failure.

   The comparison runs are non-authentic evidence (section 3): they show the item made nothing worse and are not
   cited for PSP conformance. The pull request lists each run with its result and says which could not be run.
5. **A title where it breaks gets a per-title off switch in its package.** The switch is data in that title's own
   package (section 5), which the generic code reads as "this item is off here". Generic files
   contain no title ID, no address and no branch for it. When a regression is found for a title, the item stays
   admitted and the title's switch goes in with the fix; the item is not made title-aware.
6. **The evidence tools keep their answers.** An Enhanced or Ace run is stamped, labelled and refused as section 10.4
   says: `ACE_MODE: on (...)` carries the item's key and value, the flight recorder bundle lists it in
   `runtime.enhancements`, `tools/flight_diff.py` and `tools/boot_gate.py` refuse the run and the acceptance
   checkpoint refuses it. A guest-visible item never relaxes one of those checks, and none is changed to let an item
   through.

### 11.3. Where Items Are Listed

No guest-visible item is admitted by this section alone. Each lands in its own pull request with its entry added here
(`11.3.<n>`), following the order of section 11.2. This section is the template that entry is checked against.

#### 11.3.1. Band-Limited SAS Resampling (`sas=sinc`)

- **Key.** `sas=off` or `sas=sinc` on the launch line (section 10.1), a required key (#2054). A line without it, or
  with any other value, is refused, and the run is Authentic.
- **Tier and default.** Enhanced: it applies to every game, in Enhanced and Ace runs alike (section 10.5). The
  default is off. It is the part "Clean pitched sound" (`"sas"`, `sinc` or `off`) on the library's Settings >
  Enhanced page and on each game's own page (section 10.9), so the library and each title can turn it on or off
  alone (section 11.2, item 2), and the launch line carries `sas=sinc` or `sas=off`. It stays off by default until
  the "nothing worse" runs below exist.
- **The authentic path.** Without the item the mixer does not interpolate (a zero-order hold). Each output sample
  plays the decoded sample at the voice's whole position. A 12-bit fraction then advances by the pitch (0x1000 is one
  source sample per output sample). Above 0x1000 the voice skips samples with no low-pass filter, so it aliases.
  Below 0x1000 it repeats them, so it carries images of its source band. A step that runs past the end of a 28-sample
  VAG block lands on the next block's first sample, dropping the overshoot. This rule has been in the mixer since the
  initial public export. It is not a physical-PSP measurement of the SAS core.
- **What changes.** A VAG voice's output samples are a Kaiser-windowed sinc (24 zero crossings, beta 7) evaluated at
  the voice's exact position, with a DC gain of exactly 1. Above a pitch of 0x1000 the cutoff drops by the pitch
  ratio, so the source is low-passed below the output's Nyquist frequency before it is decimated. The code is in
  `src/rt/hle_audio.c`, next to the authentic mixer, and `src/rt/sas_resample.h`. PCM and noise voices, which ignore
  the pitch today, are mixed as before.
- **Guest memory.** VRAM bytes are unchanged. The sample values the SAS core writes into the guest's output buffer
  change: by the measurements below, the energy aliased above the output's Nyquist frequency falls by 25 to 30 dB,
  and the images above the source's Nyquist frequency by 78 to 81 dB. A title that reads that buffer back (a level
  meter, its own effect chain) sees those values. Every other guest-readable SAS result is unchanged: the voice's
  decode position, fraction, envelope, end of sample, end flags and the grain run the authentic statements in the
  authentic order.
- **Readbacks.** Unchanged. The item touches no GE or display readback, save picture or screenshot.
- **Timing.** Unchanged. The grain, the SAS calls' return values, the end flags and the envelope heights are the
  authentic ones in every grain, and data that ends mid-grain ends on the same sample. The filter costs host time
  inside the SAS call, like any host cost.
- **I/O speed.** Unchanged. The item performs no disc, Memory Stick or savedata access.
- **Reading ahead.** Samples ahead of the position come from a copy of the voice that decodes ahead of it, reading
  the guest's VAG data in memory. The voice itself never decodes ahead. The copy decodes at most one sample past the
  farthest sample the filter can read (one spare). Past the last block the authentic decoder has read by the end of
  the same grain, it decodes at most 1 VAG block (16 bytes) at a pitch of 0x1000 or below, rising to 4 blocks
  (64 bytes) at 0x4000. A title that streams VAG data into its buffer just in time may not have written those blocks
  yet. The copy then mixes the stale bytes into the output samples at the end of the grain whose filter window
  reaches them. Only the mixed values are affected. The copy is discarded, so voice state never changes.
- **Authentic stays byte-identical (section 11.2, item 1).** `hle-thread-selftest` plays a two-voice authentic run
  (a loop, a key-off, an end of data) that must match a checksum taken from the mixer before #2054. Main's mixer
  after the envelope change of #1296 gives the same checksum. A run switched on and then off gives it again, and a
  mutant that only rounds the authentic mix differently is caught by that checksum alone.
- **"Nothing worse" (section 11.2, item 4).** `hle-thread-selftest` plays synthetic VAG data through the guest
  calls, off and on, and fails without the item.
  - **Upward pitches.** A sweep lying wholly above the output's Nyquist frequency once pitched, so all of its
    output is alias. At pitch 0x1800 the alias energy falls by 25.5 dB (floor 22), at 0x2A00 by 28.1 dB (floor 25)
    and at 0x4000 by 29.8 dB (floor 26).
  - **Downward pitches.** An in-band sweep, measuring the share of energy above the source's Nyquist frequency at
    that pitch, where only images land. It falls by 78.4 dB at 0x0B00 (floor 75) and 80.7 dB at 0x0800 (floor 77).
  - **Negative control.** A model of the authentic stepping replays the same stream. It matches the mixer within
    0.5 dB. On that model, linear interpolation and a 4-tap cubic reach at most 2.9 dB upward and 20.9 dB downward,
    so they fail every floor.
  - **Other cells.** A looped voice mixes the same samples as its data unrolled. A constant signal comes out
    unchanged at three pitches. Two voices mix to the sum of each alone. A key-on mid-play restarts the voice like a
    fresh one.
  - **The read bound.** A selftest-only counter checks the lookahead's read count against its bound. Stale data
    just past that bound changes nothing, and data inside it reaches only the mixed values.
  - **Mutants.** Each was built from a scratch copy, run, and failed for its intended reason: linear interpolation
    or a 4-tap cubic in place of the sinc (the floors and the model match), a key-on that keeps the filter history
    (the key-on cell), history not kept across grains (the constant-signal cells and the image floors), the
    rounding change above (the checksum), and a lookahead one block longer (the read-bound counter).
  - **Not run.** No private route or picture-gate run on *Hot Shots Tennis* or *LittleBigPlanet* and no library
    sweep, with the item on against off. Until they exist the item stays off by default.
- **Per-title off switch (section 11.2, item 5).** None. No title is known to regress with the item on.
- **Evidence tools (section 11.2, item 6).** `ACE_MODE: on (...)` and the flight recorder's `runtime.enhancements`
  carry `sas=` with the rest of the launch line, and section 10.4's refusals apply to the run unchanged.

#### 11.3.2. The 60 FPS logic transforms `count_x2_even` and `gate_30hz_hold`

Every section 9 transform rewrites guest code, so what the guest computes differs with it on. These two are the
first section 9 transforms admitted under this section; a later section 9 transform is admitted the same way, with
its own entry.

- **Key.** The title's "60 FPS game logic" setting of section 9.1, item 2: the player's
  `enhancement_60hz_discs` list (or `--experimental-60hz` for the session), which launches the title's enhanced
  package with `SR_EXPERIMENTAL_60HZ=1`. A transform acts only at the sites the title's package names with it, so
  each title's package is that title's switch for each transform.
- **Off switch.** The same setting, per title: turning a title's 60 FPS game logic off launches its authentic
  package with `SR_EXPERIMENTAL_60HZ=0`, and any value other than `1` runs every stock statement. This switch is
  coarser than section 11.2, item 2 asks: it turns off every section 9 transform of the title's package together,
  not these two alone, and there is no launch-line key or library setting for them alone. A package leaves either
  transform out for its title by not naming it (the site keeps its stock instruction, or names `count_x2` or
  `gate_30hz` instead); that is the per-title data of section 11.2, item 5.
- **What it changes.** `count_x2_even` doubles a `slti`/`sltiu` bound on a counter of logic updates and reads false
  on a non-negative odd count, so the window's events happen at the stock instants. `gate_30hz_hold` skips a call to
  an ease helper on odd logic updates and gives `f0` the value of `f12`, so each 30 Hz value shows for two updates.
- **What the guest can observe.**
  - Guest registers and memory: at a `count_x2_even` site, the compare's `rt` is 0 on an odd count below twice the
    bound, where `count_x2` would give 1. At a skipped `gate_30hz_hold` call, `f0` is the held value and the
    callee's other result registers keep their earlier values; no memory differs, because the resolver refuses a
    callee that writes memory. Everything the logic computes from these values, and so what it draws, follows them,
    as for every site of a 60 Hz package.
  - VRAM bytes: no transform writes VRAM; the frames drawn follow the logic, as above.
  - Readbacks: unchanged; neither transform reads or writes a GE or display readback.
  - Timing the guest can measure: no VBLANK, DMA, GE list or media clock changes. An odd update does less work, by
    the skipped call.
  - I/O speed: unchanged.
- **Tier and default.** Ace: on only for a title whose package names the sites and whose 60 FPS game logic is
  selected. Off by default, as 60 FPS game logic is.
- **Authentic stays byte-identical.** Without `--enhancement-package`, codegen output and the cache key's codegen
  options are those of a build without this feature (section 9.1, item 1). With the package built in and the toggle
  off, every site runs its stock statement. `test_count_x2_even_and_gate_30hz_hold_match_30hz_when_run` in
  `tools/test_enhancement_package.py` builds a fixture with the package and without it, checks that each site's
  toggle-off statement is the Authentic statement byte for byte, runs the same harness on both builds, and checks
  that the toggle-off output equals the Authentic output byte for byte.
- **Sampled checking.** With `SR_EXPERIMENTAL_60HZ=1`, the in-play verifier
  ([sampled in-play checking](DEBUGGING.md#sampled-in-play-checking)) compares the enhanced code with the stock
  reference interpreter, so a sampled call whose body holds any enhancement site, these two included, reports a
  mismatch there; there is no enhancement skip class. Run it with the toggle off, or read such a mismatch as
  expected. A skipped `gate_30hz` or `gate_30hz_hold` call never reaches its return boundary, so it starts no check.
- **"Nothing worse".** The transforms replace `count_x2` and `gate_30hz` where those were wrong, and their tests pin
  that: an event window keeps its stock count and instants, and a held ease keeps its 30 Hz values. The private
  comparison runs of section 11.2, item 4 are recorded in the pull request that turns a title's 60 FPS game logic
  on by default, not here; until they exist it stays off by default, as it is today.

##### 11.3.2.1. The 60 FPS logic transform `gate_30hz_alt`

The third section 9 transform admitted under this section, by its own entry as section 11.3.2 asks.

- **Key and off switch.** Those of section 11.3.2: the title's "60 FPS game logic" setting, which launches the
  enhanced package with `SR_EXPERIMENTAL_60HZ=1`, and per title the same setting off, which runs every stock statement.
  A package leaves the transform out for its title by not naming it.
- **What it changes.** At a `gate_30hz_alt` site the gated call keeps its stock instants, and on the updates it skips,
  the functions its `alternates` name are called in its place, in list order, each with `$a0` as it was at the site and
  `$ra` set to the site + 8. The site keeps its own phase (section 9.5), so the gated function runs first whenever the
  site is reached again.
- **What the guest can observe.**
  - Guest registers and memory: on an alternate update the gated function's work outside its alternates does not
    happen, and the alternates' does, twice as often as stock. After the site, `$v0`, `$v1`, `f0`-`f3`, HI and LO hold
    what the last alternate left; the resolver refuses a site whose caller reads them (section 9.2). Each alternate's
    other argument registers hold what the caller left there; the resolver refuses an alternate that reads them, three
    calls deep.
  - VRAM bytes: no transform writes VRAM; the frames drawn follow the logic.
  - Readbacks: unchanged.
  - Timing the guest can measure: no VBLANK, DMA, GE list or media clock changes. An alternate update does the
    alternates' work in place of the gated function's.
  - I/O speed: unchanged.
- **Tier and default.** Ace, off by default, as section 11.3.2.
- **Authentic stays byte-identical.** As section 11.3.2: without `--enhancement-package` codegen output is unchanged,
  and with the package built in and the toggle off the site runs its stock call.
  `test_gate_30hz_alt_runs_its_alternates_on_the_skipped_updates` in `tools/test_enhancement_package.py` builds a
  fixture with the package and without it, checks the toggle-off statement against the Authentic one byte for byte,
  runs both builds, and checks the toggle-on run's calls update by update against the site's phase.
- **Sampled checking.** A sampled alternate call is checked as a call of the alternate from the site, with the
  alternate as its target, never the target the site's own `jal` names. A skipped stock call starts no check.
- **"Nothing worse".** Not shown here: the private comparison runs of section 11.2, item 4 come with the pull request
  that turns a title's 60 FPS game logic on by default.

##### 11.3.2.2. The 60 FPS logic transform `gate_30hz_store`

The fourth section 9 transform admitted under this section, by its own entry as section 11.3.2 asks.

- **Key and off switch.** Those of section 11.3.2: the title's "60 FPS game logic" setting, which launches the
  enhanced package with `SR_EXPERIMENTAL_60HZ=1`, and per title the same setting off, which runs every stock statement.
  A package leaves the transform out for its title by not naming it.
- **What it changes.** A `gate_30hz_store` site runs its store (`sb`, `sh`, `sw` or `swc1`, never through `$sp`) on
  even logic updates only (section 9.5). On an odd update the store does not happen, so the field keeps the value
  it held unless another writer stores to it. The rest of the update runs as stock. The promises below rest on the
  site's evidence that the store writes a logic field in main RAM or the scratchpad (section 9.2): the resolver
  cannot see the store's address.
- **What the guest can observe.**
  - Guest registers and memory: on an odd update the stored field holds its earlier value; the register that would
    have been stored keeps the value the stock code computed. Everything the logic computes from the field, and so
    what it draws, follows the held value, as for every site of a 60 Hz package. Under `--lle-cpu`, a store that
    would fault (a misaligned address) does not fault on an odd update, because the whole statement is skipped, as
    for the `add`/`sub` forms of `gate_30hz`.
  - VRAM bytes: the store writes a logic field, never VRAM; the frames drawn follow the logic.
  - Readbacks: unchanged.
  - Timing the guest can measure: no VBLANK, DMA, GE list or media clock changes. An odd update does one store less.
  - I/O speed: unchanged.
- **Tier and default.** Ace, off by default, as section 11.3.2.
- **Authentic stays byte-identical.** As section 11.3.2: without `--enhancement-package` codegen output is unchanged,
  and with the package built in and the toggle off the site runs its stock store.
  `test_gate_30hz_store_holds_each_30hz_value_for_two_updates` in `tools/test_enhancement_package.py` builds a
  fixture with the package and without it, checks the toggle-off statement against the Authentic one byte for byte,
  runs both builds, and checks that the toggle-off run prints what the Authentic run prints and that the toggle-on
  run holds each 30 Hz value for two updates.
- **Sampled checking.** As section 11.3.2: a sampled call whose body holds the site reports a mismatch there while
  the toggle is on; run the verifier with the toggle off, or read such a mismatch as expected.
- **"Nothing worse".** Not shown here: the private comparison runs of section 11.2, item 4 come with the pull request
  that turns a title's 60 FPS game logic on by default.

##### 11.3.2.3. The 60 FPS logic transform `gate_30hz_phase`

Admitted under this section by its own entry, as section 11.3.2 asks.

- **Key and off switch.** Those of section 11.3.2: the title's "60 FPS game logic" setting, which launches the
  enhanced package with `SR_EXPERIMENTAL_60HZ=1`, and per title the same setting off, which runs every stock statement.
  A package leaves the transform out for its title by not naming it.
- **What it changes.** At a `gate_30hz_phase` site the stock instruction keeps the stock instants of one state. After
  an odd number of that state's updates a call is skipped, a compare reads as its `odd_phase_condition`, and a loaded
  value reads `+0.0` (section 9.2).
- **What it reads.** One float of guest memory, at the declared offset from the declared register, each time the site
  runs: through the guest accessors, as a load at the site would read it. No transform before this one reads guest
  memory at a declared register and offset to decide what a 60 Hz site does; `clip_x_widen` (section 11.3.6) does so
  for another class of package. The argument for allowing it is that the read replaces a counter the runtime would
  otherwise keep beside the guest (the update count of section 9.5) with the title's own record of the same thing,
  which is right where the count is wrong: after a state entered on an odd update, and across a pause that stops the
  state and not the count. The resolver proves the register holds one known value at the site. The phase is not
  estimated from the float: the float is matched exactly against the values the state's own float32 sum takes, so
  it is right for as long as the state lasts, and a float that is not one of those values is not guessed at: the
  site runs stock and reports. What the field means is the author's evidence, as for every site.
- **What the guest can observe.**
  - Guest registers and memory: no memory is written. After an odd number of updates a skipped call's work does not
    happen, and `$v0`, `$v1`, `f0`-`f3`, HI and LO keep their earlier values (the resolver refuses a site whose
    caller reads them); after a gated compare the FPU condition flag (`fpcond` and bit 23 of FCR31) holds
    `odd_phase_condition` and not the compare's result, for every later reader of the flag; after a gated `lwc1` or
    `lv.s` the loaded FPU or VFPU register holds `+0.0` and not the stored value, for every later reader of the
    register, while the memory it was loaded from is unchanged. The resolver refuses a gated load of the state's
    own time and one whose register the title stores right after it (section 9.2); what else the title does with
    the register is the site's evidence.
  - A fault stock does not have: the read is a guest access at an instruction that may have none (a `jal`, a
    compare). With a bad pointer in the declared register it faults there; under `--lle-cpu` that is an address
    error at that instruction, raised for a `jal` after its delay slot has run.
  - VRAM bytes: no transform writes VRAM; the frames drawn follow the logic.
  - Readbacks: unchanged.
  - Timing the guest can measure: no VBLANK, DMA, GE list or media clock changes. A skipped call is less work.
  - I/O speed: unchanged.
- **Lines a 60 Hz run is checked for.** `ENHANCEMENT_PHASE_KEPT: site <id> at 0x<pc> read the time <t>, which its
  state does not reach by adding <step> from zero; it runs its stock instruction`, once per site. It means a state's
  time left its sum (or reached the value where the sum stops advancing), and that the site's phases for that state
  are not to be trusted until the state is entered again. A tool that accepts a 60 Hz run checks for it.
- **Tier and default.** Ace, off by default, as section 11.3.2.
- **Authentic stays byte-identical.** As section 11.3.2: without `--enhancement-package` codegen output is unchanged,
  a package without such a site carries none of its run-time code, and with the package built in and the toggle off
  the site runs its stock statement, with no read (the `else` form of every rewritten statement).
  `test_generated_statements_match_the_rules_when_run` in `tools/test_enhancement_package.py` builds a fixture with
  the package and without it, runs both, and checks that the toggle-off output equals the Authentic output byte for
  byte, and the toggle-on run time by time against the rule, for both flag values, under `--lle-cpu`, and with
  sites held by `requires`. It also runs the generated lookup against the state's float32 sum for two million
  updates (nine hours of state time); `test_phase_follows_the_states_float32_time_for_hours` checks the rows
  themselves for five step values.
- **Sampled checking.** As section 11.3.2: a sampled call whose body holds the site reports a mismatch there with the
  toggle on. A skipped call starts no check.
- **"Nothing worse".** Not shown here: the private comparison runs of section 11.2, item 4 come with the pull request
  that turns a title's 60 FPS game logic on by default.

##### 11.3.2.4. The 60 FPS logic transform `velocity_source_x_kappa`

Admitted under this section by its own entry.

- **Key and off switch.** Those of section 11.3.2. A package leaves the transform out for its title by not naming it.
- **What it changes.** The value a site loads or stores, a source of velocity change, is multiplied by
  $\kappa = 2\mu / (1 + \mu)$, $\mu = \sqrt{1 - cL}$, where $c$ and $L$ are stock values of the title read from its
  executable's read-only data (section 9.2). For a lightly damped body $\kappa$ is just below 1.
- **What the guest can observe.**
  - Guest registers and memory: after an `lwc1`, `lv.s` or `lv.q` site the loaded FPU register, VFPU register or
    four VFPU lanes hold the stock value times $\kappa$; the memory they were loaded from is unchanged, so every
    other reader of it sees the stock value. A `swc1` site stores the scaled value and leaves its register alone, so
    every later reader of the stored field sees the scaled value. Everything computed from these follows. The
    multiply is the host's (section 9.2, "Host arithmetic").
  - VRAM bytes: no transform writes VRAM; the frames drawn follow the logic.
  - Readbacks: unchanged.
  - Timing the guest can measure: no VBLANK, DMA, GE list or media clock changes. What the logic does when differs
    in one way a player can meet (section 9.2, "Landing is not moved to the stock update"): with the arc at stock's
    positions on every second update, the character can be grounded one 60 Hz update (1/60 s) before stock would
    have grounded it. The next jump input can then be accepted one update sooner, the landing point shifts by up to
    the horizontal speed times 1/60 s, and a chain of jumps on a held button runs up to 1/60 s shorter per landing.
  - I/O speed: unchanged.
- **Tier and default.** Ace, off by default, as section 11.3.2.
- **Authentic stays byte-identical.** As section 11.3.2: every rewritten statement keeps its stock statement as its
  `else` form, and `test_emitted_forms` in `tools/test_enhancement_package.py` checks that for each site of the
  fixture and that a build without the package holds none of the rewrite.
  `test_kappa_scale_is_one_rounding_in_every_form` compiles the emitted scale of each form, runs it, and checks its
  bits against the rule and against a checked store's table.
- **Sampled checking.** As section 11.3.2.
- **"Nothing worse".** `test_emitted_statements_follow_the_stock_arc_in_float32` and
  `test_the_float32_model_stays_within_a_millionth_of_the_stock_arc` pin what the factor is for: with it, and the
  transforms of sections 11.3.2.3, 11.3.2.5 and 11.3.2.6, the emitted statements bring a float32 flight to within
  $10^{-6}$ of its height of the stock flight at the end of every pair of updates (section 9.2 states the
  conditions). The private comparison runs come with the pull request that turns a title's 60 FPS game logic on by
  default.

##### 11.3.2.5. The 60 FPS logic transform `launch_from_rest_60hz`

Admitted under this section by its own entry, and under the decision of section 9.2 ("Guest memory written by a
timing transform").

- **Key and off switch.** Those of section 11.3.2. A package leaves the transform out for its title by not naming it.
  A package that names it must also name the capability `launch_velocity_write`.
- **What it changes.** After the call at the site returns, the velocity the callee left on the body in `$a0` is mapped
  to its two-update image: on the gravity axis $\kappa v + q g$; on the other axis only the change the call made is
  scaled by $\kappa$ (section 9.2).
- **Every field read and written.** Read before the call: the float of the other axis, at `velocity_offset` or
  `velocity_offset + 4` from the site's `$a0`. Read after the call: both floats, at `velocity_offset` and
  `velocity_offset + 4`. Written after the call: the float of the gravity axis, always; the float of the other axis,
  when the call changed it. Nothing else is written. All of it goes through the guest accessors, once per run of the
  site.
- **Why a timing transform may write here.** A launch from rest is the one point where no scaling of a constant gives
  the image, because the image of a velocity that is set, not accumulated, has an additive term ($q g$, about a
  quarter of a step of gravity). The write replaces, at the same place, values the stock callee has just stored
  there: the resolver reads the callee and refuses the site unless it stores both floats, from its entry `$a0`, on
  every path that returns. The values come from those floats and from stock values read from the executable's
  read-only data. It is declared, digest-checked and fail-closed like every site. That `$a0` is the body and the
  offset its velocity is the author's evidence.
- **What the guest can observe.**
  - Guest registers and memory: the fields above; no register differs. Everything the logic computes from the
    velocity follows, as section 9.2 says of the velocity in flight. The two floats are written one after the
    other: a fault on the second leaves the first already mapped.
  - A fault stock does not have: the reads and writes are guest accesses at a `jal`. With a bad pointer in `$a0`
    they fault there; under `--lle-cpu` that is an address error at the `jal`, raised after its delay slot has run.
  - VRAM bytes: no transform writes VRAM; the frames drawn follow the logic.
  - Readbacks: unchanged.
  - Timing the guest can measure: no VBLANK, DMA, GE list or media clock changes. The launch is what puts the
    60 Hz arc on stock's positions at every second update, and with that arc the landing can come one 60 Hz update
    (1/60 s) before stock's: the next jump input can be accepted one update sooner and the landing point shifts by
    up to the horizontal speed times 1/60 s (section 9.2, "Landing is not moved to the stock update").
  - I/O speed: unchanged.
- **When the map is not applied.** The map belongs to the statement that makes the call. When the runtime does not
  continue the caller there, or a guest access of the map faults, the velocity keeps the value the call left, and
  the run's last lines say so: `ENHANCEMENT_LAUNCH_NOT_MAPPED: site <id> at 0x<pc>: <k> of <n> call(s) were not
  followed by the launch map; the velocity they left kept its stock value`. A tool that accepts a 60 Hz run checks
  for it.
- **Tier and default.** Ace, off by default, as section 11.3.2.
- **Authentic stays byte-identical.** As section 11.3.2. `test_generated_statements_match_the_rules_when_run` checks
  that the toggle-off statement leaves the body's floats, the words beside them and `$a0` as the Authentic statement
  does, byte for byte, and the toggle-on result bit for bit against the rule, for either gravity axis, with and
  without a carried velocity, and that two calls that do not return to the statement are named at the end of the run.
- **Sampled checking.** As section 11.3.2: a sampled call whose body holds the site reports a mismatch there with the
  toggle on.
- **"Nothing worse".** As section 11.3.2.4.

##### 11.3.2.6. `retention_m_to_sqrt(m)` on an Arithmetic Result

`retention_m_to_sqrt(m)` on a `lui`, an `lwc1` or an `lv.s` predates this section. Its form on `add.s`, `sub.s`,
`mul.s` or `div.s` is admitted here by its own entry.

- **Key and off switch.** Those of section 11.3.2. A package leaves the form out for its title by not naming it.
- **What it changes.** The result register of the arithmetic instruction, a per-update retention the title computes
  (such as $1 - cL$), is replaced by its square root when it lies in $(0, 1)$; outside that range it keeps its stock
  value and the site reports once (`ENHANCEMENT_FACTOR_KEPT`), as the `lwc1` form does.
- **What the guest can observe.**
  - Guest registers and memory: the one FPU register, and everything computed from it. No memory is written. The
    square root is the host's `sqrtf` (section 9.2, "Host arithmetic").
  - VRAM bytes, readbacks, timing the guest can measure and I/O speed: unchanged, as section 11.3.2.4.
- **Tier and default.** Ace, off by default, as section 11.3.2.
- **Authentic stays byte-identical.** As section 11.3.2; `test_emitted_forms` checks the emitted statement and its
  stock `else` form.
- **Sampled checking.** As section 11.3.2.
- **"Nothing worse".** As section 11.3.2.4; in the double-precision model the arc differs from stock's when the
  retention of half a step is $1 - cL/2$ in place of the square root.

##### 11.3.2.7. Decay and Retention on a Register Copy

`decay_k_to_1-sqrt(1-k)` and `retention_m_to_sqrt(m)` on a `mov.s fd, fs` that copies a per-update factor are
admitted here by their own entry. A title can load one constant once and use it twice, for example 0.2 as a blend
factor applied every update and 0.2 as a duration in seconds. A site on the load would change both; a site on the
copy changes only the factor.

- **Key and off switch.** Those of section 11.3.2. A package leaves the form out for its title by not naming it.
- **What it changes.** After the stock copy, the destination register `fd` is mapped as the `lwc1` form maps its
  loaded register: inside $(0, 1)$ to $1 - \sqrt{1-k}$ or $\sqrt{m}$; outside it, `fd` keeps its stock value and the
  site reports once (`ENHANCEMENT_FACTOR_KEPT`). The source register `fs` keeps its stock value, so a copy onto
  itself (`fd` equal to `fs`) refuses the package, as do `mov.d` and any other instruction this section and
  section 9.2 do not name.
- **What the guest can observe.**
  - Guest registers and memory: the one FPU register `fd`, and everything computed from it. No memory is written.
    The map is the one the `lwc1` form uses: the decay in double on the host, the square root the host's
    `sqrtf` (section 9.2, "Host arithmetic").
  - VRAM bytes, readbacks, timing the guest can measure and I/O speed: unchanged, as section 11.3.2.4.
- **Tier and default.** Ace, off by default, as section 11.3.2.
- **Authentic stays byte-identical.** As section 11.3.2. `test_mapped_register_copy_keeps_its_source_when_run`
  compiles the emitted statement and runs it: with the toggle off both registers hold the stock copy; with it on,
  `fd` is the mapped factor and `fs` keeps its stock bits.
- **Sampled checking.** As section 11.3.2.
- **"Nothing worse".** As section 11.3.2.4.

#### 11.3.3. Texture Scaling and the Smart 2D Filter (Not Yet Admitted)

The two features of section 10.10 (#1273). Their presentation is described there; this entry is what the guest can
observe.

- **Status: guest-visible, not admitted under section 11.2.** Each present commits the frame it shows to guest VRAM,
  so a frame drawn with a scaled copy, or with a 1:1 draw sampled nearest by smart 2D, is the frame the guest keeps.
  The features stay outside section 11.2 until the requirements below exist. Until then they are reachable only
  through environment variables, for development.
- **Key.** None. `SR_TEX_SCALE` and `SR_SMART_2D` are environment variables, not keys on the launch line (section
  10.1), and have no library or per-title setting, so the features are not admitted.
- **Never silent.** A run with either feature on prints, after its `TEX_SCALE: on` line, one line naming the features
  as guest-visible, on through the environment and not admitted under section 11.2. The `tex-scale-selftest`
  settings cell pins it.
- **What it changes:** the frame a present commits, wherever a draw used a scaled copy or smart 2D at a render scale
  above 1x.
- **VRAM bytes:** differ in those frames, at those draws' pixels.
- **Readbacks:** differ the same way, for a GE or display readback, save picture or screenshot that covers them.
  Texture readbacks and render-to-texture results are unchanged (section 10.10).
- **Timing:** differs. Scaling runs on worker threads, but storing a finished copy is an upload on the renderer
  thread, the thread the guest runs on, and a budget reclaim drains the GE once (section 10.10), so a guest frame can
  take longer. Measured at render scale 4 with `SR_TEX_SCALE=4 SR_SMART_2D=on` against the same build with both
  off, in 200-second attract runs before the per-present store bound: *Hot Shots Tennis* presented 94.1% of its
  frame intervals at one VBLANK against 98.9% (625 two-VBLANK intervals against 99); *LittleBigPlanet* 98.7%
  against 97.1%, no loss. Storing is now bounded to 8 MiB of copies per present, the first copy of a present
  always (up to 16 MiB); the bounded build's pacing has not been measured. The VBLANK schedule, the GE timing model
  and the media clocks are unchanged.
- **I/O speed:** unchanged.
- **Tier:** Enhanced. **Default:** off, and Ace-only: in an authentic run both stay off whatever the environment says.
- **Nothing worse (section 11.2, item 4):** not shown. *Hot Shots Tennis* and *LittleBigPlanet* keep smart 2D off
  (section 10.10), and neither feature may be on by default before the runs item 4 lists exist.

#### 11.3.4. Texture Replacement (Not Yet Admitted)

The feature of section 10.12 (#1880). Its presentation is described there; this entry is what the guest can observe.

- **Status: guest-visible, not admitted under section 11.2.** Each present commits the frame it shows to guest VRAM,
  so a frame drawn with a pack's image in place of a guest texture is the frame the guest keeps. The feature stays
  outside section 11.2 until the requirements below exist. Until then it is reachable only through environment
  variables, for development.
- **Key.** None. `SR_TEX_REPLACE` and `SR_TEX_REPLACE_BUDGET_MB` are environment variables, not keys on the launch
  line (section 10.1), and have no library or per-title setting, so the feature is not admitted. The pack itself is the user's own:
  the project ships none.
- **Never silent.** A run with a pack loaded prints, after its `TEX_REPLACE: on` line, one line naming the feature as
  guest-visible, on through the environment and not admitted under section 11.2.
- **What it changes:** the frame a present commits, wherever a draw used a pack's image.
- **VRAM bytes:** differ in those frames, at those draws' pixels.
- **Readbacks:** differ the same way, for a GE or display readback, save picture or screenshot that covers them.
  Texture readbacks and render-to-texture results are unchanged: a render target is never replaced.
- **Timing:** differs. The first use of an image reads it (from a ZIP, unpacking it) and decodes its PNG on the
  renderer thread, the thread the guest runs on, and storing the image is an upload there (section 10.10, "Never waited on", bounds it to 8 MiB per present
  for scaled copies; a replacement is normally stored in the draw that finds it, so a pack of large images can
  lengthen the frames that first draw them). Not measured on a title. The VBLANK schedule, the GE timing model and the media
  clocks are unchanged.
- **I/O speed:** unchanged for the guest. The host reads the pack's files from its own disk, inside the
  pack's folder or ZIP only.
- **Tier:** Enhanced. **Default:** off, and Ace-only: in an authentic run the pack is not read whatever the
  environment says.
- **Nothing worse (section 11.2, item 4):** not shown. A pack is the user's own content, so what it does to a title
  is the pack's; the runs item 4 lists, for a title and a pack, do not exist yet, and the feature may not be on by
  default before they do.

#### 11.3.5. Sharp Game Text (`glyphs`, Not Yet Admitted)

The feature of section 10.13 (#2110). Its presentation is described there; this entry is what the guest can observe.

- **Status: guest-visible, not yet admitted under section 11.2.** Each present commits the frame it shows to guest
  VRAM, so a frame drawn with a rebuilt glyph is the frame the guest keeps. Of what section 11.2 asks, the key exists
  and the library and per-title switches and the private runs of item 4 do not (below); the admission is the owner's.
- **Key and off switch.** `glyphs=on` on the launch line (section 10.1), an optional key: a line without it, or with
  `glyphs=off`, is off and formats byte for byte as a line from before the key, and any other value refuses the line.
  It needs no `tier=ace`: it is an Enhanced part. Its off switch is the key off or missing, and an authentic run,
  which has no valid line with Ace on. It has no library or per-title setting, and the player never writes it, so a
  player launch cannot turn it on: only a title's runtime started directly, with `SR_ACE` set by hand to a line that
  carries `;glyphs=on`, can. Admission under section 11.2 needs both switches. The `SR_GLYPHS` environment
  variable it replaces is retired: set, the run refuses to start and names the key (section 10.13).
- **How it relates to the `text` key.** Not at all. `text` (section 10.1) draws the on-screen keyboard at the window's
  resolution on the presented image and changes no guest frame; `glyphs` rebuilds the game's own text inside the
  frame the game draws. Neither reads or implies the other, and `text=on` with `glyphs` off leaves a game's text as
  the game drew it.
- **The cost of adding a key to the `SR_ACE` launch line.** A required key would refuse every line an older launcher
  wrote and would have to be spelled out wherever a full line is built or checked (`src/player/player_state.h`, the
  launch-resolution, player-state, flight diff and package cache tests among them). `glyphs` is optional, as `stick`,
  `load`, `depth`, `ssaa`, `peak` and `cull` are, so it costs neither: a line without it is valid and unchanged, and only
  `ace_format` writes it, when on. What it did cost: its row in the parser's table in `src/rt/ace.c` (with `on`, its
  longest value, for `ace_longest_line`), `AceConfig` and `AceFeature`, the canonical line, the start line, the
  report's `glyphs` field, the feature words, the exit check that names a part no renderer started, and
  `ace-selftest`'s cells and worst-case line. A line with every part on is longer by `;glyphs=on` (10 characters);
  `ace-selftest` builds the worst-case report from that line and checks it fits `ACE_REPORT_MAX`, and checks that
  the longest line the key table makes fits `ACE_SPEC_MAX`. Still to come for admission: the settings pages and per-title
  precedence of sections 10.5 and 10.9.
- **Never silent.** A run with the feature on prints `ACE_MODE: glyphs on (...)` with the other Ace lines and,
  after its `GLYPHS: on` line, one line naming it as guest-visible, on through the launch line's `glyphs` key and not
  admitted under section 11.2. The `glyph-recon-selftest` settings cell pins it, and the report carries `glyphs`.
- **What it changes:** the frame a present commits, wherever a sprite draw used a rebuilt glyph at a render scale of 2
  to 4.
- **VRAM bytes:** differ in those frames, at those draws' pixels.
- **Readbacks:** differ the same way, for a GE or display readback, save picture or screenshot that covers them.
  Texture readbacks and render-to-texture results are unchanged: a render target is never a page, and a page in VRAM
  is never read. The feature adds no renderer readback. With the key off or missing, and in an authentic run, every
  glyph branch sits behind one switch that stays 0, and `gpu-glyph-selftest` shows the picture of a scene is the same
  byte for byte authentic, with no key and with `glyphs=off`, and differs with `glyphs=on`.
- **Timing:** differs. A rectangle is checked or rebuilt on the renderer thread, the thread the guest runs on, at
  most 48 per present (section 10.13), so a scene that first draws many new glyphs takes a little longer for its
  first frames. Not measured on a title. The VBLANK schedule, the GE timing model and the media clocks are unchanged.
- **I/O speed:** unchanged.
- **Tier:** Enhanced. **Default:** off: only a line with `glyphs=on` turns it on, and an authentic run has no valid
  line with Ace on.
  Turning it on by default, or putting it in a preset, needs the owner's decision first, because of the trade-off in
  section 10.13 (some small, dense or filtered text is drawn smooth, and some soft art can still rib).
- **Nothing worse (section 11.2, item 4):** not shown. The tests are synthetic. The private runs item 4 lists, before
  and after at render scale 4 on *Hot Shots Tennis* and *LittleBigPlanet*, and the library sweep, do not exist for
  this item, and it may not be on by default without them.

#### 11.3.6. Widescreen Culling Widening (`clip_x_widen`, Not Yet Admitted)

The Layer 2 mechanism of section 10.8, for a title that skips objects outside its own view before they reach the GE.

- **Status: guest-visible, not admitted under section 11.2.** It is a `guest_memory_code_hook` package (section 2.3)
  built into the title's own AOT output, and it changes guest memory.
- **Mechanism.** A package site (`transform: "clip_x_widen"`, section 9.2) names the one `jal` where the title builds
  a camera's clip matrices, by symbolic function and offset with the stock word's digest. The stock call runs
  unchanged. When it returns, generated code calls `sr_enhancement_widen_clip_x` with the pointer the call returned
  (`clip_register`) and the offsets of the title's visibility matrices (`clip_offsets`). The runtime scales the x output
  row (elements 0, 4, 8 and 12 of each column-major 4x4 float matrix) by `480 / (480 + 2D)`, where `D` is the Widescreen
  wing width (section 10.8), so a bounds test against `-w <= x <= w` accepts the same extra width the wings draw. The
  matrices the GE receives are not touched, and the vertical extent and the HUD are unchanged.
- **Key.** `cull=on` or `cull=off` on the launch line (section 10.1), an optional key: a line without it reads as
  off, and `cull=on` is refused unless the line carries `tier=ace`. The player has a library setting and a per-title
  setting, "Wider culling", each able to turn it off alone (section 11.2, item 2); the line carries `;cull=on` only for
  an Ace launch with the part on, so every other line is as it was. The runtime scales only while the run asked for
  the key and Widescreen has a wing (`D` > 0); with either missing, the stock call runs and nothing is written. The run
  names it (`ACE_MODE: cull on (...)`, the report's `cull`), and reports it unavailable when no matrix was scaled by
  the end of the run: the title's build has no culling package, or Widescreen is off. The package build is chosen by
  the title's own data (section 5): `build-package --ace-cull` adds the title's culling package, beside the timing
  package when the build has one (`--experimental-60hz` or `--enhancement-package`) and as the build's one package
  otherwise (section 9.9); the launcher picking that build for an Ace launch with the part on is a separate
  change. The player cannot tell ahead of a launch which games have a culling build, so the row's text says it
  acts only in games that do, and a run without one says so in the `ACE_MODE: cull unavailable (...)` line and
  the report. The runtime remembers at most 4096 distinct matrices (see Limits below); the report's
  `cull_untracked` counts the matrix uses it left alone once that table was full, and cull is confirmed only
  while that count is 0: a run with a nonzero count reports cull unavailable as `partial` (the matrices
  tracked before the table filled were widened, the rest were not), never as a complete run. The count is read
  again when the run ends, on every exit path that writes the report, so a table that fills in the last frame
  still reads partial.
- **What it changes:** for the matrices at `clip_offsets`, 4 floats per listed matrix (its x output row) of guest memory
  per call, only while Widescreen is on and the key is on; and so which objects the title submits, and any logic the
  title gates on the same visibility test (animation or AI updates on visible objects, fixed object or display-list
  budgets), which can change what the guest computes or draw objects in the centre that were skipped before.
- **VRAM bytes:** differ in the wings and nowhere else, as objects the title used to skip are drawn there, provided
  the package names matrices that only the title's own visibility test reads (below).
- **Readbacks:** unchanged in the centre picture on that same condition; the wings are host-only (section 10.8).
- **Timing:** differs: the title submits more objects, so its own submit time and the GE list grow. The GE timing
  model, the VBLANK schedule and the media clocks are unchanged. The one private run so far (a single title, the GPU
  renderer) measured +31% to +34% guest primitives in the wings and between +0.6 and +1.2 ms of mean frame time on a
  shared host, with a 30 Hz-class tail at one aspect that has not been re-run; these figures are indicative and do
  not establish the item's frame cost.
- **I/O speed:** unchanged.
- **Package obligations.** The mechanism cannot check these, so the package author shows them for the title: (1) the
  call at the site leaves every listed matrix describing the camera it was built for, either rewritten at each call or
  unchanged since the last one (the runtime does not scale a matrix whose x row still holds the words it wrote, so a
  matrix the call does not rebuild is not scaled twice); (2) the listed offsets are matrices only the title's visibility
  test reads, so the matrices the GE receives, and with them the centre picture, are untouched; (3) the title's logic
  that depends on what is visible is not a behaviour change to guard against, or is guarded.
- **Limits.** It cannot create what the title never modelled, what it places only inside the original view, or what
  it hides by a rule that is not a horizontal clip test (a distance, a side-of-wall or a course-direction rule). The
  wing cap of section 10.8 is unchanged. The table that keeps a matrix from being scaled twice holds 4096
  distinct matrix addresses for the whole run and is never cleared; a title whose matrices move to new
  addresses can fill it, after which new matrices are left unscaled (said once on stderr, counted in the
  report's `cull_untracked`) instead of being scaled without a record.
- **Tier:** Ace (one title). **Default:** off.
- **Nothing worse (section 11.2, item 4):** not shown. The tests are synthetic.

#### 11.3.7. Finer Bezier and Spline Patches (`tess`)

The feature of section 10.14 (#2053). How it draws is described there; this entry is what the guest can observe.

- **Status: guest-visible, and it alters GE timing under `SR_GE_TIMING=1`.** Each present commits the frame it shows
  to guest VRAM, so a frame with a finer patch is the frame the guest keeps. Its key, its library and per-title
  settings and this entry are in place (section 11.2, items 2, 3 and 6). The runs of item 4 do not exist, so it stays
  off by default and in no preset.
- **Key and off switch.** `tess=off`, `tess=2`, `tess=3` or `tess=4` on the launch line (section 10.1), a required
  key. A line without it, or with any other value, is refused, and the run is Authentic. Its off switch is `tess=off`
  in a run, and `"tess": "off"` for the library or for one game (section 10.9); an unknown value in a settings
  file reads as off, an unset value in the library block is off, an unset value for one game follows the library
  setting, and an authentic run, which has no line, never has a factor. Section 11.3.5 lists what
  adding a key to the `SR_ACE` launch line costs.
- **Never silent.** A run with it on prints, after its `PATCH_TESS: on` line, one line naming the feature as
  guest-visible and as altering GE timing under `SR_GE_TIMING=1`, and at its end a `PATCH_TESS: exit` line with the
  quads drawn and the draws the vertex cap lowered, each of which can leave a crack (section 10.14).
- **What it changes:** the pixels of every triangle patch the game draws, in the frame a present commits: the curved
  surface is drawn with F x F times the triangles.
- **VRAM bytes:** differ in those frames, at the pixels where the finer surface differs from the PSP's facets.
- **Readbacks:** differ the same way, for a GE or display readback, save picture or screenshot that covers them, and
  for a render-to-texture pass that draws patches.
- **Timing:** differs with the GE timing model on. With `SR_GE_TIMING=1` every finer triangle's rows and spans are
  charged, so factor F on a division d costs what the division d x F costs, not what the PSP's division d costs
  (section 10.14). Where the fixed row charge applies (8888 without depth test, spans under 48 pixels),
  a list with small patches completes later in guest time, and `sceGeDrawSync`, `sceGeListSync` and the finish
  interrupt wait longer, and a list with large patches can complete earlier; elsewhere the change follows the
  finer surface's coverage, in either direction. With the model off (the
  default) the GE timing is the PSP path's. The VBLANK schedule and the media clocks are unchanged either way. The
  finer triangles are host work on the GE thread, the thread the guest runs on, so a guest frame with many patches
  can take longer in host time. Not measured on a title.
- **I/O speed:** unchanged.
- **Tier:** Enhanced. **Default:** off, and Ace-only: an authentic run has no launch line, so it is never on there.
  It is in no graphics preset, because it changes GE timing.
- **Nothing worse (section 11.2, item 4):** not shown. It may not be on by default before the runs item 4 lists
  exist.

#### 11.3.10. Short Loads (`load=fast`, Not Yet Admitted)

The `load` Ace part of section 9.8, for a title that paces a loading screen with display waits of its own.

- **Status: guest-visible, not admitted under section 11.2.** It changes guest timing. It is an Ace part of the
  title's own timing package, built into that title's AOT output; its key, its library and per-title settings and
  this entry are in place (section 11.2, items 2, 3 and 6); the runs of item 4 exist for one title (below) and have
  not been reviewed.
- **Mechanism.** Each `vblank_wait_skip` site in `ace_parts.load` names one `jal` to the executable's
  `sceDisplayWaitVblankStart` import stub, by symbolic function and offset with the stock word's digest (section 9.8).
  The site's delay slot runs as stock. Generated code then asks `nk_ace_load_skip_wait()` (`src/rt/hle_display.c`). It
  skips the call only where the stock call would block: waits are permitted (interrupts and thread dispatch are
  enabled), the run's line carries `load=fast` (`nk_ace_load_fast()`, `src/rt/ace.c`, which counts the skip), and
  the pacing guard (below) has not fired. Then the call is not made. The thread instead blocks for 1 µs, the shortest delay wait the scheduler has (that of
  `sceKernelDelayThread(1)`); for that moment it is visibly in a delay wait, not a display wait. Ready threads of any
  priority, lower ones included, can then run as they would during the stock wait, and the waiter resumes at its
  next scheduling point instead of the next VBLANK. The decision its block causes always runs another ready thread
  first, even when the 1 µs is already over by then (`sched_delay_current_dispatching()`, `src/rt/sched.c`). A zero delay or a ready-queue rotation would not do this: on the
  PSP, `sceKernelDelayThread(0)` returns without yielding even to a thread of equal priority (#340), and a rotation
  yields only to threads of equal or higher priority, so a lower-priority thread would not run. `$v0` is then 0,
  which is what the stock function returns after its wait. With the key
  off, or where waits are not permitted (the stock call then returns `SCE_KERNEL_ERROR_CAN_NOT_WAIT` without
  blocking), the skip steps aside and the stock call runs. Nothing else in the title's code changes.
- **Key.** `load=fast` or `load=off` on the launch line (section 10.1), an optional key: a line without it reads as
  off, any other value is refused, and `load=fast` is refused unless the line carries `tier=ace`. The player has a
  library setting and a per-title setting, "Short loads", each able to turn it off alone (section 11.2,
  item 2); the line carries `;load=fast` only for an Ace launch with the part on, so every other line is as it was.
  The run names it (`ACE_MODE: load fast (...)` at start, `ACE_MODE: load waits skipped <n> (longest run <m> logic
  updates; ...)` at exit, the report's `load`, `load_waits_skipped`, `load_skip_max_run` and `load_paced`), and
  confirms it the first time a declared wait is skipped. A run that asked for
  it and never reached a site reports it unavailable at exit (`ACE_MODE: load unavailable (...)`, the report's
  `unavailable.load`): the title's build has no load part, or none of its declared waits ran.
- **What it changes:** only the declared display waits, and only while the key is on and the wait would block: each
  returns with 0 after the 1 µs delay wait above, instead of blocking until the next VBLANK starts. A wait called where waits are not
  permitted runs as stock and returns `CAN_NOT_WAIT`, as it would without the key. The code before and after a wait runs as stock, including any fade
  or progress step the loading loop runs between waits. VBLANK interrupts, sub-interrupt handlers,
  `sceDisplayGetVcount` and the audio threads keep running on host time. So fewer VBLANKs pass during the shortened
  stretch, and the frames the wait would have shown are not presented: a fade or a progress bar can jump or finish
  before it is seen.
- **VRAM bytes:** the loop draws what it draws in stock, but fewer of its frames reach the screen, and a step the
  title counts in VBLANKs elsewhere (a fade timed by `sceDisplayGetVcount` or a VBLANK handler) can show fewer of its
  stages in the frames that are drawn.
- **Readbacks:** unchanged for a frame the loop draws; a readback of the display or of a counter the VBLANK handler
  advances sees fewer VBLANKs per pass of the loop.
- **Timing:** differs: each skipped wait shortens its pass of the loop by up to one VBLANK of guest time, and the
  loading stretch ends sooner in host time. The VBLANK schedule, the RTC and system time, the media clocks and the GE
  timing model are unchanged. A skipped wait that paced logic updates would run them unpaced, so the logic update
  counter of section 9.5 is unaffected only if each declared wait is reached only while loading.
- **What the tools check.** A package with a load part must declare an `update_boundary`, and no site may sit in
  the boundary's own function: the validator compares function names, and site-map addresses in a published
  package, and codegen compares resolved addresses, so a second name for that function does not pass. That is all
  they check. Nothing walks the call graph: a wait in a function that calls the boundary's function (a main loop
  that calls the update) or in one that function calls in every update (a present or flip helper) is accepted.
  Keeping such waits out rests on the site's own evidence (package obligation 1, below); the pacing guard only
  limits the damage at run time.
- **Pacing guard.** A package with a load part also calls `nk_ace_load_update()` at its `update_boundary`, ungated;
  it does nothing unless the run's `load=fast` is in effect, so every other run is unchanged. The runtime counts the
  consecutive logic updates in each of which at least one declared wait was skipped. When that run reaches
  `ACE_LOAD_PACING_UPDATES` (`src/rt/ace.h`, 8 updates: four times the longest run measured on the title's loads,
  2 updates, below), the site paces updates, not a load: nothing is skipped for the rest of the run, so every declared wait
  runs its stock call, and one `ENHANCEMENT_LOAD_PACING:` line names the site, by id and by its `jal`'s address,
  whose skip reached the threshold. The report's `load_skip_max_run` is the longest run seen and `load_paced` says
  whether the guard fired; the exit line names both. Until it fires, a pacing site runs that many updates unpaced,
  so the guard does not replace the evidence of obligation 1. It sees only waits skipped in consecutive updates
  that reach the boundary: a wait in a loop that never reaches it (a menu loop with its own logic) or one skipped
  only every other update does not fire it, and a real load that spans the threshold's updates (a loading loop
  that calls the update, or a loader thread) does, which turns the part off for the run and costs nothing else.
  The threshold is one value for every title, set from one title's loads; it is provisional until a second title
  is measured.
- **I/O speed:** unchanged. Reads take what they take; only the display waits between them are shorter.
- **Package obligations.** The mechanism cannot check these, so the package author shows them for the title: (1)
  each declared wait paces a loading screen only, not a loop that runs or reaches a logic update. The tools check
  only the boundary's own function (above), so the declaration shows that each wait is reached only while loading:
  the site's callers up to the title's main loop, from its decompilation or disassembly, and a run with `load=fast`
  through the title's loads and play whose report shows `load_paced` false and a `load_skip_max_run` below the
  guard's threshold; (2) the loop makes
  progress without a VBLANK: it ends on the load's own completion, not on a count of VBLANKs it waited for; (3) nothing
  the title keeps after the load depends on how many VBLANKs passed during it.
- **Limits.** It skips only the waits a package declares: a loading screen paced by another wait
  (`sceDisplayWaitVblankStartCB`, a semaphore its VBLANK handler signals, or a thread delay) is unchanged. It does
  not make a read faster. The section adds no code and changes no word when a package has no `ace_parts`, and with
  the key off the stock call runs.
- **Tier:** Ace (one title). **Default:** off, and in no graphics preset.
- **Measured (Hot Shots Tennis, UCUS98701, one declared wait, 60 FPS package, Vulkan GE, hidden window, Windows 11,
  other builds running on the machine), at the head with the shortest-delay skip and the pacing guard.** The title
  follows each file-binder open on its main thread with three passes of a fade step and a plain display wait; the
  open has completed before them. On a scripted 12,000-VBLANK route from boot through the menus, a match load and a
  rally, `load=fast` skipped 261 waits (87 opens). The longest run of consecutive logic updates with a skip was 2
  (`load_skip_max_run`), and the guard did not fire (`load_paced` false). Only the route's picture checkpoints were
  compared, and they stop at the match-type menu. Each was reached earlier, within its checkpoint's tolerance: the
  title screen at VBLANK 1,281 instead of 1,321 (40 VBLANKs, about 0.67 s sooner), the load dialog at 2,206 instead
  of 2,250, the main menu at 2,342 instead of 2,398 (56 VBLANKs, about 0.93 s), the single-player menu at 2,638
  instead of 2,686 and the match-type menu at 3,719 instead of 3,771 (52 VBLANKs, about 0.87 s). The match load and
  the rally were not compared by checkpoint. The fade step still runs three times per open, so each screen the route
  checks after a load shows the stock picture. The title's frame-pacing wait (its package's `vblank_wait_2_to_1`
  site) is not a load site and still paces every logic update.
- **Nothing worse (section 11.2, item 4):** the run above, which has not been reviewed.

#### 11.3.11. Stick Dead-Zone Remap (`stick`)

For a title whose stick reading ignores part of the stick's throw. *Hot Shots Tennis* (UCUS98701) centres each stick
byte `b` (0 to 255) as `x = b - (b > 128) - 127`, so 128 reads +1, 127 reads 0, 0 reads -127 and 255 reads +127. It
counts an axis only when `|x| >= 64`, and it moves the player by the direction of `(x, y)` scaled by `(m - 64) / 63`
with `m = max(|x|, |y|)`, nothing while `m <= 64`: a square dead zone of 64 counts, with 64 to 127 as its live range.
This reading is of the title's stick-reading function at `0x0001e268` in its `EBOOT.BIN`: it centres each axis as
`x = b - (b > 128) - 127`, treats `max(|x|, |y|) <= 64` (a magnitude of at most 64/127) as rest, and scales by `(m / 127 - 0.503937) / 0.496063`, which
is `(m - 64) / 63`. It comes from a decompilation of that function that is not yet byte-matched (a near miss), not from
a measurement. The host has its own dead zone (the input profile's per-axis `deadzone_inner`, 7849 of 32767 by
default, inside which the byte is 128, outside which the value is not rescaled), so a gamepad had to be pushed past
half its throw before the player moved. The remap maps the host's live range onto the game's: with the host's dead
zone smaller than the game's, the first live host count reaches the edge of the game's dead zone.

- **Status: guest-visible, not admitted under section 11.2.** The guest reads changed stick values. Its key, its
  library and per-title settings and this entry are in place (section 11.2, items 2, 3 and 6); the runs of item 4 do
  not exist.
- **Mechanism.** The dead zone is the title's own data: its public title manifest's optional
  `"input": {"analog_dead_zone": {"shape": "square", "counts": G}}` (`assets/title_manifest.schema.json`; `shape` is
  only `square`, `counts` an integer from 1 to 126), checked by `tools/title_manifest.py` and the native manifest
  parser alike and carried into the generated catalog as `NkTitleEntry.analog_dead_zone_counts` (0 when none is
  declared). An overlay manifest is checked the same way and carries its own value in its entry, as it carries every
  other field: nothing is merged from a public entry, so an overlay without the block has no dead zone and the line
  carries no `stick`. The player refuses an overlay for a disc the public catalog already names, so it cannot replace
  a public title's value; an overlay loaded with an override replaces the public entry whole. The runtime remaps the
  live host stick where the controller ring latches a sample in analog mode (`src/rt/hle_route.c`), before the sample
  is stored or recorded. `D` is the host's dead zone in centred counts: the larger of the X and Y axes'
  `deadzone_inner * 255 / 65535` (rounded down) from the active input profile, 30 by default, and 0 without an SDL3
  presenter. The host's own byte conversion is not symmetric about 128, so with the default 7849 its first live
  value is +30 on the positive side and -32 on the negative side: a push of +30 is still at `m = D` and reaches the
  game at `G - 1`, and both sides first move the player at 32 counts; a `D` below 0 or of 127 or more reads as 0, and any `D` from 0 to 126 is used as it is, at or above `G`
  too. With `h = b - 128` per axis, clamped to -127..127, and `m = max(|hx|, |hy|)`:
  - `m = 0` (rest) leaves both bytes as they are, and an axis at rest keeps its byte;
  - past the host's dead zone, `m > D`: the target magnitude is `m' = G + (m - D) * (127 - G) / (127 - D)`, rounded down;
  - up to it, `0 < m <= D` (a source with less dead zone than the host's, such as the mouse): `m' = m * (G - 1) / D`,
    rounded down, still inside the game's dead zone;
  - both axes scale by `m' / m`, rounded to nearest, keep their signs (the direction is kept) and are clamped to 127,
    then go back to bytes through the exact inverse of the title's centring: `x >= 1` is `x + 128`, `x <= 0` is
    `x + 127`.

  So `m = 127` maps to 127 (bytes 0 and 255 are unchanged, and with them the keyboard's and the digital edges),
  `m = D` (for `D >= 1`) reaches the game at `G - 1`, and the game's movement scale becomes `(m - D) / (127 - D)`
  within one count. With `D < G` a push just past `D` reaches the game at `G` (scale 0). With `D >= G` the host has
  fewer live counts than the game, so the first one lands one slope step past the edge, at
  `G + floor((127 - G) / (127 - D))`: `G = 10` and `D = 30` give 9 at `m = 30` and 11 at `m = 31`; `G = 64` and
  `D = 100` give 63 at `m = 100` and 66 at `m = 101`.
  With `G = 64` and `D = 30`: `m` 0, 30, 31, 40, 64, 96 and 127 give the game 1 (rest, untouched), 63, 64,
  70, 86, 106 and 127, a scale of 0, 0, 0, 0.095, 0.349, 0.667 and 1.0 against the ideal 0, 0, 0.010, 0.103, 0.351,
  0.680 and 1.0. The curve is `ace_stick_remap` in `src/rt/ace.c`, a pure function.
- **Why square, not radial.** The title's own dead zone and scale use the Chebyshev magnitude `max(|x|, |y|)` against
  64, so the remap uses the same magnitude. A radial remap that put the host's dead-zone edge at radius `G` would
  leave a diagonal push there at about `(G / sqrt(2), G / sqrt(2))`, (45, 45) for `G = 64`, still inside the game's
  square dead zone, so diagonals would keep a dead band. With the host's dead zone smaller than the game's (`D < G`,
  the default), the Chebyshev remap puts the first live host push on the game's dead-zone edge in every direction;
  with `D >= G` it lands one slope step past the edge, as above. A per-axis remap, each axis on its own curve, would
  also fit the square dead zone and would not couple the axes, but it would bend the direction: (127, 40) would
  become (127, 70), and the title moves the player along the direction of `(x, y)`, so the player would go
  somewhere else. It is the faithful choice for this title's own dead-zone code.
- **Key.** `stick=<counts>` on the launch line (section 10.1), an optional key with the title's `G` as its value, 1 to
  126 in plain decimal: a line without it reads as off, any other value is refused, and `stick=<counts>` is refused
  unless the line carries `tier=ace`. The player has a library setting and a per-title setting, "Stick dead
  zone", each able to turn it off alone (section 11.2, item 2), off in a fresh file and in no graphics preset; the
  line carries `;stick=<counts>` only for an Ace launch with the part on of a title whose catalog entry declares a
  dead zone, so every other line is as it was. The runtime does not check the value against the title: a hand-written
  `SR_ACE` line with `stick=<counts>` applies that `G` to whatever title runs. The run names it (`ACE_MODE: stick remap <counts> (...)`, the
  report's `"stick": {"counts", "host_dead_zone", "samples_remapped"}`), and reports it unavailable when no live
  sample off rest was remapped by the end of the run.
- **What it changes:** the left-stick bytes of every live host sample off rest that the guest latches in analog mode,
  and so how far the player moves for a given push, and which small pushes count in the title's menus. Because the
  remap scales both axes by one factor of the Chebyshev magnitude, it also changes two things a per-axis remap would
  not:
  - **A round host stick is pushed outward.** With `G = 64` and `D = 30`, the host diagonal (90, 90), about the full
    throw of a round gate, reaches the game as (102, 102), a Euclidean radius of about 144 counts. The game's scale
    there is `(102 - 64) / 63`, about 0.60 (0.41 without the remap), so a full host diagonal reaches only about 0.60
    of the game's full speed; only a host value of 127 on one axis reaches 1.0.
  - **Each axis's output depends on the other.** With `y` held at 40, `x` going from 40 to 127 takes `y`'s output
    from 70 down to 40 (with `x` at 40, 60, 90 and 127, the game reads `x` as 70, 83, 102 and 127 and `y` as 70, 55,
    45 and 40), so a
    per-axis test in the title, such as its `|x| >= 64` menu step, can switch off as the other axis is pushed further.

  Whether a PSP's own stick can produce such value pairs, such as (102, 102), is not measured.
- **What it never changes:** an Authentic run, an Enhanced run and an Ace run without the key latch the bytes the
  window gave, byte for byte. A sample in digital mode is not touched. A route's stick (`SR_PADSCRIPT`) and a
  replay's recorded sample (`SR_REPLAY`) are already the guest's bytes and pass through unchanged, so a replay, and
  a pad script while nobody moves the live stick, give the same samples with the item on or off (a route step that
  does not hold the stick lets the live stick through, and that is remapped); a recording made with it on records the remapped
  bytes, which its replay hands back as they are.
- **VRAM bytes:** unchanged by the item itself; the frames differ only where the game moves differently.
- **Readbacks:** unchanged by the item itself, as for VRAM.
- **Timing:** unchanged. The sample is latched at the same time; only its stick bytes differ. The VBLANK schedule,
  the GE timing model and the media clocks are unchanged.
- **I/O speed:** unchanged.
- **Tier:** Ace (one title). **Default:** off.
- **Nothing worse (section 11.2, item 4):** not shown. The tests are synthetic (`ace-selftest`'s stick cells,
  `hle-thread-selftest`'s latch cell, `gui-present-selftest`'s dead-zone cell, the player's
  `test_play_mode_stick_settings`, and the manifest tests).

#### 11.3.12. Higher-Precision Host Depth (`depth=32`, Not Yet Admitted)

The feature of section 10.20 (#2051). How it tests is described there; this entry is what the guest can observe.

- **Status: guest-visible, not admitted under section 11.2.** Each present commits the frame it shows to guest VRAM,
  so a frame whose z-fighting the key removed is the frame the guest keeps. Its key and this entry are in place
  (section 11.2, items 3 and 6). The library and per-title settings of item 2 are not: the player does not write the
  key yet. The runs of item 4 do not exist.
- **Key and off switch.** `depth=32` or `depth=off` on the launch line (section 10.1), an optional key; a line with
  any other value is refused, and the run is Authentic. `depth=off` and a line without the key are off. An authentic
  run has no line, so it never has the 32-bit buffer.
- **Never silent.** A run with it on prints `ACE_MODE: depth 32 (...)`, and `HOST_DEPTH: on (...)` when the renderer
  makes the buffer, or `ACE_MODE: depth unavailable (...)` when the device cannot.
- **What it changes:** the colour of pixels where two surfaces lie within one 16-bit depth step of each other, in the
  ways section 10.20 lists for each depth test: z-fighting surfaces show the nearer one, a draw that writes no depth
  with LEQUAL or GEQUAL can pass up to a quarter step behind, an EQUAL pass on a different surface can fail, and a
  coplanar surface that writes depth can speckle.
- **VRAM bytes:** the colour bytes of those pixels differ, in the frames a present commits. The depth bytes do not:
  the step stored at every pixel is the one the 16-bit buffer stores (section 10.20).
- **Readbacks:** a colour readback, save picture or screenshot that covers those pixels differs the same way. A depth
  readback, and depth read through guest memory, is unchanged.
- **Timing:** unchanged. The GE timing model (`SR_GE_TIMING=1`) charges depth-tested spans whatever the host
  buffer's format. The larger buffer and its readbacks are host GPU time and memory (section 10.20).
- **I/O speed:** unchanged.
- **Tier:** Enhanced. **Default:** off, and in no graphics preset.
- **Nothing worse (section 11.2, item 4):** not shown. The tests are synthetic (`ace-selftest`, and
  `gpu-coherence-selftest`'s `host-depth`); no title has been run with it.
