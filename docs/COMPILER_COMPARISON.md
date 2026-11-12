# GCC and Clang comparison

This snapshot records issue #317 on Windows 11 with MSYS2 UCRT64. The experiment used only
public synthetic fixtures. No game, private input, hardware or title route was used.

BASE_SHA: `a39f7a14ce223eaa899967b3458c926e0350227a`
EXPERIMENT_SHA: `a39f7a14ce223eaa899967b3458c926e0350227a`

The matrix was re-run on 2026-11-11 from a clean `origin/main` checkout, after #2381 fixed the
lld link of the executable HLE harness. The veto and measurement tables are what
`tools/compiler_compare.py` printed for that run (`build/c317-r3/compiler_compare.json`); nothing
was changed to make a row pass. The wall time, load figures and the notes under each failing row
are the operator's observations, marked as such.

## Command

The matrix ran through the machine RAM lock (`ramlock.py run`, one slot). Make built serially at
`-j1`. `PATH` began with UCRT64's `bin` directory, then Git for Windows' `usr\bin` for the POSIX
`sh` the Makefile recipes need ([PATH order](SETUP.md#path-order)). A first attempt without
that `sh` stopped in `native-core-objects-core` (`'true' is not recognized`) for the GCC rows; it was
an environment error and its result is discarded.

```powershell
python tools/compiler_compare.py --make C:/msys64/ucrt64/bin/mingw32-make.exe --compilers gcc,clang --linkers default,lld --repeats 3 --output build/c317-r3
```

Operator observations, not tool output: the run took 3 h 6 min of wall time while other agent
sessions built and tested on the same machine. Host CPU load sampled once at the start was 80% and
at the end 56%, and other sessions held other RAM-lock slots at the start, so the compile and link
times below may carry that contention and are not clean timings.

## Toolchains and flags

| Component | Version and target |
| --- | --- |
| Python | 3.14.8 |
| GNU Make | 4.4.1, x86_64-w64-mingw32 |
| GCC | 16.2.0, x86_64-w64-mingw32 |
| Clang | 23.1.3, x86_64-w64-windows-gnu |
| Default linker | GNU ld 2.47.20260726 |
| lld | 23.1.3 |

Each row used `-O0`, `-fno-fast-math`, `-fno-associative-math` and
`-ffp-contract=off`. The lld rows added `-fuse-ld=lld`. GCC-only
`-ftrack-macro-expansion=0` was omitted from both compiler rows. The default Makefile compiler
remains GCC.

## Veto results

| Compiler/linker | Result | Gates passed before the failure | First failure |
| --- | --- | --- | --- |
| GCC/default | MEASURED | All ten veto gates | None; the semantic hash is `UNSTABLE` (see Measurements) |
| GCC/lld | FAILED_VETO | production-smoke, production-smoke-gap, display-smoke, cosim-selftest, cosim-mutants | `native-core-tests`: `ld.lld: error: undefined symbol: sr_ge_vram_read_prepare` |
| Clang/default | FAILED_VETO | production-smoke, production-smoke-gap | `display-smoke`: `src/rt/videorec.c:83:17: error: variable 's_cur' set but not used [-Werror,-Wunused-but-set-global]` |
| Clang/lld | FAILED_VETO | production-smoke, production-smoke-gap | `display-smoke`: `src/rt/videorec.c:83:17: error: variable 's_cur' set but not used [-Werror,-Wunused-but-set-global]` |

**GCC/lld.** The tool's own `first_error` field for this gate reads `Clay error 1`: its pattern
matched an unrelated line of the gate output (a follow-up for the extractor). The first failing
reason below is taken from the gate output itself. The failing recipe is `profiler-selftest`
(`mk/selftests.mk:72`), reached by `native-core-tests`. The HLE harness link that #2381 fixed now
passes. Besides the test driver, the profiler selftest links an explicit source list (`recomp.c`,
`sr_coro.c`, `flight_recorder.c`, `guest_interp.c`, `cpu_lle.c`, `domain_mode.c`, `stale_code.c`,
`title_config.c`, `perf.c`) with `-Wl,--gc-sections`. GCC/default passed the same
`native-core-tests` gate, which includes this recipe, so GNU ld linked it. lld's first 20 errors name `sr_ge_vram_read_prepare`,
`sr_ge_vram_write_prepare`, `sr_ge_vram_write_commit`, `sr_get_ge_status`, `sr_note_mem_write`,
`sched_current_uid`, `sr_vfpu_load`, `sr_vfpu_interp`, `sr_yield`, `sr_hle_resolve_late_import`,
`sr_syscall`, `sr_timeslice`, `sr_sched_on` and the `sr_*_lut*` tables, then stops at lld's default
error limit. `sr_ge_vram_read_prepare` is defined in `src/rt/ge.c`, which this recipe does not
link. The cause is recorded here without changing the build to make lld pass.

**Clang.** In both Clang rows the display-smoke fixture itself printed `PASS` lines (60 frames,
`DISPLAY_SMOKE_VRAMDUMP` `present_bytes=IDENTICAL png_pixels=IDENTICAL`, the flight-recorder
smoke), then the `-Werror` compile of `src/rt/videorec.c` inside the `display-smoke-run` recipe
(`mk/smoke_gates.mk:93`) failed on Clang's `-Wunused-but-set-global`: `s_cur` (line 83) and
`s_clock_deterministic` (line 718). This is a project and toolchain
incompatibility; no compiler-specific guest-semantics workaround was added. The gates after
display-smoke did not run for either Clang row.

## Measurements

Only GCC/default passed the correctness veto, so only it has measurements. The other rows have
none.

| GCC/default measurement | Minimum | Median | Maximum |
| --- | ---: | ---: | ---: |
| Generated-C compile wall time (s) | 20.263 | 21.427 | 23.187 |
| Peak working set (bytes) | 53,178,368 | 56,434,688 | 63,303,680 |
| Link wall time (s) | 11.723 | 11.885 | 12.621 |
| Object bytes | 17,472,005 | 17,472,005 | 17,472,005 |
| Executable bytes | 15,785,996 | 15,785,996 | 15,785,996 |

**The semantic hash is `UNSTABLE`.** The three repeats of GCC/default produced three different
hashes: `48115df0…0f7c`, `401da44f…dea0` and `071b6204…8b7b`. The tool therefore reports no
`semantic_sha256` for the row and the comparison across all four rows is `INCOMPLETE`. The earlier
snapshot (2026-11-07) matched across its three repeats. The tool does not
keep the raw per-repeat output, so which normalised field differs is not yet known.

All nine GCC/default runtime workloads passed. Elapsed times are above the earlier snapshot's; the
machine load noted above may account for it, and they are not comparable with it (different
commit, larger executable). The `SR_PERF` summary paths for the six workloads that write one are in
`build/c317-r3/compiler_compare.json`; the three platform-ladder rows are timed only. The
generated `perf.json` files are local build artifacts and are not committed. No cross-compiler runtime comparison is available because neither
Clang row passed the veto.

| Runtime workload | Status | Elapsed wall time (s) |
| --- | --- | ---: |
| production-smoke-gap | PASS | 360.028 |
| cosim | PASS | 67.656 |
| psmf-media | PASS | 126.055 |
| audio | PASS | 16.792 |
| audio-resampler | PASS | 14.479 |
| atrac3p | PASS | 16.805 |
| platform-ladder-gap | PASS | 178.338 |
| platform-ladder-sched | PASS | 183.477 |
| platform-ladder-fs | PASS | 157.353 |

The three platform-ladder workloads use fixed trees under `build/platform-ladder` and do not
honor the row's `BUILD_ROOT`. `run_perf_benchmarks.py` runs all workload rows sequentially, so
these fixed trees are shared across toolchain rows but are not built concurrently. Make does not
track `LDFLAGS`, so a later row's platform-ladder timings may come from an earlier row's binaries;
they are not per-toolchain evidence until those trees are isolated. (This paragraph is unchanged
from the earlier snapshot and was not re-verified in this run.)

## What remains for #317

1. GCC/default: find the field that differs between repeats (keep the raw per-repeat output, diff
   the first divergent line, then normalise it or fix its source). Until the hash is stable no
   cross-toolchain semantic comparison is possible.
2. GCC/lld: make `profiler-selftest` link under lld (it references symbols defined in sources its
   list omits, such as `src/rt/ge.c`), then re-run to reach the gates after `native-core-tests`.
   Also fix the extractor's `first_error` (`Clay error 1` above).
3. Clang: `src/rt/videorec.c` `s_cur` (line 83) and `s_clock_deterministic` (line 718) are set but
   never read; then re-run to reach the later gates.
4. Repeat on a quiet machine for clean compile and link times.

This table does not establish a compiler winner or behavior equivalence. A default-toolchain change
still needs repeatable results for both compilers and linkers, plus the runtime and residency
evidence requested by #317. AOT/interpreter residency, sanitizer comparison, hosted CI,
private-title acceptance and hardware acceptance are `NOT_RUN`.
