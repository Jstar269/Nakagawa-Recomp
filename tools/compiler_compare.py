#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compare GCC and Clang on a public synthetic generated-C corpus.

The source-owned semantic gates are a hard veto. No timing or size is attached
to a compiler/linker pair unless all gates pass first. Measurements rebuild and
run the synthetic smoke and cosimulation corpus at least three times, then run
the existing #282 runtime workload matrix once with the same compiler/linker
settings. Callers must run this program through their machine RAM lock; it does
not acquire one. Compiler defaults remain untouched. lld is an independently
selected factor. Runtime workload rows run sequentially, with a per-row
BUILD_ROOT for targets that honor it. The platform-ladder workloads use fixed
trees under build/platform-ladder and remain shared across toolchain rows.
"""

from __future__ import annotations

import argparse
from collections import deque
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import difflib
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = "nakagawa-compiler-compare-v2"
PERF_SCHEMA = "nakagawa-perf-benchmark-v1"
ISSUE = 317
DEFAULT_COMPILERS = ("gcc", "clang")
DEFAULT_LINKERS = ("default", "lld")
MIN_REPEATS = 3
CONTROL_FLAGS = ("-fno-fast-math", "-fno-associative-math", "-ffp-contract=off")
GCC_ONLY_TRACKING_FLAG = "-ftrack-macro-expansion=0"
MEASUREMENT_TARGETS = ("production-smoke", "production-smoke-gap", "display-smoke-run", "cosim-selftest")
MEASUREMENT_NAMES = ("generated_c_compile_wall_seconds", "link_wall_seconds",
                     "peak_working_set_bytes", "object_bytes", "executable_bytes")
_PINNED_ASSIGNMENT_VALUES = {
    "RUNTIME_OPT": "-O0",
    "RECOMP_OPT": "-O0",
    "TRACE": "0",
    "PERF_AOT_INSTRUCTIONS": "0",
    "CODEGEN_BACKEND": "c",
    "CODEGEN_USER_ARGS": "",
    "COSIM_CODEGEN_ARGS": "--stack-census",
    "PUBLIC_SAFE": "1",
    "FUNCS_PER_CHUNK": "1",
    "CHUNK_TARGET_BYTES": "",
    "NAN_TRAP": "0",
    "GAME_NAME": "compiler_compare",
    "GAME_BASE": "0",
    "GAME_ENTRY": "0",
    "GAME_ELF": "",
    "GAME_PSP_HEADER": "",
    "GAME_EXTRA_ELFS": "",
    "TITLE_MANIFEST": "",
    "TITLE_EXTRA_SPANS": "",
    "CODEGEN_PROFILE_ARG": "",
    "SYMBOL_MAP": "",
    "NAME_IMPORTS": "0",
    "ENHANCEMENT_PACKAGE": "",
    "ENHANCEMENT_DATA_ROOT": "",
    "ENHANCEMENT_CODE_HOOK_PACKAGE": "",
    "CODEGEN_TOOL": "tools/codegen.py",
}
_PINNED_ASSIGNMENT_GROUPS = {
    "runtime": ("RUNTIME_OPT", "RECOMP_OPT", "TRACE"),
    "base_codegen": ("PERF_AOT_INSTRUCTIONS", "CODEGEN_BACKEND", "CODEGEN_USER_ARGS",
                     "COSIM_CODEGEN_ARGS", "PUBLIC_SAFE"),
    "measurement_codegen": ("CODEGEN_BACKEND", "CODEGEN_USER_ARGS", "PERF_AOT_INSTRUCTIONS",
                            "COSIM_CODEGEN_ARGS", "PUBLIC_SAFE"),
    "title": ("GAME_NAME", "GAME_BASE", "GAME_ENTRY", "GAME_ELF", "GAME_PSP_HEADER",
              "GAME_EXTRA_ELFS", "TITLE_MANIFEST", "TITLE_EXTRA_SPANS",
              "CODEGEN_PROFILE_ARG", "SYMBOL_MAP", "NAME_IMPORTS", "ENHANCEMENT_PACKAGE",
              "ENHANCEMENT_DATA_ROOT", "ENHANCEMENT_CODE_HOOK_PACKAGE"),
    "base_tail": ("NAN_TRAP", "FUNCS_PER_CHUNK", "CHUNK_TARGET_BYTES"),
    "measurement_tail": ("FUNCS_PER_CHUNK", "CHUNK_TARGET_BYTES", "NAN_TRAP"),
    "codegen_tool": ("CODEGEN_TOOL",),
}
STATUS_MEASURED = "MEASURED"
#: Passed the veto and the workload matrix, but its semantic hash changed between repeats: the
#: generated C's output is not deterministic under this toolchain, so the row fails the run.
STATUS_MEASURED_HASH_UNSTABLE = "MEASURED_HASH_UNSTABLE"
STATUS_FAILED_VETO = "FAILED_VETO"
STATUS_FAILED_MEASUREMENT = "FAILED_MEASUREMENT"
STATUS_WORKLOAD_FAILED = "WORKLOAD_FAILED"
STATUS_NOT_AVAILABLE = "NOT_AVAILABLE"
STATUSES = (STATUS_MEASURED, STATUS_MEASURED_HASH_UNSTABLE, STATUS_FAILED_VETO, STATUS_FAILED_MEASUREMENT,
            STATUS_WORKLOAD_FAILED, STATUS_NOT_AVAILABLE)
#: Rows whose measurements are complete (the veto and the workload matrix passed).
MEASURED_STATUSES = (STATUS_MEASURED, STATUS_MEASURED_HASH_UNSTABLE)
GATE_PASS = "PASS"
GATE_FAIL = "FAIL"
GATE_SKIP = "SKIP"
OUTPUT_TAIL = 8000
OUTPUT_HEAD = 4000
SAMPLE_INTERVAL_SECONDS = 0.02
COMMAND_TIMEOUT_SECONDS = 30 * 60
GENERATED_C_NAME = re.compile(r"_recomp(?:_|\.|$)", re.IGNORECASE)
SETTING_LINE = re.compile(r"^([A-Z][A-Z0-9_]*)=(.*)$")
FORBIDDEN_FP_FLAGS = ("-ffast-math", "-fassociative-math", "-funsafe-math-optimizations", "-ofast")
OPTIMIZATION_FLAG = re.compile(r"^-O(?:[0-3sgz]|fast)?$", re.IGNORECASE)
CLEAR_BUILD_ENV = frozenset({
    "CC", "CXX", "AR", "LD", "AS", "CPPFLAGS", "CFLAGS", "CXXFLAGS", "GE_CFLAGS",
    "RECOMP_FLAGS", "RECOMP_OPT", "RUNTIME_OPT", "LDFLAGS", "LIBS", "MAKEFLAGS",
    "MFLAGS", "GNUMAKEFLAGS", "MAKEOVERRIDES", "MAKELEVEL", "SHELL",
    "BUILD_ROOT", "BUILD_DIR", "LOG_DIR", "PUBLIC_SAFE", "FUNCS_PER_CHUNK",
    "CHUNK_TARGET_BYTES", "SOURCE_DATE_EPOCH",
    "GE_FRONTEND_PROFILE", "PERF_AOT_INSTRUCTIONS", "RECOMP_CC_IS_CLANG",
    "PYTHONPATH", "PYTHONHOME", "PYTHONUSERBASE",
})
CLEAR_BUILD_PREFIXES = ("GAME_", "TITLE_", "CODEGEN_", "SYMBOL_MAP", "NAME_IMPORTS",
                        "ENHANCEMENT_", "NAN_TRAP", "TRACE", "SR_", "NK_", "SDL3_", "COSIM_")
TIMING_KEYS = frozenset({
    "generated_c_compile_wall_seconds", "link_wall_seconds", "peak_working_set_bytes",
    "duration_s", "build_seconds", "disabled_median_s", "enabled_median_s", "ratio",
    "object_bytes", "executable_bytes", "min", "median", "max",
})


class CompareError(ValueError):
    """A comparison input, build, measurement or report violated its contract."""


def _pinned_assignments(*groups: str) -> list[str]:
    """The comparison's shared Make pins, in the requested command-line order."""
    return [f"{name}={_PINNED_ASSIGNMENT_VALUES[name]}"
            for group in groups for name in _PINNED_ASSIGNMENT_GROUPS[group]]


@dataclass(frozen=True)
class Outcome:
    returncode: int | None
    output: str
    duration_s: float


def _diagnostic_excerpt(output: str) -> str:
    if len(output) <= OUTPUT_HEAD + OUTPUT_TAIL:
        return output
    return output[:OUTPUT_HEAD] + "\n... output middle truncated ...\n" + output[-OUTPUT_TAIL:]


def _first_error(output: str) -> str | None:
    patterns = re.compile(
        r"(?i:fatal error:|error:|undefined reference to|cannot find\b|status=FAIL\b|"
        r"make(?:\.exe)?: \*\*\*)|\b(?<!Clay )Error \d+\b",
    )
    for line in output.splitlines():
        if patterns.search(line):
            return line.strip()
    return next((line.strip() for line in output.splitlines() if line.strip()), None)


def _run(command: list[str], *, env: dict[str, str], cwd: Path = ROOT) -> Outcome:
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command, cwd=cwd, env=env, check=False, text=True, errors="replace",
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=COMMAND_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout if exc.stdout is not None else exc.output
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", "replace")
        detail = f"{command[0]} timed out after {COMMAND_TIMEOUT_SECONDS} seconds"
        if partial:
            detail += f"\n{partial}"
        return Outcome(None, detail, time.perf_counter() - started)
    except OSError as exc:
        return Outcome(None, f"{command[0]} could not be executed: {exc}", time.perf_counter() - started)
    return Outcome(completed.returncode, completed.stdout, time.perf_counter() - started)


def _full_version(command: list[str], *, env: dict[str, str]) -> str | None:
    outcome = _run(command, env=env)
    if outcome.returncode != 0 or not outcome.output.strip():
        return None
    return outcome.output.strip()


def _make_path(path: str) -> str:
    """Pass Windows tool paths to MSYS Make with forward slashes."""
    return path.replace("\\", "/")


def _make_build_path(path: Path) -> str:
    """Use short repository-relative Make paths for outputs beneath build/."""
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(ROOT.resolve())
    except ValueError:
        return _make_path(str(resolved))
    if relative.parts and relative.parts[0].casefold() == "build":
        return relative.as_posix()
    return _make_path(str(resolved))


