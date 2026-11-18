# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
#requires -Version 7.6

<#
.SYNOPSIS
    GPU selftest status parsing and gate-outcome mapping for tools/nk_manager.ps1.
.DESCRIPTION
    `make gpu-selftest-status` runs each GPU selftest binary and prints one
    structured line per binary:

        GPU_SELFTEST_RESULT <binary> PASS|SKIP|FAIL <exit-code>

    A structured line is required because a failed recipe always leaves GNU Make
    with its own generic exit status, so a child SKIP (exit 77: Vulkan or the
    validation layer unavailable) cannot travel out through the Make exit code
    and would otherwise be reported as a failure. These helpers turn that output
    into per-gate verdicts plus a separate evidence-completeness fact, so an
    executed PASS never implies that required graphics evidence exists.

    Capability detection lives in the Makefile, not here: shader reproducibility
    (glslc) is a separate concern from whether these binaries can be built and
    run, and treating it as a prerequisite caused false skips.
#>

function Get-GpuSelftestResults {
    <#
    .SYNOPSIS
        Parse `GPU_SELFTEST_RESULT` lines out of a Make log.
    .OUTPUTS
        Hashtable of binary name -> @{ Verdict = "PASS"|"SKIP"|"FAIL"; Code = <int> }
    #>
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Log)

    $results = @{}
    foreach ($line in ($Log -split "`r?`n")) {
        if ($line -match '^\s*GPU_SELFTEST_RESULT\s+(\S+)\s+(PASS|SKIP|FAIL)\s+(\d+)\s*$') {
            $results[$Matches[1]] = @{
                Verdict = $Matches[2]
                Code    = [int]$Matches[3]
            }
        }
    }
    return $results
}

function Get-GpuSelftestBinaryName {
    param([Parameter(Mandatory = $true)][string]$Gate)
    return ($Gate -replace '-', '_') + ".exe"
}

# The terminal-fault legs `make gpu-capture-selftest` runs after its default run
# (GPU_CAPTURE_TERMINAL_CASES in mk/selftests.mk). `make gpu-selftest-status`
# reports each as `gpu_capture_selftest.exe:<case>`.
$script:GpuSelftestGateLegs = @{
    "gpu-capture-selftest" = @("surface-lost", "unclassified", "device-lost")
}

function Get-GpuSelftestResultNames {
    <#
    .SYNOPSIS
        The result-line names one gate needs: its binary's default run plus
        every leg the gate's Make target runs.
    #>
    param([Parameter(Mandatory = $true)][string]$Gate)
    $binary = Get-GpuSelftestBinaryName -Gate $Gate
    $names = @($binary)
    if ($script:GpuSelftestGateLegs.ContainsKey($Gate)) {
        foreach ($leg in $script:GpuSelftestGateLegs[$Gate]) { $names += "${binary}:$leg" }
    }
    return $names
}

function Resolve-GpuSelftestStatus {
    <#
    .SYNOPSIS
        Map parsed GPU selftest results onto manager gate outcomes.
    .DESCRIPTION
        A binary or leg with no result line never ran, so its gate is FAIL (a
        build or launch failure) even when the log text happens to contain the
        word "skip". gpu-capture-selftest also needs its terminal-fault legs
        (Get-GpuSelftestResultNames). A SKIP keeps its reason and never counts
        as a failure. The
        returned Completeness value is the evidence fact: EXECUTED when every
        binary reported a verdict, PARTIAL_SKIPPED when some did, and
        NOT_RUN_HOST_UNSUPPORTED when every binary skipped, so callers can report
        graphics evidence as optional-but-absent instead of silently complete.
    .OUTPUTS
        PSCustomObject with Gates (ordered list) and Completeness.
    #>
    param(
        [Parameter(Mandatory = $true)][hashtable]$Results,
        [Parameter(Mandatory = $true)][string[]]$Gates,
        [int]$ExitCode = 0
    )

    $outcomes = @()
    $skipped = 0
    $failedGates = 0
    foreach ($gate in $Gates) {
        # A gate passes only when its default run and every leg its Make target
        # runs reported PASS; a missing leg never ran, so it is a failure.
        $names = @(Get-GpuSelftestResultNames -Gate $gate)
        $missing = @($names | Where-Object { -not $Results.ContainsKey($_) })
        $failing = @($names | Where-Object {
            $Results.ContainsKey($_) -and $Results[$_].Verdict -eq "FAIL" })
        $skipping = @($names | Where-Object {
            $Results.ContainsKey($_) -and $Results[$_].Verdict -eq "SKIP" })
        if ($missing.Count -gt 0) {
            $failedGates++
            $outcomes += [pscustomobject]@{
                Gate    = $gate
                Status  = "FAIL"
                Reason  = "no GPU_SELFTEST_RESULT for $($missing -join ', '); the binary was not built or not run (make exit $ExitCode)"
            }
        } elseif ($failing.Count -gt 0) {
            $failedGates++
            $detail = @($failing | ForEach-Object { "$_ exit $($Results[$_].Code)" }) -join ", "
            $outcomes += [pscustomobject]@{
                Gate   = $gate
                Status = "FAIL"
                Reason = "child $($detail): GPU selftest reported a real failure"
            }
        } elseif ($skipping.Count -gt 0) {
            $skipped++
            $detail = @($skipping | ForEach-Object { "$_ exit $($Results[$_].Code)" }) -join ", "
            $outcomes += [pscustomobject]@{
                Gate   = $gate
                Status = "SKIP"
                Reason = "child $($detail): graphics evidence was not produced on this host"
            }
        } else {
            $outcomes += [pscustomobject]@{ Gate = $gate; Status = "PASS"; Reason = "child exit 0" }
        }
    }

    $completeness = if ($Gates.Count -gt 0 -and $skipped -eq $Gates.Count) {
        "NOT_RUN_HOST_UNSUPPORTED"
    } elseif ($skipped -gt 0) {
        "PARTIAL_SKIPPED"
    } elseif ($ExitCode -ne 0 -or $failedGates -gt 0) {
        "FAILED"
    } else {
        "EXECUTED"
    }

    return [pscustomobject]@{ Gates = $outcomes; Completeness = $completeness }
}
