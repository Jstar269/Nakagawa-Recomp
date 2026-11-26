# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Curated classification metadata for the HLE registration manifest.

Plain Python data module on purpose, matching tools/compat_overrides.py: this
file is meant to be read and reviewed by humans in diffs, without a
serialization round-trip. tools/hle_manifest.py cross-checks every
entry here against the registrations actually extracted from src/rt/hle.c, so
stale handler names or NIDs fail the gate instead of rotting silently.

Classification model (per registered NID):
  fake_success           -- routed to a generic always-success handler (h_ok
                            family), a handler curated as `stub`, or a handler
                            mechanically detected by tools/hle_manifest.py as
                            doing nothing but `(void)s` / logging / `return 0`.
                            The call reports success but performs none of the
                            API's contract. These are the silent-corruption risks
                            issue #71 exists to surface.
  controlled_unsupported -- a dedicated handler that deliberately refuses the
                            operation with the API's own documented error
                            (e.g. the PSMF getters returning PSMF_ERR_NO_DATA
                            until the real demux is connected). Static refusals
                            registered with an explicit PSP error code are also
                            controlled_unsupported. Unsupported, but honest.
  dedicated              -- a handler written for this API. NOT a claim of
                            completeness; see HANDLER_STATUS.

Handler implementation-status values (HANDLER_STATUS):
  complete       -- believed to implement the exercised contract.
  partial        -- real implementation with known gaps.
  compatibility  -- works for the shipped title's usage; not general.
  stub           -- fabricates success; forces classification fake_success.
  controlled_unsupported -- refuses with the API's documented error; forces
                            classification controlled_unsupported.
  unreviewed     -- default for handlers nobody has yet audited. Reported
                    as `dedicated` with status `unreviewed`.
