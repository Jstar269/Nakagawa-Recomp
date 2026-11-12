# AOT ↔ interpreter cosimulation

A differential gate between the two ways this project can execute a guest
instruction: the statically translated native body produced by
[`tools/codegen.py`](../../tools/codegen.py), and the production interpreter floor
in [`src/rt/guest_interp.c`](../../src/rt/guest_interp.c).

It exists to answer one question precisely:

> **Where is the FIRST semantic difference between native AOT execution and the
> production interpreter, on the same guest bytes?**

```bash
mingw32-make cosim-selftest
```

```bash
mingw32-make cosim-mutants
```

## How one build produces two lanes

`generate.py` emits a source-owned synthetic PSP module into the ignored build
tree. The ordinary pipeline (`prxload.py` → `codegen.py`) turns it into a
relocated guest image plus real generated C. Both lanes then run *those same
bytes*:

| lane | how it executes | how it is selected |
| --- | --- | --- |
| `AOT` | the generated `f_<addr>` body | the cell is present in the dispatch table |
| `INTERP` | `sr_guest_interp_run()` over the loaded image | the cell is absent from the dispatch table |

Lane `INTERP` is not a test mode. It is the same seam the AOT-gap smoke reaches
at build time with `--omit-aot`, selected at run time so one build can compare
every cell. Executable-span ownership lives outside the dispatch table, so
dropping the table leaves lane `INTERP` with exactly the ownership lane `AOT`
has and none of the native bodies.

Both lanes enter through the real `dispatch()` core, so the tier-selection
policy, the executable-ownership predicate and the miss path are production code
in both runs.

## What is compared

Four independent AOT/interpreter channels, ordered from most localizing to least.
Each has been shown to be the *sole* killer of at least one mutant, so none is
decorative. Scalar FPU adds a separate reference comparison described below.

1. **Canonical instruction trace** — `sr_trace_open()` / `sr_begin()` / `sr_end()`,
   the per-instruction record the generated code already emits
   ([`tools/TRACE_FORMAT.md`](../../tools/TRACE_FORMAT.md)). `guest_interp.c` now emits the same records in the
   same order, so the first differing line names the exact guest PC, the raw
   instruction word and the registers that changed.
2. **Ordered guest writes** — captured through `sr_note_mem_write()`, the
   production last-writer hook every `MEM_W*_PC` store already reaches. Each
   record carries `pc`, address, width and a real before/after pair.
3. **Guest memory window** — the whole scratch + stack region compared byte for
   byte, so a write outside the log's view still fails.
4. **State vector** — every compared 32-bit CpuState field (`r0..r31`, `hi`,
   `lo`, `fcr31`, `fpcond`, `f0..f31`, `v0..v127`, `vfpuCtrl0..15`,
   `cop0[0]..cop0[31]`, `next_pc`, `in_delay_slot`, `flow_kind`,
   `flow_target`, and `llbit`) as one ordered list, so a report names the first differing
   field rather than "states differ".

`CpuState.pc` is deliberately **not** in the vector — see below.

## The precise PC contract

`CpuState.pc` is not a shared architectural field between the lanes, and the gate
would be dishonest if it compared them directly. What each lane actually
guarantees is asserted per lane, so a change to either fails this gate instead of
quietly redefining what `pc` means:

Generated code does not maintain a **per-instruction** architectural PC. That is not
the same as never writing `pc`, and the difference matters: `tools/codegen.py` does
assign `s->pc` at a VFPU interpreter fallback, at a conditional branch whose target is
outside the current function, and in the profile stubs. None of those shapes occurs in
this fixture, which is why the per-lane assertion below is exact for these cells — and
why adding a cell that contains one would have to revisit it rather than silently
weaken it.

| concept | lane `INTERP` | lane `AOT` |
| --- | --- | --- |
| current instruction | `pc`, advanced per instruction | **not maintained per instruction.** Each instruction's address is a literal compiled into its `sr_begin()` call — which is why the trace, not `CpuState`, localizes an AOT divergence |
| next instruction | `next_pc` unmaintained | `next_pc` unmaintained |
| branch owner | the branch owns its delay slot: condition and transfer target are read at the branch, the link register is written before the slot, and the AOT tier is not reconsulted at `pc+4` | same ordering, emitted statement by statement |
| delay slot | `in_delay_slot` unmaintained | `in_delay_slot` unmaintained |
| AOT handoff | — | `jr $ra` is the host return after its target is latched; a linked call resumes only when that target matches its explicit resume PC, otherwise AOT stops at a named fail-closed boundary |
| interpreter handoff | writes `pc = destination` before dispatching into the registered native body | — |
| exception future | asserted to leave `next_pc` and `in_delay_slot` at their seeded values | same, so a future COP0 BD/EPC model can define them without colliding with an existing consumer |