def _toolchain_environment(make: str, python: str) -> dict[str, str]:
    env = os.environ.copy()
    for key in tuple(env):
        if key in CLEAR_BUILD_ENV or key.startswith(CLEAR_BUILD_PREFIXES):
            env.pop(key, None)
    env["MAKE"] = _make_path(make)
    env["PYTHON"] = python
    return env


def _resolve_linker(compiler_path: str, selection: str, *, env: dict[str, str]) -> tuple[str | None, str | None]:
    if selection == "lld":
        path = shutil.which("ld.lld") or shutil.which("ld.lld.exe")
        if path is None:
            return None, None
        return path, _full_version([path, "--version"], env=env)
    outcome = _run([compiler_path, "-print-prog-name=ld"], env=env)
    if outcome.returncode != 0 or not outcome.output.strip():
        return None, None
    name = outcome.output.strip().splitlines()[0].strip()
    candidate = Path(name)
    if candidate.is_file():
        path = str(candidate.resolve())
    else:
        path = shutil.which(name)
        if path is None and not Path(name).suffix:
            path = shutil.which(f"{name}.exe")
        if path is None:
            adjacent = Path(compiler_path).resolve().parent / name
            if adjacent.is_file():
                path = str(adjacent)
            elif not Path(name).suffix and adjacent.with_suffix(".exe").is_file():
                path = str(adjacent.with_suffix(".exe"))
    if path is None:
        return None, None
    return path, _full_version([path, "--version"], env=env)


def _parse_settings(output: str) -> dict[str, str]:
    settings: dict[str, str] = {}
    for line in output.splitlines():
        match = SETTING_LINE.match(line.strip())
        if match:
            settings[match.group(1)] = match.group(2).strip()
    return settings


def _fp_flags_ok(settings: dict[str, str]) -> None:
    for key in ("CFLAGS", "GE_CFLAGS", "RECOMP_FLAGS", "LDFLAGS"):
        words = shlex.split(settings.get(key, ""), posix=True)
        bad = [word for word in words if word.lower() in FORBIDDEN_FP_FLAGS]
        if bad:
            raise CompareError(f"{key} contains forbidden fast-math/reassociation flags: {bad}")


def _remove_one_flag(value: str, flag: str) -> list[str]:
    words = shlex.split(value, posix=True)
    for index in range(len(words) - 1, -1, -1):
        if words[index] == flag:
            del words[index]
            break
    return words


def _make_owned_flags(value: str, *, remove_perf: bool = False) -> str:
    words = _remove_one_flag(value, "-DSR_FLIGHT_RECORDER_LINKED")
    if remove_perf:
        for index in range(len(words) - 1, -1, -1):
            if words[index].startswith("-DPERF_AOT_INSTRUCTIONS="):
                del words[index]
                break
    return shlex.join(words)


def _comparison_recomp_flags(value: str) -> str:
    words = shlex.split(_make_owned_flags(value), posix=True)
    return shlex.join([word for word in words if word != GCC_ONLY_TRACKING_FLAG])


def _set_optimization_level(value: str, level: str) -> str:
    words = shlex.split(value, posix=True)
    words = [word for word in words if not OPTIMIZATION_FLAG.fullmatch(word)]
    words.append(level)
    return shlex.join(words)


def _check_ucrt_paths(compiler_path: str, linker_path: str, settings: dict[str, str]) -> None:
    if settings.get("SDL3_PROVIDER") != "msys2_ucrt64":
        return
    root = Path(settings.get("SDL3_DIR", "")).resolve()
    for label, path in (("compiler", compiler_path), ("linker", linker_path)):
        resolved = Path(path).resolve()
        if not resolved.is_relative_to(root):
            raise CompareError(
                f"{label} {path} is outside the MSYS2 UCRT64 toolchain at {root}"
            )


def _build_settings(make: str, compiler_path: str, linker: str, build_root: Path, *,
                    env: dict[str, str]) -> dict[str, str]:
    """Read and freeze the effective Make flags used by veto and measurement builds."""
    info_root = _make_build_path(build_root / "info")
    info_build_dir = _make_build_path(build_root / "info" / "compiler_compare")
    info_log_dir = _make_build_path(build_root / "info" / "logs")
    base_command = [
        make, "--no-print-directory", "--silent", f"CC={_make_path(compiler_path)}",
        f"BUILD_ROOT={info_root}", f"BUILD_DIR={info_build_dir}", f"LOG_DIR={info_log_dir}",
        *_pinned_assignments("runtime", "base_codegen", "title", "base_tail", "codegen_tool"),
        "compiler-info",
    ]
    base = _run(base_command, env=env)
    if base.returncode != 0:
        raise CompareError(f"compiler-info failed for {compiler_path}: {_diagnostic_excerpt(base.output)}")
    settings = _parse_settings(base.output)
    if not settings.get("CFLAGS"):
        raise CompareError(f"compiler-info did not report CFLAGS for {compiler_path}")
    _fp_flags_ok(settings)

    cflags = shlex.join([
        *shlex.split(_set_optimization_level(
            _make_owned_flags(settings["CFLAGS"], remove_perf=True), "-O0",
        ), posix=True),
        *CONTROL_FLAGS,
    ])
    recomp_flags = shlex.join([
        *shlex.split(_set_optimization_level(
            _comparison_recomp_flags(settings.get("RECOMP_FLAGS", "")), "-O0",
        ), posix=True),
        *CONTROL_FLAGS,
    ])
    ge_cflags = shlex.join([
        *shlex.split(settings.get("GE_CFLAGS", ""), posix=True),
        *CONTROL_FLAGS,
    ])
    ldflags = settings.get("LDFLAGS", "")
    if linker == "lld":
        ldflags = " ".join(part for part in (ldflags, "-fuse-ld=lld") if part)
    controlled = [
        make, "--no-print-directory", "--silent", f"CC={_make_path(compiler_path)}",
        f"BUILD_ROOT={info_root}", f"BUILD_DIR={info_build_dir}", f"LOG_DIR={info_log_dir}",
        *_pinned_assignments("runtime", "base_codegen", "title", "base_tail", "codegen_tool"),
        f"CFLAGS={cflags}", f"RECOMP_FLAGS={recomp_flags}",
        f"GE_CFLAGS={ge_cflags}", f"LDFLAGS={ldflags}", "compiler-info",
    ]
    outcome = _run(controlled, env=env)
    if outcome.returncode != 0:
        raise CompareError(f"controlled compiler-info failed for {compiler_path}/{linker}: "
                           f"{_diagnostic_excerpt(outcome.output)}")
    settings = _parse_settings(outcome.output)
    settings["LINKER_SELECTION"] = linker
    settings["CONTROL_FLAGS"] = " ".join(CONTROL_FLAGS)
    settings["MEASUREMENT_TARGETS"] = ",".join(MEASUREMENT_TARGETS)
    settings["CODEGEN_TOOL"] = "tools/codegen.py"
    settings["CODEGEN_USER_ARGS"] = ""
    settings["SYMBOL_MAP"] = ""
    settings["NAME_IMPORTS"] = "0"
    settings["ENHANCEMENT_PACKAGE"] = ""
    settings["ENHANCEMENT_DATA_ROOT"] = ""
    settings["ENHANCEMENT_CODE_HOOK_PACKAGE"] = ""
    settings["NAN_TRAP"] = "0"
    settings["COMPILER_IS_CLANG"] = (
        "1" if "clang" in Path(compiler_path).name.lower() else "0"
    )
    settings["RECOMP_CC_IS_CLANG_OVERRIDE"] = (
        "clang" if settings["COMPILER_IS_CLANG"] == "1" else ""
    )
    settings["RECOMP_FLAGS_EXCLUDED_FROM_COMPARISON"] = (
        f"{GCC_ONLY_TRACKING_FLAG} (GCC-only macro tracking; excluded from both comparison rows)"
    )
    settings["CORPUS_CODEGEN_FLAGS"] = (
        "production-smoke: default; production-smoke-gap: "
        + settings.get("PRODUCTION_SMOKE_GAP_CODEGEN_ARGS", "")
        + "; display-smoke-run: default; cosim-selftest: "
        + settings.get("COSIM_CODEGEN_ARGS", "")
    )
    _fp_flags_ok(settings)
    return settings


def _make_assignments(compiler: str, settings: dict[str, str], build_root: Path,
                      *, log_root: Path) -> list[str]:
    assignments = [
        f"CC={_make_path(compiler)}",
        f"BUILD_ROOT={_make_build_path(build_root)}",
        f"BUILD_DIR={_make_build_path(build_root / 'compiler_compare')}",
        f"LOG_DIR={_make_build_path(log_root)}",
        f"SDL3_DIR={settings.get('SDL3_DIR', '')}",
        *_pinned_assignments("runtime"),
        f"CFLAGS={_make_owned_flags(settings['CFLAGS'], remove_perf=True)}",
        f"GE_CFLAGS={settings.get('GE_CFLAGS', '')}",
        f"RECOMP_FLAGS={_comparison_recomp_flags(settings.get('RECOMP_FLAGS', ''))}",
        f"LDFLAGS={settings.get('LDFLAGS', '')}",
        f"LIBS={settings.get('LIBS', '')}",
        *_pinned_assignments("measurement_codegen", "measurement_tail"),
        f"RECOMP_CC_IS_CLANG={settings.get('RECOMP_CC_IS_CLANG_OVERRIDE', '')}",
        *_pinned_assignments("title", "codegen_tool"),
    ]
    return assignments


GATE_CHAIN: tuple[tuple[str, str], ...] = (
    ("production-smoke", "production-smoke"),
    ("production-smoke-gap", "production-smoke-gap"),
    ("display-smoke", "display-smoke-run"),
    ("cosim-selftest", "cosim-selftest"),
    ("cosim-mutants", "cosim-mutants"),
    ("native-core-tests", "native-core-tests"),
    ("vfpu-interp-selftest", "vfpu-interp-selftest"),
    ("fp-convert-selftest", "fp-convert-selftest"),
    ("cpu-lle-selftest", "cpu-lle-selftest"),
    ("stale-code-selftest", "stale-code-selftest"),
)
GATE_ORDER = tuple(name for name, _ in GATE_CHAIN)