Only deviations from `unreviewed` are listed; keep entries evidence-based and
cite the issue that tracks finishing the handler where one exists.
"""

from typing import NotRequired, TypedDict


class HandlerMetadata(TypedDict):
    status: str
    description: NotRequired[str]
    evidence: NotRequired[list[str]]
    limitation: NotRequired[str]

# Handlers that unconditionally fabricate success for every NID routed to
# them. Anything registered to one of these is classified fake_success.
GENERIC_SUCCESS_HANDLERS = {
    "h_ok",
}

HANDLER_STATUSES = {
    "complete",
    "partial",
    "compatibility",
    "stub",
    "controlled_unsupported",
    "unreviewed",
}

# Curated metadata per reviewed handler. Every handler named here must exist in
# hle.c's extracted registrations (tools/test_hle_manifest.py enforces it).
#
# Semantic status invariants enforced by tools/hle_manifest.py:
#   - 'complete': MUST carry a non-empty 'evidence' list citing contract source
#     (public ABI/doc, source-owned test name, hardware oracle id, or guest-module takeover).
#     Any referenced source files must exist in the repository tree.
#   - 'partial' and 'compatibility': MUST carry a non-empty 'limitation' string.
#     Handlers without recorded limitations must cite a factual gap from code or
#     be marked "limitation not yet reviewed (#341)".
HANDLER_METADATA: dict[str, HandlerMetadata] = {
    # This marker handler is intercepted by sr_syscall; each registration
    # carries its PSP-visible refusal code in sr_hle_register_unsupported.
    "h_ControlledUnsupported": {
        "status": "controlled_unsupported",
        "description": "Intercepted by sr_syscall; carries refusal code in sr_hle_register_unsupported.",
    },
    # Stores g_sdk_version for SDK-dependent paths; the retained-state
    # contract for the variants routed to it is implemented.
    # ReferSemaStatus reads the modeled semaphore: live count, create-time initCount,
    # maxCount, attr and name recorded at create, and the blocked-waiter count.
    "h_ReferSemaStatus": {
        "status": "partial",
        "limitation": "reports the modeled count, create-time initCount, maxCount, attr, name and blocked-waiter count; the caller size word is left as written and the full 56-byte span is written, a convention copied from ReferEventFlagStatus that is not hardware measured; unknown-UID and bad-pointer codes are source-selftested only",
    },
    "h_SetCompiledSdkVersion": {
        "status": "complete",
        "evidence": [
            "tests/native/sdkver_selftest.c",
            "tools/test_sdkver_c.py",
            "public ABI (PSPSDK sdkver.h: sceKernelSetCompiledSdkVersion)",
        ],
        "description": "Stores g_sdk_version for SDK-dependent paths; retained-state contract verified across all registered variants.",
    },
    "h_SysClock2USec": {
        "status": "partial",
        "limitation": "uses the runtime's microsecond system-clock representation and splits it into low/high outputs; hardware conversion and error-precedence cells are not measured",
    },
    "h_USec2SysClock": {
        "status": "partial",
        "limitation": "stores the unsigned microsecond duration as the low word of the runtime's microsecond SceKernelSysClock representation and writes a zero high word; hardware conversion and error-precedence cells are not measured",
    },
    "h_USec2SysClockWide": {
        "status": "partial",
        "limitation": "returns the unsigned microsecond duration in $v0 with a zero $v1 high word under the runtime's microsecond SceKernelSysClock representation; hardware conversion is not measured",
    },
    # Suspend/Resume/Rotate are scheduler operations: suspension is a flag on a thread
    # that keeps its wait semantics, rotation moves the head of one priority's ready
    # queue behind its peers.  Measured cells are the DORMANT, double-suspend and
    # resume-of-a-non-suspended-thread codes; the rest is project-defined.
    "h_SuspendThread": {
        "status": "partial",
        "limitation": "suspend is a scheduler flag (no nesting count); a waiting thread still completes its wait while suspended and runs only after resume; DORMANT and double-suspend codes are hardware measured, while the code for suspending UID 0 or the calling thread reuses the thread-object refusal code without a suspend-specific measurement, and interrupt/dispatch-context precedence is unmeasured",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_suspend_resume_errors_and_ready_thread",
            "tests/native/hle_thread_selftest.c:test_suspended_waiter_keeps_wait_semantics",
            "tests/native/sched_selftest.c:test_suspended_wait_still_completes",
        ],
    },
    "h_ResumeThread": {
        "status": "partial",
        "limitation": "resume clears the scheduler suspension flag and applies strict-priority preemption; the not-suspended and DORMANT codes are hardware measured, while resume of UID 0 (the running caller) answers the not-suspended code without a measurement and interrupt/dispatch-context precedence is unmeasured",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_suspend_resume_errors_and_ready_thread",
            "tests/native/sched_selftest.c:test_suspend_resume_error_codes",
        ],
    },
    "h_RotateThreadReadyQueue": {
        "status": "partial",
        "limitation": "rotates the cyclic slot-order ready queue of one priority (0 selects the caller's priority) and yields when the caller leads that queue; no range-error code is returned for an out-of-range priority because none is sourced, and ordering relative to threads that become ready after the rotation is not hardware measured",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rotate_ready_queue_selection_order",
            "tests/native/hle_thread_selftest.c:test_rotate_equal_priority_yields_to_peers",
            "tests/native/sched_selftest.c:test_rotate_ready_queue_moves_head_behind_peers",
        ],
    },
    "h_SysClock2USecWide": {
        "status": "partial",
        "limitation": "takes the 64-bit clock as the $a0/$a1 pair and writes its low/high words through $a2/$a3 on the same microsecond representation as h_SysClock2USec; invalid-output-pointer error code and hardware conversion are not measured",
    },
    "h_ReferProfilerNull": {
        "status": "partial",
        "limitation": "no profiler is modeled, so both sceKernelReferThreadProfiler and sceKernelReferGlobalProfiler always report NULL, like firmware with profiling off; NULL-ness on retail firmware is not hardware-measured",
    },
    "h_CtrlGetSamplingMode": {
        "status": "partial",
        "limitation": "reports the mode retained by sceCtrlSetSamplingMode but does not change the sampled SceCtrlData; the invalid-pointer error code (ILLEGAL_ADDR) is not hardware measured",
    },
    # The sampling setters' accepted ranges and error codes follow PSPAutotests
    # tests/ctrl/sampling and tests/ctrl/sampling2 (.expected files recorded on a PSP);
    # the stored values do not yet drive sampling (#1390).
    "h_CtrlSetSamplingMode": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ctrl_sampling_mode_cycle_matrix",
            "PSPAutotests tests/ctrl/sampling (modes 2 and -1 refused with 0x80000107, mode kept)",
        ],
        "limitation": "validates and stores the mode, but digital mode does not yet centre the sampled analog axes (#1390); whether firmware truncates the argument to 8 bits before the range check is not measured",
    },
    "h_CtrlSetSamplingCycle": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ctrl_sampling_mode_cycle_matrix",
            "PSPAutotests tests/ctrl/sampling2 (0 and 5555..20000 accepted, other cycles refused with 0x800001FE, cycle kept)",
        ],
        "limitation": "validates and stores the cycle, but samples are still latched once per VBLANK whatever the cycle (#1390)",
    },
    "h_CtrlGetSamplingCycle": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ctrl_sampling_mode_cycle_matrix",
            "PSPAutotests tests/ctrl/sampling (power-on cycle 0, set/get round trip)",
        ],
        "limitation": "reports the cycle retained by sceCtrlSetSamplingCycle; PSPAutotests leaves the NULL out-pointer call out as crashing a PSP, so the ILLEGAL_ADDR refusal used here instead is the runtime's fail-closed choice, not a measured code",
    },
    # Kernel alarms are guest-time timers whose handlers run in interrupt context from the
    # scheduler's interrupt-service step.  The measured cells are the unknown-alarm-id code
    # and the reschedule rule of PSP-HLE-INTR-001 (a non-zero return counts from the call's
    # scheduled time); the rest is project-defined.
    "h_SetAlarm": {
        "status": "partial",
        "limitation": "the handler runs in interrupt context at the next interrupt-service point once the guest-time deadline passes (so it can run later than the deadline, and only while CPU interrupts are enabled) and a non-zero return re-arms the alarm that many microseconds after the call's scheduled time, as measured (PSP-HLE-INTR-001), so a late call owes the scheduled points it fell behind; whether a return below 100 us is raised to 100 us, as a VTimer handler return is, is not measured; a NULL handler returns the invalid-address code and a full alarm table the out-of-resources code without a measurement, SetAlarm(0) is accepted without a measurement, the interrupt-context rules for blocking waits made from a handler are the per-API ones (unmeasured for most waits), and the SysClock variants (sceKernelSetSysClockAlarm, sceKernelReferAlarmStatus) are not implemented",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_alarm_one_shot_fires_once_at_its_time_with_its_argument",
            "tests/native/hle_thread_selftest.c:test_alarm_rearm_by_return_value",
            "tests/native/sched_selftest.c:test_alarm_rearm_counts_from_the_scheduled_time",
            "tests/native/hle_thread_selftest.c:test_alarm_handler_runs_under_interrupt_context_rules",
            "tests/native/hle_thread_selftest.c:test_alarm_idle_scheduler_advances_to_the_deadline",
            "tests/native/sched_selftest.c:test_alarm_runs_at_its_deadline_in_interrupt_context",
            "tests/native/sched_selftest.c:test_alarm_is_a_wake_source_for_the_idle_scheduler",
        ],
    },
    "h_CancelAlarm": {
        "status": "partial",
        "limitation": "an unknown, cancelled or already-finished alarm UID returns the hardware-measured unknown-alarm code; treating a one-shot that has already fired as unknown (its UID is released when the handler returns 0) is the project's model and is not separately measured",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_alarm_registration_and_error_codes",
            "tests/native/hle_thread_selftest.c:test_alarm_cancel_before_firing_prevents_the_handler",
            "tests/native/sched_selftest.c:test_alarm_cancel_and_ordering",
        ],
    },
    "h_AllocMemoryBlock": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_kernel_import_sweep_memory_blocks_and_sdk_alias",
            "PSPAutotests tests/sysmem/memblock.expected (2ed7c9c5, recorded on a PSP): names, types, sizes, option sizes and type-1 placement deltas",
            "uOFW src/kd/sysmem/memblock.c (MIT): validation order and the AllocPartitionMemory call",
            "tests/native/hle_thread_selftest.c:test_memory_block_high_in_low_based_partition",
            "PSPAutotests tests/sysmem/memblock.expected lines 9 and 25 to 33 (774dfddc467ae0cbb7d3d8b22fc271f74135cd38, a third-party recording made on a PSP): type 1 allocates, and a second type-1 block lies its size rounded up to 0x100 below the first",
        ],
        "limitation": "types 0 (Low, the partition's bump and free-list path) and 1 (High, from the top of the arena thread stacks share, 0x100 granularity) allocate, and the type, size, option-size and NULL-name codes are pspautotests-recorded; in a partition based below the runtime stacks a High block lies above the bump region, so the free sizes, which measure that region, do not count it (#2640); the code for an unmapped name or option pointer is uOFW's 0x800200D3, not measured; memory-block UIDs are separate from partition-block UIDs here, while the PSP shares them (memblock.expected Partition usage)",
    },
    "h_GetMemoryBlockAddr": {
        "status": "partial",
        "limitation": "resolves UIDs created by the modeled Low allocation path; output-pointer and unknown-UID results are source-tested but not hardware measured",
    },
    "h_FreeMemoryBlock": {
        "status": "partial",
        "limitation": "releases UIDs created by the modeled Low allocation path; invalid-UID result is source-tested but not hardware measured",
    },
    "h_DisplayWaitVblankStartMulti": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/display/vblankmulti.expected (the Multi block: a count of 3 waits three VBLANK starts)",
            "tests/native/hle_thread_selftest.c:test_wc_display_multi_count_signed",
        ],
        "limitation": "waits the requested positive count of scheduler VBLANK periods; a count of 0 or below as a signed int answers 0x800001FE, a rule taken from the waits matrix and not from a recorded cell; hardware timing is not measured (#341)",
    },
    "h_DisplayWaitVblankStartMultiCB": {
        "status": "partial",
        "limitation": "the requested positive count of scheduler VBLANK periods, servicing the caller's callbacks in each; the zero-count and wait-context refusals follow the non-CB form; hardware timing is not measured",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_display_multi_cb_wait",
        ],
    },
    # sceDisplayGetFramePerSec: writes the measured 60000/1001 float refresh
    # rate (59.9400599f) into $f0 under the unified display clock. The API has
    # no parameters; the full observable contract is the float bits.
    "h_DisplayGetFramePerSec": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_display_frame_per_sec_float_return",
            "tools/test_hle_manifest.py:FindingRuleTests.test_float_return_metadata_is_live_and_dedicated",
            "hardware oracle / issue #80 display-clock measurement (59.9400599f in $f0)",
        ],
        "description": "Writes measured 60000/1001 float refresh rate (59.9400599f) into $f0 under unified display clock.",
    },
    # scePower clock getters, the devkit version word and sceCtrlPeekLatch, measured on the PSP-3000
    # 6.61 oracle 2026-10-15. Defaults are measured; values after a Set call and the non-clearing peek
    # are modelled and say so in their limitations.
    "h_PowerGetPllClockFrequencyFloat": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_power_identity_getters_measured",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-001 case pll-clock-float (0x435E0000 = 222.0)",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-002 case pll-clock-float-v0 ($f0 0x435E0000; $v0 not modelled)",
        ],
        "limitation": "returns the float of the retained PLL clock in $f0: the 222.0 default is measured on PSP-3000 6.61 (PSP-HLE-POWER-001, PSP-HLE-POWER-002), while values after scePowerSetClockFrequency follow the modelled request and are not hardware measured; $v0 is 0, a runtime choice: PSP-HLE-POWER-002 read a different word in $v0 after each call, neither 0 nor the clock, which is not modelled",
    },
    "h_PowerGetCpuClockFrequencyFloat": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_power_identity_getters_measured",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-001 case cpu-clock-float (0x435E0000 = 222.0)",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-002 case cpu-clock-float-v0 ($f0 0x435E0000; $v0 not modelled)",
        ],
        "limitation": "returns the float of the retained CPU clock in $f0: the 222.0 default is measured on PSP-3000 6.61 (PSP-HLE-POWER-001, PSP-HLE-POWER-002), while values after scePowerSetClockFrequency follow the modelled request and are not hardware measured; $v0 is 0, a runtime choice: PSP-HLE-POWER-002 read a different word in $v0 after each call, neither 0 nor the clock, which is not modelled",
    },
    "h_PowerGetBusClockFrequencyFloat": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_power_identity_getters_measured",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-001 case bus-clock-float (0x42DE0000 = 111.0)",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-002 case bus-clock-float-v0 ($f0 0x42DE0000; $v0 not modelled)",
        ],
        "limitation": "returns the float of the retained bus clock in $f0: the 111.0 default is measured on PSP-3000 6.61 (PSP-HLE-POWER-001, PSP-HLE-POWER-002), while values after scePowerSetClockFrequency follow the modelled request and are not hardware measured; $v0 is 0, a runtime choice: PSP-HLE-POWER-002 read a different word in $v0 after each call, neither 0 nor the clock, which is not modelled",
    },
    "h_PowerSetCpuClockFrequency": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_cpu_clock_user_mode_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 user mode at PLL 222 (1 and 222 accepted; 0, 333, 334 and 223 refused with 0x800001fe)",
        ],
        "limitation": "accepts 1..retained PLL and refuses the rest with 0x800001fe, measured only at PLL 222; the clock the getters report after an accepted request is the retained request, not measured",
    },
    "h_PowerSetClockFrequency": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_power_set_clock_frequency_range_checks",
            "PSPAutotests tests/power/freq.expected (2ed7c9c5, recorded on a PSP): (n, n, n / 2) returns 0 for 19..333 and 0x800001fe for -1..18 and 334",
            "PPSSPP scePower.cpp (c9e2afb6): the ranges, and one function behind 0x737486f2, 0xebd177d6 and 0x469989ad",
        ],
        "limitation": "the PLL range is recorded for (n, n, n / 2) only; the independent CPU and bus limits are PPSSPP's and not measured; an accepted request is retained verbatim, while the recording shows the PSP stepping it (19 reads back as PLL 190, CPU 18, bus 95), and PPSSPP's delay on a PLL change is not modelled (#341)",
    },
    "h_PowerSetBusClockFrequency": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_power_set_bus_clock_frequency_autotest",
            "PSPAutotests tests/power/freq.expected (2ed7c9c5, recorded on a PSP): at PLL 222, 1..111 return 0 and -1, 0, 112 return 0x800001fe, and no request moves any clock",
        ],
        "limitation": "validates 1..111 against the measured range at PLL 222 and changes no clock, as measured; at any other retained PLL the range is not measured and the run stops by name",
    },
    "h_CheckThreadStack": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_check_thread_stack_contract",
            "PSPAutotests tests/threads/threads/stackfree.expected (2ed7c9c5, recorded on a PSP): 0xEB0 and 0xAB0 on 0x1000-byte stacks, 0x10 above GetThreadStackFreeSize, main thread above 0x30000",
        ],
        "limitation": "returns the caller's $sp minus its stack's low end, the quantity the recorded cells follow; a $sp outside the calling thread's stack and a call with no current thread are not measured and stop the run by name",
    },
    "h_FontGetNumFontList": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_font_list_contract",
            "PSPAutotests tests/font/fontlist.expected (2ed7c9c5, the console's libfont on a PSP): 18 and error 0 for a live library, 0 and 0x80460002 for a NULL library",
            "hardware measurement 2026-10-15, PSP-3000 6.6.1: the system font list holds 18 fonts (src/rt/flash0_font.c registry rows)",
        ],
        "limitation": "returns the measured 18 for a live library whether or not every index has a served font; a library handle that is not live and a NULL error pointer are not measured and stop the run by name",
    },
    "h_FontSetAltCharacterCode": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_font_alt_char_contract",
            "PSPAutotests tests/font/altcharcode.expected (2ed7c9c5, the console's libfont on a PSP): initial 0x005F, codes stored as 16 bits, 0x80460002 for a NULL library, and the GetCharInfo fallback heights",
        ],
        "limitation": "the fallback is kept host-side and used by GetCharInfo and GetCharGlyphImage; the PSP library struct field that holds it is not part of this runtime's library layout, and a library handle that is not live stops the run by name",
    },
    "h_OskUpdate": {
        "status": "partial",
        "evidence": [
            "#363 (PSP-3000 measurements, comments 6330742629 and 6331070692): WRONG_TYPE 0x80110005 before a keyboard starts; 0 in VISIBLE for speed 0 and 1; INVALID_STATUS 0x80110001 in INIT, QUIT and NONE after use",
            "tests/native/hle_thread_selftest.c:test_osk_update_and_shutdown_init_states",
            "tests/native/hle_thread_selftest.c:test_utility_dialog_frame_driven_status",
        ],
        "limitation": "the return codes follow the measured states, and INIT ends on the vblank after InitStart (#1386); Update does not drive the keyboard, whose text is collected from GetStatus (#281), and Update in FINISHED returns 0, which is not measured (#341)",
    },
    "h_NetInit": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_net_core_without_network",
            "PSPAutotests tests/net (70e72391, recorded on a PSP): sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000) returns 0 in every net test with no access point connected, and again after sceNetTerm (net/adhoc/socketids)",
        ],
        "limitation": "the library's state and its pool (from the user partition) only: no network stack, no socket and no host network API; the SceNetCallout and SceNetNetintr threads are not created; the PSP results were recorded with the WLAN switch on, and with the console profile's switch off sceNetInit still returns 0 (UNMEASURED; PPSSPP does not consult the switch either); the pool-size and priority codes, the pool's placement and the Init failure codes follow PPSSPP (a behaviour reference, UNMEASURED); a second Init without Term starts over with one pool, but checks its arguments before releasing the first pool, unlike PPSSPP (the PSP's order is UNMEASURED); a pool size whose 256-byte rounding passes 4 GB is refused with NO_MEMORY; core re-Init while Inet is active stops by name",
    },
    "h_NetTerm": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_net_core_without_network",
            "PSPAutotests tests/net (70e72391, recorded on a PSP): sceNetTerm returns 0 after sceNetInit (net/adhoc/apctl, apctlinit, loopback)",
        ],
        "limitation": "frees the pool and clears the library state; sceNetTerm before sceNetInit returns 0 as PPSSPP does (UNMEASURED); core Term while Inet is active stops by name",
    },
    "h_NetInetInit": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_net_inet_lifecycle_without_network",
            "PSPAutotests tests/net/adhoc/apctlinit and tests/net/inet/apscan (e37443a681c6a4d562f32b6b48e1f4e36cd69c97, source and expected captures): fresh sceNetInetInit returns 0 after utility module and core Net setup",
        ],
        "limitation": "models only Inet lifecycle state after core Net and both COMMON and INET modules are ready; duplicate Init and unmet setup stop by name; guest pool and thread effects beyond core Net are unmeasured, and sockets, access-point control, DNS, and host transport are not enabled",
    },
    "h_NetInetTerm": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_net_inet_lifecycle_without_network",
            "PSPAutotests tests/net/adhoc/apctlinit and tests/net/inet/apscan (e37443a681c6a4d562f32b6b48e1f4e36cd69c97, source and expected captures): active sceNetInetTerm returns 0 before core Net Term and module unload",
        ],
        "limitation": "clears active Inet state before core Net teardown; early or repeated Term and Term while Resolver is active stop by name; LoadExec clears Inet with the process's core Net and module state",
    },
    "h_NetResolverInit": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_net_resolver_lifecycle_without_network",
            "PSPAutotests tests/net/inet/offline (e37443a681c6a4d562f32b6b48e1f4e36cd69c97, source and expected capture): after normal Net/Inet setup, AP Term and a preceding resolver Create/lookup/Stop/Delete, first Init returns 0 and duplicate Init returns 0x80000020",
            "PPSSPP Core/HLE/sceNetResolver.cpp:sceNetResolverInit (cdf563b85c42c275ec2835a0883f48d63a9c9406): Init consults only the initialized flag; equivalence of Init without prior resolver object operations is reference-derived",
        ],
        "limitation": "tracks only lifecycle in the disconnected Inet stack with WLAN on; first Init without prior object operations is PPSSPP-derived and logs once; unsupported ordering stops by name; resolver objects, DNS and host transport remain outside the handler",
    },
    "h_NetResolverTerm": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_net_resolver_lifecycle_without_network",
            "PSPAutotests tests/net/inet/offline (e37443a681c6a4d562f32b6b48e1f4e36cd69c97, source and expected capture): first and repeated Resolver Term return 0 after Init and deletion of the resolver objects",
        ],
        "limitation": "clears active Resolver and permits repeated Term in the same disconnected Inet lifetime with WLAN on; Term without successful Init in this lifetime stops by name; LoadExec resets the lifecycle state",
    },
    "h_NetGetLocalEtherAddr": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_net_core_without_network",
            "PSPAutotests tests/net (70e72391, recorded on a PSP): 0x80410180 with the buffer untouched before, during and after sceNetApctlInit (net/adhoc/apctlinit) and before an ad hoc group (net/adhoc/adhocctl, net/adhoc/pdp)",
        ],
        "limitation": "always 0x80410180, because no ad hoc group and no access point connection ever starts with no network; the address a PSP returns once one has started is not modelled; the answer before sceNetInit is UNMEASURED",
    },
    "h_NetAdhocctlGetParameter": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_net_core_without_network",
            "tests/native/hle_thread_selftest.c:test_adhoc_online_all_39_handlers_real_tcp",
            "PSPAutotests tests/net (70e72391, recorded on a PSP): before sceNetAdhocctlInit, GetState, AddHandler and Create return NOT_INITIALIZED 0x80410b08 (net/adhoc/adhocctl \"Before init\")",
        ],
        "limitation": "offline before sceNetAdhocctlInit it returns NOT_INITIALIZED (0x80410b08) matching the measured calls; when initialized locally or online (#1139), writes channel, product ID, group name, BSSID, and nickname from console/group state (with the online active state PPSSPP-derived, not PSP-measured; pre-init code and WLAN-off 0x80410b09 remain UNMEASURED)",
    },
    "h_MsgDialogAbort": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_msg_dialog_abort_sequence",
            "tests/native/hle_thread_selftest.c:test_msg_dialog_abort_without_update",
            "PSPAutotests tests/utility/msgdialog/abort.expected (2ed7c9c5, recorded on a PSP): the return codes and status sequence before init, during init, while visible, after the next Update and through shutdown",
        ],
        "limitation": "return codes and the RUNNING to FINISHED step are pspautotests-recorded; the result written (2, SCE_UTILITY_DIALOG_RESULT_ABORT) follows PPSSPP as a behaviour reference (derived, not PSP-measured; queued for the next PSP matrix run), and a button answer the model wrote at the first Update stays",
    },
    "h_FontGetFontList": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_font_list_contract",
            "PSPAutotests tests/font/fontlist.expected (2ed7c9c5, the console's libfont on a PSP): one 0xA8-byte style per font, num 1 writes exactly one, num -1 and 0 write nothing, 0x80460002 for a NULL library and 0x80460003 for NULL styles",
        ],
        "limitation": "writes min(num, 18) styles in index order from the served registry values, of which the capture shows only the first; an index whose font is not served, a library that is not live and an unwritable array stop the run by name",
    },
    "h_PowerGetBatteryLifeTime": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (0 on AC; 376 minutes on battery)",
        ],
        "limitation": "reports the two measured samples for the modeled power source (external power by default); no discharge model",
    },
    "h_PowerCheckWlanCoexistenceClock": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "PPSSPP 7fab97c05f9568b17ac79c235b945f568eaf6384 scePower.cpp (model-dependent capability)",
        ],
        "limitation": "returns the derived PSP-1000 capability value 0 for every model; later model selection is not implemented",
    },
    "h_PowerGetBatteryChargingStatus": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (0 on AC with a full battery and on battery)",
        ],
        "limitation": "reports the modeled charging state, measured 0 on AC with a full battery and on battery; the status of a charging battery is not measured",
    },
    "h_KernelDevkitVersion": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_kernel_devkit_version_measured",
            "src/rt/sdkver.h:SR_PSP_FIRMWARE_DEVKIT_VERSION",
            "hardware oracle 2026-10-15 PSP-SYSTEM-001 case model-profile (0x06060110)",
        ],
        "limitation": "returns the 6.61 devkit word for every firmware: only the measured firmware is modelled, and other firmware devkit words are not",
    },
    "h_CtrlPeekLatch": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ctrl_peek_latch_keeps_the_latch",
            "hardware oracle 2026-10-15 PSP-HLE-CTRL-LATCH-001 cases latch-peek-initial and latch-peek-after-poll (no input gives make break press 0 and release 0xFFFFFFFF)",
        ],
        "limitation": "the transition and return words are the sceCtrlReadLatch model, source-tested; the non-clearing peek is the project's model and is not hardware measured, because PSP-HLE-CTRL-LATCH-001 cannot tell clearing from keeping (its release field stays saturated with no input)",
    },
    # These two handlers are reached by the source-owned display-smoke route.
    # Its fixed 8888 framebuffer flip and ordinary guest-thread vblank wait
    # establish that narrow production path, while the API edges below remain
    # explicitly outside the route's evidence (#341).
    "h_DisplaySetFrameBuf": {
        "status": "partial",
        "evidence": [
            "Makefile:display-smoke-run",
            "tests/fixtures/display_smoke/generate.py:verify",
            "tests/fixtures/display_smoke/generate.py:run",
        ],
        "limitation": "display-smoke covers only a sync=1 8888 flip with stride 512; other format, address, stride, and error-precedence cases remain outside this route (#341)",
    },
    "h_DisplayWaitVblankStart": {
        "status": "partial",
        "evidence": [
            "Makefile:display-smoke-run",
            "tests/fixtures/display_smoke/generate.py:run",
            "tools/test_sched_invariants.py:test_the_two_display_nids_have_separate_handlers",
        ],
        "limitation": "display-smoke covers ordinary guest-thread waits; interrupt-context behavior and callback wait variants remain outside this route (#341)",
    },
    # sceGeListEnQueue is reached by the display-smoke route with one bounded,
    # unstalled list (PRIM, FINISH, END) that the host GE runs to completion at
    # enqueue. The flight recorder pins that single production path.
    "h_GeListEnQueue": {
        "status": "partial",
        "evidence": [
            "Makefile:display-smoke-run",
            "tests/fixtures/display_smoke/generate.py:flight_smoke",
            "tests/native/hle_thread_selftest.c:test_flight_recorder_ge_present_events",
            "tests/native/hle_thread_selftest.c:test_ge_queued_lists_run_in_order",
        ],
        "limitation": "display-smoke covers one unstalled synchronous list; a list enqueued behind an active one waits its turn and runs in enqueue order once the head completes, slots come from a FIFO of free slots and ids name slots (uofw src/kd/ge/ge.c; the queued peek 1 and the 64-list 0x80000022 refusal are unmeasured), and argument and priority validation and asynchronous execution timing remain outside this route (#341)",
    },
    "h_GeListEnQueueHead": {
        "status": "partial",
        "limitation": "idle-list execution follows the shared enqueue path; head ordering is refused while a list is stalled, and asynchronous queue behavior remains unmodeled",
    },
    "h_GeBreak": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ge_break_continue",
        ],
        "limitation": "models synchronous display list pause (mode 0) and queue reset (mode 1); argument and parameter buffer inspection are K1/read checked only, asynchronous hardware command boundary timing remains unmodeled (#341); PSP-measured in the eligible PSP-GE-CONTROL-001 2026-10-15 re-run: the 0x80000020 no-active-list result, the invalid-mode result 0x80000107 (mode -1 only), the mode 0 queue id and the paused sync statuses (4, 2) for a list broken mid-draw, the mode 1 result 0 with one list stalled at its start, and the 0x80000100 sync result for a list the reset removed; a list enqueued with no stall address completes inside sceGeListEnQueue, so the runtime cannot reproduce the mid-draw break and returns 0x80000020 with sync peeks of 0 on that sequence; breaking a stalled list, a mode 1 reset of a paused list and modes above 1 are unmeasured and follow the same codes",
    },
    "h_GeContinue": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ge_break_continue",
        ],
        "limitation": "resumes a paused display list using the synchronous GE runner; hardware timing and multi-queue priority ordering remain unmodeled (#341); the 0 result when no list is paused is PSP-measured (PSP-GE-CONTROL-001, eligible 2026-10-15 re-run)",
    },
    "h_GeListDeQueue": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ge_break_continue",
            "tests/native/hle_thread_selftest.c:test_ge_sync_ids_match_psp",
            "tests/native/hle_thread_selftest.c:test_ge_queued_lists_run_in_order",
        ],
        "limitation": "a stalled list returns BUSY (0x80000021) and stays queued (PSP-GE-CONTROL-001), and so does a completed list until a sceGeDrawSync(0) frees it; an unknown id is 0x80000100 (PSP-GE-SYNC-001, measured); a queued list that has not started is removed and its waiters released, and a freed slot is 0x80000100, following uofw src/kd/ge/ge.c (unmeasured)",
    },
    # scePsmfPlayerGetVideoData / GetAudioData. Both drive the project-authored
    # PSMF producer and a host codec backend, and return 0 only for output a
    # decoder actually produced: the video getter validates the caller's stride
    # and pointer contract, warms up the documented three frames, converts the
    # picture into the guest display buffer only after preflighting every
    # destination row, and reports the picture's presentation time; the audio
    # getter stages one decoded ATRAC3+ frame as 2048 stereo s16 samples. A
    # compressed access unit is never presented as a decoded frame or PCM, and
    # a stream that runs dry reports NO_MORE_DATA. They remain partial because
    # the player above them is host HLE, the picture path needs a host backend to
    # exist at all (the null backend yields no pictures by design), and no
    # hardware-comparison tier has been measured for either getter. Since #286 they
    # serve only a title that does not stage its own libpsmfplayer.prx: a staged,
    # translated player module owns the scePsmfPlayer library and reaches the media
    # boundary through sceMpeg.
    "h_PsmfGetVideo": {
        "status": "partial",
        "limitation": "host HLE player; requires host codec backend; no hardware comparison tier measured (#341)",
    },
    "h_PsmfGetAudio": {
        "status": "partial",
        "limitation": "host HLE player; requires host codec backend; stages 2048 stereo s16 frames; no hardware comparison tier measured (#341)",
    },
    # Firmware utility modules: IDs, names and load dependencies follow PPSSPP
    # Core/HLE/sceUtility.cpp moduleLoadInfo (3f40ddc8). Loading records module state only.
    "h_UtilityLoadModule": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_utility_av_module_state",
            "tests/native/hle_thread_selftest.c:test_utility_module_table",
        ],
        "limitation": "records utility module state only; the user-partition memory a real module occupies is not reserved; IDs and dependencies follow PPSSPP, not measured on this project's PSP",
    },
    "h_UtilityUnloadModule": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_utility_av_module_state",
            "tests/native/hle_thread_selftest.c:test_utility_module_table",
        ],
        "limitation": "records utility module state only; unloading COMMON or INET while Inet is active stops by name; other module dependencies are not checked on unload (PPSSPP does not either); not measured on this project's PSP",
    },
    "h_UtilityLoadNetModule": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_utility_av_module_state",
            "tests/native/hle_thread_selftest.c:test_utility_module_table",
        ],
        "limitation": "maps net-module numbers 1..7 onto utility IDs 0x100..0x106 and their shared state; the status codes for a bad number or a repeated call are the generic module ones, unmeasured on a PSP; makes no connection and does not enable socket operations",
    },
    "h_UtilityUnloadNetModule": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_utility_av_module_state",
            "tests/native/hle_thread_selftest.c:test_utility_module_table",
        ],
        "limitation": "maps net-module numbers 1..7 onto utility IDs 0x100..0x106; the status codes for a bad number or a module that is not loaded are the generic module ones, unmeasured on a PSP; unloading COMMON or INET while Inet is active stops by name",
    },
    # scePsmfPlayerGetPsmfInfo reports the opened PSMF's header as SetPsmfCB parsed it.
    # Checks, their order and the one-picture duration offset follow PPSSPP
    # Core/HLE/scePsmf.cpp (3f40ddc8); the 567567 lastFrameTs, version 1 and zero PCM
    # streams agree with pspautotests video/psmfplayer/getpsmfinfo.expected (2321faa0).
    "h_PsmfGetPsmfInfo": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_player_get_psmf_info",
            "tests/native/hle_thread_selftest.c:test_runtime_placed_modules",
        ],
        "limitation": "host HLE player; numPcmStreams is always 0 because the stream table has no LPCM class; the width/height outputs are written only when the loaded libpsmfplayer module's exported module_sdk_version is 0x03090510 (a firmware-loaded player records no build); lastFrameTs assumes the 3003-tick 29.97 fps picture step; the ILLEGAL_ADDR refusals follow PPSSPP (the PSP faults there); not measured on this project's PSP (#308)",
    },
    "h_PsmfGetCurrentPts": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_player_av_presentation_timing",
            "PSPAutotests tests/video/psmfplayer/getcurrentpts.expected (f573fe8c, recorded on a PSP): Initial 0x80616001, Standby and Playing-before-a-picture 0x8061600c, then one 32-bit write of the last picture's pts",
        ],
        "limitation": "host HLE player; the pts is the displaypts of the last picture the video getter delivered, written as one 32-bit word; a NULL or unwritable output once a picture exists (the hardware run crashes) and a player that scePsmfPlayerStop returned to standby after playing are not measured and stop the run by name",
    },
    # The licensee key is recorded for sceNpDrmEdataSetupKey, which stays refused: no key
    # material ships with the project and nothing decrypts.
    "h_NpDrmSetLicenseeKey": {
        "status": "partial",
        "limitation": "records the 16-byte key and returns 0; the 0x80550902 refusal of a null or unreadable key follows PPSSPP and is not hardware measured; the key is never used because sceNpDrmEdataSetupKey is refused",
    },
    "h_NpDrmClearLicenseeKey": {
        "status": "partial",
        "limitation": "forgets the recorded key and returns 0; not hardware measured",
    },
    # sceMpegAvcDecodeDetail, and the ATRAC3+ access units a guest PSMF player decodes through
    # sceMpeg (#286): see src/rt/mpeg.c.
    "h_MpegAvcDecodeDetail": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_avc_decode_detail",
        ],
        "limitation": "the 13 words MEASURED on a PSP-3000 (PSP-MPEG-001): word 0 is 0, words 2 and 3 the header's frame width and height, words 4-7 zero, word 8 the last decode's frame status; word 1 is written as the pictures decoded, which the PSP's is not (0 after 240 pictures), and words 9-12 as 0, one of which holds an unidentified pointer on the PSP (#302)",
    },
    "h_MpegGetAtracAu": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_atrac_au_decode",
        ],
        "limitation": "one ATRAC3+ frame per call from an audio-only demux of the ring bytes the game fed; NO_DATA when none is demultiplexed; the AU clock is the media-engine model (first timestamp + 4180 per decoded block), not the stream's PES time; the ES buffer is a handle, as in PPSSPP, and is not written; not measured on this project's PSP",
    },
    "h_MpegAtracDecode": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_atrac_au_decode",
        ],
        "limitation": "decodes the frame the last sceMpegGetAtracAu held with the project's ATRAC3+ bridge into 2048 stereo s16 samples (a mono stream duplicated); refuses with no held frame or an undecodable frame, with unmeasured error values; not measured on this project's PSP",
    },
    "h_MpegAvcQueryYCbCrSize": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_measured_contract",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_guest_decode",
        ],
        "limitation": "the sizes and refusals MEASURED for modes 0-4 and -1 (PSP-MPEG-001); a mode-3 size wider than 480, which does not follow 1.5 w h on the PSP, is refused as not modeled (#302)",
    },
    "h_MpegAvcInitYCbCr": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_measured_contract",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_guest_decode",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_modes_2_and_3",
        ],
        "limitation": "writes the MEASURED mode-1 header (eight plane pointers into the buffer) and leaves the planes; mode 2 returns 0 for the measured 128-byte buffer (its bytes before a decode are zeroed, unmeasured) and mode 3 returns 0 and writes nothing (its effect is unmeasured); an odd number of 16-pixel strips is refused as not measured, and the query's mode-1 limits stand in for Init's own, applied to modes 2 and 3 too (only 480x272 was passed to Init there) (#302)",
    },
    "h_MpegAvcDecodeYCbCr": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_measured_contract",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_guest_decode",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_mode2_decode",
        ],
        "limitation": "runs one access unit behind as MEASURED (PSP-MPEG-001: the first call returns 0 with init 0 and no picture, each later call the picture of the unit before it); writes the decoder's samples into the MEASURED mode-1 strip layout through the header's plane pointers and sets words 0 and 1, or, into a mode-2 buffer, only the MEASURED 128-byte header (a mode-2 buffer holds no picture); a decode into a mode-3 buffer is refused (the PSP's first call returned 0 and wrote a 16-byte {h/16, w/16, 0, 0} header over a black 4:2:0 frame, its second call never returned, PSP-MPEG-001 boots 145 and 152, so the picture layout stays unmeasured); a picture larger than its InitYCbCr allocation or than 480x272 is refused as not measured, and the strip length from the decoded height rests on one measured geometry (#302)",
    },
    "h_MpegAvcDecodeStopYCbCr": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_measured_contract",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_guest_decode",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_restart_after_stop",
        ],
        "limitation": "takes the address of the buffer pointer and hands over the picture the decoder still holds with status 1, as MEASURED after the last access unit (PSP-MPEG-001); that held picture is the one the decoder runs behind by (sceMpegAvcDecodeYCbCr), the decoder is drained only once the whole stream was fed, and a stop in mid-stream is not measured, nor is a stop on a mode-3 buffer, which is refused (#302); after a stop the decoder holds no earlier unit's picture, and one a host decoder still owes is dropped as it comes (an unmeasured model, #279)",
    },
    "h_MpegAvcCopyYCbCr": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_measured_contract",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_guest_decode",
        ],
        "limitation": "copies the decoded share of every plane and words 0 and 1 through both headers; not measured on the PSP, and overlapping planes are refused (#302)",
    },
    "h_MpegAvcCsc": {
        "status": "partial",
        "evidence": [
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_measured_contract",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_guest_decode",
            "tests/native/psmf_media_selftest.c:test_mpeg_csc_swizzled_vram_mirrors",
            "tests/native/psmf_media_selftest.c:test_mpeg_csc_measured_range_and_16bit",
            "tests/native/psmf_media_selftest.c:test_mpeg_ycbcr_modes_2_and_3",
        ],
        "limitation": "converts the guest planes, read through the header's pointers, with the MEASURED PSP matrix and gives the MEASURED range refusals (including the y and h cells: y 1, 161, 200 and an odd h are invalid values); the destination is written from its first pixel with rows from the top of the picture (y never selects rows); the 16-bit modes use the MEASURED packing with alpha 0 and 32-pixel blocks 64 pixels apart (block origin at x 0 only measured, and only a 64-pixel-wide range at stride 512 was captured: a wider 16-bit range, or one whose block spacing passes the stride, is refused as not measured); a mode-2 buffer converts to zero pixels in 8888 (MEASURED) and is refused in 16-bit; that y must be a multiple of 16 is inferred from three refused values (#302)",
    },
    "h_MpegQueryPcmEsSize": {
        "status": "partial",
        "limitation": "the public 320-byte LPCM ES/output sizes are retained, but no PCM stream is exposed by this runtime (#302)",
    },
    "h_MpegGetPcmAu": {
        "status": "controlled_unsupported",
        "description": "PCM access-unit production is deliberately refused with NO_DATA; no PCM decoder or hardware-oracle queue contract is available (#302)",
    },
    "h_MpegQueryUserdataEsSize": {
        "status": "partial",
        "evidence": ["tests/native/psmf_media_selftest.c:test_mpeg_userdata_imports"],
        "limitation": "writes the 0xA0000 ES and output sizes PPSSPP c9e2afb6 uses and refuses a bad handle or pointer with -1; neither is measured on a PSP (#302)",
    },
    "h_MpegGetUserdataAu": {
        "status": "controlled_unsupported",
        "description": "userdata access-unit production is deliberately refused with NO_DATA after clearing the two result words, because the demuxer extracts no userdata and the first call says so (#302)",
    },
    "h_MpegChangeGetAuMode": {
        "status": "partial",
        "limitation": "decode mode is validated and retained; skip mode fails closed because its queue/timestamp effect is not measured (#302)",
    },
    # SAS waveform/ATRAC3 entry points whose source codecs are not implemented
    # by this runtime. They validate the core/voice identity and return the
    # documented invalid-state error instead of fabricating success.
    "h_SasUnsupportedVoice": {
        "status": "controlled_unsupported",
        "description": "SAS waveform/ATRAC3 voice entry points without implemented codecs; returns PSP_SAS_ERROR_INVALID_STATE (0x80420016, pspsascore.h).",
    },
    # The SAS envelope setters that PSP-SAS-002 (sas-adsr) measures and this runtime reproduces (#2807).
    # The record also names __sceSasSetSimpleADSR, __sceSasGetEnvelopeHeight and __sceSasCore; they stay
    # unreviewed: pair p1's end flag and the exponent attack's height at grain 0 differ from the record,
    # and the probe records no __sceSasCore result.
    "h_SasSetADSRmode": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-SAS-002: sas-adsr-*)",
            "tests/native/hle_thread_selftest.c:test_sas_adsr_mode_psp_sas_002_table",
            "tests/native/hle_thread_selftest.c:test_sas_adsr_psp_sas_002_curve_runs",
        ],
        "limitation": "this status covers the measured cells only: PSP-SAS-002 is one acceptance-eligible launch on a PSP-3000 (system software 6.61, game context) of the probe built from 06a6c942c, 696 records all PASS, and it calls __sceSasSetADSRmode on voice 0 of one core before KeyOn, in its 32-row table after the baseline curves 0/1/1/1 (sas-adsr-m00-calls..sas-adsr-m31-calls and their heights) and in the expatk, expatk-g64, bound and bound-half runs; the selftest replays those calls and asserts the measured words: 0 for attack curves 0, 2 and 4, decay and release curves 1, 3 and 5 and sustain curves 0 to 5 under mask 0xf and under each single-phase mask, 0x80420013 for attack 1, 3 or 5, decay or release 0, 2 or 4, sustain 6 and all four at 5, the baseline's heights after every refusal, mask 1 returning 0 without checking the curves it leaves out, the top bit ignored (0, and the heights of the same curve without it), and a release-only mask leaving the heights before KeyOff alone; the absolute height a curve gives is not claimed: the exponent attack reads 0x00a76d9d at grain 0 of the expatk run on the PSP and 0x00bf40cd here, which is this runtime's curve after 256 steps instead of 224, because the runtime has no key-on delay (#1296); masks above 0xf, a bad core or voice index, other voices and calls while a voice sounds are unmeasured and not reviewed here (#2807)",
    },
    "h_SasSetADSR": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-SAS-002: sas-adsr-*)",
            "tests/native/hle_thread_selftest.c:test_sas_adsr_mode_psp_sas_002_table",
            "tests/native/hle_thread_selftest.c:test_sas_adsr_psp_sas_002_curve_runs",
        ],
        "limitation": "this status covers the measured cells only: in PSP-SAS-002 (one acceptance-eligible launch on a PSP-3000, system software 6.61, game context, of the probe built from 06a6c942c) __sceSasSetADSR with mask 0xf on voice 0 before KeyOn returns 0 for the table baseline (attack 0x00400000, decay 0x00100000, sustain 0x00004000, release 0x00040000) in all 32 rows of sas-adsr-m00-calls..sas-adsr-m31-calls, and for the rates of the expatk and expatk-g64 runs (0x00020000, 0x00100000, 0, 0x00040000) and of the bound and bound-half runs (0x00400000, 0x00100000, 0, 0x00100000); the selftest replays those calls and asserts those words, and in the bound runs the rates act as linear steps: the attack reaches the top within one grain and the decay holds 0x30000000 from grain 2 to grain 15; the refusals (a mask above 0xf, a negative rate) and other masks are unmeasured, and the absolute heights after KeyOn are not claimed, since the one the record publishes (expatk, 0x00a76d9d) moves with the key-on delay this runtime does not model (#1296); they are not reviewed here (#2807)",
    },
    "h_SasSetSL": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-SAS-002: sas-adsr-*)",
            "tests/native/hle_thread_selftest.c:test_sas_adsr_mode_psp_sas_002_table",
            "tests/native/hle_thread_selftest.c:test_sas_adsr_psp_sas_002_curve_runs",
        ],
        "limitation": "this status covers the measured cells only: in PSP-SAS-002 (one acceptance-eligible launch on a PSP-3000, system software 6.61, game context, of the probe built from 06a6c942c) __sceSasSetSL on voice 0 before KeyOn returns 0 for the level 0x20000000 (all 32 rows of sas-adsr-m00-calls..sas-adsr-m31-calls and the expatk runs), 0x30000000 (bound) and 0x30080000 (bound-half); the selftest replays those calls and asserts those words, and that in both bound runs a linear decay of 0x00100000 per step from the top stops at 0x30000000, so a linear decay ends at the first step at or below the level and keeps that height (sas_envelope.h ended it only below the level and held 0x2ff00000 until #2807); the exponential and DIRECT decays keep PPSSPP's rule of ending only below the level, because no PSP row measures them at the level; other levels, the refusal of a negative level or one above 0x40000000, and the level's effect under the other decay curves are unmeasured and not reviewed here (#2807)",
    },
    # The SAS mix group's handlers that PSP-SAS-003 (sas-mix) measures and this runtime reproduces (#2807).
    # The record also calls __sceSasSetVoice, __sceSasSetVoicePCM, __sceSasSetPitch and __sceSasCore; they stay
    # unreviewed: the document publishes no SetVoice, SetVoicePCM or SetPitch answer, and the mixer's output
    # differs from the record (no key-on delay, no in-between sample, the PCM pitch, a negative volume's rounding).
    "h_SasCoreWithMix": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-SAS-003: sas-mix-*)",
            "tests/native/hle_thread_selftest.c:test_sas_mix_psp_sas_003_withmix_cells",
        ],
        "limitation": "this status covers the measured cells only: PSP-SAS-003 is one acceptance-eligible launch on a PSP-3000 (system software 6.61, game context) of the probe built from 3872796c, 204 cells and the done record all PASS, and it calls __sceSasCoreWithMix once per cell on a core of grain 256 initialised fresh, over caller PCM (i - 256) * 97 in shorts 0..511; the selftest replays those calls and asserts the published words in the three windows the probe reads: with no voice keyed, (0x1000, 0x1000) returns 0 and leaves the buffer unchanged, (0x800, 0x800) returns 0 and halves it (0x9f00 becomes 0xcf80), and (0, -0x1000), (0x2000, 0x3000) and (0x7fffffff, 0x80000000) return 0x80420018 and leave it untouched (sas-mix-w-zero-neg-calls, sas-mix-w-over-calls, sas-mix-w-extreme-calls); with voice 0 keyed at 0x1000, (0, 0) returns 0 and leaves the voice alone (0x06fe) and (0x1000, 0x1000) adds it to the caller's PCM (0x9f61 + 0x06fe = 0xa65f); in raw output mode (1), (0x1000, 0x1000) returns 0x80000004 and leaves the buffer untouched (sas-mix-w-raw-unity-calls); the handler accepted every gain and mixed in raw mode until #2807; it now refuses a gain below 0 or above 0x1000 on either side, a range the measured points bound but do not pin: 0x1001 to 0x1fff and -0xfff to -1 are unmeasured, as are the rounding of an odd sample at a gain below 0x1000, raw mode with other gains, the order of the raw-mode, gain, core, address and dispatch checks against each other, the XMB context and other consoles; they are not reviewed here (#2807)",
    },
    "h_SasSetVolume": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-SAS-003: sas-mix-*)",
            "tests/native/hle_thread_selftest.c:test_sas_mix_psp_sas_003_volume_cells",
        ],
        "limitation": "this status covers the measured cells only: in PSP-SAS-003 (one acceptance-eligible launch on a PSP-3000, system software 6.61, game context, of the probe built from 3872796c) __sceSasSetVolume sets voice 0 of a fresh core of grain 256, which plays a flat VAG held at the envelope's top, and the probe reads the grain after four warm-up grains; the selftest replays those calls and asserts the published words: with no __sceSasSetVolume after __sceSasInit every plane peaks at 0x06fe (output mode 0 and raw mode 1), 0 on all four sides is silent, 0x800 gives 0x037f, (0, 0, 0x1000, 0x1000) in raw mode leaves the dry planes 0 and the send planes at 0x06fe, -0x1000 on any side returns 0 and plays that side inverted, +0x1001 and -0x1001 on any side return 0x80420018 and keep the previous volume (0x800, so 0x037f on every plane), and a second __sceSasInit after it restores 0x06fe; the handler already answered all of these, so #2807 changes no code here; the inverted value is not claimed: the PSP plays 0xf901 and this runtime 0xf902, because its mixer (__sceSasCore, unreviewed) applies the envelope to the sample before the volume; other volumes, the core and voice-index refusals, a call on a sounding voice, the XMB context and other consoles are unmeasured and not reviewed here (#2807)",
    },
    # sceDmacMemcpy / sceDmacTryMemcpy. The measured contract is implemented and
    # regression-tested through production dispatch: the illegal-size and
    # illegal-address classes, whole-span validation with overflow-safe
    # arithmetic, failure atomicity (no byte written, no GPU dirty),
    # memmove-correct same-pointer and overlapping copies, and full-span copies
    # (the PSP-3000 size matrix copied fully valid spans completely through
    # 0x100000; there is no API-wide 0xC000 ceiling, see docs/ARCHITECTURE.md).
    # The measured cross-thread contract is modelled too: a Try during another
    # thread's pending transfer returns BUSY and a blocking call waits for it.
    # The handlers remain partial because the transfer's guest-time duration is
    # unmeasured (its window closes at a scheduling decision, not after a time)
    # and the precedence of validation for an invalid truncated tail is not
    # established by the available evidence.
    "h_DmacMemcpy": {
        "status": "partial",
        "limitation": "active-transfer guest-time duration unmeasured (ordering-only window) and invalid truncated-tail validation precedence unmodeled (#303, #341)",
    },
    "h_DmacTryMemcpy": {
        "status": "partial",
        "limitation": "active-transfer guest-time duration unmeasured (ordering-only window) and invalid truncated-tail validation precedence unmodeled (#303, #341)",
    },
    # This returns no error while the virtual ISO-backed UMD model reports its
    # disc present (activated 0x32 or deactivated 0x12). Other drive-error states
    # remain outside the modeled contract.
    "h_UmdGetErrorStat": {
        "status": "compatibility",
        "limitation": "returns no error while the virtual ISO disc is present, activated or not; the deactivated drive's error stat and other drive-error states are unmodeled (#341)",
    },
    "h_UmdActivate": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_umd_unregister_callback",
            "PPSSPP sceUmd.cpp callback-status gate with the compiled SDK version",
        ],
        "limitation": "the callback argument follows PPSSPP (0x22 when no compiled SDK version is set, 0x32 otherwise) and is host-selftested, not hardware measured; the activation result and argument checks are not hardware measured either",
    },
    "h_UmdDeactivate": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_umd_state_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (drive state 0x32 at entry, 0x12 after sceUmdDeactivate)",
        ],
        "limitation": "moves the modeled drive to the measured deactivated state 0x12 and notifies the callback with that state, as PPSSPP does; the callback argument, the return value (0) and argument checks are not hardware measured",
    },
    "h_UmdCancelWaitDriveStat": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_umd_cancel_wait_drive_stat",
            "PPSSPP sceUmd.cpp sceUmdCancelWaitDriveStat (c9e2afb6): every drive waiter resumes with SCE_KERNEL_ERROR_WAIT_CANCEL and the call returns 0",
        ],
        "limitation": "the results follow PPSSPP and are host-selftested, not hardware measured; a stronger waiter runs at once, as after sceKernelCancelSema, which PPSSPP does not do and no measurement settles (#341)",
    },
    "h_UmdUnRegisterUMDCallBack": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/umd/register.expected (an unmatched, zero or second unregister 0x80010016; a match returns the uid up to SDK version 0x3000000 and 0 above it)",
            "tests/native/hle_thread_selftest.c:test_wc_umd_unregister_callback",
            "tests/native/hle_thread_selftest.c:test_umd_unregister_callback",
        ],
        "limitation": "one callback slot; the file's last \"Zero\" row, uid 0 answering 0 after a register and unregister cycle, is not modelled (#341)",
    },
    "h_IoClose": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-write)",
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: io-open-0000..0012)",
            "tests/native/hle_thread_selftest.c:test_umd_io_console_model",
            "tests/native/hle_thread_selftest.c:test_io_async_and_path_imports",
        ],
        "limitation": "the measured cells establish close result 0 for one freshly written host0 file and for the eleven successful fixed UMD/disc/msstor0p1 opens; PSP-HLE-STDIO-001 measures sceIoClose of the Stdout handle as refused with nothing closed in both launch contexts, 0x800200d1 from the XMB and 0x80020325 in the game context (one launch); the runtime answers the game context's 0x80020325 because it starts its programs as games, and the Stdin and Stderr handles answer that refusal as a modelled, unmeasured result; double close, directory descriptors, the descriptors 0, 1 and 2 named by number and error precedence are not hardware measured, and the refusal while an asynchronous request is pending (ASYNC_BUSY) is covered by the host selftest only (#341)",
    },
    "h_IoDclose": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: io-dopen-0004 and io-dopen-0005 close results)",
            "tests/native/hle_thread_selftest.c:test_umd_io_console_model",
        ],
        "limitation": "the measured cells establish close result 0 for the two successful disc0:/ and disc0:/PSP_GAME directory descriptors only; closing a file, stale or out-of-range descriptor, and listings that were read before the close remain host-selftested only (#341)",
    },
    # Common Memory Stick devctls have explicit outputs; unsupported devices or
    # command pairs remain visible per pair and are tracked under issue #341.
    "h_IoDevctl": {
        "status": "partial",
        "limitation": "Memory Stick insert/eject callbacks, including the mscmhc0: 0x02015804/0x02015805 forms, are notified once, with argument 1, when registered; no later insert or eject event exists, and the 32-entry bound, duplicate entries and 0x80010016 refusals follow PPSSPP's model, not a hardware measurement (#1378); the GETDEV capacity under SR_DETERMINISTIC_CLOCK=1 is a project-chosen virtual card (2 GiB, 32 KiB clusters, minus the Memory Stick folder's files), not a measured one (#2161)",
    },
    "h_NetEtherNtostr": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_named_refusals",
        ],
        "limitation": "formats a readable six-byte MAC address into the bounded 18-byte lowercase text form without a network stack and returns the text address; the return value (PPSSPP treats the call as void), invalid pointer precedence and the complete text domain are not hardware measured",
    },
    "h_IoDopen": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_umd_io_console_model",
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: io-dopen-0000..0005)",
        ],
        "limitation": "the measured cells establish results and close outcomes for six fixed UMD/disc path calls only; directory contents/order, MS0 paths and general error precedence remain unmeasured (#341)",
    },
    "h_CreateThread": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/create.expected (2ed7c9c5, recorded on a PSP): NULL name 0x80020001, a 64-character name cut to 31, attribute bits outside 0xf8f060ff 0x80020191, stack under 0x200 0x80020194, 0x01000001 rounded to 0x01000100, priority outside 8..0x77 0x80020193, NULL entry creates, 0xDEADBEEF entry 0x800200D3, two threads with one name",
            "tests/native/hle_thread_selftest.c:test_create_thread_contract",
            "docs/HARDWARE_ORACLE.md (PSP-NULLCODE-001: CreateThread with a NULL entry returned a UID)",
            "docs/research/PSP_THREADING_SEMANTICS.md (FIRMWARE_CREATE_THREAD_ARG5_T0: attribute arrives in $t0)",
        ],
        "limitation": "the argument checks above match the recorded cells one argument at a time; which error wins when several arguments are wrong is not measured, kernel-mode creates (priority under 8) are not modelled, the entry check is only 'readable guest memory', the SceKernelThreadOptParam in $t1 is ignored, and the 'Create 1024' cell (a PSP fails the 29th 1 MiB stack with 0x80020190) is not reproduced because the stack arena's capacity differs (#341)",
    },
    "h_StartThread": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/start.expected (2ed7c9c5, recorded on a PSP): ids (0 -> 0x80020197, unknown or deleted -> 0x80020198, twice or current -> 0x800201a4), a negative argument length or a block that is not memory -> 0x800200d3 with the thread left dormant, a NULL block starts with no argument, the copied block at stack + size - 0x100 - round16(len); docs/HARDWARE_ORACLE.md PSP-NULLCODE-001: entry sp 0x40 below a four-byte block",
            "tests/native/hle_thread_selftest.c:test_start_thread_contract",
            "tests/native/hle_thread_selftest.c:test_start_thread_error_semantics",
            "docs/HARDWARE_ORACLE.md (PSP-NULLCODE-001: StartThread of a NULL-entry thread returned 0)",
        ],
        "limitation": "the 'Priorities' resched/deferred cells (marked reorderable in start.expected), the 'Stack' cells (a thread started with the Low-stack, no-fill or clear-stack attributes) and an argument block larger than the stack (a PSP crashes; the runtime drops the block and logs) are not reproduced; the entry $sp is argv - 0x40 for every length although only the four-byte record measures it, scratchpad and kernel-segment (0x88800000) argument blocks are unmeasured (the first is accepted; the second is accepted although a PSP may refuse it), the thread's initial $sp without an argument and the order of the not-dormant and argument errors are not measured (#341)",
    },
    "h_DisplaySetMode": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/display/setmode.expected (1a9e6c31, recorded on a PSP): mode 0 at 480x272 returns 0, any other mode 0x80000107, mode 0 at another size 0x80000104, mode checked first",
            "tests/native/hle_thread_selftest.c:test_title_config_hle_bindings",
        ],
        "limitation": "only the argument results are modelled: the PSP blocks until a vblank ('Vblank after set: 1'), the runtime returns at once, and sceDisplayGetMode reports the fixed mode 0 at 480x272 (#341)",
    },
    "h_GeListUpdateStallAddr": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/gpu/ge/queue.expected (2ed7c9c5, recorded on a PSP): an id naming no list (0, or a dequeued list) returns 0x80000100, updating an enqueued list returns 0",
            "tests/native/hle_thread_selftest.c:test_ge_update_stall_unknown_id",
            "tests/native/hle_thread_selftest.c:test_ge_guest_sentinel",
        ],
        "limitation": "a list the runtime already ran to its END returns 0 as a deliberate no-op (a GU ring buffer issues one update per chunk), where the 'Completed' cell returns 0x80000020; the 'Same stackAddr' cells that depend on the compiled SDK version are not compared (#341)",
    },
    "h_IoGetstat": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/io/stat/stat.expected (1a9e6c31, recorded on a PSP): an ms0 file reports mode 0x21ff attr 0x0020 and its size, a directory mode 0x11ff attr 0x0010, bytes [0..63] are written and st_private is left untouched",
            "tests/native/hle_thread_selftest.c:test_ms0_unified_namespace",
        ],
        "limitation": "only the Memory Stick host leg is held to the recorded shape; the PSP's ctime/atime/mtime are written as zero, the ms0 volume root (0x80010016 on a PSP) and the disc, data-archive and flash0 legs have no recorded stat cell, and the SceIoStat pointer is not validated (#341)",
    },
    "h_IoDread": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/io/directory/directory.expected (2ed7c9c5, recorded on a PSP): a bad descriptor returns 0x80020323; tests/io/stat/stat.expected (1a9e6c31): the listing reports the same mode and attr as sceIoGetstat",
            "tests/native/hle_thread_selftest.c:test_fd_namespace",
            "tests/native/hle_thread_selftest.c:test_ms0_unified_namespace",
        ],
        "limitation": "the PSP lists '.' and '..' (with a 4096-byte '..'), upper-cases 8.3 names and fills times; the runtime's listing order, size of directory entries, names' case and timestamps are not compared, d_private is zeroed where a PSP leaves the caller's pointer, st_private[0] carries the LBA, and only ms0:, fatms0: and device-less listings take the FAT mode/attr: disc0 and flash0 listings keep their 0x124 mode bits, which no cell measures (#341)",
    },
    "h_GetThreadIdSched": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/start.expected (2ed7c9c5, recorded on a PSP): StartThread(sceKernelGetThreadId()) from the running thread is 0x800201a4, and no 'stack should start with thread ID' line appears, so the stack-base and k0 UID words equal the id",
            "tests/native/hle_thread_selftest.c:test_get_thread_id_names_calling_thread",
        ],
        "limitation": "the value is the runtime's UID for the calling thread, not the number a PSP would assign; the result with no guest thread current is not measured (#341)",
    },
    "h_IoLseek": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-lseek and io-append)",
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-SEM-001: io-seek64, io-seek64-rdwr)",
            "docs/HARDWARE_ORACLE.md (PSP-IO-EOF-001: io-eof-seek-negative, io-eof-seek-negative-rdonly, io-eof-write-past, io-eof-read-past)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-001: io-seek-flash-*)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-002: io-edge-flash-*)",
            "docs/HARDWARE_ORACLE.md (PSP-IO-EOF-002: umd-eof-raw-seek-past)",
            "tests/native/hle_thread_selftest.c:test_flash0_font_device",
            "tests/native/hle_thread_selftest.c:test_umd_block_device_sectors",
            "tests/native/hle_thread_selftest.c:test_io_memory_stick_cells_through_syscall",
        ],
        "limitation": "PSP-IO-001 measures 64-bit SEEK_SET 32, SEEK_CUR -16, SEEK_END 0 and the end position after a 32-byte append on one host0 file; PSP-IO-EOF-001 measures a Memory Stick seek before the start as EINVAL with the position unchanged (pspautotests revision f573fe8c9122a8ff5db4f030d74557bed644b4e0 records -1 there, kept for other devices), a seek and write past the end extending the file, and a read past the end returning 0; PSP-KERNEL-SEM-001 measures a Memory Stick position as 32 bits wide (0xFFFFFFFF accepted, beyond it EINVAL with the position unchanged); other devices keep IO_LSEEK_OUT_OF_RANGE there (#1380); PSP-HLE-IO-SEEK-001 measures a read-only flash0: file refusing SEEK_SET and SEEK_END destinations before the start or past the end with 0x80010016 and the position unchanged (high word 0xFFFFFFFF at -1 and size + 16, 0 at 0xFFFFFFFF and 0x100000000 from SEEK_SET), measured on ltn0.pgf; PSP-HLE-IO-SEEK-002 adds, on flash0:/font/ltn0.pgf, SEEK_CUR before the start and past the end, SEEK_SET past 0x100000000 and SEEK_END to 0xFFFFFFFF and 0x100000000, all 0x80010016 with the position unchanged and the high word 0 exactly when the destination is at or above 0x80000000; the synchronous calls follow that rule on that file only; the async calls, which keep the earlier rule, and other flash0 files are unmeasured; invalid whence and error precedence remain unmeasured (#341). UMD block-device seeks follow PSP-UMDRAW-001's captured but not acceptance-qualified cells (sector positions, SEEK_END at the last sector, 0x8001b003 for a negative or far-past-the-disc position; the negative refusal rests on them alone) and PSP-IO-EOF-002's measured edge: one and two sectors past the last sector, SEEK_END +1 and SEEK_SET 0x100000000 answer 0x8001b003 with the high word 0xFFFFFFFF and the position unchanged",
    },
    "h_IoMkdir": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ms0_unified_namespace",
            "src/rt/hle_io.c:h_IoMkdir",
        ],
    },
    "h_IoOpen": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-open-create, io-write, io-read-verify, io-append, io-errors)",
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: io-open-0000..0012)",
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-SEM-001: io-missing-parent, io-excl-new, io-excl-existing)",
            "docs/HARDWARE_ORACLE.md (PSP-IO-EOF-001: io-eof-missing)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-001: io-seek-ms-*)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-002: io-edge-ms-*)",
            "tests/native/hle_thread_selftest.c:test_ms0_unified_namespace",
            "tests/native/hle_thread_selftest.c:test_io_memory_stick_cells_through_syscall",
        ],
        "limitation": "measured coverage is one host0 file's create/write/read/append/missing-file path, PSP-UMDIO-001's fixed open paths and flags, PSP-KERNEL-SEM-001's Memory Stick exclusive create and PSP-IO-EOF-001's missing-file opens; an open without PSP_O_CREAT never creates a file (write-only, read-write, read-only and write-only with PSP_O_TRUNC each answer 0x80010002), a create under a missing directory is 0x80010002, PSP_O_CREAT|PSP_O_EXCL on an existing file is EEXIST with the file unchanged, and PSP_O_RDONLY|PSP_O_CREAT on a missing Memory Stick file is 0x80010016 and creates nothing (#1380, measured on ms0:, followed on fatms0:); PSP-HLE-IO-SEEK-002 measures on ms0: that PSP_O_RDONLY|PSP_O_CREAT opens an existing file read-only, that adding PSP_O_TRUNC on an existing file is 0x80010016 with the file not cut, and that adding PSP_O_EXCL on a missing name in an existing directory is 0x80010016 with nothing created; only the PSP_O_TRUNC (0x0601) and PSP_O_EXCL (0x0A01) opens never consult the disc; a read-only 0x0201 ms0: open whose name also exists on the disc opens the disc file, even when the Memory Stick holds that file, as every read-only ms0: open does (unchanged, unmeasured); every other read-only create (an existing fatms0: file, PSP_O_TRUNC or PSP_O_EXCL on fatms0:, PSP_O_APPEND, PSP_O_TRUNC on a missing ms0: file, PSP_O_EXCL on an existing ms0: file or under a missing parent directory) is unmeasured and stops at IO_OPEN_READ_ONLY_CREATE (0x80010086, nothing opened or changed); other flag combinations and general error precedence remain unmeasured (#341). UMD block-device reads and seeks are covered under sceIoRead and sceIoLseek",
    },
    "h_IoRead": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-read-verify and io-errors)",
            "tests/native/hle_thread_selftest.c:test_ms0_unified_namespace",
            "tests/native/hle_thread_selftest.c:test_umd_block_device_sectors",
            "tests/native/hle_thread_selftest.c:test_umd_read_parks_its_thread_for_the_drives_time",
        ],
        "limitation": "PSP-IO-001 measures one 64-byte host0 read that returns 64 matching bytes, and that reading descriptor -1 returns a negative result (the exact code is not recorded in public evidence); pspautotests revision f573fe8c9122a8ff5db4f030d74557bed644b4e0 records a read past the end of a file returning 0 (#1380); short reads at end of file, zero-length file reads, partial-read position advance, unreadable buffers, write-only descriptors and standard-stream reads remain unmeasured (#341). UMD block-device reads follow PSP-UMDRAW-001's captured but not acceptance-qualified cells (counts and results in 2048-byte sectors, the position advancing by the count, 0x80010016 for a zero count); a read crossing the last sector stays at the raw-device boundary, and the runtime refuses a destination outside guest memory where the console overran the buffer. A read of a file of the disc parks its thread for a synthetic drive time, the low estimate of PPSSPP's sceIo.cpp (100 bytes per microsecond, at least 100 microseconds, no seek or spin-up time): no UMD read time is measured, nor whether two readers queue, nor a read made with dispatch or interrupts disabled or outside a guest thread, which returns at once (#1296, #2361)",
    },
    "h_IoRemove": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ms0_unified_namespace",
            "src/rt/hle_io.c:h_IoRemove",
        ],
    },
    "h_IoWrite": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-write and io-append)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-001: io-seek-ms-*)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-002: io-edge-ms-*)",
            "tests/native/hle_thread_selftest.c:test_ms0_unified_namespace",
            "tests/native/hle_thread_selftest.c:test_io_memory_stick_cells_through_syscall",
        ],
        "limitation": "PSP-IO-001 measures a 64-byte write that returns 64 on a created host0 file and a 32-byte write through an append-mode descriptor that returns 32; PSP-HLE-IO-SEEK-001 measures a write through a PSP_O_RDONLY Memory Stick descriptor as 0x80020323 with the file unchanged (#1380); PSP-HLE-IO-SEEK-002 measures sceIoWriteAsync on an idle PSP_O_RDONLY ms0: descriptor returning 0x80020323 at once with nothing queued; with a request pending both calls answer the busy error first (unmeasured), and a fatms0: sceIoWriteAsync queues and completes with this write's refusal (unmeasured); read-only descriptors on other devices, short writes, zero-length writes, unreadable source buffers and raw devices remain unmeasured (#341); PSP-HLE-STDIO-001 measures a one-byte write through the Stdout and the Stderr handle returning 1 in both launch contexts, a larger standard-stream write and a write to descriptor 0, 1 or 2 named by number return the count unmeasured, and a write through the Stdin handle is unmeasured and stops at the named boundary IO_STD_HANDLE_OPERATION (0x80010086); a write that would carry the 32-bit position past its range fails closed with IO_WRITE_OUT_OF_RANGE (#1380)",
    },
    # Async IoFileMgr requests retain per-fd results and callbacks. Their
    # deterministic import-boundary service interval is synthetic timing, not
    # a PSP scheduler measurement; open also returns its descriptor at submit.
    "h_IoOpenAsync": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_fd_namespace",
            "tests/native/hle_thread_selftest.c:test_io_open_async_notifies_later_callback",
        ],
        "limitation": "open runs its host lookup at submission to return the PSP-visible fd, then completes as a pending request on the import-boundary service and notifies the fd's async callback; async timing and callback ordering are synthetic, not hardware-measured, and a failed open returns its error directly instead of an fd carrying it",
    },
    "h_IoReadAsync": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; PSP timing and callback ordering remain unmeasured",
    },
    "h_IoWriteAsync": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-002: io-edge-ms-*)",
            "tests/native/hle_thread_selftest.c:test_io_async_and_path_imports",
            "tests/native/hle_thread_selftest.c:test_io_memory_stick_cells_through_syscall",
        ],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; PSP timing and callback ordering remain unmeasured; PSP-HLE-IO-SEEK-002 measures an idle PSP_O_RDONLY ms0: descriptor refusing at once with 0x80020323 and queueing nothing; with a request pending the busy error comes first (unmeasured, as sceIoWrite), and fatms0: and other devices queue the write and complete with sceIoWrite's answer (unmeasured)",
    },
    "h_IoLseekAsync": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_io_async_and_path_imports",
            "tests/native/hle_thread_selftest.c:test_flash0_font_device",
        ],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; 64-bit seek timing and callback ordering are not hardware-measured; a flash0: destination follows sceIoLseek's PSP-HLE-IO-SEEK-001 rule, high word included, which was measured through sceIoLseek and sceIoLseek32 only",
    },
    "h_IoLseek32Async": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_io_async_and_path_imports",
            "tests/native/hle_thread_selftest.c:test_flash0_font_device",
        ],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; PSP timing and callback ordering remain unmeasured; no asynchronous seek is measured, so a flash0: destination follows sceIoLseekAsync's rule, not sceIoLseek32's PSP-HLE-IO-SEEK-002 rule",
    },
    "h_IoIoctlAsync": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_io_async_and_path_imports",
            "tests/native/hle_thread_selftest.c:test_io_memory_stick_cells_through_syscall",
        ],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; PSP ioctl timing and callback ordering remain unmeasured; the Memory Stick file commands PSP-HLE-IO-SEEK-001 measured through sceIoIoctl (0x01010005, 0x01020003, 0x01020004, 0x01020006, 0x01020007, 0x01030008) are not measured asynchronously; the request reaches the same driver, so its completion follows sceIoIoctl's rule: the measured form completes with 0x80020325 and changes nothing, and other forms of those commands complete at IO_MS_FILE_IOCTL (0x80010086)",
    },
    "h_IoCloseAsync": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_fd_namespace"],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; close callback timing remains unmeasured; sceIoCloseAsync of a standard handle (sceKernelStdin, sceKernelStdout, sceKernelStderr) is unmeasured and stops at the named boundary IO_STD_HANDLE_OPERATION (0x80010086) with nothing queued or closed (#341)",
    },
    "h_IoWaitAsync": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "the host operation runs when the guest waits; PSP blocking, wake, and callback interleaving remain unmeasured",
    },
    "h_IoWaitAsyncCB": {
        "status": "partial",
        "limitation": "callback dispatch is modeled at the wait boundary; PSP blocking and callback interleaving remain unmeasured",
    },
    "h_IoPollAsync": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "the pending interval is a deterministic synthetic import-boundary policy, not PSP-measured timing",
    },
    "h_IoGetAsyncStat": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "the pending interval is a deterministic synthetic import-boundary policy, not PSP-measured timing",
    },
    "h_IoChangeAsyncPriority": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "priority selects queued host requests, but PSP priority ordering and its interaction with callbacks are not hardware-measured",
    },
    "h_IoSetAsyncCallback": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "completion queues the modeled thread callback; PSP callback argument and interleaving behavior remain unmeasured",
    },
    "h_IoRmdir": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "empty-directory removal and nonempty refusal are tested with synthetic roots; PSP error precedence beyond those cases remains unmeasured",
    },
    "h_IoChdir": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "the modeled current directory is per-thread and resolves contained Memory Stick paths; PSP behavior for other devices is not established",
    },
    "h_IoSync": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "Memory Stick handles are flushed and synced; other devices and nonzero unknown arguments fail closed with IO_SYNC_DEVICE or IO_SYNC_ARGUMENT",
    },
    "h_IoChstat": {
        "status": "partial",
        "evidence": ["tests/native/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "only the mode field is supported; attribute, size, and timestamp fields fail closed with IO_CHSTAT_NON_MODE_FIELDS",
    },
    "h_Memset": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_sysclib_memory_imports",
            "src/rt/hle_kernel_utils.c:h_Memset",
            "public C library contract (memset)",
        ],
        "description": "Fills a fully validated guest span and returns the guest destination.",
    },
    "h_KernelStdin": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-STDIO-001: stdio-handles)",
            "tests/native/hle_thread_selftest.c:test_std_handles_measured",
        ],
        "limitation": "this status covers the returned handle value only, 0x0093a513; PSP-HLE-STDIO-001 recorded this kernel UID on one PSP-3000 in both measured launch contexts, which agree on it (from the XMB on boots 135 and 222, in the game context on boot 200; ARK-5 5.1.0 and 5.2.0), every launch through PSPLink, whose tty the handle is; its value on a launch without PSPLink, on another console and on other system software is unmeasured; a read from Stdin is UNMEASURED and unclaimed (the probe runs none): sceIoRead and sceIoReadAsync through the handle still give the runtime's earlier standard-stream error 0x80010009, which is not a PSP result and is not reviewed here; no other call on the Stdin handle is measured either: sceIoClose answers the refusal modelled from the Stdout close and closes nothing, and sceIoWrite, sceIoWriteAsync and sceIoCloseAsync stop at the named boundary IO_STD_HANDLE_OPERATION (0x80010086) (#341)",
    },
    "h_KernelStdout": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-STDIO-001: stdio-handles, stdio-write-stdout, stdio-close-stdout)",
            "tests/native/hle_thread_selftest.c:test_std_handles_measured",
            "tests/native/hle_thread_selftest.c:test_std_handles_unmeasured_are_named",
        ],
        "limitation": "returns 0x00938a15; PSP-HLE-STDIO-001 recorded this kernel UID on one PSP-3000 in both measured launch contexts, which agree on it (from the XMB on boots 135 and 222, in the game context on boot 200; ARK-5 5.1.0 and 5.2.0), every launch through PSPLink, whose tty the handle is; its value on a launch without PSPLink, on another console and on other system software is unmeasured; measured in both contexts and reproduced through the handle: a one-byte sceIoWrite returns 1 and the import returns the same handle after the write and after the close; sceIoClose is refused and closes nothing in both contexts, with 0x800200d1 from the XMB and 0x80020325 in the game context (one launch); the runtime answers the game context's 0x80020325 because it starts its programs as games; unmeasured in either context: a write of more than one byte and sceIoWriteAsync (both return the count), sceIoRead, sceIoLseek, sceIoLseek32 and sceIoIoctl (the runtime's earlier standard-stream error 0x80010009, not a PSP result), and sceIoCloseAsync, which stops at the named boundary IO_STD_HANDLE_OPERATION (0x80010086) (#341)",
    },
    "h_KernelStderr": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-STDIO-001: stdio-handles, stdio-write-stderr)",
            "tests/native/hle_thread_selftest.c:test_std_handles_measured",
            "tests/native/hle_thread_selftest.c:test_std_handles_unmeasured_are_named",
        ],
        "limitation": "returns 0x00936f17; PSP-HLE-STDIO-001 recorded this kernel UID on one PSP-3000 in both measured launch contexts, which agree on it (from the XMB on boots 135 and 222, in the game context on boot 200; ARK-5 5.1.0 and 5.2.0), every launch through PSPLink, whose tty the handle is; its value on a launch without PSPLink, on another console and on other system software is unmeasured; measured in both contexts and reproduced through the handle: a one-byte sceIoWrite returns 1 and the import returns the same handle afterwards; unmeasured in either context: sceIoClose, which answers the refusal modelled from the Stdout close (0x80020325) and closes nothing, a write of more than one byte and sceIoWriteAsync (both return the count), sceIoRead, sceIoLseek, sceIoLseek32 and sceIoIoctl (the runtime's earlier standard-stream error 0x80010009, not a PSP result), and sceIoCloseAsync, which stops at the named boundary IO_STD_HANDLE_OPERATION (0x80010086) (#341)",
    },
    "h_Strlen": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_sysclib_memory_imports",
            "src/rt/hle_kernel_utils.c:h_Strlen",
            "public C library contract (strlen)",
        ],
        "description": "Returns the byte length through a guest NUL terminator within readable memory.",
    },
    "h_SysclibSnprintf": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_sysclib_snprintf",
            "uofw/uofw f7b0623168842f02cddbdbf9437ac0c2380c51c8 src/kd/sysmem/sysclib.c prnt/snprintf",
            "hrydgard/ppsspp cdf563b85c42c275ec2835a0883f48d63a9c9406 Core/HLE/sceKernelInterrupt.cpp sysclib_snprintf",
        ],
        "limitation": "positive-capacity truncation and the full count are reference-derived, not PSP-measured; handles bounded literal text and one-word integer, character and string fields with space padding and bounded string precision; zero capacity, unreadable arguments, and other format forms stop by name (#341)",
    },
    "h_ReferThreadStatus": {
        "status": "partial",
        "limitation": "run clocks use host scheduler time and preemption/release counters are modeled rather than PSP-measured (#311)",
    },
    # Mailboxes (issue #339). Measured contracts live in docs/HARDWARE_ORACLE.md:
    # unknown id 0x8002019B, timeout 0x800201A8 with
    # remaining 0, CancelReceiveMbx waiters 0x800201A9, empty poll 0x800201B2,
    # FIFO circular next links, attr 0x400 ascending msgPriority, waitType 5.
    # Each handler stays partial until its own named gaps are closed.
    "h_CreateMbx": {
        "status": "partial",
        "limitation": "NULL-name create error and interrupt-context placement unmeasured (#339, #341)",
    },
    "h_DeleteMbx": {
        "status": "partial",
        "limitation": "delete-while-waited is mailbox-measured on 2026-10-15 (PSP-3000, 6.61): the waiter returns WAIT_DELETE and its timeout word is rewritten to the remaining time; interrupt-context placement unmeasured (#339, #341)",
    },
    "h_SendMbx": {
        "status": "partial",
        "limitation": "invalid-message-pointer error class unmeasured; interrupt-context placement unmeasured (#339, #341)",
    },
    "h_ReceiveMbx": {
        "status": "partial",
        "limitation": "delete-while-waited is mailbox-measured on 2026-10-15 (WAIT_DELETE and timeout word rewritten to the remaining time); ILLEGAL_CONTEXT precedence for a blocking receive unmeasured; priority-mailbox circular linking not separately asserted beyond campaign facts (#339, #341)",
    },
    "h_ReceiveMbxCB": {
        "status": "partial",
        "limitation": "callback-dispatch interleaving during a mailbox wait unmeasured; ILLEGAL_CONTEXT precedence unmeasured (#339, #341)",
    },
    "h_PollMbx": {
        "status": "partial",
        "limitation": "invalid-message-pointer error class unmeasured (#339, #341)",
    },
    "h_CancelReceiveMbx": {
        "status": "partial",
        "limitation": "cancel with zero waiters and invalid numWait-thread pointer error class unmeasured (#339, #341)",
    },
    "h_ReferMbxStatus": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-STATUS-001: mbx-size-zero, mbx-size-8, mbx-size-40, and mbx-size-full)",
        ],
        "limitation": "the cited cells establish bytes written and size-word behavior at four buffer sizes; full field values, unknown UID codes, invalid pointers, and error precedence are not established by that size row (#339, #341)",
    },
    # sceKernelReferSystemStatus: bytes written = min(size, 0x1c), return 0, and the size word reads back
    # 0x1c, all PSP-measured (PR #1003 head 3072bd7d, 2026-10-15).
    "h_ReferSystemStatus": {
        "status": "partial",
        "limitation": "idle-clock, comes-out-of-idle and thread-switch counters: written as zero; hardware reports non-zero, time-dependent values (not cited). VFPU-switch counter: written as zero, not cited. Status word 1: written as zero; hardware read 0 in all three records, uncited. NULL pointer: runtime returns ILLEGAL_ADDR as a safety choice after PSP-3000 hung on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001); no game relies on the output. Other unwritable non-NULL pointers are unmeasured.",
    },
    # VTimer lifecycle and query surface (PSP-HLE-VTIMER-001, 2026-10-15, PR #1003 record). Measured:
    # Create returns a valid UID; Start returns 0, also on a restart, and 1 while running (per
    # docs/HARDWARE_ORACLE.md); Stop returns 1 while running and 0 when not started; Delete of a
    # running timer returns 0; Refer and Delete on the deleted UID return 0x800201BE. Refer fills
    # active, base (non-zero while running), current, and reads schedule, handler and common as 0.
    # PSP-HLE-VTIMER-002 and -003 (2026-10-15, build b5d6fc5b) add: Start, Stop and GetVTimerTime
    # on a deleted UID return 0x800201BE and that GetVTimerTime writes nothing; Delete of a stopped
    # timer returns 0; 64 creates in a row succeed; GetVTimerBase equals Refer's base while running
    # and both read 0 once stopped. The microsecond unit and base = system time at Start are
    # consistent with that run (deltas agree within about 8%), not measured exactly. Interrupt-driven
    # behaviour and handlers depend on issue #325.
    "h_CreateVTimer": {
        "status": "partial",
        "limitation": "the option struct is ignored, as on a PSP (pspautotests vtimers/create.expected at 2ed7c9c5, recorded on a PSP: every declared size creates the same timer), and an unreadable one stops the run by name; 64 creates in a row are measured (PSP-HLE-VTIMER-003), so the table holds 64, while the same PSP-recorded output shows 1024 creates succeed; a NULL name is accepted here, where that output shows 0x80020001; the no-slot code 0x80020190 is unmeasured; the microsecond unit is consistent with PSP-HLE-VTIMER-002, not a precise rate measurement (#325)",
    },
    "h_StartVTimer": {
        "status": "partial",
        "limitation": "deleted-UID code 0x800201BE measured (PSP-HLE-VTIMER-003); already-running return 1 is repo-documented, not measured by PSP-HLE-VTIMER-001, -002 or -003; base = system time at Start and the microsecond unit are consistent with PSP-HLE-VTIMER-002, not measured exactly (#325)",
    },
    "h_StopVTimer": {
        "status": "partial",
        "limitation": "deleted-UID code 0x800201BE measured (PSP-HLE-VTIMER-003); the microsecond unit of the held time is consistent with PSP-HLE-VTIMER-002, not a precise rate measurement (#325)",
    },
    "h_DeleteVTimer": {
        "status": "partial",
        "limitation": "delete of a stopped timer returns 0 (PSP-HLE-VTIMER-002) and a second delete returns 0x800201BE (PSP-HLE-VTIMER-001, -003); interrupt-context placement unmeasured (#325)",
    },
    "h_GetVTimerTime": {
        "status": "partial",
        "limitation": "deleted-UID code 0x800201BE with the output untouched measured (PSP-HLE-VTIMER-003); time = system time - base and the microsecond unit are consistent with PSP-HLE-VTIMER-002 (deltas within about 8%), not a precise rate measurement; NULL out-pointer returns ILLEGAL_ADDR as a runtime safety choice after PSP-3000 hung on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), not as a measured VTimer result; no game relies on the output (#325)",
    },
    "h_GetVTimerTimeWide": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_vtimer_time_wide_and_setters",
            "PSPAutotests tests/threads/vtimers/gettime.expected (2ed7c9c5, recorded on a PSP): Objects (wide), Running (wide)",
        ],
        "limitation": "returns the elapsed time GetVTimerTime writes, as $v0/$v1, and -1 for uid 0, an invalid and a deleted uid (pspautotests-recorded); the microsecond unit carries GetVTimerTime's limits (PSP-HLE-VTIMER-002, not a precise rate measurement)",
    },
    "h_SetVTimerTimeWide": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_vtimer_time_wide_and_setters",
            "tests/native/hle_thread_selftest.c:test_vtimer_set_time_while_scheduled",
            "PSPAutotests tests/threads/vtimers/settime.expected (2ed7c9c5, recorded on a PSP): Objects (wide), Values (wide), While scheduled",
        ],
        "limitation": "makes the value the timer's time and returns the previous time (all 64 bits), -1 for uid 0, an invalid and a deleted uid, and a handler already due runs on the next scheduler pass, not inside the Set (pspautotests-recorded); a running timer counts on from the set value, consistent with the While scheduled cell but its exact microseconds are not measured",
    },
    "h_SetVTimerTime": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_vtimer_time_wide_and_setters",
            "PSPAutotests tests/threads/vtimers/settime.expected (2ed7c9c5, recorded on a PSP): Objects, Values",
        ],
        "limitation": "swaps the time through the SceKernelSysClock pointer and returns 0, 0x800201BE for uid 0, an invalid and a deleted uid with the clock untouched (pspautotests-recorded); a NULL or unwritable pointer (a crash on the PSP per the test's note) stops the run by name; running-timer timing as for SetVTimerTimeWide",
    },
    "h_GetVTimerBase": {
        "status": "partial",
        "limitation": "returns the same base as ReferVTimerStatus, measured equal while running and 0 once stopped (PSP-HLE-VTIMER-002); the value, guest time at the most recent Start, is consistent with that run's system-clock reads, not measured exactly; NULL out-pointer returns ILLEGAL_ADDR as a runtime safety choice after PSP-3000 hung on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), not as a measured VTimer result; deleted-UID code is unmeasured and reused by analogy; no game relies on the NULL output (#325)",
    },
    "h_ReferVTimerStatus": {
        "status": "partial",
        "limitation": "return 0, the deleted-UID code, and the active, current, schedule, handler and common states are measured (PR #1003); base equals GetVTimerBase while running and reads 0 once stopped (PSP-HLE-VTIMER-002), and its value (guest time at Start) and the microsecond unit are consistent with that run, not measured exactly; NULL pointer returns ILLEGAL_ADDR as a runtime safety choice after PSP-3000 hung on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), not a measured VTimer result; the caller-size write rule for size 0 is the sema/evf/mbx rule applied without a VTimer cite (#325)",
    },
    # sceAudioSRC* (issue #339). Measured on PSP-3000 firmware 6.6.1, 2026-10-15
    # (PSP-HLE-AUDIO-SRC-001): the unreserved, reserve, second-reserve, release and
    # Output2-exclusion returns, the sample count returned by OutputBlocking, and the
    # accepted and refused reserve shapes listed in docs/HARDWARE_ORACLE.md.
    "h_AudioSRCChReserve": {
        "status": "partial",
        "limitation": "the returns for an unreserved slot, a held slot (0x80268002, including Output2 holding it), sample counts 0 (0x80000104), 65 and 4096 (accepted), 48000 Hz (accepted) and channel counts 1 (0x80000104) and 4 (0x80000003) are measured (PSP-HLE-AUDIO-SRC-001); the upper sample-count bound reuses the Output2 range 17..4111, channel counts other than 1, 2 and 4 take the code measured for 4, and rates other than 44100 and 48000 are refused with that same code and a message, all unmeasured (#339)",
    },
    "h_AudioSRCChRelease": {
        "status": "partial",
        "limitation": "release after a blocking output (0) and release with nothing reserved (0x80260008) are measured (PSP-HLE-AUDIO-SRC-001); whether a release succeeds while samples are still queued, as the Output2 busy rule does, and releasing a reservation made through the other API (refused here as not reserved) are unmeasured (#339)",
    },
    "h_AudioSRCOutputBlocking": {
        "status": "partial",
        "limitation": "returns 0x80260008 with nothing reserved and the reserved sample count after a reserve (0x400), both measured (PSP-HLE-AUDIO-SRC-001); the volume bound 0xFFFFF (shared with Output2, from PPSSPP caa91b54b535 Core/HLE/sceAudio.cpp) and the NULL or unreadable buffer result, which follows the regular output, are both unmeasured; the host mixer runs at 44100 Hz, so a 48000 Hz block passes through a band-limited converter and keeps its source-rate pacing (#339)",
    },
    # sceVaudio* (#286): played through the SRC slot above. No PSP measurement; the
    # behaviour follows the public reference named at h_VaudioChReserve.
    "h_VaudioChReserve": {
        "status": "partial",
        "limitation": "reserves the SRC slot with the SRC reserve's exclusion (0x80268002) and rates (44100 and 48000 Hz; a rate of 0 is taken as 44100 Hz); the sample counts 256, 576, 1024, 1152 and 2048 (others 0x80000104), stereo format 2 only (others 0x80000108) and the second-reserve word 0x80000021 follow the public reference and are unmeasured on a PSP",
        "evidence": ["tests/native/hle_thread_selftest.c:test_vaudio_plays_through_the_src_slot"],
    },
    "h_VaudioChRelease": {
        "status": "partial",
        "limitation": "releases only a Vaudio reservation (0x80260008 otherwise, so an SRC reservation stays); whether a release waits for queued samples, and whether it frees an SRC reservation as the public reference does, are unmeasured",
        "evidence": ["tests/native/hle_thread_selftest.c:test_vaudio_plays_through_the_src_slot"],
    },
    "h_VaudioOutputBlocking": {
        "status": "partial",
        "limitation": "the SRC blocking output for a Vaudio reservation: returns the reserved sample count, 0x80260008 with no Vaudio reservation, and 0x8026000b above volume 0xFFFFF, a project choice by analogy with the Output2 and SRC bound because this model plays Vaudio through the SRC output (the public reference checks no Vaudio volume); unmeasured on a PSP (#1986)",
        "evidence": ["tests/native/hle_thread_selftest.c:test_vaudio_plays_through_the_src_slot"],
    },
    # Route-A graduation batch 1 (#341, 2026-10-19). Each status is bounded to
    # the named accepted hardware cells; audio's unmeasured argument/error paths
    # remain partial instead of being promoted as universal firmware behavior.
    "h_AudioChRelease": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "tests/fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cells: audio-ch-release, audio-ch-query-released",
        ],
        "limitation": "the cells release channel 0 after the two valid blocking-output calls and query it after release; a release clears the channel's length, format and volumes; the out-of-range (0x80260003) and not-reserved (0x80260008) refusals follow PPSSPP caa91b54b535 Core/HLE/sceAudio.cpp sceAudioChRelease and are unmeasured, as are other channel-state release results (#341)",
    },
    "h_AudioChReserve": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "tests/fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cell: audio-ch-reserve",
        ],
        "limitation": "audio-ch-reserve measures a successful reservation of channel 0 for 1024 stereo samples; automatic channel selection, invalid size or format, duplicate reservation, and exhaustion/error precedence are unmeasured (#341)",
    },
    "h_AudioRestLen": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "tests/fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cells: audio-ch-query-before, audio-ch-query-after, audio-ch-query-released",
            "tools/psp_oracle/manifest.json:PSP-HLE-AUDIO-REST-001",
            "tests/fixtures/psp_oracle/probe_hle_measure.c:hle_arest_size",
        ],
        "limitation": "serves sceAudioGetChannelRestLength and sceAudioGetChannelRestLen; the PSP-AUDIO-001 cells query both on channel 0 before and after blocking output and just after release, and PSP-HLE-AUDIO-REST-001 finds them equal (F or F - 64) after each blocking output on stereo channels of 1024, 2048 and 4096 frames, which is evidence that the two calls give one answer, not that this handler gives the console's: the runtime's blocking output returns with up to two blocks queued (#67), so both calls can read up to 2F there where the console held one block (#279, open); invalid-channel error behavior and other queue states are unmeasured for both calls (#341, #2361)",
    },
    "h_AudioOutput2Rest": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "tests/fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cells: audio-out2-query-before, audio-out2-query-after",
        ],
        "limitation": "the cells query rest samples on one valid 1024-sample Output2 reservation before and after one blocking output; the not-reserved code and other queue states are unmeasured (#341)",
    },
    "h_AudioOutput2Blocking": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "tests/fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cell: audio-out2-output-blocking",
        ],
        "limitation": "audio-out2-output-blocking measures one valid silent 1024-sample block at volume 0x8000 and makes no acoustic claim; volumes above 0xFFFFF are refused with 0x8026000b following PPSSPP caa91b54b535 Core/HLE/sceAudio.cpp, unmeasured; an unreadable buffer returns audio_output's 0x80000103 to the guest, a host contract that is unmeasured; the not-reserved code is not covered (#341)",
    },
    "h_AudioOutput2Release": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "tests/fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cell: audio-out2-release",
            "docs/HARDWARE_ORACLE.md",
        ],
        "limitation": "audio-out2-release measures success after the queue drains; the documented #311 cell measures 0x80268002 while samples remain queued and preserves the reservation; releasing when not reserved is not measured (#341)",
    },
    "h_AudioOutput2Reserve": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "tests/fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cell: audio-out2-reserve",
        ],
        "limitation": "audio-out2-reserve measures a valid 1024-sample reservation; duplicate Output2 reservation behavior and invalid size bounds are unmeasured (#341)",
    },
    "h_AudioOutputBlocking": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "tests/fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cells: audio-ch-output-blocking-0, audio-ch-output-blocking-1",
        ],
        "limitation": "the cells exercise two maximum-volume outputs with a valid silent stereo buffer and record timing and rest-length values, not audible output; mono, other volumes, invalid buffers, and error precedence are unmeasured; a channel of 8 or more (0x80260003) and an unreserved channel (0x80260001) are refused following PPSSPP caa91b54b535, corroborated by PSPAutotests output.expected for channel -1 and unreserved channel 5 (#341)",
    },
    # sceAudioOutputPannedBlocking (#341): no PSP record measures this API. The refusal codes
    # follow PPSSPP as a behaviour reference; PSPAutotests output.expected corroborates the
    # volume cells and the reserved-channel length it records for this NID, and nothing more.
    "h_AudioOutputPannedBlocking": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_audio_output_panned_blocking_unreserved_channel",
            "tests/native/hle_thread_selftest.c:test_audio_regular_output_refuses_bad_channels",
            "tests/native/hle_thread_selftest.c:test_audio_regular_output_refuses_bad_volumes",
            "PPSSPP caa91b54b53585811b0fd84405ea04aec72a952b Core/HLE/sceAudio.cpp "
            "sceAudioOutputPannedBlocking and `Core/HLE/__sceAudio.cpp` `__AudioEnqueue` (a behaviour "
            "reference; derived, not PSP-measured): the two volumes ORed and compared unsigned above "
            "0xFFFF give 0x8026000b first, a channel of 8 or more 0x80260003, a channel not reserved "
            "0x80260001",
            "PSPAutotests tests/audio/sceaudio/output.expected "
            "(ea71108f00933712c4662276261b39cd42249b1e, a third-party expectation file; corroboration "
            "only): for this API, equal left and right volumes on a reserved channel with NULL data "
            "give 0x8026000b for -256, -2, -1, -2147483648 and 65536 and up and the length for 0 to "
            "65535, and a reserved channel at 0x8000 returns its length (0x1040); the bad-channel "
            "(0x80260003, channel -1) and unreserved-channel (0x80260001, channel 5) cells are "
            "recorded for sceAudioOutputBlocking only",
        ],
        "limitation": "PPSSPP-derived, not PSP-measured: a volume above 0xFFFF or a negative one "
        "(0x8026000b, checked first), a channel of 8 or more (0x80260003) and a channel 0 to 7 that "
        "is not reserved (0x80260001, nothing played, the caller not parked, the first refusal of "
        "a run reported by name) follow PPSSPP caa91b54b535; PSPAutotests output.expected (ea71108f) "
        "records this API's volume refusals and its length on a reserved channel, but records the "
        "bad-channel and unreserved-channel cells for sceAudioOutputBlocking only; there is no PSP "
        "measurement of this API (no docs/HARDWARE_ORACLE.md or tools/psp_oracle/manifest.json "
        "record); the blocking duration and wake timing, the mixer result, a busy channel, unequal "
        "left and right volumes and the volume-before-channel order stay unmeasured (#341)",
    },
    "h_GeEdramGetAddr": {
        "status": "complete",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-HLE-GE-EDRAM-001",
            "tests/fixtures/psp_oracle/probe_hle_measure.c:hle_run",
            "src/rt/hle_ge.c:h_GeEdramGetAddr",
            "PSP-HLE-GE-EDRAM-001 cell: edram-addr = 0x04000000",
        ],
    },
    "h_GeEdramGetSize": {
        "status": "complete",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-HLE-GE-EDRAM-001",
            "tests/fixtures/psp_oracle/probe_hle_measure.c:hle_run",
            "src/rt/hle_ge.c:h_GeEdramGetSize",
            "PSP-HLE-GE-EDRAM-001 cell: edram-size = 0x00200000",
        ],
    },
    # sceRtc family (issue #341). Contract sources: the public PSPSDK header
    # psprtc.h (declarations, 0-on-success/<0-on-error, pspRtcCheckValidErrors
    # component ranges) and the firmware-measured PSPAutotests tests/rtc corpus
    # (convert.expected, rtc.expected, arithmetic.expected).  PSPAutotests
    # records firmware CRASHING on several NULL arguments (rtc.c/convert.c
    # "Crash." comments), so the NULL edges have no firmware return code to
    # match; the runtime fails closed with SCE_KERNEL_ERROR_ILLEGAL_ADDR there
    # and the tests declare that as a product boundary, not an equivalence.
    "h_RtcGetCurrentTick": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_time_domains_are_coherent",
            "tests/native/hle_thread_selftest.c:test_rtc_pointer_validation_and_measured_conversions",
            "src/rt/hle_rtc.c:h_RtcGetCurrentTick",
            "public PSPSDK contract (psprtc.h: sceRtcGetCurrentTick, 0 on success, <0 on error)",
            "PSPAutotests tests/rtc/rtc.expected (tick advances across a 2 ms delay)",
        ],
    },
    "h_RtcGetCurrentClock": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_time_domains_are_coherent",
            "tests/native/hle_thread_selftest.c:test_rtc_pointer_validation_and_measured_conversions",
            "src/rt/hle_rtc.c:h_RtcGetCurrentClock",
            "public PSPSDK contract (psprtc.h: sceRtcGetCurrentClock, tz is minutes from UTC)",
            "PSPAutotests tests/rtc/rtc.expected (0/+13/+60/-60/-600000/INT_MAX/-INT_MAX all return 0; -60 is one hour before the UTC baseline)",
        ],
    },
    "h_RtcGetCurrentClockLocal": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_local_time_follows_the_system_profile",
            "src/rt/hle.c:psp_profile_local_offset_minutes",
        ],
        "limitation": "applies the time zone and daylight saving of the system profile, the registry keys sceUtilityGetSystemParamInt reports; on the first run the setting starts from the host's local time zone (SR_HOST_TIME_ZONE) and is retained, else the seed is the oracle console's measured setting, not a factory default; the 60-minute daylight-saving shift is the setting's meaning, not measured (#1382, #341)",
    },
    "h_RtcConvertUtcToLocal": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_local_time_follows_the_system_profile",
        ],
        "limitation": "applies the time zone and daylight saving of the system profile, the registry keys sceUtilityGetSystemParamInt reports; the 60-minute daylight-saving shift is the setting's meaning, not measured, and the checked-overflow failure class is not autotest-verified (#1382, #341)",
    },
    "h_RtcConvertLocalToUtc": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_local_time_follows_the_system_profile",
        ],
        "limitation": "applies the time zone and daylight saving of the system profile, the registry keys sceUtilityGetSystemParamInt reports; the 60-minute daylight-saving shift is the setting's meaning, not measured, and the checked-overflow failure class is not autotest-verified (#1382, #341)",
    },
    "h_RtcGetTick": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_conversion_errors_and_full_range",
            "tests/native/hle_thread_selftest.c:test_rtc_pointer_validation_and_measured_conversions",
            "src/rt/hle_rtc.c:h_RtcGetTick",
            "public PSPSDK contract (psprtc.h: sceRtcGetTick, pspRtcCheckValidErrors component ranges)",
            "PSPAutotests tests/rtc/convert.expected (year 0/10000 -> 0x800001fe with output untouched; year 10/9998/9999 exact ticks)",
        ],
    },
    "h_RtcSetTick": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_conversion_errors_and_full_range",
            "tests/native/hle_thread_selftest.c:test_rtc_pointer_validation_and_measured_conversions",
            "src/rt/hle_rtc.c:h_RtcSetTick",
            "public PSPSDK contract (psprtc.h: sceRtcSetTick, 0 on success, <0 on error)",
            "PSPAutotests tests/rtc/convert.expected (checkSetTick: 835072 -> 0001-01-01, 62135596800000000 -> 1970-01-01)",
        ],
    },
    "h_RtcGetTime64_t": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_rtc_time64_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (valid dates convert; year 1 and pre-1970 dates return 0x800001fe; month 13, day 32, Feb 30 and hour 24 return 0 and normalise; NULL output returns 0; NULL date faults inside the module)",
        ],
        "limitation": "out-of-range fields normalise by calendar carry, whose exact seconds are this arithmetic rather than measured; year 0, month 0 and day 0 are refused with 0x800001fe and a refusal leaves the output unwritten, both unmeasured; a NULL or unreadable date and an unwritable non-NULL output take the HLE guest-fault stop",
    },
    "h_RtcGetTime_t": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_time_t_add_years_resolution_autotest",
            "PSPAutotests tests/rtc/convert.expected (2ed7c9c5, recorded on a PSP): checkRtcGetTime_t, 2012-09-20 07:12:15.000500 -> 1348125135",
            "docs/HARDWARE_ORACLE.md (PSP-SAFE-SCALAR-001: rtc-time-1969, rtc-time-1601, rtc-time-2106-past, rtc-time-9999 and the seven one-field-out-of-range cells, rtc-time-month-0 to rtc-time-second-60)",
            "tests/native/hle_thread_selftest.c:test_psp_measured_safe_scalar_rtc_20261117",
        ],
        "limitation": "converts in-range dates from 1970 whose seconds fit 32 bits, as one 32-bit word; PSP-SAFE-SCALAR-001 measured eleven dates: 1601-01-01 00:00:00 and 1969-12-31 23:59:59 return 0x800001fe and write 0; 2106-02-07 06:28:16 (the first second past 32 bits) and 9999-12-31 23:59:59 return 0x800001fe and write 0x7fffffff; the runtime applies each answer to the valid dates between its two measured ends, which is its rule and not a measurement (the seconds count is monotone in the date and both ends give one answer); seven dates with one field out of range (2024-00-10, 2024-13-01, 2024-01-00, 2024-01-32, 2024-02-30, 2024-01-01 24:00:00 and 2024-01-01 00:00:60, microsecond 0) return 0 and write the measured seconds, and are answered as those exact dates only; every other date with a field out of range, a valid date before 1601 and a NULL or unmapped pointer are not measured and stop the run by name; where between 2038-01-19 03:14:08 and 2106-02-07 06:28:15 the PSP begins to refuse is not measured, and those dates still convert",
    },
    "h_RtcTickAddYears": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_time_t_add_years_resolution_autotest",
            "PSPAutotests tests/rtc/arithmetic.expected (2ed7c9c5, recorded on a PSP): checkRtcTickAddYears",
            "docs/HARDWARE_ORACLE.md (PSP-SAFE-SCALAR-001: rtc-addyears-y10000-plus-0, rtc-addyears-y10000-plus-1, rtc-addyears-y10000-minus-1, rtc-addyears-y10001-minus-2)",
            "tests/native/hle_thread_selftest.c:test_psp_measured_safe_scalar_rtc_20261117",
        ],
        "limitation": "moves the calendar year with the leap day clamped to February 28, and a result outside years 1..9999 returns 0 with dest unwritten (pspautotests-recorded); PSP-SAFE-SCALAR-001 measured two source ticks past year 9999, 10000-01-01 00:00:00 and 10001-01-01 00:00:00, which follow the same rule; the runtime applies it to the source ticks between those two (the rest of year 10000), which is its rule and not a measurement; a source tick past 10001-01-01 00:00:00 and NULL or unmapped pointers are not measured and stop the run by name",
    },
    "h_RtcTickAddMicroseconds": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_tick_add_day_of_week_check_valid_autotest",
            "PSPAutotests tests/rtc/arithmetic.expected (f573fe8c, recorded on a PSP): checkRtcTickAddMicroseconds",
        ],
        "limitation": "adds the 64-bit count modulo 2^64 and returns 0, a wrap past year 1 or 9999 included (pspautotests-recorded); a NULL or unmapped source or destination is not measured and stops the run by name",
    },
    "h_RtcTickAddSeconds": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_tick_add_day_of_week_check_valid_autotest",
            "PSPAutotests tests/rtc/arithmetic.expected (f573fe8c, recorded on a PSP): checkRtcTickAddSeconds",
        ],
        "limitation": "adds the 64-bit count times 1000000 modulo 2^64 and returns 0, a wrap past year 1 or 9999 included (pspautotests-recorded); a NULL or unmapped source or destination is not measured and stops the run by name",
    },
    "h_RtcTickAddDays": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_tick_add_day_of_week_check_valid_autotest",
            "PSPAutotests tests/rtc/arithmetic.expected (f573fe8c, recorded on a PSP): checkRtcTickAddDays",
        ],
        "limitation": "adds the sign-extended 32-bit day count times 86400000000 modulo 2^64 and returns 0, a wrap past year 1 or 9999 included (pspautotests-recorded); a NULL or unmapped source or destination is not measured and stops the run by name",
    },
    "h_RtcTickAddMinutes": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_tick_add_minutes_hours_weeks_autotest",
            "PSPAutotests tests/rtc/arithmetic.expected (f573fe8c, recorded on a PSP): checkRtcTickAddMinutes, the 8 fixed rows",
        ],
        "limitation": "adds the 64-bit count times 60000000 modulo 2^64 and returns 0, a wrap past year 1 or 9999 included (pspautotests-recorded, the fixed rows; the autotest's loop rows are not checked); a NULL or unmapped source or destination is not measured and stops the run by name",
    },
    "h_RtcTickAddHours": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_tick_add_minutes_hours_weeks_autotest",
            "PSPAutotests tests/rtc/arithmetic.expected (f573fe8c, recorded on a PSP): checkRtcTickAddHours, the 8 fixed rows",
        ],
        "limitation": "adds the sign-extended 32-bit hour count times 3600000000 modulo 2^64 and returns 0, a wrap past year 1 or 9999 included (pspautotests-recorded, the fixed rows; the autotest's loop rows are not checked); a NULL or unmapped source or destination is not measured and stops the run by name",
    },
    "h_RtcTickAddWeeks": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_tick_add_minutes_hours_weeks_autotest",
            "PSPAutotests tests/rtc/arithmetic.expected (f573fe8c, recorded on a PSP): checkRtcTickAddWeeks, the 8 fixed rows",
        ],
        "limitation": "adds the sign-extended 32-bit week count times 604800000000 modulo 2^64 and returns 0, a wrap past year 1 or 9999 included (pspautotests-recorded, the fixed rows; the autotest's loop rows are not checked); a NULL or unmapped source or destination is not measured and stops the run by name",
    },
    "h_RtcGetDayOfWeek": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_tick_add_day_of_week_check_valid_autotest",
            "PSPAutotests tests/rtc/lookup.expected (f573fe8c, recorded on a PSP): checkDayOfWeek",
            "docs/HARDWARE_ORACLE.md (PSP-SAFE-SCALAR-001: rtc-dow-month-0, rtc-dow-month-13, rtc-dow-month-13-again, rtc-dow-month-14, rtc-dow-month-24, rtc-dow-year-0, rtc-dow-year-10000, rtc-dow-day-1000001, rtc-dow-day-max)",
            "tests/native/hle_thread_selftest.c:test_psp_measured_safe_scalar_rtc_20261117",
        ],
        "limitation": "Sunday is 0; months 1..12 of years 1..9999 with the day carried linearly up to 1000000 (the pspautotests cells); a month of 0 or above 12, a year outside 1..9999 and a day above 1000000 answer only the fifteen measured triples (seven pspautotests, eight PSP-SAFE-SCALAR-001), which fit no single calendar rule, and any other such month, year or day stops the run by name",
    },
    "h_RtcCheckValid": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_tick_add_day_of_week_check_valid_autotest",
            "PSPAutotests tests/rtc/lookup.expected (f573fe8c, recorded on a PSP): checkRtcCheckValid and checkMaxYear",
            "docs/HARDWARE_ORACLE.md (PSP-SAFE-SCALAR-001: rtc-valid-day-0, rtc-valid-feb-29-2023, rtc-valid-feb-29-1900, rtc-valid-feb-30-2024, rtc-valid-feb-31-2024, rtc-valid-apr-31-2024, rtc-valid-day-0-hour-24)",
            "tests/native/hle_thread_selftest.c:test_psp_measured_safe_scalar_rtc_20261117",
        ],
        "limitation": "returns 0 or the first failing field as -1 year, -2 month, -3 day, -4 hour, -5 minute, -6 second, -7 microsecond (measured); a day of 0 or past the month's length is -3, measured by PSP-SAFE-SCALAR-001 in January, February and April and applied to the other months from the same month table; a NULL or unmapped pointer is not measured and stops the run by name",
    },
    "h_DisplayIsForeground": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_imports_20261102_measured_contracts",
            "PSPAutotests tests/display/isstate.expected (1a9e6c31, recorded on a PSP): 0 with no framebuffer, 1 with one set, 1 after a latched request for address 0",
        ],
        "limitation": "reports whether the active scanout address is non-zero; the result before sceDisplaySetMode or any framebuffer is not measured and follows from the zero address",
    },
    "h_DisplaySetHoldMode": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-MISC-001: display-basic): mode 0 from a user thread returns 0",
            "tests/native/hle_thread_selftest.c:test_display_set_hold_mode",
        ],
        "limitation": "mode 0 from a thread returning 0 is PSP-measured; a nonzero mode returning 0 is PPSSPP-derived and the interrupt-context refusal (0x80020064) JPCSP-derived, neither PSP-measured; the recorded mode has no modeled effect on the picture (#2886)",
    },
    "h_GetModuleIdList": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_imports_20261102_measured_contracts",
            "PSPAutotests tests/modules/idlist/idlist.expected (774dfddc467ae0cbb7d3d8b22fc271f74135cd38, a third-party recording made on a PSP): byte size, whole entries, count is the total, the caller's module is listed; 0x800200D3 for a NULL count and for a count pointer or a buffer in the kernel segment (lines 14 to 16)",
            "uOFW src/kd/modulemgr/queryModule.c (f7b0623168842f02cddbdbf9437ac0c2380c51c8, MIT): both pointers pass the kernel's user-pointer check before anything is written",
        ],
        "limitation": "lists only the main module and the modules the runtime loaded (a PSP also lists firmware modules), main first and then in load order, an order that is not measured; the recorded 0x800200D3 covers a NULL pointer and one whose address, end or size has bit 31 set, of which the recording has 0x88000000; a count pointer or buffer that passes that check and is not guest memory has no recorded cell and stops the run by name",
    },
    "h_SelfStopUnloadModule": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-SELFSTOP-001: self-stop-arg0, self-stop-arg1)",
            "tests/native/hle_thread_selftest.c:test_self_stop_unload_measured_forms",
        ],
        "limitation": "only (0, 0, NULL) and (1, 0, NULL) from a started helper's own thread "
        "are reproduced: module_stop runs once with length zero and a null pointer, the caller "
        "does not resume, and address lookup returns 0x8002012e after unload. For (0, 4, argp past RAM) "
        "the PSP ran module_stop with a substituted pointer and unloaded; the runtime refuses the span "
        "with 0x80000103 and preserves the module (#2397). Other first-argument and argument-block forms, "
        "a call from the module_start thread, sibling threads, no stop entry and a nonzero module_stop "
        "return remain unmeasured (#341)",
    },
    "h_LoadExecForUser_362A956B": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_imports_20261102_measured_contracts",
            "PSPAutotests tests/threads/callbacks/exit.expected (1a9e6c31, recorded on a PSP, compiled SDK 3.95): checkpoints under LoadExecForUser_362A956B",
        ],
        "limitation": "reads the registered exit callback's argument block as {mode, params, common}: no live callback 0x800201a1, NULL argument 0x800200d3, mode 4 or above 0x800200d2, params size below 12 0x800201bc, else clears params->unk1 and sets params->unk2 to -1; an unusable params pointer stops the run by name; the check order when mode and size both fail, and other compiled SDK versions, are not measured",
    },
    "h_FontPixelToPointH": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_font_measured_contract",
            "PSPAutotests tests/font/resolution.expected (f573fe8c, recorded on a PSP): sceFontPixelToPointH conversions",
        ],
        "limitation": "pixels * 72 / hRes in float with errorCode 0; a NULL library writes INVALID_LIB and returns 0.0 (measured); a library already closed (the PSP still converts) and a NULL errorCode (the PSP crashes) are not modeled and stop the run by name",
    },
    "h_FontPixelToPointV": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_font_measured_contract",
            "PSPAutotests tests/font/resolution.expected (f573fe8c, recorded on a PSP): sceFontPixelToPointV conversions",
        ],
        "limitation": "pixels * 72 / vRes in float with errorCode 0; a NULL library writes INVALID_LIB and returns 0.0 (measured); a library already closed (the PSP still converts) and a NULL errorCode (the PSP crashes) are not modeled and stop the run by name",
    },
    "h_FontPointToPixelH": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_font_measured_contract",
            "PSPAutotests tests/font/resolution.expected (f573fe8c, recorded on a PSP): sceFontPointToPixelH conversions",
        ],
        "limitation": "points * hRes / 72 in float with errorCode 0; a NULL library writes INVALID_LIB and returns 0.0 (measured); a library already closed (the PSP still converts) and a NULL errorCode (the PSP crashes) are not modeled and stop the run by name",
    },
    "h_FontPointToPixelV": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_font_measured_contract",
            "PSPAutotests tests/font/resolution.expected (f573fe8c, recorded on a PSP): sceFontPointToPixelV conversions",
        ],
        "limitation": "points * vRes / 72 in float with errorCode 0; a NULL library writes INVALID_LIB and returns 0.0 (measured); a library already closed (the PSP still converts) and a NULL errorCode (the PSP crashes) are not modeled and stop the run by name",
    },
    "h_HtmlViewerStatus": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_imports_20261102_measured_contracts",
            "PSPAutotests tests/utility/dialog/status.expected and htmlviewer.expected (1a9e6c31, recorded on a PSP): html=WRONG_TYPE when no HTML viewer was started",
        ],
        "limitation": "always WRONG_TYPE: the runtime has no HTML viewer dialog, and its InitStart import is not registered",
    },
    "h_ScreenshotStatus": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_imports_20261102_measured_contracts",
            "PSPAutotests tests/utility/dialog/status.expected (1a9e6c31, recorded on a PSP): shot=WRONG_TYPE when no screenshot dialog was started",
        ],
        "limitation": "always WRONG_TYPE: the runtime has no screenshot dialog, and its InitStart import is not registered",
    },
    "h_RtcGetTickResolution": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_rtc_time_t_add_years_resolution_autotest",
            "uOFW src/kd/rtc/rtc.c (MIT): sceRtcGetTickResolution returns 1000000, marked as matching the firmware assembly",
            "PSPAutotests tests/rtc (2ed7c9c5): every RTC tick conversion uses one-microsecond ticks",
        ],
    },
    "h_RtcGetWin32FileTime": {
        "status": "partial",
        "limitation": "cold-first-call error reporting may differ from firmware (PSPAutotests convert.c notes errors report properly only after a prior error and that the rules are hard to determine); component bounds beyond the measured year/epoch/day-carry cases stay fail-closed rather than measured (#341)",
    },
    # sceReg virtual system registry (src/rt/hle.c, sceReg block): the /CONFIG tree measured on
    # PSP-3000 6.6.1 (2026-10-09, read-only enumeration) with a per-user overlay persisted by the
    # flush calls. sceUtilityGetSystemParamInt reads the registry's shared integer keys back.
    "h_RegOpenRegistry": {
        "status": "partial",
        "limitation": "opens the one virtual system registry in mode 1 or 2 with 512 project-bounded handles (the console served at least 257 at once, measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration); RegParam regtype and name are not checked; the first open loads the measured /CONFIG tree and the per-user overlay; write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegCloseRegistry": {
        "status": "partial",
        "limitation": "frees a registry handle slot; closed handles get the bad-handle code 0x8008272E, measured on sceRegGetKeysNum and applied here by analogy",
    },
    "h_RegFlushRegistry": {
        "status": "partial",
        "limitation": "persists the whole per-user overlay atomically when the registry changed (whether firmware persists less, or commits on close, is unmeasured); a failed write is EIO and the change stays in memory",
    },
    "h_RegOpenCategory": {
        "status": "partial",
        "limitation": "opens a category of the measured tree with 512 project-bounded handles; an open of a missing category creates it in either mode, cutting new names to 26 bytes (measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration for mode 1); the console's failure to reopen such a cut name is not modeled; write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegCloseCategory": {
        "status": "partial",
        "limitation": "frees a category handle slot, including one made stale by sceRegRemoveCategory; closed handles get the bad-handle code 0x8008272E (measured on sceRegGetKeysNum, applied by analogy)",
    },
    "h_RegFlushCategory": {
        "status": "partial",
        "limitation": "persists the whole per-user overlay, not only the flushed category (firmware scope unmeasured); a failed write is EIO and the change stays in memory",
    },
    "h_RegRemoveCategory": {
        "status": "partial",
        "limitation": "removes an empty category a game created through a mode-2 registry handle; measured default categories are refused (EPERM) and non-empty ones too (ENOTEMPTY); write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegGetKeyInfo": {
        "status": "partial",
        "limitation": "reports type, size and a key handle for the measured /CONFIG entries (names, types, sizes and DIR size 0 measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration), created keys and child categories; an unknown key returns the measured 0x8008271D; values other than six measured settings are neutral placeholders",
    },
    "h_RegGetKeyInfoByName": {
        "status": "partial",
        "limitation": "the same lookup as sceRegGetKeyInfo without a key handle; the not-found code is applied by analogy",
    },
    "h_RegGetKeyValue": {
        "status": "partial",
        "limitation": "copies a value key with bounds checks (a buffer smaller than the value is EINVAL, a DIR entry EFTYPE; both unmeasured); stale or foreign key handles get the bad-handle code by analogy; reading a neutral placeholder logs once",
    },
    "h_RegGetKeyValueByName": {
        "status": "partial",
        "limitation": "the same copy as sceRegGetKeyValue, by name; error codes other than not-found are not hardware measured",
    },
    "h_RegSetKeyValue": {
        "status": "partial",
        "limitation": "writes an existing key through a mode-2 category handle (mode 1 is EACCES): INT exactly 4 bytes, a STR takes the size of its new value (string sizes follow their values, measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration), BIN up to its fixed size with the rest zero-filled; persists on a flush; write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegCreateKey": {
        "status": "partial",
        "limitation": "creates an INT (4 bytes), STR or BIN key (up to a 4096-byte project bound) with zero-filled space through a mode-2 category handle; names over 26 bytes are cut to 26 (inferred from the measured category cut); DIR and unknown types are EINVAL, existing names EEXIST; write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegGetKeysNum": {
        "status": "partial",
        "limitation": "counts a category's keys and child categories; a forged handle returns 0x8008272E and leaves the count untouched (measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration)",
    },
    "h_RegGetKeys": {
        "status": "partial",
        "limitation": "writes the first num entry names as NUL-padded 27-byte slots in the console's order for the measured tree (measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration) and creation order after it; truncation below the entry count is a project choice",
    },
    # Reviewed Route-A handlers: hardware evidence covers named cells, while
    # the remaining branches stay explicit instead of being promoted complete.
    "h_GeEdramSetAddrTranslation": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-GE-EDRAM-001: initial width, 512/1024/2048/4096 replacements, restore and final query)",
        ],
        "limitation": "replacement-width returns and restoration to boot width 1024 are measured; width 0 as an input, unsupported-width error 0x80000107 (a project choice), and applying the setting to actual GE address translation are not measured",
    },
    "h_TryLockLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 LwMutex cells)",
        ],
        "limitation": "normal acquisition, count-zero refusal, non-recursive self-contention, and another-owner contention are measured; negative counts, invalid-workarea error precedence, interrupt context, and several-waiter ordering are not measured",
    },
    "h_UnlockLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 LwMutex cells)",
        ],
        "limitation": "owner release, direct hand-off to one waiter, and non-owner/underflow behavior are measured; invalid workareas, negative or excessive unlock counts, and several-waiter ordering are not measured: waiters are served in arrival order, or by current priority under attr 0x100 (PPSSPP-derived, not PSP-measured)",
    },
    "h_AllocateFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 FPL cells)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001 FPL create, exhaustion, status, free and delete cells)",
        ],
        "limitation": "ordinary allocation, one finite timeout, one-waiter hand-off, delete wake, and measured block/status layout are covered; invalid output pointers, negative timeout handling, and multiple-waiter ordering are not measured",
    },
    "h_AllocateVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 VPL cells)",
            "PSPAutotests tests/threads/vpl/allocate.expected (0 bytes, -1 bytes and more than the pool 0x800201B7)",
            "tests/native/hle_thread_selftest.c:test_wb_tlspl_vpl_cells",
            "tests/native/hle_thread_selftest.c:test_vpl_blocking_waits",
        ],
        "limitation": "ordinary allocation, one finite timeout, measured pool accounting, one-waiter hand-off and the invalid sizes are covered; a request within the create size that no block can hold is refused with 0x800201B7 instead of waiting (unmeasured for the last 8 bytes below poolSize); invalid output pointers and fragmented-pool behaviour beyond the measured cases are not measured (#341)",
    },
    # Route-A batch 3: each citation was checked against the exact API path in
    # its hardware cell. These handlers remain partial where the cell does not
    # cover all branches or where the current implementation has a known gap.
    "h_CancelEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: event-cancel-before-deadline-late-dispatch returns WAIT_CANCEL to the waiter)",
        ],
        "limitation": "the cited cell measures a blocked waiter's WAIT_CANCEL outcome after event cancellation; it does not establish the cancel call's return, pattern assignment, waiter-count output, unknown-ID behavior, pointer errors, or multiple-waiter ordering",
    },
    "h_CancelMsgPipe": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/msgpipe (cancel case: waiters released with WAIT_CANCEL, counts and remaining timeout written)",
            "tests/native/hle_thread_selftest.c:test_msgpipe_cancel_and_refer",
        ],
        "limitation": "releases every active send and receive waiter with WAIT_CANCEL, writes both waiter counts and each timed waiter's remaining timeout and empties the buffer; a waiter paused inside a callback, the unknown-UID code and error precedence are not hardware measured (#1381)",
    },
    "h_CancelSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B2-01: CancelSema(-1) restores the create-time initial count)",
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: sema-cancel-before-deadline-late-dispatch returns WAIT_CANCEL to the waiter)",
        ],
        "limitation": "the cited cells establish the -1 reset and a blocked waiter's WAIT_CANCEL outcome; other replacement counts, over-max refusal, optional waiter-count output, unknown IDs, pointer errors, and multiple-waiter ordering are not established by those citations",
    },
    "h_ClearEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: ClearEventFlag(mask) keeps only the bits in mask)",
        ],
        "limitation": "the measured mask behavior matches the normal-object path; an unknown UID, or a UID of another kernel-object kind, returns the measured unknown-event-flag code 0x8002019A; error precedence is not covered",
    },
    "h_CreateFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: fpl-create and valid pool status/allocation cells)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics: FPL blocks are blockSize apart)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-002: block size 0 is 0x800201B7, a 0x7FFFFFFF-byte block 0x80020190)",
        ],
        "limitation": "valid pool creation, block size 0 and a pool the partition cannot hold are measured; a block count of 0 still becomes 1, and partition, attribute and option error precedence need separate evidence",
    },
    "h_CreateLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01 and PSP-B2-01: LwMutex creation workarea and one-waiter hand-off)",
        ],
        "limitation": "the cells cover recursive and non-recursive initialization, workarea fields, and one waiter hand-off; invalid workareas, unsupported attribute bits, negative counts, option data, and multi-waiter ordering are not established",
    },
    "h_CreateMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-refer-unlocked creation and observed status fields)",
        ],
        "limitation": "the measured cell covers ordinary unlocked and initially-held mutex status; name and attribute validation, recursive edge cases, invalid arguments, and allocation exhaustion are not covered",
    },
    "h_CreateSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: CreateSema accepts init greater than max and max zero, and rejects invalid attributes)",
            "docs/HARDWARE_ORACLE.md (PSP-B2-01: CancelSema restores the create-time initial count)",
        ],
        "limitation": "the cited cells cover the measured count and attribute rules plus initial-count retention; name-pointer behavior, object exhaustion, and invalid-argument precedence remain unmeasured",
    },
    "h_CreateVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01 and PSP-B2-01: valid VPL creation, accounting, allocation, and deletion)",
        ],
        "limitation": "valid pool creation and measured accounting are covered; the handler maps size zero to VPL_EXHAUSTED and ignores the partition argument, but the cited valid-pool cells do not establish those edge results or option behavior; a pool the partition cannot hold returns NO_MEMORY as pspautotests threads/vpl/create.expected records",
    },
    "h_DeleteFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: valid fpl-delete and refer-after-delete cells)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B2-01: deleting an FPL with a waiter)",
        ],
        "limitation": "valid deletion and the delete-wait outcome are covered; an invalid or already-deleted UID returns UNKNOWN_FPLID as pspautotests threads/fpl/delete.expected records, outside the cited cells, which use a valid pool, and multi-waiter teardown ordering is not measured",
    },
    "h_DeleteLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: deleting a held LwMutex updates the workarea fields)",
        ],
        "limitation": "the cited cells cover valid deletion and the measured workarea fields; forged workareas, invalid-address precedence, stale UIDs, and multiple-waiter deletion behavior are not fully covered",
    },
    # Route-A batch 4: the cited cells cover concrete lifecycle/allocation
    # paths. The worker-only cells do not establish main-thread teardown.
    "h_DeleteSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01 and PSP-B2-01: unknown semaphore ID and deleting a waited-on semaphore)",
        ],
        "limitation": "a valid delete with one waiter and the unknown-semaphore code are covered; no-waiter behavior, multiple-waiter ordering, and interrupt-context precedence are not established",
    },
    "h_DeleteThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-THREAD-EXIT-001: ED-D77, ED-D00, ED-DNEG, and ED-DERR post-exit delete/restart cells)",
        ],
        "limitation": "post-exit delete and restart outcomes are covered for four worker exit statuses; deleting a live or waiting thread, unknown IDs, and multi-joiner cleanup are not established",
    },
    "h_DeleteVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: valid VPL deletion while blocks are held)",
        ],
        "limitation": "valid deletion with allocated blocks is covered; deletion with blocked waiters, unknown IDs, arbitrary fragmentation, and invalid-free precedence are not established",
    },
    "h_ExitDeleteThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-THREAD-EXIT-001: ED-D77, ED-D00, ED-DNEG, and ED-DERR)",
        ],
        "limitation": "worker ExitDeleteThread status, joiner result, and post-exit UID/delete behavior are covered for four values; main/launcher teardown is not covered by these cells and PSP-TEARDOWN-001 remains captured-only",
    },
    "h_ExitThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-THREAD-EXIT-001: ED-X77, ED-X00, ED-XNEG, and ED-XERR)",
        ],
        "limitation": "worker ExitThread status and WaitThreadEnd/ReferThreadStatus outcomes are covered for four values; main/launcher teardown, callbacks during exit, and other thread roles are not established",
    },
    "h_FreeFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: fpl-free-a, fpl-free-b, and status after free)",
        ],
        "limitation": "valid block frees and resulting free counts are covered; invalid, unaligned, repeated, or foreign-pool frees and waiter ordering are not established by the cited cells",
    },
    "h_FreeVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: VPL pool accounting, allocation placement, and free sequence)",
        ],
        "limitation": "valid frees under the measured pool accounting are covered; invalid IDs/addresses, misalignment, arbitrary fragmentation/coalescing, and zero-size allocation behavior are not established",
    },
    "h_ReleaseWaitThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B3-01: ReleaseWaitThread wakes a blocked target with WAIT_RELEASE)",
        ],
        "limitation": "the cited cell covers releasing one blocked target and the waiter's WAIT_RELEASE result; self/UID-zero targets, already-finished targets, and multiple-waiter ordering are not established; a DelayThread wait is released with WAIT_RELEASE as PPSSPP does (PPSSPP-derived, not PSP-measured), and a ready, dormant or runtime-internally delayed target stops by name",
    },
    "h_TryAllocateFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: fpl-try-alloc-a, fpl-try-alloc-b, and fpl-try-alloc-exhausted)",
        ],
        "limitation": "two valid allocations, the measured exhaustion code, and pool status are covered; invalid output pointers, invalid UIDs, and other exhaustion/error precedence are not established",
    },
    "h_TryAllocateVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: 1024-byte VPL accounting and high-down allocation placement)",
            "PSPAutotests tests/threads/vpl/try.expected (0 bytes, -1 bytes and more than the pool 0x800201B7; a request no free block can hold 0x80020190)",
            "tests/native/hle_thread_selftest.c:test_wb_tlspl_vpl_cells",
            "tests/native/hle_thread_selftest.c:test_vpl_nonblocking_roundtrip",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-002: a 0x1000-byte VPL gives 15 allocations of 0x100, the 16th 0x80020190)",
            "tests/native/hle_thread_selftest.c:test_kernel_pool_bounds_measured",
        ],
        "limitation": "the recorded codes and the measured header accounting are reproduced; where the header bytes sit, a request within the create size but beyond what the pool hands out (0x800201B7 here), and invalid output pointers are unmeasured (#341)",
    },
    # Route-A batch 5: retain only the behaviors named by the existing cells.
    "h_LockMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-timeout-quanta, mutex-priority-inheritance, and accepted interrupt-context cell)",
        ],
        "limitation": "measured cases cover finite timeout, priority inheritance, and the listed interrupt-context refusals; full attribute/recursion ranges, negative counts, multi-waiter priority ordering, and invalid timeout-pointer precedence are not measured",
    },
    "h_LockMutexCB": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-interrupt-context)",
            "tests/native/hle_thread_selftest.c:test_lock_mutex_cb_refused_in_vblank_subintr",
        ],
        "limitation": "this status covers one cell only: LockMutexCB with count 1 and no timeout on an unlocked mutex, called from a VBLANK sub-interrupt handler, returns 0x80020064 (ILLEGAL_CONTEXT) without blocking the handler; PSP-MUTEX-001 recorded that in 20 of 20 trials on one PSP-3000 (system software 6.61) in one acceptance-eligible launch of the probe built from ec5dd563, and the selftest runs the same six-call trial through the production sub-interrupt delivery; in the separate mutex-timeout-quanta case, the probe calls LockMutexCB on a held mutex with a 250 us timeout, discards the return code, and records the timeout word and elapsed time in slots 13 and 14. The public summary does not qualify timeout behavior for that call. Thread-context acquisition, recursion, bad counts, contention, wake outcomes and callback delivery remain outside this review; the mutex state after the refusal is a runtime check, not a measured cell (#2807)",
    },
    "h_NotifyCallback": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics: two callback notifications coalesce into one handler call with count 2)",
        ],
        "limitation": "the cited cell covers notification coalescing in one callback case; callback creation state, invalid UID behavior, dispatch context, and ordering with multiple callbacks are not established",
    },
    "h_PollEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01: unmet poll output and zero-bit refusal)",
        ],
        "limitation": "the cited cells cover an unmet poll's outBits and the zero-bit refusal; other wait modes, invalid output pointers, and error precedence are not established",
    },
    "h_ReceiveMsgPipe": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: blocked receiver is served directly by a sender)",
        ],
        "limitation": "the cited cell covers a blocked receiver served by a sender; empty/short receive result sizes, invalid pointers, timeout precedence, and multiple-receiver ordering are not established",
    },
    "h_ReferEventFlagStatus": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-STATUS-001: event-size-zero, event-size-8, event-size-40, and event-size-full)",
        ],
        "limitation": "the cited cells establish bytes written and size-word behavior at four buffer sizes; full field values, unknown UID codes, invalid pointers, and error precedence are not established by that size row",
    },
    "h_ReferFplStatus": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: valid FPL layout, short/zero-size writes, free counts, and refer-after-delete)",
        ],
        "limitation": "valid pool layout and the cited short/zero-size behavior are covered; tick fields, invalid pointers, unknown IDs beyond the after-delete refer, and error precedence are not established",
    },
    "h_ReferMsgPipeStatus": {
        "status": "partial",
        "evidence": [
            "public ABI (PSPSDK pspthreadman.h: SceKernelMppInfo, 56 bytes)",
            "PSPAutotests tests/threads/msgpipe (refer counts beside the cancel and delete cases)",
            "tests/native/hle_thread_selftest.c:test_msgpipe_cancel_and_refer",
        ],
        "limitation": "reports name, attr, buffer size, free size and the active send and receive waiter counts for a known pipe, honours a short or zero size word and checks the whole span before writing; the unknown-UID and NULL-pointer codes and error precedence are source-selftested, not hardware measured (#1381)",
    },
    "h_ReferMutexStatus": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-refer-unlocked status fields)",
        ],
        "limitation": "the cited cell covers an unlocked mutex and its measured status words; size variants, recursive ownership, invalid pointers, and unknown-UID fields are not established",
    },
    "h_SendMsgPipe": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: no-room, oversize, and blocked-receiver message-pipe cells)",
        ],
        "limitation": "the cited cells cover no-room refusal, oversize refusal, and a receiver served by a sender; result-size pointers, timeout behavior, multi-waiter ordering, and all mode/error-precedence combinations are not established",
    },
    "h_SetEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: event-signal-before-deadline-late-dispatch)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics: selected event-flag pattern and wait-mode cells)",
        ],
        "limitation": "the cited cells cover setting bits that satisfy a waiter and selected wait modes; invalid UIDs, all-bit masks, result precedence, and multiple-waiter ordering are not established",
    },
    "h_SignalSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: semaphore signal count boundaries)",
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: sema-signal-before-deadline-late-dispatch)",
        ],
        "limitation": "the cited cells cover selected count boundaries and waking a waiter; invalid output pointers, negative-count ranges outside the tested values, and multi-waiter ordering are not established",
    },
    # Route-A batch 6: these remaining kernel wait/transfer rows have direct
    # cells, but their unmeasured argument and ordering branches stay partial.
    "h_SleepThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: SleepThread and wakeup sequence)",
        ],
        "limitation": "the cited cell covers a helper sleeping and resuming after a wakeup; no-current-thread, disabled-dispatch precedence, callbacks during sleep, and other wake races are not established",
    },
    "h_TryLockMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: accepted mutex-interrupt-context TryLockMutex result)",
        ],
        "limitation": "the accepted interrupt-context cell confirms ILLEGAL_CONTEXT for TryLockMutex; ordinary acquisition, recursive count boundaries, and owner-contention results are not covered by that cell",
    },
    "h_TryReceiveMsgPipe": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: message-pipe buffered and empty receive cells)",
        ],
        "limitation": "the cited message-pipe cells cover selected buffered and empty receives; result-size pointer handling, partial-mode edge cases, invalid pointers, and error precedence are not fully summarized by the public oracle row",
    },
    "h_TrySendMsgPipe": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: message-pipe no-room and oversize cells)",
        ],
        "limitation": "the cited cells cover no-room refusal, oversize refusal, and a partial-mode send; result-size pointer handling, invalid pointers, and all mode/error-precedence combinations are not established",
    },
    "h_UnlockMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-priority-inheritance owner release and accepted interrupt-context cell)",
        ],
        "limitation": "the cited cases cover owner release in the priority-inheritance control and interrupt-context refusal; invalid UIDs, bad counts, and multi-waiter priority ordering are not established",
    },
    "h_WaitEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: event signal/cancel before deadline and late dispatch)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01: event flag poll/wait modes and outputs)",
        ],
        "limitation": "the cited cells cover the named signal/cancel outcomes and selected wait modes; an unknown UID, or a UID of another kernel-object kind, returns 0x8002019A; other mode, pointer, and multi-waiter precedence is not established",
    },
    "h_WaitSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: semaphore signal/cancel before deadline and late dispatch)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01: semaphore count and wait results)",
        ],
        "limitation": "the cited cells cover selected count, signal/cancel, and timeout outcomes; invalid timeout pointers, negative counts, general multi-waiter ordering, and all error-precedence cases are not established; the time left written back after a satisfied timed wait is PPSSPP-derived, not PSP-measured",
    },
    "h_WakeupThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: wakeups accumulate while the target is not sleeping)",
        ],
        "limitation": "the cited cells cover wakeup-count accumulation and a sleeping target; specific invalid-target errors, repeated wakeup order, and interactions with termination are not all established",
    },
    # Route-A batch 7: the measured system-clock delta supports a partial status,
    # while the probe's offset intervals do not establish a precise clock rate.
    "h_GetSystemTimeWide": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-VTIMER-002: system-clock reads around the bounded delay)",
        ],
        "limitation": "the two system-clock reads advanced 220,608 around a bounded 20 ms delay, but host0 logging offset the intervals and the VTimer/system-clock deltas agree only within about 8%; this is consistent with the microsecond unit but does not establish a precise rate or absolute system-time values",
    },
    # Interrupt-enable predicate: returns 1 when the CPU interrupt state is enabled. The selftest
    # reads it before and after SuspendIntr/ResumeIntr in the production scheduler path.
    "h_CpuIsIntrEnable": {
        "status": "complete",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_interrupt_nid_semantics",
        ],
    },
    # System time: writes the 64-bit microsecond clock to the caller's SceKernelSysClock and returns 0.
    # The illegal-address code is the runtime's own; no PSP measurement names it.
    "h_GetSystemTime": {
        "status": "partial",
        "limitation": "writes the 64-bit modeled microsecond clock and returns 0; the illegal-address result is the runtime's own code (RTC_ILLEGAL_ADDR), not a PSP measurement, and the clock origin is not compared with hardware",
    },
    # System time, low word: returns the low 32 bits of the modeled microsecond clock.
    "h_GetSystemTimeLow": {
        "status": "partial",
        "limitation": "returns the low 32 bits of the modeled microsecond clock, which wraps; the clock origin and wrap timing are not compared with hardware",
    },
    # Guest memcpy: copies n bytes and returns dst. Overlapping spans use memmove semantics; the PSP's
    # overlap behaviour is unmeasured, and bad spans are refused through the runtime's out-of-range path.
    "h_Memcpy": {
        "status": "partial",
        "limitation": "copies with memmove semantics for overlapping spans, which the PSP's memcpy does not promise and whose overlap behaviour is unmeasured; bad guest spans are refused through the runtime's out-of-range path, not a PSP error code",
    },
    # ---- #341 Route-A batch 11: measured getters, GE sync, UMD state, thread waits ----
    # Each entry cites PSP cells whose values docs/HARDWARE_ORACLE.md (or the oracle
    # manifest's measurement note) publishes, plus the production-NID selftest that pins
    # the runtime to them. Nothing here is complete: each names what the cells do not cover.
    "h_GetSystemParamInt": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-SYSPARAM-001: int-id-2..int-id-9, int-unknown-0, int-unknown-10, int-unknown-64)",
            "tests/native/hle_thread_selftest.c:test_sysreg_virtual_registry",
        ],
        "limitation": "ids 2 to 10 return 0 and write a value and ids 0 and 64 return 0x80110103 with the output untouched, as measured; the seeded values are the oracle console's configured settings (WLAN_POWERSAVE is an unmeasured project default), not factory defaults; an invalid output pointer is not measured",
    },
    "h_GetSystemParamString": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-SYSPARAM-001: string-id-1-len-0x80, string-id-1-len-0x04)",
            "tests/native/hle_thread_selftest.c:test_sysparam_nickname_string",
            "tests/native/hle_thread_selftest.c:test_sysparam_nickname_from_launcher",
        ],
        "limitation": "the nickname (id 1) copy with its NUL, which writes the string and its NUL only and nothing past them in the 128-byte field, and the short-buffer 0x80110102 refusal that writes nothing are measured; other string ids, a null buffer and len <= 0 return 0x80110103 without a measurement; the nickname is the registry value, by default the user-chosen SR_NICKNAME or the neutral \"Player\" (never derived from the OS), not the console's; the player's 32-byte cap is the project's setting limit (PPSSPP's Nickname editor also caps at 32), below the PSP's 128-byte field (127 characters and the NUL), and how long a nickname the PSP's own System Settings accepts is unmeasured",
    },
    "h_CtrlReadLatch": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-CTRL-LATCH-001: latch-read-initial, latch-read-after-poll, latch-read-clears-peek-keeps)",
            "tests/native/hle_thread_selftest.c:test_ctrl_read_latch",
        ],
        "limitation": "with no input the measured make, break and press words are 0 and release is 0xFFFFFFFF, which the runtime matches; whether a read clears the latch is not established by the cells (the release word stays saturated), so the clearing contract and the returned cycle count are the project's model, and the null-pointer code is source-selftested only",
    },
    "h_HprmIsRemoteExist": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-HPRM-001: hprm-remote-first, hprm-remote-second)",
            "tests/native/hle_thread_selftest.c:test_td24b_cheap_hle_batch",
        ],
        "limitation": "reports the modeled accessory state, 0 by default as measured twice on the oracle console; the accessory state during that run was not recorded, so the value with a remote attached is not measured",
    },
    "h_HprmIsHeadphoneExist": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-HPRM-001: hprm-headphone-first, hprm-headphone-second)",
            "tests/native/hle_thread_selftest.c:test_td24b_cheap_hle_batch",
        ],
        "limitation": "reports the modeled accessory state, 0 by default as measured twice on the oracle console; the accessory state during that run was not recorded, so the conditions under which firmware reports 1 are not measured",
    },
    "h_HprmIsMicrophoneExist": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-HPRM-001: hprm-microphone-first, hprm-microphone-second)",
            "tests/native/hle_thread_selftest.c:test_td24b_cheap_hle_batch",
        ],
        "limitation": "reports the modeled state, 1 by default as measured twice on the PSP-3000 oracle console; other models (without a built-in microphone) are not measured",
    },
    "h_PowerGetPllClockFrequencyInt": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-POWER-001: pll-clock-int)",
            "tests/native/hle_thread_selftest.c:test_td24b_cheap_hle_batch",
        ],
        "limitation": "the power-on 222 MHz is measured; after a Set call the getter reports the retained request verbatim, which is the project's model (the oracle makes no Set call)",
    },
    "h_PowerGetCpuClockFrequencyInt": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-POWER-001: cpu-clock-int, cpu-clock-alias, cpu-clock-int-repeat)",
            "docs/HARDWARE_ORACLE.md (PSP-SYSTEM-001: model-profile)",
            "tests/native/hle_thread_selftest.c:test_power_identity_getters_measured",
            "tests/native/hle_thread_selftest.c:test_td24b_cheap_hle_batch",
        ],
        "limitation": "the power-on 222 MHz is measured for both registered NIDs; after a Set call the getter reports the retained request verbatim, which is the project's model (the oracle makes no Set call)",
    },
    "h_PowerGetBusClockFrequencyInt": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-POWER-001: bus-clock-int, bus-clock-alias)",
            "tests/native/hle_thread_selftest.c:test_power_identity_getters_measured",
            "tests/native/hle_thread_selftest.c:test_td24b_cheap_hle_batch",
        ],
        "limitation": "the power-on 111 MHz is measured for both registered NIDs; after a Set call the getter reports the retained request verbatim, which is the project's model (the oracle makes no Set call)",
    },
    "h_GeListSync": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-GE-CONTROL-001: ge-list-sync-paused, ge-list-sync-cancelled, ge-list-dequeue-stalled)",
            "docs/HARDWARE_ORACLE.md (PSP-GE-SYNC-001: ge-list-sync-unknown-poll, ge-sync-invalid-mode, ge-list-sync-cancelled-wait, ge-list-sync-stale-wait, ge-list-sync-after-draw-wait)",
            "tests/native/hle_thread_selftest.c:test_ge_break_continue",
            "tests/native/hle_thread_selftest.c:test_ge_blocking_sync_waits_for_stalled_list",
            "tests/native/hle_thread_selftest.c:test_ge_sync_ids_match_psp",
        ],
        "limitation": "the syncType 1 peeks (paused 4, stalled 3, 0x80000100 for an unknown id, a reset-removed list or a list after a sceGeDrawSync(0)), the 0x80000107 for other sync types and the syncType 0 results (0 at once for a completed list, no return for a list a reset removed) are measured (PSP-GE-CONTROL-001, PSP-GE-SYNC-001); the queued peek 1 follows uofw unmeasured; a completed list's peek before any sceGeDrawSync(0) read 0 in one PSP launch and often 0x80000100 in others, and the runtime keeps 0 until a sceGeDrawSync(0); command-boundary timing is not measured",
    },
    "h_GeDrawSync": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-GE-CONTROL-001: ge-draw-sync-paused, ge-draw-sync-cancelled, ge-list-dequeue-stalled)",
            "docs/HARDWARE_ORACLE.md (PSP-GE-SYNC-001: ge-sync-invalid-mode, ge-list-sync-after-draw-wait, ge-draw-sync-after-reset-wait)",
            "tests/native/hle_thread_selftest.c:test_ge_break_continue",
            "tests/native/hle_thread_selftest.c:test_ge_blocking_sync_waits_for_stalled_list",
            "tests/native/hle_thread_selftest.c:test_ge_sync_ids_match_psp",
        ],
        "limitation": "the mode 1 peeks (paused 2, stalled 3, 0 after a reset) and the 0x80000107 for modes 2 and -1 are measured (PSP-GE-CONTROL-001, PSP-GE-SYNC-001); the mode 0 wait frees completed lists' slots, so their peeks read 0x80000100 afterwards (measured); after a reset of a stalled list the PSP's mode 0 returned in some launches and did not return in others, and the runtime returns",
    },
    "h_DcacheRangeMaintenance": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-CACHE-001: cache-alias-init, cache-writeback-contrast, cache-inval-contrast)",
            "tests/native/hle_thread_selftest.c:test_dcache_maintenance_notifies_renderer",
        ],
        "limitation": "guest RAM is one coherent array, so the post-maintenance words the cells check are met, but the pre-maintenance contrast words (an uncached read before writeback, a stale cached read before invalidate) are not reproduced; a title that reads stale data through the cached alias is outside this model",
    },
    "h_DelayThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-002: delay-thread-zero)",
            "tests/native/hle_thread_selftest.c:test_delay_zero_probe_semantics",
        ],
        "limitation": "a zero delay returning 0 without yielding to an equal-priority ready thread or dispatching a pending callback is measured; nonzero delays block for the modeled microsecond duration, whose rounding and wake latency are not measured (#325)",
    },
    "h_DelayThreadCB": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-002: delay-threadcb-zero)",
            "tests/native/hle_thread_selftest.c:test_delay_zero_probe_semantics",
            "tests/native/hle_thread_selftest.c:test_delay_threadcb_zero_bounds_a_self_renotifying_callback",
        ],
        "limitation": "a zero delay returning 0 without yielding and dispatching the pending callback exactly once is measured; nonzero delays and the callback pump while blocked are not measured (#325)",
    },
    "h_ExitGame": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-SMOKE-001: module_start, sceKernelExitGame)",
            "tests/native/hle_thread_selftest.c:test_exit_game_ignores_argument_registers",
        ],
        "limitation": "the call ends the guest process and ignores the argument registers (the PSPSDK signature takes none); the smoke cell shows only that a module reaching it exits, and exit callbacks and the return to the system menu are not modeled",
    },
    "h_WaitThreadEnd": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-THREAD-EXIT-001: ED-R77, ED-R00, ED-RNEG, ED-RERR, ED-X77, ED-X00, ED-XNEG, ED-XERR, ED-D77, ED-D00, ED-DNEG, ED-DERR)",
            "tests/native/hle_thread_selftest.c:test_wait_thread_end_invalid_targets",
            "tests/native/hle_thread_selftest.c:test_wait_thread_end_already_ended",
            "tests/native/hle_thread_selftest.c:test_wait_thread_end_blocking_and_resume",
        ],
        "limitation": "the cells join a worker that exits at once (return, ExitThread or ExitDeleteThread with four status values) with a NULL timeout; a join that blocks on a running thread, timeouts, multiple joiners and the remaining-time write are source-selftested only",
    },
    "h_UmdCheckMedium": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: umd-state-before, umd-state-after)",
            "tests/native/hle_thread_selftest.c:test_psp_measured_umd_state_20261024",
        ],
        "limitation": "always reports a medium present (1), as measured with a disc inserted; the runtime serves one ISO, so the no-disc result and disc removal are not modeled",
    },
    "h_UmdDriveStat": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: umd-state-before, umd-state-after)",
            "tests/native/hle_thread_selftest.c:test_psp_measured_umd_state_20261024",
        ],
        "limitation": "reports the modeled drive state: 0x32 with the disc inserted, as measured before and after the PSP-UMDIO-001 opens, and 0x12 after sceUmdDeactivate (a 2026-10-24 measurement recorded in the handler source); no-disc and transitional states are not modeled",
    },
    # ---- #341 Route-A batch 12: PSPAutotests 2ed7c9c5 expectations (recorded on a PSP) ----
    "h_CreateEventFlag": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/events/create/create.expected at 2ed7c9c5 (NULL name 0x80020001, attr 0x100/0x122/0x300/0x900/0x1200 refused with 0x80020191, long names cut to 31 bytes, 1024 flags created in a row)",
            "tests/native/hle_thread_selftest.c:test_evf_create_and_refer",
            "tests/native/hle_thread_selftest.c:test_sync_objects_past_the_old_table",
        ],
        "limitation": "the name, attribute, pattern and 1024-flag cells match; the shared semaphore/event-flag/LwMutex table grows with no fixed cap, and the code 0x80020000 it returns when the host cannot allocate more (and names on stderr) is not measured (the PSP's own limit is not measured); the option struct is ignored",
    },
    "h_DeleteEventFlag": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/events/delete/delete.expected at 2ed7c9c5 (NULL, invalid and deleted ids 0x8002019A; a blocked waiter leaves with 0x800201B5)",
            "tests/native/hle_thread_selftest.c:test_evf_typed_uids_and_delete",
        ],
        "limitation": "unknown-id and waiter-release cells match; the outBits written to a waiter at deletion follow the measured Cancel rule, and a nonzero pattern at deletion is not measured",
    },
    "h_DeleteMsgPipe": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/msgpipe/delete.expected at 2ed7c9c5 (NULL, invalid and deleted ids 0x8002019E; blocked receivers and senders leave with 0x800201B5)",
            "tests/native/hle_thread_selftest.c:test_msgpipe_delete_unknown_ids",
            "tests/native/hle_thread_selftest.c:test_msgpipe_callback_and_delete",
        ],
        "limitation": "unknown-id and blocked-receiver release cells are selftested; the remaining-timeout word a timed waiter receives at deletion and blocked senders are not asserted, and interrupt-context precedence is not established",
    },
    "h_GetBlockHeadAddr": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/sysmem/partition.expected at 2ed7c9c5 (Get deleted, NULL and invalid all 00000000)",
            "tests/native/hle_thread_selftest.c:test_partition_unknown_block_codes",
            "tests/native/hle_thread_selftest.c:test_td28_partition_free_reuse",
        ],
        "limitation": "returns a live block's base and 0 for a freed, zero or unknown UID, as recorded; block placement itself follows the runtime allocator, whose High and aligned types do not yet match partition.expected (sceKernelAllocPartitionMemory stays unreviewed)",
    },
    "h_FreePartitionMemory": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/sysmem/partition.expected at 2ed7c9c5 (Free deleted, NULL and invalid all 0x800200CB)",
            "tests/native/hle_thread_selftest.c:test_partition_unknown_block_codes",
            "tests/native/hle_thread_selftest.c:test_td28_partition_free_reuse",
        ],
        "limitation": "a live block is freed once and coalesced, and a freed, zero or unknown UID returns 0x800200CB as recorded; freeing during a reschedule and the kernel's per-block header accounting are not modeled",
    },
    "h_MaxFreeMemSize": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MODULE-START-001: after the module_start thread's 0x40000-byte stack is freed, sceKernelMaxFreeMemSize does not change, because the stack of the thread module_start created sits directly below the freed range)",
            "tests/native/hle_thread_selftest.c:test_partition_ends_at_lowest_live_stack",
            "tests/native/hle_thread_selftest.c:test_max_free_unchanged_after_top_stack_deleted",
            "tests/native/hle_thread_selftest.c:test_partition_spans_the_whole_user_partition",
        ],
        "limitation": "the result is the larger of the span below the lowest live thread stack and the biggest freed block; only the unchanged-after-stack-free case is measured, so the absolute value at boot, the kernel's block-header and alignment accounting, and the value with several free blocks are not measured (#341)",
    },
    "h_GeSetCallback": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-NULLCODE-001: sceGeSetCallback with a {NULL, NULL} struct returned ID 1 and queued no handler)",
            "tests/native/hle_thread_selftest.c:test_ge_guest_sentinel",
        ],
        "limitation": "registration with null handlers returning an ID is captured on the PSP-3000 (the P1/P2 envelope is CAPTURED, not acceptance-eligible); the PSP returned 1 for the first registration it saw while this runtime hands out the lowest free ID from 0, the table size (16 here) and the full-table result are not measured, and a struct that is not readable returns the illegal-address code unmeasured (#341)",
    },
    "h_GeUnsetCallback": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-NULLCODE-001: sceGeUnsetCallback of the ID that sceGeSetCallback returned gave 0)",
            "tests/native/hle_thread_selftest.c:test_ge_guest_sentinel",
        ],
        "limitation": "unsetting a registered ID returning 0 is captured on the PSP-3000 (CAPTURED, not acceptance-eligible); this runtime also returns 0 for an unregistered or out-of-range ID, which no PSP cell records (#341)",
    },
    "h_LockLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 LwMutex cells: an owner's unlock hands ownership directly to the waiter; a self-held non-recursive timed Lock fails immediately with 0x800201CF)",
            "tests/native/hle_thread_selftest.c:test_lwmutex_hardware_codes",
            "tests/native/hle_thread_selftest.c:test_b23_second_round",
        ],
        "limitation": "uncontended and recursive acquisition, blocking until the owner's hand-off, the self-held non-recursive refusal and the timeout code follow the recorded cells; invalid workareas, a count of 0 or below, an unwritable timeout pointer, callback delivery during the wait, interrupt context and several-waiter ordering are not measured (#341)",
    },
    "h_CpuSuspendIntr": {
        "status": "partial",
        "evidence": [
            "docs/ARCHITECTURE.md#clocks (PSP-DISPLAY-001 display-mask-vcount and sceKernelCpuSuspendIntr: VCOUNT and VBLANK delivery stop while masked, system time and the display controller keep running)",
            "PSPAutotests tests/intr/suspended.expected (the saved-state token: 1 from an enabled CPU, 0 when already suspended)",
            "tests/native/hle_thread_selftest.c:test_interrupt_nid_semantics",
            "tests/native/hle_thread_selftest.c:test_vcount_freezes_while_cpu_interrupts_are_masked",
        ],
        "limitation": "the returned token (1 enabled, 0 nested) and the masked-window VCOUNT/VBLANK freeze are measured; nesting beyond two levels, a suspend taken from an interrupt or callback context, and any thread switch while masked are not measured (#341)",
    },
    "h_CpuResumeIntr": {
        "status": "partial",
        "evidence": [
            "docs/ARCHITECTURE.md#clocks (PSP-DISPLAY-001 display-mask-vcount: a mask crossing no source period credits +0 VCOUNT on resume, one or more credit +1)",
            "tests/native/hle_thread_selftest.c:test_interrupt_nid_semantics",
            "tests/native/hle_thread_selftest.c:test_vcount_credits_one_deferred_period_on_resume",
        ],
        "limitation": "the one coalesced VBLANK delivery and the +0/+1 VCOUNT credit on resume are measured; a token other than 0 or 1 is treated as keeping interrupts suspended, which no PSP cell records, and resume from an interrupt handler is not measured (#341)",
    },
    "h_CpuResumeIntrWithSync": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_interrupt_nid_semantics",
        ],
        "limitation": "an alias of sceKernelCpuResumeIntr with no PSP measurement of its own; the memory synchronisation the name implies is not modelled and the Resume limitations apply (#341)",
    },
    "h_CpuIsIntrSuspended": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/intr/suspended.expected (0 -> 1, 1 -> 0, 2 -> 0, 0xDEADBEEF -> 0, identical with interrupts enabled and suspended)",
            "tests/native/hle_thread_selftest.c:test_is_cpu_intr_suspended_is_token_predicate",
        ],
        "limitation": "a pure predicate that returns 1 only for token 0; the four measured tokens match and every other token value is inferred from them (#341)",
    },
    "h_DisplayGetAccumulatedHcount": {
        "status": "partial",
        "evidence": [
            "docs/ARCHITECTURE.md#clocks (PSP-DISPLAY-001 sceDisplayGetAccumulatedHcount: +69 scanlines over 4 ms and +857 over 50 ms through every interrupt mask, 286 lines per period)",
            "tests/native/hle_thread_selftest.c:test_display_clock_reads_are_observational",
        ],
        "limitation": "the counter advances at the measured 286 lines per display period through a CPU interrupt mask; its absolute value after boot and its wrap width are not measured, and the runtime derives the phase from the 60000/1001 model rather than the device's measured 16,682,850 ns period (#341)",
    },
    "h_DisplayGetVcount": {
        "status": "partial",
        "evidence": [
            "docs/ARCHITECTURE.md#clocks (PSP-DISPLAY-001 display-mask-vcount: VCOUNT stops under CpuSuspendIntr and credits +0 or +1 on resume)",
            "tests/native/hle_thread_selftest.c:test_vcount_freezes_while_cpu_interrupts_are_masked",
            "tests/native/hle_thread_selftest.c:test_vcount_credits_one_deferred_period_on_resume",
            "tests/native/hle_thread_selftest.c:test_vcount_tracks_elapsed_source_periods",
        ],
        "limitation": "the masked-window freeze and the resume credit are measured; the value is the count last updated at a service point, so a VBLANK edge already passed but not yet delivered can be missing, which a live register read would not show (#341)",
    },
    "h_SetVTimerHandler": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/vtimers/sethandler.expected at 2ed7c9c5 (store of schedule, handler and common; uid 0 0x800201BF; unknown uid 0x800201BE)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-TIMER-ORDER-001: timer-order-vtimer-handler, timer-order-vtimer-event)",
            "tests/native/hle_thread_selftest.c:test_vtimer_handler_registration_and_errors",
            "tests/native/hle_thread_selftest.c:test_vtimer_handler_runs_at_its_schedule_and_reschedules",
        ],
        "limitation": "registration, the error codes, the handler arguments and the reschedule rule follow the recorded cells; a NULL time pointer is not measured (the PSP crashes) and stops the run by name, as does a Set from inside the timer's own handler (#341)",
    },
    "h_SetVTimerHandlerWide": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/vtimers/sethandler.expected at 2ed7c9c5 (the Wide form takes the time as a 64-bit value and behaves like SetVTimerHandler)",
            "tests/native/hle_thread_selftest.c:test_vtimer_handler_registration_and_errors",
            "tests/native/hle_thread_selftest.c:test_vtimer_handler_runs_at_its_schedule_and_reschedules",
        ],
        "limitation": "the 64-bit argument placement (a2/a3, handler and common from the stack) follows the public signature and the shared registration cells; the Wide form has no hardware cell of its own, and a Set from inside the timer's own handler stops the run by name (#341)",
    },
    "h_CancelVTimerHandler": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/vtimers/cancelhandler.expected at 2ed7c9c5 (clears the handler, keeps schedule and common, repeatable; from inside the timer's own handler 0x800201BF for that timer and 0x800201BE for uid 0)",
            "tests/native/hle_thread_selftest.c:test_vtimer_handler_registration_and_errors",
            "tests/native/hle_thread_selftest.c:test_vtimer_handler_catches_up_and_refuses_cancel_inside",
        ],
        "limitation": "the recorded cells match; error precedence for a cancel from inside a handler on another timer beyond the recorded uid cases is not measured (#341)",
    },
    "h_AllocPartitionMemory": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/sysmem/partition.expected at 2ed7c9c5 (partitions 1, 3 and 4 0x800200D6, other numbers 0x800200D2, type past 4 0x800200D8)",
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-SEM-001: vmem-alloc-free and vmem-alloc-held, a partition-5 Low block at 0x08400000 and a partition-2 Addr block there 0x800200D9)",
            "tests/native/hle_thread_selftest.c:test_partition_unknown_block_codes",
            "tests/native/hle_thread_selftest.c:test_td23_guest_pointer_validation",
            "tests/native/hle_thread_selftest.c:test_td28_partition_free_reuse",
        ],
        "limitation": "partition and type refusals, the volatile-partition cells and the unplaceable-Addr code match the recorded cells; where a PSP places a partition-6 block is unmeasured, partitions 2 and 6 accept a NULL name that the PSP refuses (existing callers rely on it), and blocks are placed from a bump heap rather than the kernel's allocator (#341)",
    },
    "h_CtrlReadBuffer": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-SEM-001: ctrl-empty-read, ctrl-short-read, ctrl-short-read-next consumption; a read right after a read waits for the next sample)",
            "PSPAutotests tests/ctrl/sampling and tests/ctrl/vblank at f573fe8c (a read right after a read answers 1)",
            "tests/native/hle_thread_selftest.c:test_ctrl_read_buffer_contract",
            "tests/native/hle_thread_selftest.c:test_ctrl_sampling_contract",
        ],
        "limitation": "consumption of every unread sample, the newest-sample window and the one-sample wait match the recorded cells; samples are latched once per VBLANK whatever the sampling cycle, and digital mode does not centre the stick in the delivered data (#1390, #341)",
    },
    "h_CtrlPeekBufferPositive": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-SEM-001: ctrl-peek-count, a peek of 64, 3 and 1 right after a read returns 64, 3 and 1)",
            "PSPAutotests tests/ctrl/sampling and tests/ctrl/vblank at f573fe8c (a peek never blocks, also inside a VBLANK handler)",
            "tests/native/hle_thread_selftest.c:test_ctrl_read_buffer_contract",
            "tests/native/hle_thread_selftest.c:test_ctrl_sampling_contract",
        ],
        "limitation": "the peek counts and non-consumption match the recorded cells; sample timestamps beyond the recorded counts and the stick bytes in digital mode are not measured (#1390, #341)",
    },
    "h_CtrlReadBufferNegative": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ctrl_read_buffer_contract",
        ],
        "limitation": "the positive read contract with the button word inverted, from the public pspctrl.h description; no hardware cell measures the negative variants, and the positive-read limitations apply (#341)",
    },
    "h_CtrlPeekBufferNegative": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_ctrl_read_buffer_contract",
        ],
        "limitation": "the positive peek contract with the button word inverted, from the public pspctrl.h description; no hardware cell measures the negative variants, and the positive-peek limitations apply (#341)",
    },
    # sceVideocodec and sceMpegbase: the Media Engine video boundary under a disc mpeg.prx (#286).
    "h_VideocodecOpen": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_context_words",
            "tests/native/videocodec_selftest.c:test_reopen_after_release",
        ],
        "limitation": "words synthetic, from a W-LLE-1 read PSP-VCODEC-001 does not record yet (zeroed context): type 0 writes +0 = 0x05100601, +8 = 1 and +24 = 0x8344 and returns 0; type 1 writes +0, +8 = 0x80000000 and +24 = 0x264C and returns 0x80628001, and the later calls on a type 1 context are refused; other types are refused. A type 0 context the runtime already holds opens again and returns 0 (#2885): JPCSP-derived, not PSP-measured (jpcsp/jpcsp 00ac4d6daedd89c0492093ae83fb0d3d819af414, sceVideocodec.sceVideocodecOpen), logged once per run; the host decoder and picture state go while the ME memory and band buffers stay, a project rule no PSP cell measures",
    },
    "h_VideocodecInit": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_context_words",
        ],
        "limitation": "words synthetic, from a W-LLE-1 read PSP-VCODEC-001 does not record yet (type 0): writes +12 = +20 and +8 = 0 and nothing else",
    },
    "h_VideocodecGetEDRAM": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_context_words",
        ],
        "limitation": "writes +92 = the raw ME address and +20 = it aligned to 64 bytes, the rule synthetic, from a W-LLE-1 read PSP-VCODEC-001 does not record yet (type 0); allocates ((+24) + 63) | 0x3F bytes of a host 2 MiB ME RAM model, low-first, first 0x10000 reserved, so the addresses themselves differ from the PSP's",
    },
    "h_VideocodecReleaseEDRAM": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_teardown",
        ],
        "limitation": "clears +20 and +92 and leaves +12, the rule synthetic, from a W-LLE-1 read PSP-VCODEC-001 does not record yet (type 0), and frees the host ME RAM allocation",
    },
    "h_VideocodecSetMemory": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_context_words",
        ],
        "limitation": "accepts the call and writes nothing; no context word changed in that read either (synthetic, from a W-LLE-1 read PSP-VCODEC-001 does not record yet; type 0, zeroed +64..+76); the meaning of words +64..+76 is not modelled",
    },
    "h_VideocodecStop": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_teardown",
        ],
        "limitation": "before any decode returns 0x806201FE and changes nothing (synthetic, from a W-LLE-1 read PSP-VCODEC-001 does not record yet; type 0); after a decode drops the host decoder and returns 0 (JPCSP, synthetic pending W-LLE-1); whether the PSP drains pending pictures is not measured",
    },
    "h_VideocodecDelete": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_teardown",
        ],
        "limitation": "synthetic context words from JPCSP pending W-LLE-1; frees the decoder and ME memory and closes the context; returns at once (JPCSP notes about 40 ms on a PSP)",
    },
    "h_VideocodecDecode": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_decode",
        ],
        "limitation": "synthetic context words from JPCSP pending W-LLE-1; type 0 (AVC) only; synchronous host H.264 decode; reads the access unit and writes the ME info blocks through JPCSP's ME memory view (ME RAM, else guest memory); writes the picture into its ME band buffers in JPCSP's band layout; buffer2, YUV struct, ME info blocks, buffer3 and SEI words are JPCSP's; a decoded size that is not whole macroblocks (a cropped 480 x 270, for example) is refused; fails closed without an H.264 backend",
    },
    "h_MpegBaseCscInit": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_csc",
        ],
        "limitation": "synthetic context words from JPCSP pending W-LLE-1; records the default row width only; the coefficient table the PSP writes is not reproduced (conversion is the measured sr_h264_psp_rgb)",
    },
    "h_MpegBaseCscAvc": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_csc",
        ],
        "limitation": "synthetic context words from JPCSP pending W-LLE-1; reads the eight bands through JPCSP's ME memory view (ME RAM, else guest memory) in JPCSP's band layout and converts the struct's whole picture, up to 720 x 576; band addresses are converted as given, so a zeroed struct converts ME RAM from offset 0 (the ME memory view) and returns 0, and a picture never decoded is no longer refused; 16-bit pixel modes and alpha follow sceMpegAvcCsc and are not measured",
    },
    "h_MpegBasePESpacketCopy": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_packet_copy",
        ],
        "limitation": "synthetic context words from JPCSP pending W-LLE-1; checks the whole packet list (sources, destinations, lengths, cycles) before copying; destinations go through JPCSP's ME memory view (ME RAM, else guest memory); refusal codes other than the zero-length one are project-chosen",
    },
    "h_Mpegbase_0530BE4E": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_csc",
        ],
        "limitation": "synthetic context words from JPCSP pending W-LLE-1; mode mapping 0=8888, 1=5650, 2=5551, 3=4444 from JPCSP; unknown modes return -1",
    },
    "h_VideocodecGetVersion": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_version_sei",
        ],
        "limitation": "synthetic context word from JPCSP pending W-LLE-1 (JPCSP cites its own JpcspTrace record); writes context +4 = 0x78 and returns 0; codec type 0 only",
    },
    "h_VideocodecGetSEI": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_version_sei",
        ],
        "limitation": "synthetic word from JPCSP pending W-LLE-1; clears word +28 of the SEI block named by context +80 and nothing else; a context without a writable SEI block is refused",
    },
    "h_MpegBaseCscAvcRange": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_copy_range",
        ],
        "limitation": "synthetic layout from JPCSP pending W-LLE-1; the range words are 16-pixel units and the rectangle's top-left lands at the destination, as JPCSP writes it; otherwise the CscAvc limitations apply",
    },
    "h_MpegBaseYCrCbCopy": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_copy_range",
        ],
        "limitation": "synthetic band split from JPCSP pending W-LLE-1; flags bit 0 copies bands 0, 1, 4 and 5 and bit 1 bands 2, 3, 6 and 7 from the source buffers (ME memory view) into the guest buffers, byte for byte; other bits are refused; the byte counts are the decoder's band sizes, not JPCSP's handler counts (16-pixel units; buffer 5 given band 1's count and buffer 6 band 4's); every span is checked before the first byte moves",
    },
    # sceAudiocodec: the Media Engine audio half under a disc mpeg.prx (#286), in hle_videocodec.c.
    "h_AudiocodecCheckNeedMem": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_audiocodec",
        ],
        "limitation": "codec 0x1000 (ATRAC3plus) only; the wrapper checks follow uofw, except that an in-range codec other than 0x1000 (0x1001-0x1005) is refused with 0x80000004, a project-chosen synthetic code (uofw goes on; the W-LLE-1 read returned 0 for 0x1001 and 0x1002, synthetic); every call writes +0 = 0x05100601 as uofw does; the words are synthetic, from a W-LLE-1 read PSP-VCODEC-001 does not record yet, for the three halfwords at +40 it was given: 0x5C28 (+16 = 0x7BC0, +52 = 44100, +60 = 2, +64 = 0x2E8), 0x5C5C (+16 = 0x1F6C0, +52 = 48000, +60 = 7, +64 = 0x2E8), and 0 (0x807F00FD; +8 = 0x20F, +52 = 0x7D00, +64 = 8); any other halfword is refused",
    },
    "h_AudiocodecGetEDRAM": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_audiocodec",
        ],
        "limitation": "codec 0x1000 only; allocates ((+16) + 0x3F) | 0x3F bytes of the host ME RAM model, owned by the work area, as uofw sizes it; a work area whose +104 still names its allocation is refused with 0x807F0003 (uofw allocates again and orphans the first; the refusal is synthetic pending W-LLE-1); one whose +104 no longer names it (recycled without a release) gets a new allocation and the stale one is freed; the addresses are not PSP-measured",
    },
    "h_AudiocodecReleaseEDRAM": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_audiocodec",
        ],
        "limitation": "frees only the host ME RAM allocation the work area owns: a +104 naming any other allocation (another work area's, a video context's) is refused with 0x807F0004 and nothing is freed; clears +104 and +12 and drops the host decoder as JPCSP does",
    },
    "h_AudiocodecInit": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_audiocodec",
        ],
        "limitation": "codec 0x1000 only; as in uofw, any work area may name the EDRAM at +12 (a disc mpeg.prx initialises a work area other than the one it allocated with); refuses a +12 that is not an EDRAM sceAudiocodecGetEDRAM published, or a +16 larger than it (project-chosen code 0x807F00FF, synthetic pending W-LLE-1)",
    },
    "h_AudiocodecDecode": {
        "status": "partial",
        "evidence": [
            "tests/native/videocodec_selftest.c:test_audiocodec",
        ],
        "limitation": "codec 0x1000 only, PSMF frames with the 0x0FD0 header only; mono or stereo input into one stereo 16-bit block of 2048 samples (JPCSP's sceAudiocodecInit output; a mono frame is duplicated); uofw's +56/+72 size rule is written at +36 and does not choose the layout; synchronous decode; the +28 and +36 words and the refusal code are synthetic pending W-LLE-1",
    },
    # #341 graduations (2026-11-06): handlers that already reproduce every pspautotests cell the
    # runtime can replay. Cells are from the pinned pspautotests clone at 70e7239 unless an entry
    # names f573fe8c.
    "h_RegisterSubIntr": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/intr/registersub.expected at f573fe8c (the code for every line and every VBLANK sub-interrupt -2 to 69; a NULL handler answers 0 and registers nothing; a held number answers FOUND_HANDLER and keeps the first handler)",
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-SEM-001: VBLANK sub-interrupt handlers run in number order with their own (no, arg), #1287)",
            "tests/native/hle_thread_selftest.c:test_vblank_subintr_measured_codes",
        ],
        "limitation": "the recorded codes and VBLANK delivery are reproduced; the GE interrupt (line 25) stops the run by name because this runtime delivers no GE sub-interrupt, and a Register from interrupt context is not measured (#341)",
    },
    "h_ReleaseSubIntr": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/intr/releasesub.expected and enablesub.expected at f573fe8c (the code for every line and VBLANK sub-interrupt -2 to 69; NOTFOUND_HANDLER for a number never registered, released or NULL-registered; Release clears the enable state)",
            "tests/native/hle_thread_selftest.c:test_vblank_subintr_measured_codes",
        ],
        "limitation": "the recorded codes are reproduced; the GE interrupt (line 25) stops the run by name, and a Release from inside the handler being released is not measured (#341)",
    },
    "h_EnableSubIntr": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/intr/enablesub.expected at f573fe8c (0 for a registered number, a second time and a number with no handler; Enable before Register delivers once the handler is registered; an invalid line or sub-interrupt answers ILLEGAL_INTRCODE)",
            "tests/native/hle_thread_selftest.c:test_vblank_subintr_measured_codes",
        ],
        "limitation": "VBLANK (line 30) sub-interrupts follow the recorded cells; lines other than 30 that the cells do not record stop the run by name (#341)",
    },
    "h_DisableSubIntr": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/intr/enablesub.expected at f573fe8c (Disable answers as Enable does; a disabled handler is not delivered)",
            "tests/native/hle_thread_selftest.c:test_vblank_subintr_measured_codes",
            "tests/native/hle_thread_selftest.c:test_td24b_cheap_hle_batch",
        ],
        "limitation": "as sceKernelEnableSubIntr; whether a Disable during the VBLANK being delivered stops that delivery is not measured (#341)",
    },
    "h_CheckCallback": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/callbacks/check.expected (0 with none pending; 1 with one or two pending, run in slot order; 0 from a thread whose callbacks are not pending)",
            "tests/native/hle_thread_selftest.c:test_callback_check_pump_services_pending_in_slot_order",
        ],
        "limitation": "the result and the slot-order dispatch follow the recorded cells; nested and recursive dispatch and a check from interrupt context are not measured (#341)",
    },
    "h_ReferLwMutexStatus": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/lwmutex/refer.expected (size 64, name, attr, uid, workarea, count and lock thread; input size 0 is left alone, any other size reads back 0x40; 0x800201CA after delete)",
            "tests/native/hle_thread_selftest.c:test_td24d_hle_batch",
        ],
        "limitation": "the recorded fields, sizes and deleted-workarea code are reproduced; a NULL or unmapped workarea or info pointer is not measured (#341)",
    },
    "h_ReferLwMutexStatusByID": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/lwmutex/refer.expected (the sceKernelReferLwMutexStatusByID block: the same fields and sizes as by workarea)",
            "tests/native/hle_thread_selftest.c:test_td24d_hle_batch",
        ],
        "limitation": "as sceKernelReferLwMutexStatus; an unmapped info pointer is not measured (#341)",
    },
    "h_ChangeThreadPriority": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/change.expected (unknown and deleted ids 0x80020198; created and finished threads 0x800201a2; priorities outside 0x08-0x77 0x80020193 with the priority unchanged; priority 0 takes the caller's)",
            "tests/native/hle_thread_selftest.c:test_change_thread_priority_contract",
        ],
        "limitation": "user-mode priorities and the recorded thread states are reproduced; kernel-mode priorities and the ready-queue position a non-current target takes are not measured (#341)",
    },
    "h_ChangeCurrentThreadAttr": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/change.expected (the sceKernelChangeCurrentThreadAttr blocks)",
            "tests/native/hle_thread_selftest.c:test_change_current_thread_attr",
        ],
        "limitation": "the accepted and refused attribute bits follow the recorded cells; a call from interrupt context is not measured (#341)",
    },
    "h_TerminateThread": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/terminate.expected (unknown and deleted ids 0x80020198; 0 and the current thread 0x80020197; created and finished threads 0x800201a2; ready, suspended and waiting threads 0; a thread waiting for the end wakes with 0x800201ac)",
            "tests/native/hle_thread_selftest.c:test_issue339_terminate_waiting_thread",
        ],
        "limitation": "the recorded thread states and the waiter's wake code are reproduced; terminating a thread that holds a mutex or is blocked inside an HLE I/O wait is not measured (#341); from an interrupt handler the call answers ILLEGAL_CONTEXT for a compiled SDK version of 3.08 or later (PPSSPP-derived, not PSP-measured) and stops by name for an earlier one",
    },
    "h_TerminateDeleteThread": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/terminate.expected (sceKernelTerminateDeleteThread: created, ready, finished, suspended and waiting threads 0; 0 and the current thread 0x80020197; the id answers 0x80020198 afterwards)",
            "tests/native/hle_thread_selftest.c:test_issue339_terminate_waiting_thread",
        ],
        "limitation": "as sceKernelTerminateThread; the release of the thread's stack partition is not observed by any cell (#341)",
    },
    "h_GetThreadStackFreeSize": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/stackfree.expected (0xea0 free on an unused 0x1000 stack and after 1 KiB of 0xFF, 0xaa0 after 1 KiB of zeros, 0 without the stack-fill attribute; unknown ids 0x80020198)",
            "tests/native/hle_thread_selftest.c:test_thread_stack_free_size_contract",
        ],
        "limitation": "the recorded free sizes are reproduced; the low-stack attribute is not modelled (#341)",
    },
    "h_SuspendDispatchThread": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/scheduling/dispatch.expected (Suspend answers 1; with interrupts suspended 0x80020066)",
            "tests/native/hle_thread_selftest.c:test_dispatch_suspend_resume_nid_semantics",
        ],
        "limitation": "the token and the interrupts-suspended refusal follow the recorded cells; the token a nested Suspend returns is not measured (#341)",
    },
    "h_ResumeDispatchThread": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/scheduling/dispatch.expected (Resume with the token answers 0; with interrupts suspended 0x80020066)",
            "tests/native/hle_thread_selftest.c:test_dispatch_suspend_resume_nid_semantics",
        ],
        "limitation": "Resume with the token 1 is measured; other token values are not (#341)",
    },
    "h_PowerGetBatteryLifePercent": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (96 on AC with a full battery; 93 on battery)",
        ],
        "limitation": "reports the two measured samples for the modelled power source (external power by default); no discharge model (#341)",
    },
    "h_PowerIsBatteryExist": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (1 on AC and on battery); PSPAutotests tests/power/power.expected (1)",
        ],
        "limitation": "always 1: a PSP running without a battery is not measured (#341)",
    },
    "h_PowerIsPowerOnline": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (1 on AC, 0 on battery); PSPAutotests tests/power/power.expected (1)",
        ],
        "limitation": "reports the modelled power source; a change of source during a run is not modelled (#341)",
    },
    "h_PowerIsLowBattery": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (0 on AC and on battery); PSPAutotests tests/power/power.expected (0)",
        ],
        "limitation": "0 in both measured states; the answer for a low battery is not measured (#341)",
    },
    "h_PowerIsBatteryCharging": {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (0 on AC with a full battery and on battery)",
        ],
        "limitation": "0 in both measured states; the answer while the battery charges is not measured (#341)",
    },
    "h_PowerUnregisterCallback": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/power/power.expected (an owned slot 0; a second time and a slot never registered 0x80000025; slots -4 and 32 0x80000102; slot 31 0x80000023)",
            "hardware measurement 2026-10-22, PSP-3000 6.61, PR #1153 (slot 16 0x80000023; slots -1, -2 and 1000 0x80000102)",
            "tests/native/hle_thread_selftest.c:test_power_unregister_callback_slots",
        ],
        "limitation": "the recorded slots are reproduced; slots 17 to 30 are not measured (#341)",
    },
    "h_PowerLock": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/power/power.expected (Lock(0) 0; Lock(1) 0x80000107)",
            "tests/native/hle_thread_selftest.c:test_td24d_hle_batch",
        ],
        "limitation": "the result codes are reproduced; the lock count has no PSP-visible effect here because suspend is not modelled (#341)",
    },
    "h_PowerUnlock": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/power/power.expected (Unlock(0) 0, and 0 again)",
            "tests/native/hle_thread_selftest.c:test_td24d_hle_batch",
        ],
        "limitation": "Unlock(0) is measured; the 0x80000107 answer for another type is not (#341)",
    },
    "h_PowerTick": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/power/power.expected (Tick(-1) 0)",
            "tests/native/hle_thread_selftest.c:test_td24d_hle_batch",
        ],
        "limitation": "only type -1 is measured; the idle timer a tick resets is not modelled (#341)",
    },
    "h_DisplayGetFrameBuf": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/display/setframebuf.expected (address, width and format read back after each immediate and latched sceDisplaySetFrameBuf)",
            "tests/native/hle_thread_selftest.c:test_display_framebuf_latch",
        ],
        "limitation": "the read-back the cells record is reproduced; unwritable output pointers are not measured (#341)",
    },
    "h_DisplayIsVblank": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/display/isstate.expected (1 just after a VBLANK starts, 0 after it ends)",
            "tests/native/hle_thread_selftest.c:test_display_clock_reads_are_observational",
            "tests/native/sched_selftest.c:test_display_isvblank_uses_live_edge_time",
        ],
        "limitation": "the VBLANK window is modelled from the measured period; the wake latency after an edge (vblankphase.expected) is not (#341)",
    },
    "h_DisplayWaitVblank": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/display/vblankmulti.expected (the Vblank block: the first wait 0; a wait inside the same VBLANK answers 1 without blocking)",
            "tests/native/hle_thread_selftest.c:test_issue339_callback_wait_dispatch",
        ],
        "limitation": "the in-window and out-of-window results follow the recorded cells; a call from interrupt context is not modelled (#341)",
    },
    "h_DisplayWaitVblankCB": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/display/vblankmulti.expected (the VblankCB block, as the Vblank block)",
            "tests/native/hle_thread_selftest.c:test_issue339_callback_wait_dispatch",
        ],
        "limitation": "as sceDisplayWaitVblank (#341)",
    },
    "h_UmdGetDiscInfo": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/umd/api/api.expected (NULL, size 0, size 12 and a kernel pointer 0x80010016)",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (size 8 answers 0 and type 0x10 with a game disc)",
            "tests/native/hle_thread_selftest.c:test_umd_replace_permission_and_disc_info",
            "tests/native/hle_thread_selftest.c:test_psp_measured_umd_state_20261024",
        ],
        "limitation": "only the game-disc type 0x10 is reported; video and audio discs are not modelled (#341)",
    },
    "h_UmdRegisterUMDCallBack": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/umd/register.expected and tests/umd/api/api.expected (invalid and zero ids, semaphore, event-flag and thread ids 0x80010016; a callback 0, and 0 again)",
            "tests/native/hle_thread_selftest.c:test_umd_unregister_callback",
        ],
        "limitation": "one callback slot; the notification argument it later receives follows PPSSPP's model, not a measurement (#341)",
    },
    "h_IoLseek32": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/io/file/file.expected (sceIoLseek32(fd, -10, SEEK_END) 26 on a 36-byte file)",
            "docs/HARDWARE_ORACLE.md (PSP-IO-EOF-001 and PSP-IO-EOF-002: a Memory Stick seek before the start answers 0x80010016 and a disc0: seek outside the file 0x80010005, position unchanged)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-001: io-seek-flash-*)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-002: io-edge-flash-*)",
            "tests/native/hle_thread_selftest.c:test_fd_namespace",
            "tests/native/hle_thread_selftest.c:test_flash0_font_device",
        ],
        "limitation": "the Memory Stick, disc0: and flash0: (SEEK_SET and SEEK_END, PSP-HLE-IO-SEEK-001, on ltn0.pgf) rules are measured, and PSP-HLE-IO-SEEK-002 adds flash0:/font/ltn0.pgf SEEK_CUR before the start and past the end (0x80010016, position unchanged); sceIoLseek32Async keeps the earlier rule (no async seek is measured), other flash0 files are unmeasured, and a seek before the start on other devices is not measured (#341)",
    },
    # #341 fixes (2026-11-06): handlers changed to match their pspautotests cells, with a
    # production-dispatch test that fails on the old code.
    "h_CancelMutex": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/mutex/cancel.expected (a negative count releases the mutex to count 0 with no owner; a count above 1 on a non-recursive mutex answers 0x800201BD)",
            "tests/native/hle_thread_selftest.c:test_psp_mutex",
        ],
        "limitation": "the recorded counts and the waiters' wake code are reproduced; the order of the count and the recursion refusals is not measured (#341)",
    },
    "h_PollSema": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/semaphores/poll.expected (a count of 0 or below answers 0x800201BD before the id is looked up, a NULL id included)",
            "tests/native/hle_thread_selftest.c:test_sema_hardware_codes",
        ],
        "limitation": "the recorded codes and their order are reproduced; a poll from interrupt context is not measured (#341)",
    },
    "h_TryLockLwMutex600": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/lwmutex/try600.expected (counts 0, -1 and 2 on a non-recursive mutex 0x800201BD; self-held 0x800201CF; recursive overflow 0x800201CD; held by another thread 0x800201CB; a deleted workarea 0x800201CA)",
            "tests/native/hle_thread_selftest.c:test_trylock600_cells",
        ],
        "limitation": "the recorded codes and lock state are reproduced; the file's \"(fake)\" cells, a workarea whose uid word is 0, are not modelled and answer 0x800201CB (#341)",
    },
    "h_CreateMsgPipe": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/msgpipe/create.expected (partitions 2 and 6 accepted, 1, 3 and 4 0x800200D1, others 0x800200D2; attributes outside 0x51FF 0x80020191; sizes 0xFFFFFFFF and 0x04000000 0x80020190, 1 MiB accepted; a NULL name 0x80020190)",
            "tests/native/hle_thread_selftest.c:test_msgpipe_create_cells",
        ],
        "limitation": "each recorded argument is reproduced on its own; the order when several are wrong, partition 5, and sizes between 1 MiB and 64 MiB (refused here) are not measured (#341)",
    },
    "h_GetThreadExitStatus": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/exitstatus.expected (uid 0 0x80020198, not the current thread)",
            "tests/native/hle_thread_selftest.c:test_thread_exit_status_zero_uid",
        ],
        "limitation": "uid 0 and the recorded thread states are reproduced; the status of a thread ended by an exception is not measured (#341)",
    },
    "h_GetThreadmanIdList": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/threads/threadmanidlist.expected (a negative size 0x800200D3 with the count unwritten; types 1 to 14 listed, 15 and above illegal)",
            "tests/native/hle_thread_selftest.c:test_threadman_id_list_cells",
            "docs/HARDWARE_ORACLE.md (PSP-SAFE-SCALAR-001: tmid-9-list and tmid-10-list, an empty list returns 0 with a count of 0 and no slot written)",
            "tests/native/hle_thread_selftest.c:test_threadman_id_list",
        ],
        "limitation": "the recorded sizes and types are reproduced; the thread event handler class (type 9) answers only the measured call shape, a 16-slot buffer and a count pointer, with the measured empty list, whose precondition (no handler registered) holds for every guest because this runtime has no call that registers one; type 9 with any other buffer size, or a NULL or unmapped buffer or count pointer, is not measured and stops the run by name; the list order for objects created in another order than uid order is not measured (#341)",
    },
    "h_VolatileMemUnlock": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/power/volatile/unlock.expected (mode 0 answers 0, every other mode 0x80000107 with the lock still held, through the sceKernel and the scePower NIDs)",
            "tests/native/hle_thread_selftest.c:test_wc_volatile_unlock_mode",
            "tests/native/hle_thread_selftest.c:test_volatile_mem_unlock_states",
        ],
        "limitation": "the recorded modes are reproduced; the order of the mode check and the not-locked 0x800201AE answer is not measured (#341)",
    },
    "h_VolatileMemLock": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/power/volatile/lock.expected (mode 0 answers 0 and writes 0x08400000 and 0x00400000; other modes 0x80000107 with nothing written; a NULL output pointer is skipped)",
            "tests/native/hle_thread_selftest.c:test_volatile_mem_lock_blocks_and_hands_off",
            "tests/native/hle_thread_selftest.c:test_volatile_mem_lock_departures_and_type",
        ],
        "limitation": "the recorded modes, outputs and waits are reproduced; partition-5 use while the lock is held is not modelled (#341)",
    },
    "h_VolatileMemTryLock": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/power/volatile/trylock.expected (as lock.expected, and 0x802B0200 while locked)",
            "tests/native/hle_thread_selftest.c:test_volatile_mem_lock_departures_and_type",
        ],
        "limitation": "as sceKernelVolatileMemLock (#341)",
    },
    "h_CtrlSetIdleCancelThreshold": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/ctrl/idle/idle.expected (each value -1 to 128 accepted; either value outside that range 0x800001FE with nothing stored)",
            "tests/native/hle_thread_selftest.c:test_wc_ctrl_idle_cancel_threshold",
        ],
        "limitation": "the stored values and refusals are reproduced; the idle-cancel effect itself is not modelled because no idle timer runs (#341)",
    },
    "h_CtrlGetIdleCancelThreshold": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/ctrl/idle/idle.expected (reads back -1, -1 at start and each stored pair; NULL pointers are skipped; an unusable non-NULL pointer 0x80000023 before any write)",
            "tests/native/hle_thread_selftest.c:test_wc_ctrl_idle_cancel_threshold",
        ],
        "limitation": "the recorded read-backs are reproduced (#341)",
    },
    "h_UmdWaitDriveStat": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/umd/api/api.expected (a stat with no valid bit 0x80010016, checked first; dispatch or interrupts disabled 0x800201A7; an already satisfied wait 0; a cancel 0x800201A9)",
            "tests/native/hle_thread_selftest.c:test_wc_umd_wait_drive_stat",
        ],
        "limitation": "the recorded codes and waits are reproduced; the drive states a disc swap passes through are not modelled (#341)",
    },
    "h_UmdWaitDriveStatWithTimer": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/umd/api/api.expected (as sceUmdWaitDriveStat; a timeout of 0 waits until cancelled; timeouts of 5 to 200 us end after about 250 us with 0x800201A8)",
            "tests/native/hle_thread_selftest.c:test_wc_umd_wait_drive_stat",
        ],
        "limitation": "as sceUmdWaitDriveStat; the timeout rounding follows the recorded buckets, not a measured rule (#341)",
    },
    "h_UmdWaitDriveStatCB": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/umd/api/api.expected (as sceUmdWaitDriveStatWithTimer, with callbacks serviced; a cancel during a callback 0x800201A9)",
            "tests/native/hle_thread_selftest.c:test_wc_umd_wait_drive_stat",
        ],
        "limitation": "as sceUmdWaitDriveStatWithTimer; a cancel from inside the callback answering 0x800201A8 is not modelled (#341)",
    },
    "h_CreateCallback": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/callbacks/create.expected",
            "tests/native/hle_thread_selftest.c:test_callback_object_cells",
        ],
        "limitation": "the recorded results are reproduced; the 31-character name truncation and the notify argument read back through sceKernelReferCallbackStatus are not asserted (#341)",
    },
    "h_DeleteCallback": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/callbacks/delete.expected",
            "tests/native/hle_thread_selftest.c:test_callback_object_cells",
        ],
        "limitation": "the recorded results are reproduced; deleting a callback while it runs is not measured (#341)",
    },
    "h_CancelCallback": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/callbacks/cancel.expected",
            "tests/native/hle_thread_selftest.c:test_callback_object_cells",
        ],
        "limitation": "the recorded results are reproduced (#341)",
    },
    "h_GetCallbackCount": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/callbacks/count.expected",
            "tests/native/hle_thread_selftest.c:test_callback_object_cells",
        ],
        "limitation": "the recorded counts are reproduced (#341)",
    },
    "h_PowerRegisterCallback": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/power/power.expected (slots 0 to 2 0; an invalid callback 0x80000100; slots -4 and 32 0x80000102; slot 31 0x80000023)",
            "tests/native/hle_thread_selftest.c:test_wc_power_register_callback_errors",
        ],
        "limitation": "the recorded slots and the first notification are reproduced; slots other than the recorded ones are not measured (#341)",
    },
    "h_DisplayGetMode": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/display/setmode.expected (the mode, width and height set read back)",
            "tests/native/hle_thread_selftest.c:test_wc_display_getmode_and_start_cb",
        ],
        "limitation": "the read-back is reproduced; unwritable output pointers are not measured (#341)",
    },
    "h_DisplayWaitVblankStartCB": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/display/vblankmulti.expected (the StartCB block: 0 after the next VBLANK start)",
            "tests/native/hle_thread_selftest.c:test_wc_display_getmode_and_start_cb",
        ],
        "limitation": "returns 0 after one delivered VBLANK with callbacks serviced; the guest VCOUNT step is not asserted, and a call from interrupt context is not modelled (#341)",
    },
    "h_CreateTlspl": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/tls/create.expected (a NULL name 0x80020190; partitions 2 and 6 accepted, 1, 3 and 4 0x800200D1, others 0x800200D2; a block size or count of 0 or -1 0x800201B7; power-of-two alignments; the 17th pool 0x800201D1)",
            "tests/native/hle_thread_selftest.c:test_wb_tlspl_vpl_cells",
            "PSPAutotests tests/threads/tls/create.expected line 29 (774dfddc467ae0cbb7d3d8b22fc271f74135cd38, a third-party recording made on a PSP): attribute 0x4000 creates a pool that reports the attribute and takes 1024 bytes from the partition's free total, returned by the delete",
            "tests/native/hle_thread_selftest.c:test_tlspl_highmem_pool",
            "docs/HARDWARE_ORACLE.md (PSP-SAFE-SCALAR-001: tls-create-normal, tls-place-normal, tls-addr-normal, tls-create-high, tls-place-high, tls-addr-high, tls-create-high-2, tls-addr-high-2: in partition 2 a pool of four 0x100-byte blocks with NULL options lies at the low end of the free memory without attribute 0x4000 and at the high end with it, a second such pool below the first)",
            "tests/native/hle_thread_selftest.c:test_psp_measured_safe_scalar_tls_20261117",
        ],
        "limitation": "the recorded arguments are reproduced; where an attribute 0x4000 pool lies is PSP-measured for one pool shape only (PSP-SAFE-SCALAR-001: partition 2, attribute exactly 0x4000, four 0x100-byte blocks, NULL options): the top of the partition; for every other attribute 0x4000 pool (another block size or count, an options block, another attribute bit beside 0x4000, partition 6) the top of the partition is PPSSPP-derived, not PSP-measured (hrydgard/ppsspp 65181eea1deb7361beb3be6062ea98e8427f2786, Core/HLE/sceKernelMemory.cpp sceKernelCreateTlspl; jpcsp/jpcsp 00ac4d6daedd89c0492093ae83fb0d3d819af414 SceKernelTls agrees) and is logged once per run; the measured free-size words are not reproduced exactly: the free total carries a 0x100 tail per block slot, and in a partition based below the runtime stacks the free sizes do not count such a pool, which lies above the bump region they measure (#2640); partitions 5 and 11 and above and alignments above 0x10000 keep a refusal the cells do not record (#341)",
    },
    "h_DeleteTlspl": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/tls/delete.expected (NULL, invalid and deleted ids 0x800201D0; a pool with a block still held 0x800201D2)",
            "tests/native/hle_thread_selftest.c:test_wb_tlspl_vpl_cells",
        ],
        "limitation": "the recorded results are reproduced; the wake code of threads still waiting when a pool is deleted is not asserted (#341)",
    },
    "h_FreeTlspl": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/tls/free.expected (a free answers 0 and a second free 0; NULL, invalid and deleted ids 0x800201D0; a waiting thread receives the freed block)",
            "tests/native/hle_thread_selftest.c:test_wb_tlspl_vpl_cells",
        ],
        "limitation": "the recorded results are reproduced; the block is cleared on free; the hand-off order with several waiters is not measured (#341)",
    },
    "h_ReferTlsplStatus": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/tls/refer.expected (a 60-byte record with index at +40; input size 0 is left alone, any other size reads back 0x3c; unknown ids 0x800201D0)",
            "tests/native/hle_thread_selftest.c:test_wb_tlspl_vpl_cells",
        ],
        "limitation": "the recorded fields and sizes are reproduced; index is the pool's slot number, which matches the cells only while pools are created in slot order (#341)",
    },
    "h_ReferVplStatus": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/vpl/refer.expected (the name at +4; input size 0 is left alone)",
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: a 1024-byte VPL reports 992 bytes; each block costs roundup(size, 8) + 8)",
            "tests/native/hle_thread_selftest.c:test_wb_tlspl_vpl_cells",
            "tests/native/hle_thread_selftest.c:test_kernel_pool_bounds_measured",
        ],
        "limitation": "the name, size word, wait count, poolSize and freeSize are reproduced; a pool size that is not a multiple of 8 is rounded down here, which is unmeasured (#341)",
    },
    "h_AllocateVplCB": {
        "status": "partial",
        "evidence": [
            "PSPAutotests tests/threads/vpl/allocate.expected (0 bytes, -1 bytes and more than the pool 0x800201B7)",
            "tests/native/hle_thread_selftest.c:test_wb_tlspl_vpl_cells",
            "tests/native/hle_thread_selftest.c:test_vpl_blocking_waits",
        ],
        "limitation": "as sceKernelAllocateVpl, with callbacks serviced while waiting (#341)",
    },
    # ---- #2807: single imports a MEASURED oracle record publishes a result for ----
    "h_GetTlsAddr": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-SAFE-SCALAR-001: tls-addr-normal, tls-status-normal, tls-addr-high, tls-status-high, tls-addr-high-2, tls-thread)",
            "tests/native/hle_thread_selftest.c:test_psp_measured_safe_scalar_tls_20261117",
        ],
        "limitation": "this status covers the PSP-SAFE-SCALAR-001 TLS cells only: for a pool of four 0x100-byte blocks in partition 2 with NULL options, created without attribute 0x4000 and (twice) with it, a thread's first call returns a 0x100-aligned block and its second call the same address; the main thread's block lies 0x100 above the low bracket's head in the pool without the attribute, 0x400 below the high bracket's head in the first pool with it, and 0x800 below that head (0x400 below the first pool's block) in the second; a second thread's block is 0x100 above the main thread's in the pool without the attribute and in the first pool with it; and after the main thread's calls the status of each of the first two pools reads 3 of 4 blocks free; PSP-SAFE-SCALAR-001 recorded that on one PSP-3000 (system software 6.61, operator-declared) in one acceptance-eligible launch in the XMB context (boot 221, the probe built from 86279e852, 2026-11-17), and the selftest replays the same calls through sr_syscall in both partition layouts; the record stores distances and low bytes, never an address, and it does not measure the block's contents (the runtime zero-fills a block it hands out), an unknown or deleted pool, a call when every block is held (the runtime returns NULL and names it on stderr, #339; no wait for a block is modelled), another pool shape or partition, a third thread, or a call from interrupt context; those are not reviewed here (#2807)",
    },
    "h_WlanOn": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-SEM-001: wlan-state)",
            "tests/native/hle_thread_selftest.c:test_wlan_state_with_the_switch_on",
        ],
        "limitation": "this status covers the PSP-KERNEL-SEM-001 wlan-state cell only: sceWlanGetSwitchState (0xd7763699) and then sceWlanDevIsPowerOn (0x93440b11), the two NIDs this handler serves, each called with no argument, return 1 with the console's WLAN switch on; PSP-KERNEL-SEM-001 recorded that on one PSP-3000 (system software 6.61, ARK-5.1.0) in one acceptance-eligible launch in the game context (boot 84, the probe built from 0c3cf0da, 2026-10-31), and the selftest makes the same two calls in that order through sr_syscall with the default console profile, whose switch is on; the answer with the switch off, which the runtime takes for both calls from the console profile's wlan_switch setting, a switch moved during a run, the power state while a WLAN or network library is running, and the XMB context are not measured by the record and not reviewed here (#2807)",
    },
    "h_DcacheWritebackAll": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-CACHE-001: cache-wball)",
            "tests/native/hle_thread_selftest.c:test_dcache_writeback_all_cached_store_reaches_the_uncached_alias",
        ],
        "limitation": "this status covers the PSP-CACHE-001 cache-wball cell only: after 0xDEADBEEF is stored through the cached address of the second word of a 64-byte-aligned buffer, sceKernelDcacheWritebackAll() returns to its caller and the same word read through the uncached alias (the address with 0x40000000 set) is 0xDEADBEEF; PSP-CACHE-001 recorded that on one PSP-3000 (system software 6.61, ARK-5.1.0) in the 2026-10-10 campaign launch of f6ccfb33 and again in the acceptance-eligible 2026-10-15 requalification launch of f2d6c91a, and the selftest makes the same store, call and read through sr_syscall; the probe discards the call's return value, so the 0 the runtime returns is not measured; guest RAM is one coherent array here, so the alias reads the stored word whatever this call does, and the only handler behaviour the selftest can check is that the call comes back without a named stop; the renderer notice the handler sends, lines other than the probe's, and data the cache would have held stale are not measured and not reviewed here (#2807)",
    },
    "h_IoIoctl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-IO-SEEK-001: io-seek-ms-*, io-seek-umd-*)",
            "tests/native/hle_thread_selftest.c:test_io_memory_stick_cells_through_syscall",
            "tests/native/hle_thread_selftest.c:test_io_ioctl_disc0_seek_range_and_position",
        ],
        "limitation": "this status covers the sceIoIoctl cells of PSP-HLE-IO-SEEK-001 only: on a probe-owned 16-byte Memory Stick file opened read-write, the seek 0x01010005 (a 16-byte record: the offset, a reserved word of 0, SEEK_SET; no output) to -1, 8, 16 and 32 from a position of 0, and the queries 0x01020004, 0x01020003, 0x01020006 and 0x01020007 (no input, an 8-byte output) from a position of 8 and 0x01030008 (no input, a 4-byte output) from a position of 0, each return 0x80020325 and change neither the position, the output nor the file; on disc0:/UMD_DATA.BIN opened read-only, from a position of 8, the same seek to -1 and to the size + 1 returns 0x80010005 and to the size returns 0, and the position query 0x01020004 with an 8-byte output returns 0 and writes only the low out word; PSP-HLE-IO-SEEK-001 recorded that on one PSP-3000 (system software 6.6.0 as reported by pspver) in one acceptance-eligible launch in the XMB context (boot 156, the probe built from 59520eb0, 2026-11-08), and the selftest makes the same calls through sr_syscall (test_io_memory_stick_cells_through_syscall for the Memory Stick file, a synthetic 2048-byte disc file for disc0:); the record publishes neither the low out word's value nor the position after a disc0: seek, so the selftest's positions are runtime checks; the selftest also runs Memory Stick offsets and buffer sizes the probe did not, which the runtime refuses the same way, unmeasured; every other command (the PGD and DRM commands among them, PPSSPP-derived), any other whence, input or buffer size, other devices and files, and the position after a successful Memory Stick seek are not measured by this record and not reviewed here; sceIoIoctlAsync is a separate handler (#2807)",
    },
}

# Savedata, the message dialog, netconf, gamedata install and game sharing with no utility dialog
# started (#2807): the measured no-InitStart sequence of PSP-HLE-UTILITY-NONE-001 (GetStatus,
# Update(1), GetStatus, ShutdownStart, GetStatus, ShutdownStart, GetStatus), one selftest cell per
# kind that makes the same seven calls through sr_syscall. Each handler is reviewed for its own
# calls in it only. Each handler here is registered under its one NID only.
_UTILITY_NONE_KINDS = (
    # (kind, PSP-HLE-UTILITY-NONE-001 cell, selftest cell, GetStatus, Update, ShutdownStart handlers)
    ("savedata", "utility-none-savedata", "test_savedata_calls_with_no_dialog_started",
     "h_DlgGetStatus", "h_SavedataUpdate", "h_DlgShutdown"),
    ("message dialog", "utility-none-msgdialog", "test_msg_dialog_calls_with_no_dialog_started",
     "h_MsgDialogStatus", "h_MsgDialogUpdate", "h_MsgDialogShutdown"),
    ("netconf", "utility-none-netconf", "test_netconf_calls_with_no_dialog_started",
     "h_NetDialogStatus", "h_NetDialogUpdate", "h_NetDialogShutdown"),
    ("gamedata install", "utility-none-gamedata", "test_gamedata_install_calls_with_no_dialog_started",
     "h_GamedataDialogStatus", "h_GamedataDialogUpdate", "h_GamedataDialogShutdown"),
    ("game sharing", "utility-none-sharing", "test_game_sharing_calls_with_no_dialog_started",
     "h_SharingDialogStatus", "h_SharingDialogUpdate", "h_SharingDialogShutdown"),
)
_UTILITY_NONE_CALLS = (
    ("GetStatus", "its four calls in the sequence (the first call, and the calls after Update(1), "
     "after the first ShutdownStart and after the second) each return 0x80110005 (WRONG_TYPE)"),
    ("Update", "its one call in the sequence, Update(1) after the first GetStatus, returns 0x80110005 "
     "(WRONG_TYPE); an Update with any other argument is not in the record"),
    ("ShutdownStart", "its two calls in the sequence (after the second GetStatus, and again after the "
     "third) each return 0x80110005 (WRONG_TYPE)"),
)
for _kind, _oracle_cell, _selftest_cell, *_kind_handlers in _UTILITY_NONE_KINDS:
    for _handler, (_call, _measured) in zip(_kind_handlers, _UTILITY_NONE_CALLS, strict=True):
        HANDLER_METADATA[_handler] = {
            "status": "partial",
            "evidence": [
                f"docs/HARDWARE_ORACLE.md (PSP-HLE-UTILITY-NONE-001: {_oracle_cell})",
                f"tests/native/hle_thread_selftest.c:{_selftest_cell}",
            ],
            "limitation": (
                f"this {_kind} {_call} covers one measured sequence only: with no InitStart of any "
                f"utility dialog in the process, GetStatus, Update(1), GetStatus, ShutdownStart, "
                f"GetStatus, ShutdownStart and GetStatus are called in that order and {_measured}; "
                f"PSP-HLE-UTILITY-NONE-001 recorded that on one PSP-3000 (system software 6.61) in "
                f"one launch from the XMB context (boot 115), and the selftest reaches the state by a "
                f"synthetic reset, not a fresh process; the record measures nothing after an "
                f"InitStart, so every result the handler returns then (in the INIT, RUNNING, "
                f"FINISHED and SHUTDOWN states and at NONE after use) and their timing are unmeasured "
                f"by it and not reviewed here; it does not measure the game context either, in which "
                f"the runtime starts its programs, or the answer once another dialog kind holds the "
                f"slot, so both are unmeasured for this handler; the sequence's other calls belong to "
                f"separate handlers, reviewed on the same cell for their own calls only (#2807)"
            ),
        }

# Opt-in online ad hoc handlers (#1139): PPSSPP-derived, not PSP-measured; fail closed with
# 0x80010086 unless SR_ADHOC_ENABLED=1.
_ADHOC_ONLINE_HANDLERS = (
    "h_NetAdhocctlJoin",
    "h_NetAdhocctlGetAddrByName",
    "h_NetAdhocctlGetNameByAddr",
    "h_NetAdhocInit",
    "h_NetAdhocTerm",
    "h_NetAdhocPdpCreate",
    "h_NetAdhocPdpDelete",
    "h_NetAdhocPdpSend",
    "h_NetAdhocPdpRecv",
    "h_NetAdhocGetPdpStat",
    "h_NetAdhocPtpOpen",
    "h_NetAdhocPtpConnect",
    "h_NetAdhocPtpListen",
    "h_NetAdhocPtpAccept",
    "h_NetAdhocPtpSend",
    "h_NetAdhocPtpRecv",
    "h_NetAdhocPtpFlush",
    "h_NetAdhocPtpClose",
    "h_NetAdhocGetPtpStat",
    "h_NetAdhocPollSocket",
    "h_NetAdhocSetSocketAlert",
    "h_NetAdhocGetSocketAlert",
    "h_NetAdhocMatchingInit",
    "h_NetAdhocMatchingTerm",
    "h_NetAdhocMatchingCreate",
    "h_NetAdhocMatchingDelete",
    "h_NetAdhocMatchingStart",
    "h_NetAdhocMatchingStart2",
    "h_NetAdhocMatchingStop",
    "h_NetAdhocMatchingSelectTarget",
    "h_NetAdhocMatchingCancelTarget",
    "h_NetAdhocMatchingCancelTargetWithOpt",
    "h_NetAdhocMatchingSetHelloOpt",
    "h_NetAdhocMatchingGetHelloOpt",
    "h_NetAdhocMatchingGetMembers",
    "h_NetAdhocMatchingSendData",
    "h_NetAdhocMatchingAbortSendData",
    "h_NetAdhocMatchingGetPoolMaxAlloc",
    "h_NetAdhocMatchingGetPoolStat",
)
for _adhoc_handler in _ADHOC_ONLINE_HANDLERS:
    HANDLER_METADATA[_adhoc_handler] = {
        "status": "partial",
        "evidence": [
            "tests/native/hle_thread_selftest.c:test_adhoc_offline_refusal_stays_closed",
            "tests/native/hle_thread_selftest.c:test_adhoc_online_all_39_handlers_real_tcp",
        ],
        "limitation": (
            "PPSSPP-derived, not PSP-measured (derived-translated from PPSSPP rev "
            "3bc60270eae3069f13dfbfdb7292a7e1375de6f6); stays fail-closed (0x80010086) "
            "unless SR_ADHOC_ENABLED=1 (#1139)"
        ),
    }

# handler name -> status mapping. Preserved for direct consumers and gate checks.
HANDLER_STATUS: dict[str, str] = {h: meta["status"] for h, meta in HANDLER_METADATA.items()}

# handler name -> evidence list (for complete handlers)
HANDLER_EVIDENCE = {
    h: meta.get("evidence", [])
    for h, meta in HANDLER_METADATA.items()
    if isinstance(meta, dict) and "evidence" in meta
}

# handler name -> limitation string (for partial/compatibility handlers)
HANDLER_LIMITATIONS = {
    h: meta.get("limitation", "")
    for h, meta in HANDLER_METADATA.items()
    if isinstance(meta, dict) and "limitation" in meta
}

#: Partial or compatibility handlers whose status predates the rule that every such
#: status cites evidence (#341). The list may only shrink: `validate_meta` rejects a
#: partial/compatibility handler with no `evidence` that is not named here, and rejects
#: an entry here that has since gained evidence or left those statuses. Graduating one
#: means adding its evidence and deleting its line.
PARTIAL_WITHOUT_EVIDENCE = frozenset({
    "h_AudioSRCChRelease",
    "h_AudioSRCChReserve",
    "h_AudioSRCOutputBlocking",
    "h_CancelReceiveMbx",
    "h_CreateMbx",
    "h_CreateVTimer",
    "h_CtrlGetSamplingMode",
    "h_DeleteMbx",
    "h_DeleteVTimer",
    "h_DmacMemcpy",
    "h_DmacTryMemcpy",
    "h_FreeMemoryBlock",
    "h_GeListEnQueueHead",
    "h_GetMemoryBlockAddr",
    "h_GetSystemTime",
    "h_GetSystemTimeLow",
    "h_GetVTimerBase",
    "h_GetVTimerTime",
    "h_IoDevctl",
    "h_IoWaitAsyncCB",
    "h_Memcpy",
    "h_MpegChangeGetAuMode",
    "h_MpegQueryPcmEsSize",
    "h_NpDrmClearLicenseeKey",
    "h_NpDrmSetLicenseeKey",
    "h_PollMbx",
    "h_PsmfGetAudio",
    "h_PsmfGetVideo",
    "h_ReceiveMbx",
    "h_ReceiveMbxCB",
    "h_ReferProfilerNull",
    "h_ReferSemaStatus",
    "h_ReferSystemStatus",
    "h_ReferThreadStatus",
    "h_ReferVTimerStatus",
    "h_RegCloseCategory",
    "h_RegCloseRegistry",
    "h_RegCreateKey",
    "h_RegFlushCategory",
    "h_RegFlushRegistry",
    "h_RegGetKeyInfo",
    "h_RegGetKeyInfoByName",
    "h_RegGetKeyValue",
    "h_RegGetKeyValueByName",
    "h_RegGetKeys",
    "h_RegGetKeysNum",
    "h_RegOpenCategory",
    "h_RegOpenRegistry",
    "h_RegRemoveCategory",
    "h_RegSetKeyValue",
    "h_RtcGetWin32FileTime",
    "h_SendMbx",
    "h_StartVTimer",
    "h_StopVTimer",
    "h_SysClock2USec",
    "h_SysClock2USecWide",
    "h_USec2SysClock",
    "h_USec2SysClockWide",
    "h_UmdGetErrorStat",
})

# Alias-consistency rules: every static registration whose *registered name*
# starts with `name_prefix` must route to `required_handler`. This is how the
# gate rejects a firmware-variant NID that silently bypasses shared retained
# state (the sceKernelSetCompiledSdkVersion603_605 regression recorded on
# issue #71).
ALIAS_RULES = (
    {
        "name_prefix": "sceKernelSetCompiledSdkVersion",
        "required_handler": "h_SetCompiledSdkVersion",
        "why": "every SetCompiledSdkVersion firmware variant must update g_sdk_version",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/71",
    },
)

# Canonical names for NIDs with a history of being registered under a wrong
# or generic label. Source: current PPSSPP NID tables (public), mirrored by the
# tracked tools/nid_corpus.json and its generated src/rt/nid_names.h. A
# registration whose name disagrees with this map is a `mislabeled_nid`
# finding, so the fixed label cannot silently regress.
#
# tools/hle_manifest.py also cross-checks EVERY registration against
# src/rt/nid_names.h (the exhaustive NID->name integrity pass, issues
# #75/#78/#83/#86). Every entry below repeats its nid_names.h name on purpose:
# these are regression pins for the NIDs that were once mislabeled, kept
# independent of the generated table. A rename in tools/nid_corpus.json must
# change the matching pin here in the same commit, or the two checks disagree.
KNOWN_NID_NAMES = {
    0x1B4217BC: "sceKernelSetCompiledSdkVersion603_605",
    # --- sceSasCore routing integrity ---
    0x9EC3676A: "__sceSasSetADSRmode",
    0x33D4AB37: "__sceSasRevType",
    # --- issue #78 (sceReg routing) ---
    0x0CAE832B: "sceRegCloseCategory",
    0x1D8A762E: "sceRegOpenCategory",
    0x28A8E98A: "sceRegGetKeyValue",
    0x92E41280: "sceRegOpenRegistry",
    0xD4475AA8: "sceRegGetKeyInfo",
    0xFA8A5739: "sceRegCloseRegistry",
    # --- issue #83 (sceDisplay VBLANK routing) ---
    0x36CDFADE: "sceDisplayWaitVblank",
    # --- issue #86 (scePower routing) ---
    0x2085D15D: "scePowerGetBatteryLifePercent",
    0x0AFD0D8B: "scePowerIsBatteryExist",
    0x87440F5E: "scePowerIsPowerOnline",
    0xFDB5BFE9: "scePowerGetCpuClockFrequencyInt",
    0x478FE6F5: "scePowerGetBusClockFrequency",
}

# NIDs whose PSP ABI returns a single-precision float through $f0 (the MIPS
# float-return convention) while $v0 carries the integer status.  Routing one
# of these to a generic integer-return stub (h_ok family) reports success in
# $v0 but leaves $f0 stale/poisoned for the guest.  The gate rejects any
# registration whose handler is not proven to write the float return.
# Provenance: the #80 display-clock campaign recorded
# sceDisplayGetFramePerSec (0xDBA6C4C4) returning the measured 59.9400599f
# through $f0; the runtime implements it under the unified display clock.
FLOAT_RETURN_NIDS = {
    0xDBA6C4C4: {
        "name": "sceDisplayGetFramePerSec",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/80",
    },
    # scePower Float getters: the default clock values are measured on the PSP-3000 6.61 oracle
    # 2026-10-15 (PSP-HLE-POWER-001); the float-return shaping is tracked by #86.
    0xEA382A27: {
        "name": "scePowerGetPllClockFrequencyFloat",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    },
    0xB1A52C83: {
        "name": "scePowerGetCpuClockFrequencyFloat",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    },
    0x9BADB3EB: {
        "name": "scePowerGetBusClockFrequencyFloat",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    },
}

# Handlers proven to write the float return into $f0 before returning.  A
# float-return NID must route to one of these; anything else (including the
# generic integer-return h_ok) is a float_return_handler_mismatch finding.
FLOAT_RETURN_HANDLERS = {
    "h_DisplayGetFramePerSec",
    "h_PowerGetPllClockFrequencyFloat",
    "h_PowerGetCpuClockFrequencyFloat",
    "h_PowerGetBusClockFrequencyFloat",
}

# Acknowledged findings. The gate requires the live finding set to equal this
# waiver set exactly: a new finding fails CI, and a waiver whose finding no
# longer reproduces is stale and also fails CI (so fixes must retire their
# waiver in the same change). Never add a waiver without an issue link.
#
# Currently empty: the 0x1b4217bc alias_mismatch/mislabeled_nid waivers were
# retired when the 603_605 SDK-version variant was rerouted to
# h_SetCompiledSdkVersion under its canonical name (issue #71).
class FindingWaiver(TypedDict):
    nid: int
    finding: str
    handler: str
    issue: str


WAIVERS: tuple[FindingWaiver, ...] = ()

# Issue #341 acceptance: no handler the public profile-zero guest imports stays
# silently `unreviewed`. These are the NIDs the guest built from
# tests/fixtures/profile_zero/main.c imports today that still route to a dedicated
# `unreviewed` handler. The list only shrinks:
#   - tools/hle_manifest.py rejects an entry whose NID is no longer registered,
#     whose name disagrees with the registration, that does not name #341, or
#     whose handler is no longer `unreviewed` (graduating a handler retires its
#     entries in the same change);
#   - `tools/hle_manifest.py --imports <guest> --check-profile-zero-imports`, run
#     by the hosted public_export job on the digest-checked build, fails
#     on an imported unreviewed NID that is missing here and on an entry the
#     guest no longer imports.
# Never add an entry to make that gate pass: review the handler instead.
PROFILE_ZERO_ISSUE = "https://github.com/Jstar269/nakagawa-recomp/issues/341"
PROFILE_ZERO_UNREVIEWED_IMPORTS: dict[int, dict[str, str]] = {}

# Runtime subsystem that owns each registration (issue #307). The split of
# src/rt/hle.c into subsystem modules needs one obvious owner per registered
# NID before any handler moves; this map states it. A subsystem is an
# ownership grouping for the runtime source, never a support claim and not a
# claim about which PSP library exports the NID.
#
# Resolution (tools/hle_manifest.py subsystem_for): strip the registered
# name's leading underscores, then the LONGEST key below that prefixes it
# decides, so `sceAudiocodec` beats `sceAudio` and `sceKernelPower` claims
# the power NIDs before any kernel entry. The manifest fails closed: a
# registration no key matches, a key no registration resolves through, and a
# value outside SUBSYSTEMS are all errors. Keys are unique by construction;
# tools/test_hle_manifest.py also rejects a key written twice, which a Python
# dict literal would otherwise collapse silently to its last value.
#
# Kernel names lead with a verb (sceKernelCreateSema, sceKernelWaitSema), so
# no short prefix separates threads from semaphores; those keys are the
# verb-and-object stems, and each also covers its CB, Wide, Low and firmware
# variants (sceKernelLockLwMutexCB, sceKernelTryLockLwMutex_600).
SUBSYSTEMS = frozenset({
    "audio",
    "audio_codec",
    "cache_dma",
    "display",
    "font",
    "ge",
    "input",
    "io",
    "kernel_interrupt",
    "kernel_memory",
    "kernel_module",
    "kernel_sync",
    "kernel_thread",
    "kernel_time",
    "kernel_utils",
    "mpeg",
    "network",
    "npdrm",
    "power",
    "rtc",
    "system",
    "umd",
    "utility",
    "videocodec",
})

SUBSYSTEM_BY_PREFIX = {
    # --- audio output and the SAS mixer ---
    "sceAudio": "audio",
    "sceSas": "audio",
    "sceVaudio": "audio",
    # --- compressed-audio decoders ---
    "sceAtrac": "audio_codec",
    "sceMp3": "audio_codec",
    # --- CPU caches and DMA copies ---
    "sceDmac": "cache_dma",
    "sceKernelDcache": "cache_dma",
    "sceKernelIcache": "cache_dma",
    # --- display, graphics engine, input ---
    "sceDisplay": "display",
    "sceGe": "ge",
    "sceCtrl": "input",
    "sceHprm": "input",
    # --- fonts ---
    "sceFont": "font",
    # --- file I/O ---
    "sceIo": "io",
    # --- video and stream demux ---
    "sceMpeg": "mpeg",
    "scePsmf": "mpeg",
    # --- the Media Engine video boundary under a disc mpeg.prx (#286) ---
    "sceMpegBase": "videocodec",
    "sceMpegbase": "videocodec",
    "sceVideocodec": "videocodec",
    # sceAudiocodec is the Media Engine's audio codec, served beside the video one (#286).
    "sceAudiocodec": "videocodec",
    # --- network (fail-closed stubs) ---
    "sceHttp": "network",
    "sceNet": "network",
    "sceSsl": "network",
    "sceWlan": "network",
    # --- NP DRM keys ---
    "sceNpDrm": "npdrm",
    # --- power, UMD, clock ---
    "sceKernelPower": "power",
    "scePower": "power",
    "sceUmd": "umd",
    "sceRtc": "rtc",
    # --- system registry and console identity ---
    "sceOpenPSID": "system",
    "sceReg": "system",
    # --- utility dialogs and impose ---
    "sceImpose": "utility",
    "sceUtility": "utility",
    # --- kernel: interrupts ---
    "sceKernelCpuResumeIntr": "kernel_interrupt",
    "sceKernelCpuSuspendIntr": "kernel_interrupt",
    "sceKernelDisableSubIntr": "kernel_interrupt",
    "sceKernelEnableSubIntr": "kernel_interrupt",
    "sceKernelIsCpuIntrEnable": "kernel_interrupt",
    "sceKernelIsCpuIntrSuspended": "kernel_interrupt",
    "sceKernelRegisterSubIntrHandler": "kernel_interrupt",
    "sceKernelReleaseSubIntrHandler": "kernel_interrupt",
    # --- kernel: memory partitions, pools, TLS, SDK version ---
    "sceKernelAllocMemoryBlock": "kernel_memory",
    "sceKernelAllocPartitionMemory": "kernel_memory",
    "sceKernelAllocateFpl": "kernel_memory",
    "sceKernelAllocateVpl": "kernel_memory",
    "sceKernelCreateFpl": "kernel_memory",
    "sceKernelCreateTlspl": "kernel_memory",
    "sceKernelCreateVpl": "kernel_memory",
    "sceKernelDeleteFpl": "kernel_memory",
    "sceKernelDeleteTlspl": "kernel_memory",
    "sceKernelDeleteVpl": "kernel_memory",
    "sceKernelDevkitVersion": "kernel_memory",
    "sceKernelFreeFpl": "kernel_memory",
    "sceKernelFreeMemoryBlock": "kernel_memory",
    "sceKernelFreePartitionMemory": "kernel_memory",
    "sceKernelFreeTlspl": "kernel_memory",
    "sceKernelFreeVpl": "kernel_memory",
    "sceKernelGetBlockHeadAddr": "kernel_memory",
    "sceKernelGetMemoryBlockAddr": "kernel_memory",
    "sceKernelGetTlsAddr": "kernel_memory",
    "sceKernelMaxFreeMemSize": "kernel_memory",
    "sceKernelReferFplStatus": "kernel_memory",
    "sceKernelReferTlsplStatus": "kernel_memory",
    "sceKernelReferVplStatus": "kernel_memory",
    "sceKernelSetCompiledSdkVersion": "kernel_memory",
    "sceKernelSetCompilerVersion": "kernel_memory",
    "sceKernelTotalFreeMemSize": "kernel_memory",
    "sceKernelTryAllocateFpl": "kernel_memory",
    "sceKernelTryAllocateVpl": "kernel_memory",
    "sceKernelVolatileMem": "kernel_memory",
    # --- kernel: module loading and process exit ---
    "sceKernelExitGame": "kernel_module",
    "LoadExecForUser_362A956B": "kernel_module",
    "sceKernelGetModuleId": "kernel_module",
    "sceKernelLoadExec": "kernel_module",
    "sceKernelLoadModule": "kernel_module",
    "sceKernelQueryModuleInfo": "kernel_module",
    "sceKernelRegisterExitCallback": "kernel_module",
    "sceKernelSelfStopUnloadModule": "kernel_module",
    "sceKernelStartModule": "kernel_module",
    "sceKernelStopModule": "kernel_module",
    "sceKernelStopUnloadSelfModuleWithStatus": "kernel_module",
    "sceKernelUnloadModule": "kernel_module",
    # --- kernel: semaphores, event flags, mutexes, message boxes and pipes ---
    "sceKernelCancelEventFlag": "kernel_sync",
    "sceKernelCancelMsgPipe": "kernel_sync",
    "sceKernelCancelMutex": "kernel_sync",
    "sceKernelCancelReceiveMbx": "kernel_sync",
    "sceKernelCancelSema": "kernel_sync",
    "sceKernelClearEventFlag": "kernel_sync",
    "sceKernelCreateEventFlag": "kernel_sync",
    "sceKernelCreateLwMutex": "kernel_sync",
    "sceKernelCreateMbx": "kernel_sync",
    "sceKernelCreateMsgPipe": "kernel_sync",
    "sceKernelCreateMutex": "kernel_sync",
    "sceKernelCreateSema": "kernel_sync",
    "sceKernelDeleteEventFlag": "kernel_sync",
    "sceKernelDeleteLwMutex": "kernel_sync",
    "sceKernelDeleteMbx": "kernel_sync",
    "sceKernelDeleteMsgPipe": "kernel_sync",
    "sceKernelDeleteMutex": "kernel_sync",
    "sceKernelDeleteSema": "kernel_sync",
    "sceKernelLockLwMutex": "kernel_sync",
    "sceKernelLockMutex": "kernel_sync",
    "sceKernelPollEventFlag": "kernel_sync",
    "sceKernelPollMbx": "kernel_sync",
    "sceKernelPollSema": "kernel_sync",
    "sceKernelReceiveMbx": "kernel_sync",
    "sceKernelReceiveMsgPipe": "kernel_sync",
    "sceKernelReferEventFlagStatus": "kernel_sync",
    "sceKernelReferLwMutexStatus": "kernel_sync",
    "sceKernelReferMbxStatus": "kernel_sync",
    "sceKernelReferMsgPipeStatus": "kernel_sync",
    "sceKernelReferMutexStatus": "kernel_sync",
    "sceKernelReferSemaStatus": "kernel_sync",
    "sceKernelSendMbx": "kernel_sync",
    "sceKernelSendMsgPipe": "kernel_sync",
    "sceKernelSetEventFlag": "kernel_sync",
    "sceKernelSignalSema": "kernel_sync",
    "sceKernelTryLockLwMutex": "kernel_sync",
    "sceKernelTryLockMutex": "kernel_sync",
    "sceKernelTryReceiveMsgPipe": "kernel_sync",
    "sceKernelTrySendMsgPipe": "kernel_sync",
    "sceKernelUnlockLwMutex": "kernel_sync",
    "sceKernelUnlockMutex": "kernel_sync",
    "sceKernelWaitEventFlag": "kernel_sync",
    "sceKernelWaitSema": "kernel_sync",
    # --- kernel: threads, callbacks, delays, dispatch, profilers ---
    "sceKernelCancelCallback": "kernel_thread",
    "sceKernelCancelWakeupThread": "kernel_thread",
    "sceKernelChangeCurrentThreadAttr": "kernel_thread",
    "sceKernelChangeThreadPriority": "kernel_thread",
    "sceKernelCheckCallback": "kernel_thread",
    "sceKernelCreateCallback": "kernel_thread",
    "sceKernelCreateNotifyCallback": "kernel_thread",
    "sceKernelCreateThread": "kernel_thread",
    "sceKernelDelaySysClockThread": "kernel_thread",
    "sceKernelDelayThread": "kernel_thread",
    "sceKernelDeleteCallback": "kernel_thread",
    "sceKernelDeleteNotifyCallback": "kernel_thread",
    "sceKernelDeleteThread": "kernel_thread",
    "sceKernelExitDeleteThread": "kernel_thread",
    "sceKernelExitThread": "kernel_thread",
    "sceKernelGetCallbackCount": "kernel_thread",
    "sceKernelGetThreadCurrentPriority": "kernel_thread",
    "sceKernelGetThreadExitStatus": "kernel_thread",
    "sceKernelGetThreadId": "kernel_thread",
    "sceKernelGetThreadStackFreeSize": "kernel_thread",
    "sceKernelCheckThreadStack": "kernel_thread",
    "sceKernelGetThreadmanIdList": "kernel_thread",
    "sceKernelNotifyCallback": "kernel_thread",
    "sceKernelReferCallbackStatus": "kernel_thread",
    "sceKernelReferGlobalProfiler": "kernel_thread",
    "sceKernelReferSystemStatus": "kernel_thread",
    "sceKernelReferThreadProfiler": "kernel_thread",
    "sceKernelReferThreadRunStatus": "kernel_thread",
    "sceKernelReferThreadStatus": "kernel_thread",
    "sceKernelReleaseWaitThread": "kernel_thread",
    "sceKernelResumeDispatchThread": "kernel_thread",
    "sceKernelResumeThread": "kernel_thread",
    "sceKernelRotateThreadReadyQueue": "kernel_thread",
    "sceKernelSleepThread": "kernel_thread",
    "sceKernelStartThread": "kernel_thread",
    "sceKernelSuspendDispatchThread": "kernel_thread",
    "sceKernelSuspendThread": "kernel_thread",
    "sceKernelTerminateDeleteThread": "kernel_thread",
    "sceKernelTerminateThread": "kernel_thread",
    "sceKernelWaitThreadEnd": "kernel_thread",
    "sceKernelWakeupThread": "kernel_thread",
    # --- kernel: system time, alarms, virtual timers, libc clock ---
    "sceKernelCancelAlarm": "kernel_time",
    "sceKernelCancelVTimerHandler": "kernel_time",
    "sceKernelCreateVTimer": "kernel_time",
    "sceKernelDeleteVTimer": "kernel_time",
    "sceKernelGetSystemTime": "kernel_time",
    "sceKernelGetVTimerBase": "kernel_time",
    "sceKernelGetVTimerTime": "kernel_time",
    "sceKernelLibcClock": "kernel_time",
    "sceKernelLibcGettimeofday": "kernel_time",
    "sceKernelLibcTime": "kernel_time",
    "sceKernelReferVTimerStatus": "kernel_time",
    "sceKernelSetAlarm": "kernel_time",
    "sceKernelSetVTimerHandler": "kernel_time",
    "sceKernelSetVTimerTime": "kernel_time",
    "sceKernelStartVTimer": "kernel_time",
    "sceKernelStopVTimer": "kernel_time",
    "sceKernelSysClock2USec": "kernel_time",
    "sceKernelUSec2SysClock": "kernel_time",
    # --- kernel: stdio, debug output, GPIO, libc memory/string helpers ---
    "memset": "kernel_utils",
    "newlibModuleStreamWrite": "kernel_utils",
    "printf": "kernel_utils",
    "snprintf": "kernel_utils",
    "sceKernelGetGPI": "kernel_utils",
    "sceKernelMemcpy": "kernel_utils",
    "sceKernelMemset": "kernel_utils",
    "sceKernelPrintf": "kernel_utils",
    "sceKernelSetGPO": "kernel_utils",
    "sceKernelStderr": "kernel_utils",
    "sceKernelStdin": "kernel_utils",
    "sceKernelStdout": "kernel_utils",
    "strlen": "kernel_utils",
}