The comparator normalizes both lanes to a single **handoff target** — `$ra` for
`AOT`, `pc` for `INTERP` — and requires them to agree. No field was repurposed
and no consumer of `pc` changed.

That normalization has a precondition, and the gate now checks it rather than assuming
it: lane `AOT`'s `$ra` is read **at termination**, so it is the transfer's latched
target only while nothing rewrote `$ra` afterwards. A return delay slot that writes
`$ra` would make the comparator report a destination control never reached. The
canonical trace already records which registers each instruction changed, so the final
delay slot's line is inspected and the cell fails with that explanation instead of
silently comparing the wrong thing.

## Cross-tier cells

`xcall`, `xtail` and `xnest` add a third lane, `MIXED`: every native body **except one**. That is
the seam the production AOT-gap floor actually crosses — native caller, dispatch miss,
production interpreter, back — and it is the one shape a whole-cell lane cannot express.
`install_lane_mixed()` refuses to run if the omitted address was never registered, so a
cell cannot degrade into a second all-native run and pass vacuously.

* `xtail` is the **control**: a tail transfer into an interpreted middle that stores and
  tail-transfers on to a registered entry which reads the store back. It is an ordinary
  PASS, and it is the shape `fixtures/production_smoke` (aot-gap) and the platform ladder
  already use.
* `xcall` is the **returning call**, which no other gate exercises. Generated code emits
  `dispatch(s, _t);` for a `jal`/`jalr` with no host return after it, so the caller's
  native frame is still live. It is an ordinary PASS.
* `xnest` turns `xcall` inside out: a native caller reaches an **interpreted** middle
  (`xnestmid`, the omission) whose own `jalr` calls a **registered** leaf. The guest
  expects the middle to resume at its link address, mark the accumulator and unwind its
  frame. The interpreter used to treat that linked transfer like a tail and hand off for
  good, so the middle's continuation never ran, `$sp` stayed 8 bytes low because the
  middle never unwound, and the native caller reloaded `$ra` from its own unused slot; a linked transfer into a registered body is now a CALL, as it
  is in generated code. It is an ordinary PASS, and against the earlier interpreter it
  diverges on `r2` and the handoff target.

* `lj` is **`longjmp` into a live frame**. `lj_f` saves `$sp` and `$ra` with `setjmp`
  (`lj_set`, reached through `jalr`, which lane MIXED leaves to the interpreter), then calls
  `lj_g`, which calls `longjmp` (`lj_jmp`). `longjmp` reloads `lj_f`'s `$sp` and `$ra` and
  returns to the word after the `setjmp` call, a call that returned long before. In the
  native lanes no live linked call resumes there: the runtime abandons `lj_g`, finds `lj_f`'s
  frame live inside `lj`'s call (its callee entry `$sp` is above the restored one), and runs
  the rest of `lj_f` through the interpreter floor as that call's continuation; `lj`'s own
  continuation then runs once natively. Every lane ends with `v0` = 0x1107 and a balanced
  `$sp`; the stack census counts the two abandoned frames per native lane as excluded.

`xcall` is also this gate's first worked example of a declared defect retiring itself.
It was added while the interpreter stopped only at a registered *entry*, so an
interpreted callee's `jr $ra` landed in the caller's interior and the caller's tail ran
twice — double-counting its work and reloading `$ra` from an already-popped frame. That
was recorded as a `CosimKnownDefect` naming exactly `r2` and `r31`.

A `CosimKnownDefect` is enforced in **both** directions: a declared field that stops
differing fails the gate just as loudly as an undeclared one that starts. When
[`rt: preserve AOT continuations across interpreter calls`][pr127] gave the interpreter
an explicit call boundary, this gate went red demanding the declaration be deleted —
which is what happened. Nothing had to remember to revisit it.

[pr127]: https://github.com/Jstar269/nakagawa-recomp/pull/127