def _gate_command(make: str, compiler: str, settings: dict[str, str], target: str,
                  build_root: Path) -> list[str]:
    assignments = _make_assignments(compiler, settings, build_root, log_root=build_root / "logs")
    return [make, "--no-print-directory", "--silent", "-j1", *assignments, target]


def _run_veto(make: str, compiler: str, settings: dict[str, str], build_root: Path, *,
              env: dict[str, str]) -> tuple[list[dict[str, Any]], str | None]:
    gates: list[dict[str, Any]] = []
    for name, target in GATE_CHAIN:
        command = _gate_command(make, compiler, settings, target, build_root / "veto")
        outcome = _run(command, env=env)
        passed = outcome.returncode == 0
        gates.append({
            "name": name,
            "target": target,
            "status": GATE_PASS if passed else GATE_FAIL,
            "returncode": outcome.returncode,
            "command": command,
            "output": (_diagnostic_excerpt(outcome.output) if not passed
                       else outcome.output[-OUTPUT_TAIL:]),
            "first_error": _first_error(outcome.output) if not passed else None,
        })
        if not passed:
            return gates, name
    return gates, None


_POSIX_HOST_TOP_LEVEL = (
    "tm" + "p",
    "var",
    "ho" + "me",
    "Us" + "ers",
    "root",
    "mnt",
    "private",
    "Volumes",
    "etc",
    "usr",
    "opt",
    "bin",
    "sbin",
    "dev",
    "proc",
    "sys",
    "run",
    "lib",
    "lib64",
    "System",
    "Applications",
    "Library",
)
_POSIX_TOP_GROUP = "|".join(_POSIX_HOST_TOP_LEVEL)
_QUOTED_WIN_DQ_RE = re.compile(r'(?<=")[A-Za-z]:(?:\\|/)[^"\r\n]*(?=")')
_QUOTED_WIN_SQ_RE = re.compile(r"(?<=')[A-Za-z]:(?:\\|/)[^'\r\n]*(?=')")
_QUOTED_POSIX_DQ_RE = re.compile(rf'(?<=")/(?:{_POSIX_TOP_GROUP})/[^"\r\n]*(?=")')
_QUOTED_POSIX_SQ_RE = re.compile(rf"(?<=')/(?:{_POSIX_TOP_GROUP})/[^'\r\n]*(?=')")
_FILE_URI_RE = re.compile(r"file://[^\s\"'`<>|;,)\]]+")
_UNC_PATH_RE = re.compile(r"\\\\[A-Za-z0-9_.-]+\\[^\s\"'`<>|;,)\]]+")
_TILDE_PATH_RE = re.compile(r"(?<![A-Za-z0-9_./~-])~(?:[A-Za-z0-9_.-]+)?(?:/|\\)[^\s\"'`<>|;,)\]]*")
_WIN_PROFILE_SUBDIR_RE = re.compile(
    r"(?:(?<![A-Za-z0-9_:])|(?<=-[IL]))[A-Za-z]:\\(?:"
    + r"Us"
    + r"ers|Documents and Settings)\\[^\\/:*?\"<>|\r\n]+\\"
    + r"(?:AppData\\(?:Local|Roaming)|Desktop|Documents|Downloads|One"
    + r"Drive[^\\]*)(?:\\[^\s\"'`<>|;,)\]]*)?",
    re.IGNORECASE,
)
_WIN_DRIVE_PATH_RE = re.compile(
    r"(?:(?<![A-Za-z0-9_:])|(?<=-[IL]))[A-Za-z]:(?:\\|/)(?:[^\s\"'`<>|;,)\]:]|:(?![\d\s]|$))*",
)
_POSIX_NAMED_ROOT_RE = re.compile(
    rf"(?:(?<=-[IL])|(?<![A-Za-z0-9_.>/:~-]))/(?:{_POSIX_TOP_GROUP})"
    r"(?:/[^\s\"'`<>|;,)\]]*)?(?![A-Za-z0-9_.-])",
)
_POSIX_MULTI_SEGMENT_RE = re.compile(
    r"(?:(?<=-[IL])|(?<![A-Za-z0-9_.>/:~-]))/(?:[A-Za-z0-9_.@-]+/)+[^\s\"'`<>|;,)\]]*",
)
_GUEST_VFS_PATH_RE = re.compile(
    r"(?<![A-Za-z0-9_])/(?:PSP_GAME(?:/[^\s\"'`<>|;,\)\]]*)?|UMD_DATA\.BIN)",
    re.IGNORECASE,
)
_HOST_KEYED_PTR_RE = re.compile(r"\b(host_?(?:addr|ptr)=)(?:0x)?[0-9a-fA-F]+\b", re.IGNORECASE)
_CALL_ADDR_RE = re.compile(r"((?:-> calling|<- returned from) fn )(?:0x)?[0-9a-fA-F]{8,16}")
_WIDE_HEX_PTR_RE = re.compile(r"(?<![A-Za-z0-9_])0x[0-9a-fA-F]{9,16}\b")
_WIN_BARE_PTR_RE = re.compile(r"(?<![A-Za-z0-9_])0000(?=[0-9a-fA-F]{12}\b)(?!0{11})[0-9a-fA-F]{12}\b")
_GUEST_WIDE_FIELD_PREFIX_RE = re.compile(r"\b(?:acc|hi_lo|ticks)=$", re.IGNORECASE)
_TIMING_FIELD_RE = re.compile(r"\b([A-Za-z][A-Za-z0-9_]*_ms|fps)=\d+(?:\.\d+)?", re.IGNORECASE)

_EQ_FIELD_RE = re.compile(
    r'(?<![A-Za-z0-9_.\-=])([A-Za-z_][A-Za-z0-9_.-]*)='
    r'(?!=)("[^"]*"|(?:[^\s,;]|,(?!\s*(?:$|[)\]}]|[A-Za-z_][A-Za-z0-9_.-]*=)))*)',
)
_QUOTED_EQ_VALUE_RE = re.compile(r'="[^"]*"')
_JSON_FIELD_RE = re.compile(r'"([A-Za-z_][A-Za-z0-9_.-]*)"\s*:\s*("[^"]*"|[^,\s}\]]+)')
_COLON_FIELD_RE = re.compile(
    r"(?<![A-Za-z0-9_.\\/-])"
    r"(?:"
    r'([A-Za-z_][A-Za-z0-9_.-]+):(?!//|\\\\|\\|/)\s*("[^"]*"|[^,\s}\]]+)'
    r"|"
    r'([A-Za-z_]):\s+("[^"]*"|[^,\s}\]]+)'
    r")",
)


def _path_variants(path: Path) -> tuple[str, ...]:
    variants: list[str] = []
    for candidate in (path, path.resolve()):
        for text in (str(candidate), candidate.as_posix()):
            if not text:
                continue
            variants.append(text)
            if len(text) >= 2 and text[1] == ":":
                variants.append(text[0].lower() + text[1:])
                variants.append(text[0].upper() + text[1:])
    return tuple(sorted(dict.fromkeys(variants), key=len, reverse=True))


def _host_path_roots() -> tuple[str, ...]:
    """Return known host user-home and temp directory roots, longest first."""
    candidates: list[str] = []
    raw_roots = (
        str(Path.home()),
        tempfile.gettempdir(),
        *(os.environ.get(name, "") for name in (
            "USERPROFILE", "HOME", "LOCALAPPDATA", "APPDATA", "TEMP", "TMP", "TMPDIR",
        )),
    )
    for raw in raw_roots:
        cleaned = raw.strip().rstrip("\\/")
        if not cleaned:
            continue
        path_obj = Path(cleaned)
        if not path_obj.is_absolute() or len(path_obj.parts) <= 1:
            continue
        for variant in (str(path_obj).rstrip("\\/"), path_obj.as_posix().rstrip("/")):
            if variant:
                candidates.append(variant)
        try:
            resolved = path_obj.resolve()
        except OSError:
            continue
        for variant in (str(resolved).rstrip("\\/"), resolved.as_posix().rstrip("/")):
            if variant:
                candidates.append(variant)
    return tuple(sorted(set(candidates), key=len, reverse=True))


def _redact_host_details(text: str, *, preserve_guest_wide_fields: bool = False) -> str:
    """Redact host paths (user homes, temp dirs, absolute paths) and hex host addresses."""
    guest_paths: list[str] = []

    def preserve_guest_path(match: re.Match[str]) -> str:
        guest_paths.append(match.group(0))
        return f"__SR_GUEST_PATH_{len(guest_paths) - 1}__"

    result = _GUEST_VFS_PATH_RE.sub(preserve_guest_path, text)
    for root_prefix in _host_path_roots():
        flags = re.IGNORECASE if (len(root_prefix) >= 2 and root_prefix[1] == ":") else 0
        pattern = re.escape(root_prefix) + r"(?:[\\/][^\s\"'`<>|;,)\]]*)*"
        result = re.sub(pattern, "<HOST_PATH>", result, flags=flags)
    result = _QUOTED_WIN_DQ_RE.sub("<HOST_PATH>", result)
    result = _QUOTED_WIN_SQ_RE.sub("<HOST_PATH>", result)
    result = _QUOTED_POSIX_DQ_RE.sub("<HOST_PATH>", result)
    result = _QUOTED_POSIX_SQ_RE.sub("<HOST_PATH>", result)
    result = _FILE_URI_RE.sub("<HOST_PATH>", result)
    result = _UNC_PATH_RE.sub("<HOST_PATH>", result)
    result = _TILDE_PATH_RE.sub("<HOST_PATH>", result)
    result = _WIN_PROFILE_SUBDIR_RE.sub("<HOST_PATH>", result)
    result = _WIN_DRIVE_PATH_RE.sub("<HOST_PATH>", result)
    result = _POSIX_NAMED_ROOT_RE.sub("<HOST_PATH>", result)
    result = _POSIX_MULTI_SEGMENT_RE.sub("<HOST_PATH>", result)
    result = re.sub(r"SR_HOST=0x[0-9a-fA-F]+", "SR_HOST=<HOST_ADDR>", result)
    result = _HOST_KEYED_PTR_RE.sub(r"\1<HOST_ADDR>", result)
    result = _CALL_ADDR_RE.sub(r"\1<HOST_ADDR>", result)
    def redact_wide(match: re.Match[str]) -> str:
        prefix = result[max(0, match.start() - 64):match.start()]
        if preserve_guest_wide_fields and _GUEST_WIDE_FIELD_PREFIX_RE.search(prefix):
            return match.group(0)
        return "<HOST_ADDR>"

    result = _WIDE_HEX_PTR_RE.sub(redact_wide, result)
    result = _WIN_BARE_PTR_RE.sub(redact_wide, result)
    for index, path in enumerate(guest_paths):
        result = result.replace(f"__SR_GUEST_PATH_{index}__", path)
    return result