Two details from that episode are worth keeping. `$sp` was deliberately *never* declared
even though it is also unwound twice: the generated entry epilogue restores it from
`_sp_entry`, which masks the leak, so naming it would itself have failed the gate. And
the declaration was written from measurement rather than expectation — the first draft
named `r29` and the comparator rejected it.

## Resume at a PC

```bash
mingw32-make cosim-selftest-resume
```

This lane translates the same cells with `codegen.py --state-resume` and adds the test of the
resume-at-PC contract (#2172,
[`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md#resume-at-a-pc-direct-save-states)). It is a
comparison of generated code with generated code: a run that starts from a parked thread's data
against the run that was never interrupted.

The harness has no scheduler, so its `sr_yield` is the park. A thread is parked once `sr_yield`'s
own switch has switched it out, and the harness does what that switch does to the call records:
it swaps them out of the running side, as `sched_run` does around every fiber switch, describes
the swapped-out records and swaps them back. A case turns the slice counter on and starts it at
`n`, so the `n`-th yield point the run passes enters `sr_yield`; there the harness copies the
registers and the memory window as a save state would, takes the frame list that way, and
returns. One run is therefore both the uninterrupted reference and the source of the snapshot. The resumed run re-registers every body, empties the return-boundary stack,
writes the parked memory and registers back and calls `dispatch_resume_from_host` with the frame
list: no host frame of the first run exists in it. Every yield point of a case is tried.

Compared, all exactly: the whole `CpuState` (`pc` too, since neither run is the interpreter), the
memory window, the guest writes after the park, the instruction trace after the park, and the
slice counter, which catches a resume that counted a yield point the uninterrupted run did not.
The reference run writes its trace to a second file from the park on, so the two traces start at
the same instruction and must be equal in length and line for line.

Each resume is then repeated with the slice counter at 1, so the resumed thread parks again at
its first yield point. That second park (registers, memory, the description and the frame list)
must equal the second park of an uninterrupted run that parked at the same two points. This is the
check on the rebuilt call records: a resumed thread has to be describable like any other. The
reference parks twice too, because a park writes `s->pc`.

| case | what its yield points cover |
| --- | --- |
| `rs` | `rs` calls `rs_mid` in a loop, `rs_mid` calls `rs_inner` from two call sites, `rs_inner` calls leaf A in a loop: parks at an entry four frames deep, at a loop edge three deep, and at a loop edge in the outermost frame. `rs + 0x14` is a call's return PC and that loop's target, so both kinds of point exist at one PC |
| `link` | a direct call in a conventional frame |
| `zlink` | a direct call behind a taken linked branch: the point's label sits inside the branch's block |
| `rs_tail` | its own entry resumes; `rs_inner` then runs under a tail drain and every park there must be refused as `tail-chain-live` |
| `linkr` | its own entry resumes; its `jalr` goes through the dispatcher and every park under it must be refused as `frame-not-described` |
| `xtailmid` | entered by the host with no translation of its own (lane `MIXED`): the interpreter runs it and hands the frame to `xread`, whose park must be refused as `root-not-host-entry` in every build |
| `flow-again` | the host enters the harness's trampoline, which leaves exception flow to `rs`; the dispatcher dispatches `rs` under the same call, and every park in it and under it must be refused as `root-not-host-entry` in every build |

The first yield point of every case is the dispatcher's own, before any call record exists, and
must be refused as `no-frames`; `xtailmid` and `flow-again` meet two of them.

Two fail-closed corpora run from one snapshot four frames deep. Frame lists the build cannot
resume (no frames, an unknown kind, a call point innermost, a parked point below a frame, an
unregistered body, a registered body with no points, a return PC that is no call's, a call's
return PC offered as a parked point, a frame the call below did not enter, a resume while one is
in progress, a fiber that already holds a call record) must each be refused by name with the registers, the memory, the trace, the
write log and the slice counter untouched; the body with no points must not be called. Call
records the frame list cannot describe are built from the same snapshot, first checked to
describe as exactly that list, then changed one field at a time, and each must be refused by
name. Five more refusals are staged at the live park of each `rs` yield point: another
`CpuState`, room for one frame, an open callback scope, and two for the running fiber. Inside
the yield, before any switch, the body has named its point and the running fiber must still be
refused as `not-parked`; the hook then services an interrupt the way `sr_yield` does (a handler in
a scope on the same fiber) and the fiber must be refused again. These two show only that the
running fiber is never described; which switch parks a thread is the scheduler test's to show. One more is a
live run of its own: a
callback entered, in a scope, while the thread's own dispatch has begun no call record; its body
reaches a yield point there and must be refused as `callback-or-interrupt-live` in every build,
though its call record is the fiber's first and a host entry's own.

The ordinary lanes run the same cases on output without resume points. There every thread must be
refused, and a hand-written frame for `rs` must be refused as `body-not-resumable`.

The resume cases run last, and only when the phases before them reported no divergence: a resumed
run is compared with the uninterrupted one, which means nothing once the lanes disagree, and a run
that has already failed keeps its `cosim: FAIL` verdict instead of entering resumed runs, which
have no step bound of their own.

The three frames of the `rs` chain run nested in lane INTERP during the cell comparison, so they
are written to end under every interpreter mutant: the loop counter of `rs` steps outside a delay
slot, after the loop head and before the call, and the three frames pop their stack before
`jr $ra` instead of in its slot. With the counter in a `jal` slot and the pops in the return
slots, the `skip-delay-slot` mutant never left `rs` (a callee that keeps its frame hands its caller
the callee's saved `$ra`), and the mutation gate could not count the run as a kill.

What this does not prove: the harness's park is a stub and it has one thread, so nothing here
runs the real `sr_yield` or describes another fiber's records, and the uninterrupted run and the
snapshot come from one process. `mingw32-make aot-return-boundary-thread-selftest` covers the
first two with the real scheduler (`test_resume_describe_from_the_scheduler` in
`tests/native/hle_thread_selftest.c`): a thread whose yield services an alarm and then switches to
a peer is refused from the alarm handler and described from the peer, on call records the test
begins by hand, since that harness has no generated bodies. A thread parked in a blocking import,
under a dispatched call or under a tail drain is refused, not resumed.

## Fail-closed negative corpus

The comparison above only ever exercises *successful* execution, so none of the
interpreter's refusals were covered: six safety guards could be deleted with every cell
still agreeing. These are **interpreter-tier** assertions rather than lane comparisons,
because lane `AOT` is deliberately more permissive — `MEM_*` absorbs an out-of-range
access through `sr_oor()` — and teaching it to manufacture matching faults purely to fit
a two-lane shape would be fabricating agreement.

Each case asserts the exact `SrGuestInterpResult`, the fault record, that no guest byte
in the observed window changed, and that architectural state changed in exactly the
declared ways. Alongside them:

* a **sweep** of all 14 control encodings, each placed in a delay slot, so the property
  asserted is "no control encoding is ever decoded as arithmetic" rather than one
  representative of it;
* the `jalr rd, rs` **link-register shape**, which the two-lane comparison structurally
  cannot reach (a callee entered through a link register other than `$ra` cannot return
  under the AOT frame model);
* an **interpreter form census** — see below.

Every negative program terminates even when the guard it tests is deleted. The
interpreter deliberately has no instruction cap, so a program that transfers back into
the scratch pad would turn a removed guard into a hang instead of a failure; transfers
name the registered trampoline, and a `break` sentinel bounds every straight-line run.
That is not hypothetical: the first version of the delay-slot case did hang under the
`skip-delay-slot` mutant.

## The form census

"Every form the interpreter implements exists because a cell executes it" used to be
enforced from one side only — a hand-maintained list describing the *fixture*. Nothing
checked the interpreter, so an opcode added with no cell behind it would have gone
unnoticed, which is exactly the speculative coverage the claim rules out.

The harness now asks the production decoder directly. Each candidate encoding is
executed as a real instruction; a form the interpreter does not decode fails closed as
`SR_GUEST_INTERP_UNSUPPORTED` *at that pc*, and anything else means it was decoded. The
decoded set must equal the set the cells execute, **in both directions**.

It earned its keep immediately: `mfc1`/`mtc1` dispatched on `fmt` alone, so every
reserved COP1 encoding sharing that `fmt` decoded as a register move. `src/rt/guest_interp.c`
now requires the defined-zero function field as well.

## The cells

Each cell is a self-contained guest function that relinquishes control through a
register transfer, so "the cell returned" is a deterministic synchronization point
in both lanes. Most end in `jr $ra`; `jrtail` ends in a computed tail call and
returns through its callee's `jr $ra` instead.

| cell | question |
| --- | --- |
| `alu` | three-operand and immediate ALU, all three shift kinds, signed vs unsigned compare |
| `r0` | `$r0` write suppression across the immediate, register and shift forms |
| `ldst` | word/half/byte traffic, sign vs zero extension, sub-word store lanes |
| `branch` | conditional branch delay-slot ownership, taken and not taken, condition read before the slot |
| `branch_likely` | all nine compiler branch-likely forms, with real taken slots and annulled not-taken slots |
| `bitops` | `ext`/`ins` edge sizes, `seb`/`seh`, aliased `movn`/`movz`, and unsigned-to-float via `bltzl` |
| `imm` | `addi`, `slti`, `sltiu` (unsigned against the sign-extended immediate), `andi`, `xori` |
| `alu_more` | `sllv`/`srlv`/`rotrv`/`srav` with masked amounts, `nor`, `add`/`sub`, `clz`/`clo`, `max`/`min` |
| `muldiv` | `div`/`divu`, including the PSP's zero-divisor and overflow results, `mthi`/`mtlo`, `madd`/`maddu`/`msub`/`msubu` |
| `bshfl` | `wsbh`, `wsbw`, `bitrev` |
| `zbranch` | `bltz`/`bgez`/`blez`/`bgtz`, each taken and not taken |
| `zlink` | `bltzal` calling a leaf, and a not-taken `bgezal` that still links `$ra` |
| `jump` | direct `j` with its slot, and a word only a mis-computed target could reach |
| `jthunk` / `jtarget` | a known-entry direct `j` tail crossing with a this-adjusting delay slot |
| `bjoin` | words after a `b` (`beq $zero, $zero`) that only a later backward branch reaches, beside a not-taken `beq` whose fall-through is the rest of the cell |
| `bslot` / `jslot` | a branch into the delay slot of `b` / `j`: the slot runs as an ordinary word and the guest continues after it |
| `bself` | `b` to its own delay slot: the slot runs twice |
| `link` / `linkr` | `jal` and `jalr` link semantics with a conventional `$ra`-preserving frame |
| `jrslot` / `jrtail` | a computed call and a computed tail call whose delay slots rewrite the target register |
| `hilo` | `HI`/`LO` through signed and unsigned multiply (they differ in the high word only) |
| `llsc` | `ll`/`sc` and the link bit: an unlinked `sc` fails, a linked one stores and reports 1, `sc` leaves the link set, `ll $zero` links, an atomic-increment retry loop, and `sc` whose `rt` is its own base |
| `fpu` | scalar FPU over the #120 helper path, re-run under all four FCR31 rounding modes and with FS set |
| `fpu_moves` | raw `mtc1`/`mov.s`/`mfc1` and `swc1`/`lwc1` traffic for NaNs, denormals, negative zero and integer aliases |
| `fpu_compare` | all `c.cond.s` predicates with quiet/signaling NaNs on either operand side; checks FCC0 results |
| `fpu_aot` | AOT-only instruction inventory checked against the independent scalar-FPU reference |
| `rs` / `rs_mid` / `rs_inner` / `rs_tail` | **resume at a PC**: a three-deep call chain whose loops park, and a TAIL transfer into a body that parks (see above) |
| `xcall` / `xtail` / `xnest` | **cross-tier** — a returning call and a tail transfer that each drop into the interpreter mid-run and hand back, and an interpreted caller that calls back into a registered body |
| `spleak` | **positive control** — an unbalanced `$sp` epilogue |

`spleak` is not a defect fixture. It pins the one architectural asymmetry the
lanes genuinely have: generated code closes every callable entry with
`s->r[29] = _sp_entry` on an o32 callee-saved-SP assumption, while the
interpreter executes only the instructions present. The cell declares that `r29`
must differ — and *nothing else* — so a comparator that stopped detecting it
fails, and so does one that started reporting extra fields.

## Independent scalar-FPU reference

[`fpu_reference.c`](fpu_reference.c) evaluates binary32 words with explicit
IEEE-754 classification, 512-bit integer significands, integer square root and
division, directed rounding, conversions, comparisons and branch predicates. It
does not include `fp_convert.h`, call an `sr_fpu_*` helper, use a host
floating-point expression or consult the host rounding environment.

The fixture entry calls `fpu_aot` so real codegen discovers its body; the
ordinary AOT/interpreter comparison and interpreter form census exclude this
AOT-only cell. The oracle then executes its generated body and compares the
results, FCC0 and FCR31 flow with the reference. It covers `add.s`, `sub.s`,
`mul.s`, `div.s`, `sqrt.s`, `abs.s`, `mov.s`, `neg.s`, `round.w.s`, `trunc.w.s`,
`ceil.w.s`, `floor.w.s`, `cvt.w.s`, `cvt.s.w`, all 16 `c.cond.s` predicates,
`bc1t`, `bc1f`, `cfc1.s` and `ctc1.s`. Direct helper checks cover the same
production kernels without letting shared code provide its own expected result.

The two-tier `fpu` cell checks `add.s`, `mul.s` and `cvt.w.s`; `fpu_moves` checks
raw scalar-FPU transfers (`mtc1`, `mfc1`, `mov.s`, `lwc1`, `swc1`), `fpu_compare`
checks all sixteen `c.cond.s` predicates through FCC0 and `bc1f`, and `bitops`
checks `cvt.s.w` in the unsigned-to-float idiom. The two-tier `branch_likely`
cell also executes `bc1tl`/`bc1fl`. These cells run the project-owned AOT and
interpreter implementations over the same guest words.
The two-tier `fpu_interp` cell also covers `sub.s`, `div.s`, unary and word
conversion forms, and FCR31 control moves. The independent reference oracle
checks a wider corpus of scalar operands, rounding modes and comparisons.
There is no scalar FPU madd/msub implementation to claim: the similarly named
instructions in the fixture are integer HI/LO `madd`/`msub`.

The 38-value corpus is the Cartesian product of both operands, four FCR31 rounding
modes and FS clear/set: 11,552 contexts and 901,056 exact result checks per run.
It includes signed zero, both infinities, quiet/signaling NaNs, both subnormal
edges, the inexact subnormal-to-minimum-normal tie, conversion limits,
directed-rounding ties, maximum finite values and the repository's PSP rounding
anchors.

A reference result with `EXPECTED_UNKNOWN (#312)` is counted and reported but not
treated as agreement. The boundary is explicit: unmeasured NaN payload/sign
propagation and signaling-NaN behavior, exact subnormal results with FS set,
non-nearest-even `sqrt.s` PSP behavior, and signaling/NaN behavior for the upper
eight `c.cond.s` predicates. COP1 exception flags and enables are outside this
oracle. None of those cases is guessed or counted as a pass.

### A constraint the fixture had to obey

Generated code models `jal`/`jalr` as a host **call**: the callee runs as a
nested C frame and its `jr $ra` is a host return after the latched guest target
is checked against the call's explicit resume PC. A mismatch stops before the
native caller continuation; it is not translated into an arbitrary cross-frame
host jump. A computed transfer whose
target is *not* a registered function entry therefore returns into the middle of
an already-executing native body and double-executes it. Every computed transfer
in this fixture consequently targets a leaf that ends in `jr $ra`. A `jalr`
naming a link register other than `$ra` cannot be exercised coherently under that
model and is recorded as the next expansion rather than faked.

## What this proves, and what it does not

* Integer, memory and control-flow semantics are **independently implemented** in
  the two lanes, so a disagreement is real evidence.
* The listed scalar FPU forms now have an **independent integer reference** for
  the generated AOT body and the production helper calls. The shared-helper
  `fpu` cell still covers operand selection, register indexing and FCR31
  threading between AOT and the interpreter.
* This remains a **software/white-box oracle**, not a physical-PSP oracle. Real
  codegen, the production dispatch core and production helpers execute, but the
  `CpuState` corpus, dispatch-table reset and cell entry are test-specific.
  `EXPECTED_UNKNOWN (#312)` cases remain explicitly unverified.
* The interpreter is deliberately **more fail-closed** than the generated code on
  out-of-range and misaligned data access. The cells stay in range, so this never
  shows up as a divergence; it is a documented lane asymmetry, not a comparison.

## Proving the comparator is load-bearing

`mutate.py` applies one semantic defect at a time to a *copy* of the
interpreter, `src/rt/fp_convert.h`, `tools/codegen.py`, or `src/rt/sched.c` under
the ignored build tree. The scheduler mutant removes both per-thread AOT boundary
swaps and runs the HLE-thread regression with the real scheduler, recomp.c, and
blocking `sceKernelWaitSema`; it must fail with `AOT_RETURN_TARGET_MISMATCH`. The
other mutants rebuild the cosim harness through the corresponding Make override.
A mutant that only breaks the build is reported `INVALID` and fails the campaign:
a compile error proves the compiler noticed, not the comparator.

Each gate is a serial build, so the campaign runs in parallel lanes, one per CPU
(`--jobs` overrides the count). Every lane builds beneath its own `BUILD_ROOT` under
`build/cosim/lanes/` and must pass the unmutated gate there before it judges a
mutant; the scheduler mutants share one lane. Results are reported in campaign order,
and the default `build/` trees are left untouched.

Mutating both sides matters. A differential proven against one lane is half
proven, so one mutant restores the **generator** to its pre-fix
computed-transfer emission order — the exact production defect this fixture was
built to find — and `jrslot`/`jrtail` must both diverge. Four additional
`fp-convert` mutants break nearest-even ties, positive conversion saturation,
infinity-times-zero canonicalization and the unordered `C.cond` predicate.
The branch-likely and Allegrex bit-operation cells carry their own interpreter
mutants (an unannulled slot, an inverted `bc1` sense, an inverted `movn`, an
off-by-one `ext` size, an `ins` that ORs instead of replacing, a `seb` that does not
sign-extend, and a `mov.s` that quiets a signaling NaN) and two generator mutants (a
branch-likely slot that always runs, an `ins` that keeps the old field). Two more
generator mutants break the body walk (#1730): one gives every `beq` no not-taken
successor, and one drops the continuation after a delay slot that a branch lands on,
with the emitter's own refusal removed so the kill is a divergence, not a build failure.
The campaign counts any divergence as a kill; the first it reports for these is in
`bjoin` (its not-taken `beq`) and in `bslot` respectively, the cells built for them.
Making `beql $zero, $zero` a terminator too would be an equivalent mutant (its slot
and target are the same either way when taken) and is not claimed.

Six more generator mutants damage the pass that writes resume points, and run in a lane of
their own against the resume lane (`--state-resume`): a call point that continues without running
the frame above it, a loop edge that parks naming its own PC instead of its target (killed as a
description the build then refuses), a loop point that runs the body from its entry (a point that
exists and validates, killed by the comparison), an entry that
counts its yield point again when it is resumed, a resumed body that takes the live `$sp` for its
entry `$sp`, and a call whose resume label sits after its drain and end check. The entry mutant
changes no register and no byte of memory: only the yield-point count and the second park catch it.
The rule that a point's label sits at function scope has no mutant: the fixture has no body it
would change, so `StateResumeTests` in `tools/test_codegen_continuations.py` pins it on constructed
bodies.

Two interpreter guards are not killed by this gate, and each is layering rather than a
hole:

* deleting the `is_control_opcode()` guard inside `execute_noncontrol()` changes nothing
  observable, because every control encoding also lacks an arithmetic handler and fails
  closed through the default path. The sweep asserts the property; the guard becomes the
  load-bearing layer only once a primary opcode is implemented that a control encoding
  shares.
* `sr_exec_span_owns_fetch()` is the *dispatch* authority predicate, not the
  interpreter's fetch path — `tests/native/dispatch_isolation_selftest.c` owns it, and it does
  fail when that predicate is weakened (measured).

The campaign has already earned its keep: the `allow-r0-write` mutant **survived**
the first run, because the `r0` cell ended with a `nop` — which encodes as
`sll $zero, $zero, 0` and repaired `$r0` on the way out. No guest read can observe
`$r0` either, so the cell was silently vacuous.
`tools/test_cosim_fixture.py` now pins the fix.

## Files

| path | role |
| --- | --- |
| `generate.py` | the fixture recipe: guest module, relocations, FPU corpus and generated C manifests |
| `cosim_selftest.c` | the AOT/interpreter comparator and scalar-FPU reference harness |
| `fpu_reference.c` / `fpu_reference.h` | the integer-only binary32 expected-result evaluator |
| `mutate.py` | the mutation campaign driver |
| `../../tools/test_cosim_fixture.py` | structural gates that need no toolchain |

No binary fixture is committed. `generate.py` is the source of truth; everything
it emits lands under the Git-ignored `build/` tree.