def _redact_report_details(value: Any) -> Any:
    """Redact host paths and addresses recursively before a report is serialized."""
    if isinstance(value, dict):
        return {key: _redact_report_details(child) for key, child in value.items()}
    if isinstance(value, list):
        return [_redact_report_details(child) for child in value]
    if isinstance(value, str):
        return _redact_host_details(value, preserve_guest_wide_fields=True)
    return value


def _normalise_semantic_output(text: str, build_root: Path) -> str:
    result = text
    for variant in _path_variants(build_root):
        result = result.replace(variant, "<BUILD_ROOT>")
    resolved_build_root = build_root.resolve()
    relative_variants: tuple[str, ...]
    try:
        relative_path = resolved_build_root.relative_to(ROOT.resolve())
        relative_posix = relative_path.as_posix()
        relative_variants = (
            str(relative_path),
            relative_posix,
            relative_posix.replace("/", "\\"),
        )
    except ValueError:
        relative_variants = ()
    for variant in dict.fromkeys(relative_variants):
        if variant and variant != ".":
            result = result.replace(variant, "<BUILD_ROOT>")
    for variant in _path_variants(ROOT):
        result = result.replace(variant, "<REPO_ROOT>")
    result = re.sub(r"SR_HOST=0x[0-9a-fA-F]+", "SR_HOST=<HOST_ADDR>", result)
    result = _CALL_ADDR_RE.sub(r"\1<HOST_ADDR>", result)
    result = _TIMING_FIELD_RE.sub(r"\1=<HOST_TIME>", result)
    return result.replace("\r\n", "\n").replace("\r", "\n")


def _digest_normalised_outputs(records: list[dict[str, str]]) -> str:
    canonical = [{"target": item["target"], "output": item["normalized"]} for item in records]
    payload = json.dumps(canonical, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _semantic_digest(outputs: list[tuple[str, str]], build_root: Path) -> str:
    records = [
        {"target": name, "normalized": _normalise_semantic_output(output, build_root)}
        for name, output in outputs
    ]
    return _digest_normalised_outputs(records)


def _safe_target_filename(target: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9._-]+", "_", target).strip("_")
    return slug or "target"


def _persist_semantic_outputs(trial_root: Path, outputs: list[tuple[str, str]]) -> list[dict[str, str]]:
    """Save raw and normalised outputs per target under trial_root/semantic_outputs."""
    out_dir = trial_root / "semantic_outputs"
    out_dir.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, str]] = []
    for name, raw_output in outputs:
        normalised = _normalise_semantic_output(raw_output, trial_root)
        slug = _safe_target_filename(name)
        (out_dir / f"{slug}.raw.txt").write_text(raw_output, encoding="utf-8")
        (out_dir / f"{slug}.normalized.txt").write_text(normalised, encoding="utf-8")
        records.append({"target": name, "raw": raw_output, "normalized": normalised})
    (out_dir / "semantic_outputs.json").write_text(
        json.dumps(records, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return records


def _extract_line_fields(line: str) -> dict[str, list[str]]:
    fields: dict[str, list[str]] = {}
    for match in _EQ_FIELD_RE.finditer(line):
        fields.setdefault(match.group(1), []).append(match.group(2))
    masked_for_json = _QUOTED_EQ_VALUE_RE.sub('=""', line)
    for match in _JSON_FIELD_RE.finditer(masked_for_json):
        fields.setdefault(match.group(1), []).append(match.group(2))
    if fields:
        return fields
    for match in _COLON_FIELD_RE.finditer(line):
        key = match.group(1) or match.group(3)
        value = match.group(2) or match.group(4)
        if key is not None and value is not None:
            fields.setdefault(key, []).append(value)
    return fields


def _divergent_fields_on_line(line_a: str, line_b: str) -> list[str]:
    fields_a = _extract_line_fields(line_a)
    fields_b = _extract_line_fields(line_b)
    if not fields_a and not fields_b:
        return []
    ordered_keys = list(dict.fromkeys([*fields_a.keys(), *fields_b.keys()]))
    return [key for key in ordered_keys if fields_a.get(key) != fields_b.get(key)]


_MAX_ALIGN_PAIR_LINES = 32
_MAX_ALIGN_LINE_CHARS = 1024


def _line_signature(line: str) -> tuple[frozenset[str], str]:
    keys = frozenset(_extract_line_fields(line))
    head_parts = line.split(None, 1)
    head = head_parts[0] if head_parts else ""
    if "=" in head or ":" in head:
        head = ""
    return keys, head


def _score_line_pair(
    sig_a: tuple[frozenset[str], str],
    sig_b: tuple[frozenset[str], str],
    matcher: difflib.SequenceMatcher[str],
) -> tuple[int, float] | None:
    keys_a, head_a = sig_a
    keys_b, head_b = sig_b
    shared_keys = len(keys_a & keys_b)
    same_head = 1 if (head_a and head_a == head_b) else 0
    overlap_head = shared_keys + same_head
    threshold = 0.45 if overlap_head >= 1 else 0.65
    if matcher.quick_ratio() < threshold:
        return None
    ratio = matcher.ratio()
    if ratio < threshold:
        return None
    return (overlap_head, ratio)


def _align_first_divergence(
    norm_a_lines: list[str], norm_b_lines: list[str],
) -> tuple[str, int, int | None, int | None, str, str]:
    """Return (kind, line_number, idx_a, idx_b, norm_a_line, norm_b_line) for differing line lists."""
    opcodes = difflib.SequenceMatcher(None, norm_a_lines, norm_b_lines, autojunk=False).get_opcodes()
    diff_opcodes = [op for op in opcodes if op[0] != "equal"]
    first_tag, i1, i2, j1, j2 = diff_opcodes[0]

    a_entries = [(i, norm_a_lines[i]) for _t, ai1, ai2, _bj1, _bj2 in diff_opcodes for i in range(ai1, ai2)]
    b_entries = [(j, norm_b_lines[j]) for _t, _ai1, _ai2, bj1, bj2 in diff_opcodes for j in range(bj1, bj2)]

    b_buckets: dict[str, deque[int]] = {}
    for pos, (_j, line) in enumerate(b_entries):
        b_buckets.setdefault(line, deque()).append(pos)
    used_b: set[int] = set()
    rem_a: list[tuple[int, str]] = []
    for i, line in a_entries:
        bucket = b_buckets.get(line)
        if bucket:
            used_b.add(bucket.popleft())
        else:
            rem_a.append((i, line))
    rem_b = [entry for pos, entry in enumerate(b_entries) if pos not in used_b]

    if not rem_a and not rem_b:
        idx_a = i1 if i1 < len(norm_a_lines) else None
        idx_b = j1 if j1 < len(norm_b_lines) else None
        line_number = (i1 + 1) if idx_a is not None else (j1 + 1)
        return (
            "reordered",
            line_number,
            idx_a,
            idx_b,
            norm_a_lines[idx_a] if idx_a is not None else "",
            norm_b_lines[idx_b] if idx_b is not None else "",
        )

    if (
        first_tag == "replace"
        and (i2 - i1) == 1
        and (j2 - j1) == 1
        and rem_a
        and rem_b
        and rem_a[0][0] == i1
        and rem_b[0][0] == j1
    ):
        return ("changed", i1 + 1, i1, j1, norm_a_lines[i1], norm_b_lines[j1])

    # Bound pairing work to the earliest differing window so large diffs stay linear-ish;
    # the earliest divergence always involves the leading unmatched lines.
    cand_a = rem_a[:_MAX_ALIGN_PAIR_LINES]
    cand_b = rem_b[:_MAX_ALIGN_PAIR_LINES]
    sigs_a = [_line_signature(line) for _idx, line in cand_a]
    sigs_b = [_line_signature(line) for _idx, line in cand_b]

    candidates: list[tuple[tuple[int, float], int, int]] = []
    matcher = difflib.SequenceMatcher(None, "", "", autojunk=False)
    for pos_b, (_jb, line_b) in enumerate(cand_b):
        sig_b = sigs_b[pos_b]
        matcher.set_seq2(line_b[:_MAX_ALIGN_LINE_CHARS])
        for pos_a, (_ia, line_a) in enumerate(cand_a):
            matcher.set_seq1(line_a[:_MAX_ALIGN_LINE_CHARS])
            score = _score_line_pair(sigs_a[pos_a], sig_b, matcher)
            if score is not None:
                candidates.append((score, pos_a, pos_b))
    candidates.sort(key=lambda item: (-item[0][0], -item[0][1], abs(item[1] - item[2]), item[1], item[2]))

    paired_a: set[int] = set()
    paired_b: set[int] = set()
    items: list[tuple[tuple[int, int], str, int, int | None, int | None, str, str]] = []
    for _score, pos_a, pos_b in candidates:
        if pos_a in paired_a or pos_b in paired_b:
            continue
        paired_a.add(pos_a)
        paired_b.add(pos_b)
        idx_a, line_a = cand_a[pos_a]
        idx_b, line_b = cand_b[pos_b]
        items.append(((min(idx_a, idx_b), 0), "changed", idx_a + 1, idx_a, idx_b, line_a, line_b))
    for pos_a, (idx_a, line_a) in enumerate(cand_a):
        if pos_a not in paired_a:
            items.append(((idx_a, 1), "deleted", idx_a + 1, idx_a, None, line_a, ""))
    for pos_b, (idx_b, line_b) in enumerate(cand_b):
        if pos_b not in paired_b:
            items.append(((idx_b, 2), "inserted", idx_b + 1, None, idx_b, "", line_b))

    items.sort(key=lambda item: item[0])
    _key, kind, line_number, idx_a_opt, idx_b_opt, norm_a_line, norm_b_line = items[0]
    return (kind, line_number, idx_a_opt, idx_b_opt, norm_a_line, norm_b_line)


def _first_semantic_divergence(trial_outputs: list[list[dict[str, str]]], hashes: list[str]) -> dict[str, Any] | None:
    """Return the first normalised target/line/field divergence against repeat 1."""
    if len(trial_outputs) < 2:
        return None
    baseline_idx = 0
    divergent_idx = next((idx for idx, h in enumerate(hashes) if h != hashes[baseline_idx]), None)
    if divergent_idx is None:
        return None
    base_records = trial_outputs[baseline_idx]
    other_records = trial_outputs[divergent_idx]
    base_by_target = {item["target"]: item for item in base_records}
    other_by_target = {item["target"]: item for item in other_records}
    ordered_targets = list(dict.fromkeys([*(item["target"] for item in base_records), *(item["target"] for item in other_records)]))
    divergent_targets = [
        target for target in ordered_targets
        if base_by_target.get(target, {}).get("normalized") != other_by_target.get(target, {}).get("normalized")
    ]
    if not divergent_targets:
        return None
    target = divergent_targets[0]
    base_item = base_by_target.get(target, {"raw": "", "normalized": ""})
    other_item = other_by_target.get(target, {"raw": "", "normalized": ""})
    base_norm = base_item["normalized"]
    other_norm = other_item["normalized"]
    norm_a_lines = base_norm.splitlines()
    norm_b_lines = other_norm.splitlines()
    raw_a_lines = base_item["raw"].splitlines()
    raw_b_lines = other_item["raw"].splitlines()

    if norm_a_lines == norm_b_lines:
        trailing_diff = base_norm.endswith("\n") != other_norm.endswith("\n")
        kind = "trailing_newline" if trailing_diff else "non_line_difference"
        unified = list(itertools.islice(difflib.unified_diff(
            base_norm.splitlines(keepends=True),
            other_norm.splitlines(keepends=True),
            fromfile=f"{target} (trial {baseline_idx + 1})",
            tofile=f"{target} (trial {divergent_idx + 1})",
            lineterm="",
        ), 60))
        return {
            "target": target,
            "baseline_trial": baseline_idx + 1,
            "divergent_trial": divergent_idx + 1,
            "kind": kind,
            "line_number": None,
            "fields": [],
            "baseline_trailing_newline": base_norm.endswith("\n"),
            "divergent_trailing_newline": other_norm.endswith("\n"),
            "baseline_line": "",
            "divergent_line": "",
            "baseline_normalised_line": "",
            "divergent_normalised_line": "",
            "divergent_targets": divergent_targets,
            "unified_diff": [line.rstrip("\r\n") for line in unified],
        }

    kind, line_number, idx_a, idx_b, norm_a_line, norm_b_line = _align_first_divergence(norm_a_lines, norm_b_lines)
    raw_a_line = raw_a_lines[idx_a] if idx_a is not None and idx_a < len(raw_a_lines) else norm_a_line
    raw_b_line = raw_b_lines[idx_b] if idx_b is not None and idx_b < len(raw_b_lines) else norm_b_line
    fields = [] if kind == "reordered" else _divergent_fields_on_line(norm_a_line, norm_b_line)
    unified = list(itertools.islice(difflib.unified_diff(
        norm_a_lines,
        norm_b_lines,
        fromfile=f"{target} (trial {baseline_idx + 1})",
        tofile=f"{target} (trial {divergent_idx + 1})",
        lineterm="",
    ), 60))
    return {
        "target": target,
        "baseline_trial": baseline_idx + 1,
        "divergent_trial": divergent_idx + 1,
        "kind": kind,
        "line_number": line_number,
        "fields": fields,
        "baseline_line": raw_a_line,
        "divergent_line": raw_b_line,
        "baseline_normalised_line": norm_a_line,
        "divergent_normalised_line": norm_b_line,
        "divergent_targets": divergent_targets,
        "unified_diff": unified,
    }


def _format_semantic_diff_text(diff: dict[str, Any]) -> str:
    fields = ", ".join(diff["fields"]) if diff.get("fields") else "(none detected)"
    kind = diff.get("kind", "changed")
    line_label = str(diff["line_number"]) if diff.get("line_number") is not None else f"({kind})"
    lines = [
        f"target: {diff['target']}",
        f"trials: {diff['baseline_trial']} vs {diff['divergent_trial']}",
        f"kind: {kind}",
        f"line: {line_label}",
        f"fields: {fields}",
    ]
    if kind == "trailing_newline":
        base_nl = "present" if diff.get("baseline_trailing_newline") else "absent"
        div_nl = "present" if diff.get("divergent_trailing_newline") else "absent"
        lines.extend([
            f"baseline trailing newline: {base_nl}",
            f"divergent trailing newline: {div_nl}",
        ])
    else:
        lines.extend([
            f"baseline raw:  {diff['baseline_line']}",
            f"divergent raw: {diff['divergent_line']}",
            f"baseline norm:  {diff['baseline_normalised_line']}",
            f"divergent norm: {diff['divergent_normalised_line']}",
        ])
    lines.extend(["", *diff.get("unified_diff", [])])
    return "\n".join(lines) + "\n"


def _read_events(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        raise CompareError(f"compiler event log missing: {path}")
    records: list[dict[str, Any]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            raise CompareError(f"{path}:{line_number}: invalid compiler event: {exc}") from exc
        if not isinstance(event, dict):
            raise CompareError(f"{path}:{line_number}: compiler event is not an object")
        records.append(event)
    return records


def _byte_total(root: Path, suffix: str) -> int:
    return sum(path.stat().st_size for path in root.rglob(f"*{suffix}") if path.is_file())


def _trial_summary(events: list[dict[str, Any]], trial_root: Path,
                   semantic_outputs: list[tuple[str, str]], linker: str = "default") -> dict[str, Any]:
    generated = [event for event in events
                 if event.get("category") == "compile" and event.get("generated_c") is True]
    links = [event for event in events if event.get("category") == "link"]
    if not generated:
        raise CompareError(f"no generated-C compiler invocations were recorded in {trial_root}")
    if not links:
        raise CompareError(f"no link invocations were recorded in {trial_root}")
    for event in links:
        arguments = event.get("argv", [])
        if linker == "lld" and "-fuse-ld=lld" not in arguments:
            raise CompareError(f"link invocation did not use the selected lld linker: {arguments}")
        if linker == "default" and any(str(arg).startswith("-fuse-ld=") for arg in arguments):
            raise CompareError(f"default-linker invocation carries an explicit linker override: {arguments}")
    if any(not isinstance(event.get("duration_s"), (int, float)) for event in generated + links):
        raise CompareError(f"compiler event has no wall time in {trial_root}")
    memory = [event.get("peak_working_set_bytes") for event in generated]
    memory_values: list[int] = []
    for value in memory:
        if not isinstance(value, int) or value <= 0:
            raise CompareError(f"generated-C peak working set is unavailable in {trial_root}")
        memory_values.append(value)
    generated_commands = sorted({
        json.dumps({"compiler": event.get("compiler"), "argv": event.get("argv")},
                   sort_keys=True)
        for event in generated
    })
    link_commands = sorted({
        json.dumps({"compiler": event.get("compiler"), "argv": event.get("argv")},
                   sort_keys=True)
        for event in links
    })
    object_bytes = _byte_total(trial_root, ".o")
    executable_bytes = _byte_total(trial_root, ".exe")
    if executable_bytes <= 0:
        raise CompareError(f"measurement targets produced no executable in {trial_root}")
    persisted_outputs = _persist_semantic_outputs(trial_root, semantic_outputs)
    return {
        "generated_c_compile_wall_seconds": sum(float(event["duration_s"]) for event in generated),
        "peak_working_set_bytes": max(memory_values),
        "link_wall_seconds": sum(float(event["duration_s"]) for event in links),
        "object_bytes": object_bytes,
        "executable_bytes": executable_bytes,
        "semantic_sha256": _digest_normalised_outputs(persisted_outputs),
        "_semantic_outputs": persisted_outputs,
        "compiler_commands": [
            {
                "compiler": event.get("compiler"),
                "category": event.get("category"),
                "generated_c": event.get("generated_c"),
                "argv": event.get("argv"),
            }
            for event in events if event.get("category") in ("compile", "link")
        ],
        "generated_c_commands": [json.loads(item) for item in generated_commands],
        "link_commands": [json.loads(item) for item in link_commands],
    }


def _wrapper_cc(python: str, compiler_path: str) -> str:
    python_path = Path(python)
    launcher = python_path.as_posix().replace("\\", "/")
    if " " in launcher:
        on_path = shutil.which(python_path.name) or shutil.which("python")
        if on_path and Path(on_path).resolve() == python_path.resolve():
            launcher = python_path.name
        elif sys.platform == "win32":
            short_buffer = ctypes.create_unicode_buffer(32768)
            short_length = ctypes.WinDLL("kernel32", use_last_error=True).GetShortPathNameW(
                str(python_path), short_buffer, len(short_buffer),
            )
            if not short_length or " " in short_buffer.value:
                raise CompareError(f"cannot pass the Python launcher through Make safely: {python}")
            launcher = Path(short_buffer.value).as_posix()
        else:
            raise CompareError(f"cannot pass the Python launcher through Make safely: {python}")
    arguments = [
        launcher, "-I",
        Path(__file__).resolve().as_posix(), "--_compiler-wrapper",
        "--compiler", Path(compiler_path).as_posix().replace("\\", "/"), "--",
    ]
    if any(" " in argument for argument in arguments):
        raise CompareError("the compiler wrapper paths contain spaces that Make cannot pass safely")
    return " ".join(arguments)


def _measurement_trials(compiler_path: str, make: str, python: str, settings: dict[str, str],
                        build_root: Path, repeats: int, *, env: dict[str, str]) -> list[dict[str, Any]]:
    trials: list[dict[str, Any]] = []
    for index in range(1, repeats + 1):
        trial_root = build_root / f"trial-{index}"
        if trial_root.exists():
            shutil.rmtree(trial_root)
        trial_root.mkdir(parents=True)
        events_path = trial_root / "compiler_events.jsonl"
        measured_env = env.copy()
        measured_env["NK_COMPILER_COMPARE_EVENTS"] = str(events_path)
        measured_env["NK_COMPILER_COMPARE_BUILD_ROOT"] = str(trial_root.resolve())
        compiler = _wrapper_cc(python, compiler_path)
        outputs: list[tuple[str, str]] = []
        commands: list[list[str]] = []
        for target in MEASUREMENT_TARGETS:
            assignments = _make_assignments(compiler, settings, trial_root, log_root=trial_root / "logs")
            command = [make, "--no-print-directory", "--silent", "-j1", *assignments, target]
            commands.append(command)
            outcome = _run(command, env=measured_env)
            outputs.append((target, outcome.output))
            if outcome.returncode != 0:
                raise CompareError(
                    f"measurement build/run {target} failed in trial {index} "
                    f"(exit {outcome.returncode}): {_diagnostic_excerpt(outcome.output)}"
                )
        record = _trial_summary(
            _read_events(events_path), trial_root, outputs,
            linker=settings.get("LINKER_SELECTION", "default"),
        )
        record["trial"] = index
        record["commands"] = commands
        trials.append(record)
    return trials


def _min_median(values: list[int | float]) -> dict[str, int | float]:
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        median: int | float = ordered[middle]
    else:
        median = (ordered[middle - 1] + ordered[middle]) / 2
    return {"min": ordered[0], "median": median, "max": ordered[-1]}


def _measurement_summary(trials: list[dict[str, Any]]) -> dict[str, Any]:
    if len(trials) < MIN_REPEATS:
        raise CompareError(f"measurements require at least {MIN_REPEATS} repetitions")
    trial_outputs = [trial.pop("_semantic_outputs", None) for trial in trials]
    summary: dict[str, Any] = {
        key: _min_median([trial[key] for trial in trials]) for key in MEASUREMENT_NAMES
    }
    hashes = [trial["semantic_sha256"] for trial in trials]
    summary["semantic_sha256"] = hashes[0] if len(set(hashes)) == 1 else None
    summary["semantic_hashes"] = hashes
    summary["semantic_hash_stable"] = len(set(hashes)) == 1
    if not summary["semantic_hash_stable"] and all(isinstance(item, list) for item in trial_outputs):
        summary["semantic_diff"] = _first_semantic_divergence(
            [item for item in trial_outputs if isinstance(item, list)],
            hashes,
        )
    else:
        summary["semantic_diff"] = None
    return summary


def _unavailable(compiler: str, linker: str, *, reason: str) -> dict[str, Any]:
    return {
        "name": f"{compiler}/{linker}",
        "compiler": compiler,
        "compiler_path": None,
        "compiler_version": None,
        "linker_selection": linker,
        "linker_path": None,
        "linker_version": None,
        "build_root": None,
        "build_settings": None,
        "veto": None,
        "measurements": None,
        "status": STATUS_NOT_AVAILABLE,
        "reason": reason,
    }


def _workload_measurements(compiler_path: str, make: str, python: str, settings: dict[str, str],
                           perf_dir: Path, build_prefix: Path, *,
                           env: dict[str, str]) -> dict[str, Any]:
    """Run the existing public #282 runtime matrix after the semantic veto.

    Every compiler/linker pair gets a disjoint make-relative BUILD_DIR prefix
    and a row-specific BUILD_ROOT for workloads that honor it. The platform
    ladder uses fixed trees under build/platform-ladder; its workloads run
    sequentially. The runner inherits this pair's compiler and frozen flags so
    runtime results do not silently fall back to the host default compiler or linker.
    """
    perf_dir.mkdir(parents=True, exist_ok=True)
    (perf_dir / "benchmark.json").unlink(missing_ok=True)
    command = [
        python, str(ROOT / "tools" / "run_perf_benchmarks.py"),
        "--make", make, "--python", python,
        "--output", str(perf_dir), "--build-prefix", _make_build_path(build_prefix),
    ]
    # Runtime workload rows choose their own game/title inputs and codegen tool; pin only
    # the compiler and codegen settings shared with the comparison trials.
    make_assignments = [
        f"CC={_make_path(compiler_path)}",
        f"SDL3_DIR={settings.get('SDL3_DIR', '')}",
        *_pinned_assignments("runtime"),
        f"CFLAGS={_make_owned_flags(settings['CFLAGS'], remove_perf=True)}",
        f"GE_CFLAGS={settings.get('GE_CFLAGS', '')}",
        f"RECOMP_FLAGS={_comparison_recomp_flags(settings.get('RECOMP_FLAGS', ''))}",
        f"LDFLAGS={settings.get('LDFLAGS', '')}",
        f"BUILD_ROOT={_make_build_path(build_prefix)}",
        f"LIBS={settings.get('LIBS', '')}",
        *_pinned_assignments("measurement_codegen", "measurement_tail"),
        f"RECOMP_CC_IS_CLANG={settings.get('RECOMP_CC_IS_CLANG_OVERRIDE', '')}",
    ]
    for assignment in make_assignments:
        command.extend(("--make-arg", assignment))
    workload_env = env.copy()
    workload_env.update({"MAKE": make, "PYTHON": python})
    for assignment in make_assignments:
        key, value = assignment.split("=", 1)
        workload_env[key] = value

    outcome = _run(command, env=workload_env)
    report_path = perf_dir / "benchmark.json"
    if not report_path.is_file():
        return {
            "status": GATE_FAIL,
            "detail": f"benchmark report absent (runner exit {outcome.returncode}): "
                      f"{_diagnostic_excerpt(outcome.output)}",
            "workloads": [],
        }
    try:
        matrix = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {"status": GATE_FAIL, "detail": f"{report_path}: unreadable benchmark report: {exc}",
                "workloads": []}
    if not isinstance(matrix, dict) or matrix.get("schema") != PERF_SCHEMA:
        return {"status": GATE_FAIL, "detail": f"{report_path}: unexpected schema", "workloads": []}
    runs = matrix.get("runs")
    if not isinstance(runs, list) or not runs:
        return {"status": GATE_FAIL, "detail": f"{report_path}: no workload runs recorded", "workloads": []}

    workloads: list[dict[str, Any]] = []
    for index, run in enumerate(runs):
        if not isinstance(run, dict) or not all(
            key in run for key in ("name", "targets", "status", "duration_s", "summary")
        ):
            return {"status": GATE_FAIL, "detail": f"{report_path}: runs[{index}] is not a workload record",
                    "workloads": []}
        status = run["status"]
        if status not in (GATE_PASS, GATE_SKIP, GATE_FAIL):
            status = GATE_FAIL
        workloads.append({
            "name": run["name"],
            "targets": run["targets"],
            "status": status,
            "duration_s": run["duration_s"],
            "summary": run["summary"],
            **({"note": run["note"]} if "note" in run else {}),
            "output": str(run.get("output", ""))[-OUTPUT_TAIL:],
        })
    status = (GATE_FAIL if any(run["status"] == GATE_FAIL for run in workloads)
              else GATE_SKIP if any(run["status"] != GATE_PASS for run in workloads)
              else GATE_PASS)
    return {"status": status, "detail": None, "workloads": workloads}


def _measure_pair(name: str, compiler_name: str, compiler_path: str, linker: str,
                  linker_path: str, linker_version: str, make: str, python: str,
                  build_root: Path, repeats: int, *, env: dict[str, str]) -> dict[str, Any]:
    diff_path = build_root / "measurements" / "semantic_diff.txt"
    diff_path.unlink(missing_ok=True)
    settings = _build_settings(make, compiler_path, linker, build_root, env=env)
    _check_ucrt_paths(compiler_path, linker_path, settings)
    compiler_version = _full_version([compiler_path, "--version"], env=env)
    compiler_target = _full_version([compiler_path, "-dumpmachine"], env=env)
    if compiler_version is None:
        raise CompareError(f"compiler did not report a version: {compiler_path}")
    gates, failed_gate = _run_veto(make, compiler_path, settings, build_root, env=env)
    record: dict[str, Any] = {
        "name": name,
        "compiler": compiler_name,
        "compiler_path": compiler_path,
        "compiler_version": compiler_version,
        "compiler_target": compiler_target,
        "linker_selection": linker,
        "linker_path": linker_path,
        "linker_version": linker_version,
        "build_root": build_root.as_posix(),
        "build_settings": settings,
        "veto": {
            "status": GATE_FAIL if failed_gate else GATE_PASS,
            "failed_gate": failed_gate,
            "gates": gates,
        },
        "measurements": None,
    }
    if failed_gate:
        record["status"] = STATUS_FAILED_VETO
        return record
    try:
        trials = _measurement_trials(
            compiler_path, make, python, settings, build_root / "measurements", repeats, env=env,
        )
        summary = _measurement_summary(trials)
        if isinstance(summary.get("semantic_diff"), dict):
            diff_path.parent.mkdir(parents=True, exist_ok=True)
            diff_path.write_text(
                _format_semantic_diff_text(summary["semantic_diff"]),
                encoding="utf-8",
            )
        else:
            diff_path.unlink(missing_ok=True)
        workloads = _workload_measurements(
            compiler_path, make, python, settings,
            build_root / "perf", build_root / "perf-build", env=env,
        )
    except CompareError as exc:
        record["status"] = STATUS_FAILED_MEASUREMENT
        record["measurement_error"] = str(exc)
        return record
    record["measurements"] = {
        "repeats": repeats, "trials": trials, "summary": summary,
        "workloads_status": workloads["status"],
        "workloads_detail": workloads["detail"],
        "workloads": workloads["workloads"],
    }
    if workloads["status"] != GATE_PASS:
        record["status"] = STATUS_WORKLOAD_FAILED
    elif summary["semantic_hash_stable"]:
        record["status"] = STATUS_MEASURED
    else:
        record["status"] = STATUS_MEASURED_HASH_UNSTABLE
    return record


def _timing_keys(value: Any, path: str = "$") -> list[str]:
    found: list[str] = []
    if isinstance(value, dict):
        for key, child in value.items():
            found.extend(_timing_keys(child, f"{path}.{key}"))
            if key in TIMING_KEYS:
                found.append(f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            found.extend(_timing_keys(child, f"{path}[{index}]"))
    return found


def validate_report(report: Any) -> None:
    """Reject malformed reports and any measurement that bypasses the full veto."""
    if not isinstance(report, dict) or report.get("schema") != SCHEMA or report.get("issue") != ISSUE:
        raise CompareError(f"report must use schema {SCHEMA!r} for issue {ISSUE}")
    entries = report.get("toolchains")
    if not isinstance(entries, list) or not entries:
        raise CompareError("$.toolchains: expected a non-empty array")
    seen: set[str] = set()
    for index, entry in enumerate(entries):
        path = f"$.toolchains[{index}]"
        if not isinstance(entry, dict):
            raise CompareError(f"{path}: expected an object")
        name = entry.get("name")
        status = entry.get("status")
        if not isinstance(name, str) or not name or name in seen:
            raise CompareError(f"{path}.name: expected a unique non-empty name")
        seen.add(name)
        if status not in STATUSES:
            raise CompareError(f"{path}.status: expected one of {STATUSES}")
        if status == STATUS_NOT_AVAILABLE:
            if entry.get("measurements") is not None or entry.get("veto") is not None or _timing_keys(entry):
                raise CompareError(f"{path}: unavailable toolchains carry no veto or measurements")
            continue
        veto = entry.get("veto")
        if not isinstance(veto, dict):
            raise CompareError(f"{path}.veto: expected an object")
        gates = veto.get("gates")
        if not isinstance(gates, list) or not gates:
            raise CompareError(f"{path}.veto.gates: expected a non-empty array")
        names = [gate.get("name") if isinstance(gate, dict) else None for gate in gates]
        if names != list(GATE_ORDER[:len(names)]):
            raise CompareError(f"{path}.veto.gates: gates must follow the fixed order")
        if status == STATUS_FAILED_VETO:
            if veto.get("status") != GATE_FAIL or veto.get("failed_gate") != names[-1]:
                raise CompareError(f"{path}.veto: FAILED_VETO must name its final failed gate")
            if gates[-1].get("status") != GATE_FAIL or entry.get("measurements") is not None:
                raise CompareError(f"{path}: failed vetoes carry no performance measurements")
            if _timing_keys(entry):
                raise CompareError(f"{path}: failed veto leaks performance values")
            continue
        if names != list(GATE_ORDER):
            raise CompareError(f"{path}.veto.gates: measured toolchains require the full gate chain")
        if any(gate.get("status") != GATE_PASS for gate in gates):
            raise CompareError(f"{path}.veto.gates: every gate must pass before measurement")
        if veto.get("status") != GATE_PASS or veto.get("failed_gate") is not None:
            raise CompareError(f"{path}.veto: measured toolchain must pass every gate")
        if status == STATUS_FAILED_MEASUREMENT:
            if entry.get("measurements") is not None:
                raise CompareError(f"{path}: failed measurement carries no performance values")
            if _timing_keys(entry):
                raise CompareError(f"{path}: failed measurement leaks performance values")
            continue
        measurements = entry.get("measurements")
        if not isinstance(measurements, dict):
            raise CompareError(f"{path}.measurements: expected an object")
        if measurements.get("workloads_status") not in (GATE_PASS, GATE_SKIP, GATE_FAIL):
            raise CompareError(f"{path}.measurements.workloads_status: expected PASS, SKIP or FAIL")
        if not isinstance(measurements.get("workloads"), list):
            raise CompareError(f"{path}.measurements.workloads: expected an array")
        trials = measurements.get("trials")
        repeats = measurements.get("repeats")
        if not isinstance(trials, list) or len(trials) < MIN_REPEATS or repeats != len(trials):
            raise CompareError(f"{path}.measurements: at least {MIN_REPEATS} measured trials are required")
        for trial in trials:
            if any(key not in trial for key in (*MEASUREMENT_NAMES, "semantic_sha256")):
                raise CompareError(f"{path}.measurements.trials: missing a required measurement")
        summary = measurements.get("summary")
        if not isinstance(summary, dict) or not isinstance(summary.get("semantic_hashes"), list):
            raise CompareError(f"{path}.measurements.summary: missing repeat summary")
        if status in MEASURED_STATUSES and measurements["workloads_status"] != GATE_PASS:
            raise CompareError(f"{path}: {status} requires a passing runtime workload matrix")
        if status in MEASURED_STATUSES and summary.get("semantic_hash_stable") is not (status == STATUS_MEASURED):
            raise CompareError(
                f"{path}: MEASURED requires a stable semantic hash and MEASURED_HASH_UNSTABLE an unstable one"
            )
        if status == STATUS_WORKLOAD_FAILED and measurements["workloads_status"] not in (GATE_FAIL, GATE_SKIP):
            raise CompareError(f"{path}: WORKLOAD_FAILED requires a failed or skipped runtime workload")


def summarize(report: dict[str, Any]) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {status: [] for status in STATUSES}
    for entry in report["toolchains"]:
        result[entry["status"]].append(entry["name"])
    return result


def _semantic_hash_comparison(entries: list[dict[str, Any]]) -> dict[str, Any]:
    measured = [entry for entry in entries if entry.get("status") in MEASURED_STATUSES]
    hashes = {
        entry["name"]: entry["measurements"]["summary"]["semantic_sha256"]
        for entry in measured
    }
    if not measured:
        status = "NOT_COMPARABLE"
    elif len(measured) != len(entries):
        status = "INCOMPLETE"
    elif any(not entry["measurements"]["summary"]["semantic_hash_stable"] for entry in measured):
        status = "UNSTABLE"
    else:
        status = "MATCH" if len(set(hashes.values())) == 1 else "DIFFERENT"
    return {"status": status, "sha256_by_toolchain": hashes}


def _format_range(value: Any, divisor: float = 1.0, suffix: str = "") -> str:
    if not isinstance(value, dict):
        return "—"
    return f"{value['min'] / divisor:.3f}/{value['median'] / divisor:.3f}{suffix}"


def _format_markdown_line(line: str, *, absent: bool = False) -> str:
    if not line:
        return "(line absent)" if absent else "(empty line)"
    # Semantic diff lines may contain guest state (acc/hi_lo/ticks) wider than a host pointer.
    # Preserve those keyed values while continuing to redact unkeyed host addresses.
    redacted = _redact_host_details(line, preserve_guest_wide_fields=True)
    fence = "`" * (max((len(match.group(0)) for match in re.finditer(r"`+", redacted)), default=0) + 1)
    pad = " " if redacted.startswith("`") or redacted.endswith("`") else ""
    return f"{fence}{pad}{redacted}{pad}{fence}"


def render_markdown(report: dict[str, Any]) -> str:
    validate_report(report)
    lines = [
        "# Compiler comparison summary",
        "",
        f"Schema: {SCHEMA}. Measurements require {MIN_REPEATS} or more repeats and follow the source-owned veto.",
        "",
        "| Compiler | Linker | Veto | Runtime workloads | Generated C compile min/median (s) | Peak working set min/median (MiB) | Link min/median (s) | Objects min/median (bytes) | Executables min/median (bytes) | Semantic SHA-256 |",
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for entry in report["toolchains"]:
        if entry["status"] not in MEASURED_STATUSES:
            veto = entry.get("veto") or {}
            measurements = entry.get("measurements") or {}
            workload_status = measurements.get("workloads_status", "—")
            if entry["status"] == STATUS_WORKLOAD_FAILED:
                failure = f"runtime workload matrix {workload_status}"
            else:
                failure = _redact_host_details(str(
                    veto.get("failed_gate") or entry.get("measurement_error")
                    or entry.get("reason", "N/A")
                ))
            lines.append(
                f"| {entry['compiler']} | {entry['linker_selection']} | {entry['status']} "
                f"({failure}) | {workload_status} | — | — | — | — | — | — |"
            )
            continue
        summary = entry["measurements"]["summary"]
        semantic = summary["semantic_sha256"] or "UNSTABLE"
        veto_cell = "PASS" if entry["status"] == STATUS_MEASURED else f"PASS; {entry['status']}"
        lines.append(
            f"| {entry['compiler']} | {entry['linker_selection']} | {veto_cell} "
            f"| {entry['measurements']['workloads_status']} "
            f"| {_format_range(summary['generated_c_compile_wall_seconds'], suffix=' s')} "
            f"| {_format_range(summary['peak_working_set_bytes'], 1024 * 1024, ' MiB')} "
            f"| {_format_range(summary['link_wall_seconds'], suffix=' s')} "
            f"| {_format_range(summary['object_bytes'])} "
            f"| {_format_range(summary['executable_bytes'])} "
            f"| {semantic} |"
        )
    comparison = report.get("semantic_hash_comparison", {})
    lines.extend([
        "",
        f"Semantic hash comparison across toolchains: {comparison.get('status', 'NOT_RECORDED')}.",
        "",
    ])
    for entry in report["toolchains"]:
        measurements = entry.get("measurements")
        if not isinstance(measurements, dict):
            continue
        diff = (measurements.get("summary") or {}).get("semantic_diff")
        if not isinstance(diff, dict):
            continue
        kind = diff.get("kind")
        fields = f", unstable field(s): {', '.join(diff['fields'])}" if diff.get("fields") else ""
        if kind == "trailing_newline":
            base_nl = "present" if diff.get("baseline_trailing_newline") else "absent"
            div_nl = "present" if diff.get("divergent_trailing_newline") else "absent"
            lines.extend([
                f"- `{entry['name']}` repeat divergence: target `{diff['target']}`, "
                f"trailing newline differs, trial {diff['baseline_trial']} vs "
                f"{diff['divergent_trial']}.",
                f"  - trial {diff['baseline_trial']}: trailing newline {base_nl}",
                f"  - trial {diff['divergent_trial']}: trailing newline {div_nl}",
            ])
            continue
        if diff.get("line_number") is None:
            detail = "output text differs outside line content" if kind == "non_line_difference" else "output differs"
            lines.append(
                f"- `{entry['name']}` repeat divergence: target `{diff['target']}`, "
                f"{detail}, trial {diff['baseline_trial']} vs "
                f"{diff['divergent_trial']}{fields}.",
            )
            continue
        kind_note = f" ({kind})" if kind in ("inserted", "deleted", "reordered") else ""
        base_line = str(diff.get("baseline_normalised_line", ""))
        div_line = str(diff.get("divergent_normalised_line", ""))
        lines.extend([
            f"- `{entry['name']}` repeat divergence: target `{diff['target']}`, "
            f"line {diff['line_number']}{kind_note}, trial {diff['baseline_trial']} vs "
            f"{diff['divergent_trial']}{fields}.",
            # Normalised lines only (re-redacted before rendering): never print raw lines.
            f"  - trial {diff['baseline_trial']}: {_format_markdown_line(base_line, absent=(kind == 'inserted'))}",
            f"  - trial {diff['divergent_trial']}: {_format_markdown_line(div_line, absent=(kind == 'deleted'))}",
        ])
    lines.append("")
    return "\n".join(lines)


def compare(compilers: list[str], linkers: list[str], *, make: str, python: str,
            output: Path, repeats: int = MIN_REPEATS) -> dict[str, Any]:
    if repeats < MIN_REPEATS:
        raise CompareError(f"--repeats must be at least {MIN_REPEATS}")
    make_path = shutil.which(make)
    if make_path is None:
        raise CompareError(f"make executable not found: {make}")
    make_path = _make_path(make_path)
    if any(linker not in ("default", "lld") for linker in linkers):
        raise CompareError("linkers must be default or lld")
    records: list[dict[str, Any]] = []
    for compiler_name in compilers:
        compiler_path = shutil.which(compiler_name)
        if compiler_path is not None:
            compiler_path = _make_path(compiler_path)
        for linker in linkers:
            name = f"{compiler_name}/{linker}"
            if compiler_path is None:
                records.append(_unavailable(compiler_name, linker, reason=f"compiler not found: {compiler_name}"))
                continue
            env = _toolchain_environment(make_path, python)
            linker_path, linker_version = _resolve_linker(compiler_path, linker, env=env)
            if linker_path is None or linker_version is None:
                records.append(_unavailable(compiler_name, linker, reason=f"linker not available: {linker}"))
                continue
            linker_path = _make_path(linker_path)
            build_root = (output / compiler_name / linker).resolve()
            try:
                record = _measure_pair(
                    name, compiler_name, compiler_path, linker, linker_path, linker_version,
                    make_path, python, build_root, repeats, env=env,
                )
            except CompareError as exc:
                raise CompareError(f"{name}: {exc}") from exc
            records.append(record)
    report = {
        "schema": SCHEMA,
        "issue": ISSUE,
        "public_source_owned": True,
        "repeats_requested": repeats,
        "host_tools": {
            "make_path": make_path,
            "make_version": _full_version([make_path, "--version"], env=os.environ.copy()),
            "python_path": python,
            "python_version": _full_version([python, "--version"], env=os.environ.copy()),
        },
        "measurement_method": {
            "compile_wall_time": "sum of serial compiler-driver wall times for generated C sources under each isolated trial root",
            "peak_memory": f"sampled aggregate process-tree working set; {int(SAMPLE_INTERVAL_SECONDS * 1000)} ms interval",
            "link_wall_time": "sum of serial compiler-driver wall times for link invocations under each isolated trial root",
            "runtime_workloads": "existing #282 run_perf_benchmarks matrix after the complete veto, with a row-specific BUILD_ROOT where honored, sequential fixed-tree platform-ladder workloads, and the row's compiler/linker flags",
            "semantic_hash": "SHA-256 of combined stdout/stderr from repeated fixture targets after normalizing build roots, host addresses and host timing fields",
            "inputs": "clears ambient game, title, codegen, SR and NK overrides; uses only pinned source-owned fixtures and toolchain paths",
        },
        "toolchains": records,
    }
    report["summary"] = summarize(report)
    report["semantic_hash_comparison"] = _semantic_hash_comparison(records)
    validate_report(report)
    return report


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260),
    ]


class _PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


_PROCESS_APIS: tuple[Any, Any] | None = None


def _process_apis() -> tuple[Any, Any]:
    """Load and bind the process-sampling APIs once, on the first Windows sample."""
    global _PROCESS_APIS
    if sys.platform != "win32":
        raise OSError("process sampling through kernel32 and psapi is implemented for Windows only")
    if _PROCESS_APIS is None:
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]
        kernel.Process32FirstW.restype = wintypes.BOOL
        kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]
        kernel.Process32NextW.restype = wintypes.BOOL
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(_PROCESS_MEMORY_COUNTERS_EX), wintypes.DWORD,
        ]
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        _PROCESS_APIS = (kernel, psapi)
    return _PROCESS_APIS


def _process_tree_working_set(root_pid: int) -> int:
    if sys.platform != "win32":
        raise OSError("process-tree working-set sampling is currently implemented for Windows")
    kernel, psapi = _process_apis()
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateToolhelp32Snapshot failed")
    try:
        parents: dict[int, int] = {}
        entry = _PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            parents[int(entry.th32ProcessID)] = int(entry.th32ParentProcessID)
            ok = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    children: dict[int, list[int]] = {}
    for pid, parent in parents.items():
        children.setdefault(parent, []).append(pid)
    descendants = {root_pid}
    pending = deque((root_pid,))
    while pending:
        parent = pending.popleft()
        for child in children.get(parent, ()):
            if child not in descendants:
                descendants.add(child)
                pending.append(child)
    total = 0
    for pid in descendants:
        process = kernel.OpenProcess(0x0410, False, pid)
        if not process:
            continue
        try:
            counters = _PROCESS_MEMORY_COUNTERS_EX()
            counters.cb = ctypes.sizeof(counters)
            if psapi.GetProcessMemoryInfo(process, ctypes.byref(counters), counters.cb):
                total += int(counters.WorkingSetSize)
        finally:
            kernel.CloseHandle(process)
    if total <= 0:
        raise OSError("no readable working-set data for the compiler process tree")
    return total


def _sample_process_tree(root_pid: int, stop: threading.Event, result: list[int]) -> None:
    while not stop.is_set():
        try:
            result.append(_process_tree_working_set(root_pid))
        except OSError:
            pass
        stop.wait(SAMPLE_INTERVAL_SECONDS)
    try:
        result.append(_process_tree_working_set(root_pid))
    except OSError:
        pass


def _append_event(path: Path, event: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = (json.dumps(event, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
    descriptor = os.open(path, os.O_CREAT | os.O_APPEND | os.O_WRONLY, 0o600)
    try:
        os.write(descriptor, encoded)
    finally:
        os.close(descriptor)


def _classify_compiler_args(arguments: list[str], build_root: Path) -> tuple[str, bool]:
    if "-c" in arguments:
        category = "compile"
    elif "-o" in arguments and any(Path(arg).suffix.lower() in (".o", ".a", ".c", ".obj", ".exe")
                                   for arg in arguments):
        category = "link"
    else:
        return "other", False
    root = build_root.resolve()
    generated = False
    for argument in arguments:
        if not argument.lower().endswith(".c") or not GENERATED_C_NAME.search(argument):
            continue
        source = Path(argument)
        if not source.is_absolute():
            source = Path.cwd() / source
        try:
            if source.resolve().is_relative_to(root):
                generated = True
                break
        except OSError:
            continue
    return category, generated


def _compiler_wrapper(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--_compiler-wrapper", action="store_true")
    parser.add_argument("--compiler", required=True)
    if "--" in argv:
        separator = argv.index("--")
        parsed, _unknown = parser.parse_known_args(argv[:separator])
        command_args = argv[separator + 1:]
    else:
        parsed, command_args = parser.parse_known_args(argv)
    if not parsed._compiler_wrapper:
        raise CompareError("compiler wrapper was invoked without its internal mode flag")
    compiler_path = parsed.compiler
    command = [compiler_path, *command_args]
    started = time.perf_counter()
    process = subprocess.Popen(command)
    samples: list[int] = []
    stop = threading.Event()
    sampler = threading.Thread(target=_sample_process_tree, args=(process.pid, stop, samples), daemon=True)
    sampler.start()
    returncode = process.wait()
    finished = time.perf_counter()
    stop.set()
    sampler.join()
    duration = finished - started
    build_root = Path(os.environ.get("NK_COMPILER_COMPARE_BUILD_ROOT", ROOT / "build")).resolve()
    category, generated = _classify_compiler_args(command_args, build_root)
    event_path = os.environ.get("NK_COMPILER_COMPARE_EVENTS")
    if event_path:
        event = {
            "compiler": compiler_path,
            "argv": command_args,
            "category": category,
            "generated_c": generated,
            "duration_s": duration,
            "peak_working_set_bytes": max(samples) if samples else None,
            "memory_method": "sampled_process_tree_working_set",
        }
        _append_event(Path(event_path), event)
    return int(returncode)


def main(argv: list[str] | None = None) -> int:
    args_list = list(sys.argv[1:] if argv is None else argv)
    if "--_compiler-wrapper" in args_list:
        return _compiler_wrapper(args_list)
    parser = argparse.ArgumentParser(
        description="Compare host C compilers on a synthetic generated-C corpus behind a semantic veto")
    parser.add_argument("--compilers", default=",".join(DEFAULT_COMPILERS),
                        help="comma-separated compiler commands resolved against PATH")
    parser.add_argument("--linkers", default=",".join(DEFAULT_LINKERS),
                        help="comma-separated linker factors: default,lld")
    parser.add_argument("--make", default=os.environ.get("MAKE", "mingw32-make"))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--repeats", type=int, default=MIN_REPEATS,
                        help=f"measurement repetitions; minimum {MIN_REPEATS}")
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "compiler-compare")
    args = parser.parse_args(args_list)
    compilers = [item.strip() for item in args.compilers.split(",") if item.strip()]
    linkers = [item.strip() for item in args.linkers.split(",") if item.strip()]
    if not compilers:
        parser.error("--compilers must name at least one compiler")
    if not linkers:
        parser.error("--linkers must name at least one linker")
    if args.repeats < MIN_REPEATS:
        parser.error(f"--repeats must be at least {MIN_REPEATS}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    try:
        report = compare(compilers, linkers, make=args.make, python=args.python,
                         output=output, repeats=args.repeats)
    except CompareError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    json_path = output / "compiler_compare.json"
    markdown_path = output / "compiler_compare.md"
    public_report = _redact_report_details(report)
    json_path.write_text(json.dumps(public_report, indent=2, sort_keys=True), encoding="utf-8")
    markdown = render_markdown(report)
    markdown_path.write_text(markdown, encoding="utf-8")
    print(json.dumps(public_report, indent=2, sort_keys=True))
    print(markdown)
    # A semantic hash that changes between repeats (MEASURED_HASH_UNSTABLE) or differs across the
    # toolchains (DIFFERENT) means the generated C does not behave the same way: the run fails.
    return 0 if report["summary"][STATUS_MEASURED] and not (
        report["summary"][STATUS_NOT_AVAILABLE] or report["summary"][STATUS_FAILED_VETO]
        or report["summary"][STATUS_FAILED_MEASUREMENT] or report["summary"][STATUS_WORKLOAD_FAILED]
        or report["summary"][STATUS_MEASURED_HASH_UNSTABLE]
        or report["semantic_hash_comparison"]["status"] == "DIFFERENT"
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
