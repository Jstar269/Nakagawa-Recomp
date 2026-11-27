#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run only the local gates that apply to the files you changed.

``make contrib-check`` is the local fast path for a pull request.  The full
``make check`` and ``make readiness`` remain the authoritative gates; this
target exists because a contributor who must run the whole native and tooling
matrix to find out whether a one-line documentation fix is well formed gives up
instead of opening the pull request.

It does not classify the change with a second, private copy of the rules: the
surfaces come from ``tools/ci_paths.py``, the same classifier the hosted
workflow uses, and are printed.  Each local gate then checks only the files it
can check here (ruff the changed Python, the strict compile the changed C
sources), so a surface hosted CI builds that no local gate covers, such as a
header-only native change, is reported ``NOT_RUN`` with that reason.  Each
gate reports one of ``PASS``, ``FAIL``, ``SKIP`` (the tool is not installed) or
``NOT_RUN`` (the change does not touch that surface).  A gate that cannot run is
never reported as a pass.

The strict C compile builds each changed source with the include paths and
defines the Makefile compiles it with (``STRICT_C_UNITS``), plus ``-Werror``.
A source whose SDK header is not installed is ``SKIP``; one that includes a
header only a build step writes, when that step has not run, is ``NOT_RUN``.

The Python gate names failing unittest cases alongside its short output excerpt.
Each unittest process gets a time limit scaled to its measured work; with
``CONTRIB_TEST_JOBS`` above 1 the modules run in that many parallel processes
planned from the measured weights (``plan_unit_batches``).
The publication audit's provenance self-consistency leg compares the working
tree against the checked-in ledger, which only a maintainer holding the private
trusted ledger can regenerate.  Those findings are therefore reported as
``MAINTAINER-SIDE`` and do not fail a local run; every other publication
finding does.  Both lists are always printed, so nothing is hidden behind the
split.
"""

from __future__ import annotations

import argparse
import ast
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from functools import lru_cache
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from . import ci_paths, ci_test_shards  # noqa: E402
    from .vulkan_sdk import VulkanSdkError, discover_vulkan_sdk  # noqa: E402
except ImportError:  # pragma: no cover - direct script execution
    import ci_paths  # type: ignore[no-redef]  # noqa: E402
    import ci_test_shards  # type: ignore[no-redef]  # noqa: E402
    from vulkan_sdk import VulkanSdkError, discover_vulkan_sdk  # type: ignore[no-redef]  # noqa: E402

#: Findings the publication audit raises only because the generated controls
#: have not been regenerated yet.  The ledger is maintained outside the
#: repository, so a contributor cannot clear these and must not be asked to.
MAINTAINER_SIDE_CODES = (
    "PROVENANCE_CONTENT_MISMATCH",
    "PROVENANCE_UNVERIFIED",
    "POLICY_EXPORT_STALE",
)

#: The shape of one publish_audit finding line: ``CODE: path: detail``.
AUDIT_FINDING = re.compile(r"^[A-Z][A-Z0-9_]+: ")

STRICT_C_FLAGS = (
    "-fsyntax-only", "-std=c11", "-Wall", "-Wextra", "-Werror",
    # Every include directory below is one the Makefile puts on the compile line
    # of the sources that need it: CFLAGS (`src/rt`, `src/core`), PLAYER_INCLUDES
    # (`src/player`, `src/player/ui`, `src/ui`, `third_party/clay`,
    # `src/core/generated`), RT_NK_UI_FLAGS (`src/rt/overlay`), the Ed25519
    # selftest (`third_party/monocypher`), HLE_INCLUDES (the ATRAC3+ subset) and
    # the achievements build (`third_party/rcheevos/include`, which rcheevos's own
    # sources and every caller include by bare name).
    # A `tests/native/*.c` unit lives outside `src/`, so a bare include of a
    # player or UI header resolves only with that directory on the path.
    "-Isrc/rt", "-Isrc/core",
    "-Isrc/player", "-Isrc/player/ui", "-Isrc/ui", "-Isrc/rt/overlay",
    "-Isrc/core/generated", "-Ithird_party/clay", "-Ithird_party/monocypher",
    "-Isrc/rt/atrac3p", "-Isrc/rt/atrac3p/libavcodec", "-Isrc/rt/atrac3p/libavutil",
    "-Ithird_party/rcheevos/include",
)


@dataclass(frozen=True)
class GeneratedHeader:
    """A header a source includes that only a build step writes.

    *directory* is where the Makefile writes it, spelled with ``{build_root}``
    and ``{build_dir}`` (the Makefile's ``BUILD_ROOT`` and ``BUILD_DIR``
    defaults, overridable from the environment the way make reads them).
    *include* is the ``-I`` spelling on the recipe that compiles the source, and
    *producer* is what a contributor runs to create the header.
    """

    name: str
    directory: str
    include: str
    producer: str


@dataclass(frozen=True)
class StrictUnit:
    """The extra flags the build compiles one source with.

    *recipes* name the Makefile rules (their target text) whose compile line
    these flags come from; *builder* names a ``(path, function)`` Python
    helper when a tool builds the source directly instead of a Makefile rule.
    ``tools/test_contrib_check.py`` reads those recipes or that builder and
    fails if a define, include or warning listed here drifts from the build.
    """

    recipes: tuple[str, ...] = ()
    builder: tuple[str, str] | None = None
    defines: tuple[str, ...] = ()
    includes: tuple[str, ...] = ()
    #: Include folders the build puts before the common ones, in its order.
    #: The compiler searches ``-I`` folders left to right, so a folder whose
    #: header shadows one under ``src/rt`` (the H.264 subset's ``config.h``)
    #: must come first, as it does on the build's own line.
    leading_includes: tuple[str, ...] = ()
    #: ``-Wno-*`` suppressions the build owns, with their reason, for this
    #: source's warning-audited build.  Only the ones this source needs.
    warnings: tuple[str, ...] = ()
    generated: tuple[GeneratedHeader, ...] = ()


#: Sources the Makefile compiles with more than the common flags.  A selftest
#: defines the hooks it is built against (`SR_HLE_THREAD_SELFTEST`, ...), and a
#: header it includes is declared only under that define, so compiling it bare
#: reports implicit declarations that the real build never sees.
STRICT_C_UNITS: dict[str, StrictUnit] = {
    "tests/native/hle_thread_selftest.c": StrictUnit(
        recipes=("hle-thread-selftest-build", "$(PSP_ORACLE_SMOKE_EXE)"),
        defines=("SR_HLE_THREAD_SELFTEST", "SR_CORO_LIFECYCLE_TEST", "SR_SCHED_LIVENESS_TEST"),
        # The Makefile's audited suppression list for this source: the
        # intr_conformance.h IcProbe rows omit the trailing `skip` field on purpose.
        warnings=("-Wno-missing-field-initializers",),
    ),
    "tests/native/sched_selftest.c": StrictUnit(
        recipes=("sched-selftest-one",),
        defines=("SR_SCHED_LIVENESS_TEST",),
    ),
    "tests/native/gpu_coherence_selftest.c": StrictUnit(
        recipes=("$(GPU_COHERENCE_BIN)",),
        defines=("SR_GPU_COHERENCE_SELFTEST",),
    ),
    # The UI regression harness is empty without its define; only the UI-test
    # player links it.
    "src/player/player_ui_test.c": StrictUnit(
        recipes=("$(PLAYER_UI_TEST_EXE)",),
        defines=("NK_PLAYER_UI_REGRESSION_TEST",),
    ),
    # The RA bridge uses pinned player-only rcheevos headers. Those include roots come from
    # the player recipe, not the generic native-core include set.
    "src/core/nk_achievements.c": StrictUnit(
        recipes=("$(PLAYER_EXE)",),
        includes=("third_party/rcheevos/include", "third_party/rcheevos/src"),
    ),
    # The bridge's selftest reads rcheevos' version from rc_version.h, which is in its src.
    "tests/native/test_nk_achievements.c": StrictUnit(
        recipes=("achievements-selftest",),
        includes=("third_party/rcheevos/include", "third_party/rcheevos/src"),
    ),
    "tests/native/atrac3p_bridge_selftest.c": StrictUnit(
        recipes=("atrac3p-bridge-selftest",),
        includes=("tests/native",),
    ),
    "src/rt/vfpu_oracle_host.c": StrictUnit(
        recipes=("$(PSP_VFPU_ORACLE_EXE)",),
        includes=("tests/fixtures/vfpu_oracle",),
    ),
    # FSR 3's host and pass read AMD's resource identifiers from the imported SDK headers.
    "src/rt/fsr3_host.c": StrictUnit(
        recipes=("fsr3-host-selftest",),
        includes=("third_party/fidelityfx_sdk/upscalers/fsr3/include/gpu",),
    ),
    "src/rt/gpu_sdl3vk/fsr3_pass.c": StrictUnit(
        recipes=("fsr3-pass-selftest",),
        includes=("third_party/fidelityfx_sdk/upscalers/fsr3/include/gpu",),
    ),
    # So do frame generation's, from its own headers.
    "src/rt/fsr3_fg_host.c": StrictUnit(
        recipes=("fsr3-fg-host-selftest",),
        includes=("third_party/fidelityfx_sdk/framegeneration/fsr3/include/gpu",),
    ),
    "src/rt/gpu_sdl3vk/fsr3_fg_pass.c": StrictUnit(
        recipes=("fsr3-fg-pass-selftest",),
        includes=("third_party/fidelityfx_sdk/framegeneration/fsr3/include/gpu",),
    ),
    "src/rt/title_config.c": StrictUnit(
        recipes=("$(BUILD_DIR)/title_config.o",),
        generated=(GeneratedHeader(
            "sr_title_config.h", "{build_dir}", "-I$(TITLE_CONFIG_DIR)",
            "make {build_dir}/sr_title_config.h",
        ),),
    ),
    "tests/native/vfpu_interp_selftest.c": StrictUnit(
        recipes=("vfpu-interp-selftest",),
        defines=("SR_FLIGHT_RECORDER_STANDALONE",),
        generated=(GeneratedHeader(
            "vfpu_overlap_diff_cases.h", "{build_dir}", "-I$(BUILD_DIR)",
            "make vfpu-interp-selftest",
        ),),
    ),
    "src/rt/vfpu_fuzz.c": StrictUnit(
        recipes=("$(BUILD_DIR)/vfpu_fuzz.o",),
        defines=("SR_SDL3VK",),
        generated=(GeneratedHeader(
            "vfpu_fuzz_cases.h", "{build_dir}", "-I$(BUILD_DIR)", "make vfpu_fuzz",
        ),),
    ),
    "tests/fixtures/cosim/cosim_selftest.c": StrictUnit(
        recipes=("cosim-selftest-run",),
        defines=("SR_INSTRUCTION_TRACE",),
        # cosim-selftest generates the fixture into $(BUILD_ROOT)/cosim and runs
        # cosim-selftest-run with BUILD_DIR=$(COSIM_DIR) and GAME_NAME=cosim.
        generated=(
            GeneratedHeader(
                "cosim_recomp_funcs.h", "{build_root}/cosim", "-I$(BUILD_DIR)",
                "make cosim-selftest",
            ),
            GeneratedHeader(
                "cosim_cells.h", "{build_root}/cosim/fixture", "-I$(COSIM_FIXTURE)",
                "make cosim-selftest",
            ),
            GeneratedHeader(
                "cosim_fpu_corpus.h", "{build_root}/cosim/fixture", "-I$(COSIM_FIXTURE)",
                "make cosim-selftest",
            ),
        ),
    ),
    # The CHD reader and its test include libchdr's headers, which only the
    # recipes that link libchdr put on the path ($(LIBCHDR_INC)).
    "src/core/nk_disc_chd.c": StrictUnit(
        recipes=("native-core-tests",),
        includes=("third_party/libchdr/include",),
    ),
    "tests/native/test_disc_expand.c": StrictUnit(
        recipes=("native-core-tests",),
        includes=("third_party/libchdr/include",),
    ),
    # The texture-pack selftest counts sort comparisons and inflater builds;
    # nk_zip.h declares those counters only under these defines. It also
    # swaps a pack's ZIP through texrepl.c's NK_TEXREPL_TEST_HOOKS seam.
    "tests/native/texrepl_selftest.c": StrictUnit(
        recipes=("texrepl-selftest",),
        defines=("NK_SORT_COUNT_COMPARES", "NK_INFLATE_COUNT_BUILDS", "NK_TEXREPL_TEST_HOOKS"),
    ),
    # The standalone GE VRAM decoder is compiled by tools/nk_cli.py
    # (_build_vram_decoder) with -DSR_GE_VRAM_DECODER_CLI and the common
    # include paths (-Isrc/rt, -Isrc/core, -Isrc/core/generated, -Isrc/player).
    "src/rt/ge_vram_cli.c": StrictUnit(
        builder=("tools/nk_cli.py", "_build_vram_decoder"),
        defines=("SR_GE_VRAM_DECODER_CLI",),
    ),
    # The project's own wrapper over the FFmpeg H.264 subset keeps the project
    # warning set, but its rule puts the subset folder first (-I$(H264_SUB_DIR)
    # before $(CFLAGS)) so the subset's config.h wins over any other, and adds
    # FFmpeg's build defines (H264_SUB_BUILD_FLAGS).
    "src/rt/h264_ffmpeg_sub/nk_h264_api.c": StrictUnit(
        recipes=("$(BUILD_DIR)/h264sub_nk_h264_api.o",),
        leading_includes=("src/rt/h264_ffmpeg_sub",),
        defines=("HAVE_AV_CONFIG_H", "_ISOC99_SOURCE", "_FILE_OFFSET_BITS=64", "_LARGEFILE_SOURCE",
                 "_POSIX_C_SOURCE=200112", "_XOPEN_SOURCE=600"),
    ),
}


@dataclass(frozen=True)
class VendoredBuild:
    """How the build compiles the upstream files of one vendored source set.

    The build holds imported upstream code to upstream's warning policy, not
    the project's: `-w` for the H.264 subset, no warning flags for libchdr, and
    the runtime's `-Wall -Wextra` without `-Werror` for the ATRAC3/ATRAC3+
    subset.  The common strict line (`-std=c11 ... -Werror`) failed every
    change to such a file on upstream warnings the build never treats as
    errors.  *recipes* name the Makefile pattern rules that compile the set;
    *flags* are the standard, warning, define and include flags those rules
    pass in the default build (no opt-in debug or trace switch), minus
    optimisation and dependency flags, with the include folders in the rule's
    order.  ``tools/test_contrib_check.py``
    fails if they drift from the rules.
    """

    recipes: tuple[str, ...]
    flags: tuple[str, ...]


#: Vendored source sets, keyed by the folder that holds their `PROVENANCE.md`.
#: Only a `.c` file that manifest lists as imported (byte-identical, or a LOCAL
#: DELTA) takes these flags; a project-authored file in the same folder, such
#: as `atrac3p_api.c` or `nk_h264_api.c`, keeps the strict line.
VENDORED_BUILDS: dict[str, VendoredBuild] = {
    "src/rt/atrac3p/": VendoredBuild(
        recipes=("$(BUILD_DIR)/atrac3p_%.o",),
        flags=("-Wall", "-Wextra", "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
               "-DSR_FLIGHT_RECORDER_LINKED", "-DPERF_AOT_INSTRUCTIONS=0", "-DSR_PUBLIC_SAFE",
               "-Isrc/rt", "-Isrc/core", "-Isrc/rt/atrac3p",
               "-Isrc/rt/atrac3p/libavcodec", "-Isrc/rt/atrac3p/libavutil"),
    ),
    "src/rt/h264_ffmpeg_sub/": VendoredBuild(
        recipes=("$(BUILD_DIR)/h264sub_%.o",),
        flags=("-Isrc/rt/h264_ffmpeg_sub", "-Isrc/rt", "-Isrc/core", "-Wall", "-Wextra", "-w", "-std=c11",
               "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
               "-DSR_FLIGHT_RECORDER_LINKED", "-DPERF_AOT_INSTRUCTIONS=0", "-DSR_PUBLIC_SAFE",
               "-DHAVE_AV_CONFIG_H",
               "-D_ISOC99_SOURCE", "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE",
               "-D_POSIX_C_SOURCE=200112", "-D_XOPEN_SOURCE=600",
               "-include", "src/rt/h264_ffmpeg_sub/nk_h264_symbols.h"),
    ),
    "third_party/libchdr/": VendoredBuild(
        recipes=("$(LIBCHDR_OBJ_DIR)/%.o",),
        flags=("-std=c99", "-Ithird_party/libchdr/include",
               "-Ithird_party/libchdr/deps/lzma-26.02/include",
               "-Ithird_party/libchdr/deps/miniz-3.1.2",
               "-Ithird_party/libchdr/deps/zstd-1.5.7",
               "-DMINIZ_NO_ARCHIVE_APIS", "-DMINIZ_NO_DEFLATE_APIS",
               "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME"),
    ),
}

#: A `PROVENANCE.md` file-manifest row: the file, then its kind.
_MANIFEST_ROW = re.compile(r"^\|\s*`([^`]+)`\s*\|\s*([^|]*?)\s*\|", re.M)
#: The kinds that mark upstream bytes: an import, kept as is or with a LOCAL DELTA.
_IMPORTED_KIND = re.compile(r"^(?:imported\b|LOCAL DELTA\b)")


@lru_cache(maxsize=None)
def _manifest_imports(folder: str) -> frozenset[str]:
    """The files, relative to *folder*, that its `PROVENANCE.md` lists as imported."""
    manifest = ROOT / folder / "PROVENANCE.md"
    if not manifest.is_file():
        return frozenset()
    text = manifest.read_text(encoding="utf-8", errors="replace")
    return frozenset(name for name, kind in _MANIFEST_ROW.findall(text) if _IMPORTED_KIND.match(kind))


def vendored_build(source: str) -> VendoredBuild | None:
    """The vendored build of *source*, if its manifest lists it as imported upstream code."""
    for folder, build in VENDORED_BUILDS.items():
        if source.startswith(folder) and source[len(folder):] in _manifest_imports(folder):
            return build
    return None

#: A missing header from an SDK the build discovers rather than vendors.  The
#: compile cannot run without it, so the file is SKIP (never PASS, never FAIL).
_MISSING_HEADER = re.compile(r"fatal error: ([^:\s]+): No such file or directory")
_REAL_ERROR = re.compile(r"(?<!fatal )\berror:")
_SDK_HEADER_PREFIXES = {
    "vulkan/": "the Vulkan SDK",
    "vk_video/": "the Vulkan SDK",
    "SDL3/": "the SDL3 development headers",
    "SDL3_ttf/": "the SDL3_ttf development headers",
}
#: PSPSDK headers (`pspkernel.h`, `psputility_osk.h`, ...) sit at the include
#: root with no directory.  A letter or digit must follow `psp`, so the
#: project's own `psp_*` headers are never mistaken for the SDK.
_PSPSDK_HEADER = re.compile(r"psp[a-z0-9]+(?:_[a-z0-9]+)*\.h")

#: A shared tool is exercised by more than the module named after it.  These
#: extra modules are the ones whose failures the hosted Python gate attributes
#: to a change in the shared tool, so the local run must too.
SHARED_TOOL_TESTS = {
    "tools/public_export.py": ("tools.test_public_export", "tools.test_publish_audit"),
    "tools/build_public_export.py": ("tools.test_public_export",),
    "tools/publish_audit.py": ("tools.test_publish_audit",),
    "tools/policy_sync.py": ("tools.test_publication_policy_gate", "tools.test_publish_audit"),
    "tools/provenance_ledger.py": ("tools.test_provenance_ledger",),
    "tools/provenance_attest_verify.py": (
        "tools.test_provenance_attest_verify", "tools.test_provenance_attestation_gate",
    ),
    "tools/provenance_refresh.py": ("tools.test_provenance_ledger",),
    "tools/publication_policy.py": ("tools.test_publication_policy_gate",),
    "tools/ci_paths.py": ("tools.test_ci_paths", "tools.test_ci_required"),
    "tools/contrib_check.py": ("tools.test_contrib_check",),
    "tools/provenance_record_gap.py": ("tools.test_provenance_record_gap",),
    # Covered by a test module that is not named after them.
    "tools/nk_core/disc_expand_tool.py": ("tools.test_disc_expand",),
    "tests/fixtures/cosim/mutate.py": ("tools.test_cosim_mutation_gate_outcome",),
}

#: Source trees whose files may be read, compiled, or mutated directly by a
#: ``tools/test_*.py`` module.
SOURCE_TEST_ROOTS = ("src/", "tests/native/")

#: Inline ``#include "<file>"`` inside generated C test harnesses.  When the
#: included file is a ``.c`` or ``.inc`` unit that resolves uniquely under
#: ``SOURCE_TEST_ROOTS``, the module compiles that unit and must run when it
#: changes.
_INLINE_C_INCLUDE = re.compile(r'#\s*include\s*"([^"]+)"')

#: ``tools/test_*.py`` modules that mention literal paths under ``src/`` or
#: ``tests/native/`` but are intentionally NOT selected by ``contrib-check``
#: when those source files change.  Every exclusion carries its explicit reason
#: and is verified by ``tools/test_contrib_check.py`` so no miss hides here.
SOURCE_TEST_EXCLUSIONS: dict[str, str] = {
    "tools.test_production_smoke": (
        "too slow for the local fast path (~971 s in hosted CI); runs full "
        "multi-fixture codegen and native smoke builds"
    ),
    "tools.test_ci_paths": (
        "passes source paths only as synthetic inputs to ci_paths.classify(); "
        "never reads, compiles, or mutates those files"
    ),
    "tools.test_dco_policy": (
        "passes source paths only as synthetic commit paths in DCO policy "
        "fixtures; never reads, compiles, or mutates those files"
    ),
    "tools.test_admit_paths": (
        "uses source paths only as synthetic pre-commit regex and fixture "
        "inputs; never reads, compiles, or mutates those files"
    ),
    "tools.test_lint_docs": (
        "writes synthetic src/player/main.c links in temporary Markdown "
        "fixtures; contrib-check runs tools/lint_docs.py directly on docs"
    ),
    "tools.test_public_export": (
        "uses src/rt/recomp.c only in synthetic export fixtures (~78 s); "
        "contrib-check runs policy_sync.py and publish_audit.py directly"
    ),
    "tools.test_publication_policy_gate": (
        "uses src/rt/ge.c only as a policy classification string (~42 s); "
        "contrib-check runs policy_sync.py directly"
    ),
    "tools.test_publish_audit": (
        "tests publish_audit.py with synthetic fixtures and policy checks; "
        "contrib-check runs publish_audit.py directly in publication_gate()"
    ),
    "tools.test_provenance_ledger": (
        "tests provenance_ledger.py metadata and synthetic entries; "
        "contrib-check runs publication_gate() directly"
    ),
    "tools.test_provenance_record_gap": (
        "queries independence metadata by sample path string; never reads, "
        "compiles, or mutates those C sources"
    ),
    "tools.test_contrib_check": (
        "passes sample C paths to mocked contrib-check gates and unchanged-main "
        "compiles; contrib-check already runs c_gate and purity_gate directly"
    ),
    "tools.test_hst_manager_manifest": (
        "writes synthetic text to a temporary src/rt/recomp.c (~40 s); never "
        "reads, compiles, or mutates the repository's src/rt/recomp.c"
    ),
}

PASS, FAIL, SKIP, NOT_RUN, MAINTAINER_SIDE = "PASS", "FAIL", "SKIP", "NOT_RUN", "MAINTAINER-SIDE"

#: The interpreter that runs the sub-gates. It defaults to the one running
#: this script and is passed explicitly by the make target, so a repository
#: whose default ``python`` is not the interpreter holding the project's tools
#: can say so once instead of failing gate by gate.
GATE_PYTHON = os.environ.get("CONTRIB_PYTHON") or sys.executable


def find_tool(name: str) -> str | None:
    """Locate *name* on PATH, tolerating the mingw32-make PATH shape.

    ``mingw32-make`` hands a Windows interpreter the MSYS-style PATH it was
    given (``/c/Program Files/nodejs``), which ``shutil.which`` cannot use: a
    Windows ``CreateProcess`` does not understand a leading slash-drive path.
    Without this, the Windows target silently reports SKIP for every tool while
    the same command works from the contributor's shell -- a gate that reports
    its own environment wrongly is worse than no gate.
    """
    found = shutil.which(name)
    if found:
        return found
    if os.name != "nt":
        return None
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        match = re.fullmatch(r"/([A-Za-z])/(.+?)/?", entry.strip())
        if not match:
            continue
        windows = f"{match.group(1).upper()}:\\{match.group(2).replace('/', os.sep)}"
        for suffix in (".exe", ".cmd", ".bat", ""):
            candidate = Path(windows) / f"{name}{suffix}"
            if candidate.is_file():
                return str(candidate)
    return None


class Gate:
    """One gate's result. ``detail`` is what is shown; ``output`` is a failed command's whole output, which
    a caller that classifies the failure reads (``detail`` keeps only its last lines)."""

    __slots__ = ("name", "status", "detail", "output", "unchecked")

    def __init__(self, name: str, status: str, detail: str = "", output: str = "",
                 *, unchecked: bool = False) -> None:
        self.name = name
        self.status = status
        self.detail = detail
        self.output = output
        #: A NOT_RUN that leaves a changed surface unchecked locally, rather than one the
        #: change does not touch; the summary names it next to the skipped gates.
        self.unchecked = unchecked


def _git(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, check=check,
    )


def changed_paths(base: str) -> list[str]:
    """Committed changes against *base* plus anything still uncommitted."""
    # An unknown base (origin/main never fetched) or one with no common ancestor makes
    # `git merge-base` exit non-zero; that is the named stop below, not a traceback.
    found = _git("merge-base", base, "HEAD", check=False)
    merge_base = found.stdout.strip()
    if found.returncode != 0 or not merge_base:
        why = found.stderr.strip() or (
            "it has no common ancestor with HEAD" if found.returncode == 1
            else f"git merge-base exited {found.returncode}")
        raise SystemExit(f"contrib-check: cannot resolve a merge base with {base}: {why}")
    # NUL-separated output: paths are never quoted or split on spaces. In
    # `status --porcelain=v1 -z` each entry is one field "XY path"; a rename or
    # copy is followed by one more field holding its source path.
    committed = _git("diff", "--name-only", "-z", "--find-renames", merge_base, "HEAD").stdout.split("\0")
    status = _git("status", "--porcelain=v1", "-z", "--untracked-files=all", check=False).stdout
    working: list[str] = []
    entries = status.split("\0")
    index = 0
    while index < len(entries):
        entry = entries[index]
        index += 1
        if len(entry) < 4:
            continue
        code, name = entry[:2], entry[3:]
        if code[0] in "RC":  # a rename or copy: the next entry is the source path
            index += 1
        if "D" in code:  # deleted in the index or the worktree: nothing to check
            continue
        working.append(name)
    return sorted({path for path in committed + working if path})


def _normalise_repo_path(raw: str) -> str:
    normalised = raw.replace("\\", "/").strip("/")
    while normalised.startswith("./"):
        normalised = normalised[2:]
    return normalised


def _extract_source_prefix(joined: str) -> str | None:
    for root_dir in ("src", "tests/native"):
        if joined == root_dir:
            return root_dir
        marker = root_dir + "/"
        if joined.startswith(marker):
            return joined
        idx = joined.find("/" + marker)
        if idx != -1:
            return joined[idx + 1:]
    return None


@lru_cache(maxsize=4)
def _existing_source_files(root: Path) -> tuple[frozenset[str], dict[str, tuple[str, ...]]]:
    """Return every file under ``src/`` and ``tests/native/`` plus a basename map."""
    existing: set[str] = set()
    by_basename: dict[str, list[str]] = {}
    for base in ("src", "tests/native"):
        base_dir = root / base
        if not base_dir.is_dir():
            continue
        for dirpath, _dirnames, filenames in os.walk(base_dir):
            rel_dir = Path(dirpath).relative_to(root).as_posix()
            for filename in filenames:
                rel_path = f"{rel_dir}/{filename}"
                existing.add(rel_path)
                by_basename.setdefault(filename, []).append(rel_path)
    return (
        frozenset(existing),
        {name: tuple(sorted(paths)) for name, paths in by_basename.items()},
    )


@lru_cache(maxsize=4)
def _test_module_texts(root: Path) -> tuple[tuple[str, Path, str], ...]:
    """Read ``tools/test_*.py`` sources once per root for literal path matching."""
    tools_dir = root / "tools"
    if not tools_dir.is_dir():
        return ()
    entries: list[tuple[str, Path, str]] = []
    for path in sorted(tools_dir.glob("test_*.py")):
        if not path.is_file():
            continue
        entries.append((f"tools.{path.stem}", path, path.read_text(encoding="utf-8")))
    return tuple(entries)


def _docstring_node_ids(tree: ast.AST) -> set[int]:
    docs: set[int] = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.body and isinstance(node.body[0], ast.Expr):
                value = node.body[0].value
                if isinstance(value, ast.Constant) and isinstance(value.value, str):
                    docs.add(id(value))
    return docs


def _resolve_path_div(node: ast.BinOp, env: dict[str, str]) -> str | None:
    parts: list[str] = []
    cur: ast.AST = node
    while isinstance(cur, ast.BinOp) and isinstance(cur.op, ast.Div):
        if isinstance(cur.right, ast.Constant) and isinstance(cur.right.value, str):
            parts.append(cur.right.value)
        else:
            return None
        cur = cur.left
    if isinstance(cur, ast.Constant) and isinstance(cur.value, str):
        parts.append(cur.value)
    elif isinstance(cur, ast.Name) and cur.id in env:
        parts.append(env[cur.id])
    elif isinstance(cur, ast.Attribute) and cur.attr in env:
        parts.append(env[cur.attr])
    parts.reverse()
    joined = "/".join(_normalise_repo_path(p) for p in parts if p)
    return _extract_source_prefix(joined)


def _resolve_path_call(node: ast.Call, env: dict[str, str]) -> str | None:
    fn = node.func
    base: str | None = None
    if isinstance(fn, ast.Attribute) and fn.attr == "joinpath":
        if isinstance(fn.value, ast.Name) and fn.value.id in env:
            base = env[fn.value.id]
        elif isinstance(fn.value, ast.BinOp) and isinstance(fn.value.op, ast.Div):
            base = _resolve_path_div(fn.value, env)
    elif isinstance(fn, ast.Attribute) and fn.attr == "join":
        pass
    elif isinstance(fn, ast.Name) and fn.id in {"Path", "PurePosixPath", "PurePath"}:
        pass
    else:
        return None
    parts: list[str] = [base] if base else []
    for index, arg in enumerate(node.args):
        if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
            parts.append(arg.value)
        elif isinstance(arg, ast.Name) and arg.id in env:
            parts.append(env[arg.id])
        elif index == 0 and base is None:
            # Leading anchor such as ROOT or str(ROOT) in os.path.join(ROOT, "src", ...)
            continue
        else:
            return None
    if not parts:
        return None
    joined = "/".join(_normalise_repo_path(p) for p in parts if p)
    return _extract_source_prefix(joined)


@lru_cache(maxsize=512)
def _module_source_paths(root: Path, module_path: Path, text: str) -> frozenset[str]:
    """Extract existing ``src/`` and ``tests/native/`` paths referenced by *module_path*."""
    existing, by_basename = _existing_source_files(root)
    tree = ast.parse(text, filename=str(module_path))
    docs = _docstring_node_ids(tree)
    env: dict[str, str] = {}
    assigns = [n for n in ast.walk(tree) if isinstance(n, ast.Assign) and len(n.targets) == 1]
    for _ in range(2):
        for assign in assigns:
            target = assign.targets[0]
            name = (
                target.id if isinstance(target, ast.Name)
                else target.attr if isinstance(target, ast.Attribute)
                else None
            )
            if not name:
                continue
            resolved: str | None = None
            if isinstance(assign.value, ast.BinOp) and isinstance(assign.value.op, ast.Div):
                resolved = _resolve_path_div(assign.value, env)
            elif isinstance(assign.value, ast.Call):
                resolved = _resolve_path_call(assign.value, env)
            elif isinstance(assign.value, ast.Constant) and isinstance(assign.value.value, str):
                candidate = _normalise_repo_path(assign.value.value)
                if candidate in {"src", "tests/native"} or candidate.startswith(SOURCE_TEST_ROOTS):
                    resolved = candidate
            if resolved:
                env[name] = resolved

    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if id(node) in docs:
                continue
            literal = _normalise_repo_path(node.value)
            if literal in existing:
                found.add(literal)
            if "#include" in node.value:
                for match in _INLINE_C_INCLUDE.finditer(node.value):
                    inc = match.group(1).replace("\\", "/")
                    if inc.endswith((".c", ".inc")):
                        matches = by_basename.get(inc, ())
                        if len(matches) == 1:
                            found.add(matches[0])
        elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
            div_resolved = _resolve_path_div(node, env)
            if div_resolved and div_resolved in existing:
                found.add(div_resolved)
        elif isinstance(node, ast.Call):
            call_resolved = _resolve_path_call(node, env)
            if call_resolved and call_resolved in existing:
                found.add(call_resolved)
    return frozenset(found)


def _source_tests_for_paths(paths: list[str], root: Path | None = None) -> set[str]:
    """Select ``tools/test_*.py`` modules that reference changed ``src/`` or ``tests/native/`` files."""
    active_root = ROOT if root is None else root
    targets = {
        _normalise_repo_path(path)
        for path in paths
        if _normalise_repo_path(path).startswith(SOURCE_TEST_ROOTS)
    }
    if not targets:
        return set()
    basenames = {PurePosixPath(path).name for path in targets}
    modules: set[str] = set()
    for module_name, module_path, text in _test_module_texts(active_root):
        if module_name in SOURCE_TEST_EXCLUSIONS:
            continue
        if not any(name in text for name in basenames):
            continue
        if _module_source_paths(active_root, module_path, text) & targets:
            modules.add(module_name)
    return modules


#: ``tools/test_*.py`` modules not selected when a ``tools/`` data file they name changes.
#: The source exclusions above do not apply: a module that only feeds synthetic ``src/``
#: paths to a check (``tools.test_lint_docs``) can still be the reader of a data file
#: (``tools/README.md``).
TOOLS_DATA_TEST_EXCLUSIONS: dict[str, str] = {
    "tools.test_production_smoke": SOURCE_TEST_EXCLUSIONS["tools.test_production_smoke"],
}


def _is_tools_data(path: str) -> bool:
    """A file under ``tools/`` that hosted CI routes to the Python job but is not Python:
    ``tools/**/*.json``, ``*.toml`` and ``tools/README.md`` (``ci_paths._is_python_tool``)."""
    return path.startswith("tools/") and not path.endswith(".py") and ci_paths._is_python_tool(path)


def _file_parent_depth(node: ast.AST) -> int:
    """How many ``.parent`` steps above ``__file__`` *node* is (``Path(__file__).resolve().parent``
    is 1), or 0 when it is not such an expression."""
    depth = 0
    cur = node
    while isinstance(cur, ast.Attribute) and cur.attr == "parent":
        depth += 1
        cur = cur.value
    if depth and any(isinstance(n, ast.Name) and n.id == "__file__" for n in ast.walk(cur)):
        return depth
    return 0


def _repo_path_div(node: ast.BinOp, env: dict[str, str]) -> str | None:
    """The repository path a ``base / "part" / ...`` expression in a ``tools/test_*.py`` module
    names. A base that is ``Path(__file__)...parent`` is ``tools/`` (one step) or the
    repository root (two); a name bound earlier to such a path is that path; any other base
    (``ROOT``, a temporary directory) is taken as the root, which can only over-select."""
    parts: list[str] = []
    cur: ast.AST = node
    while isinstance(cur, ast.BinOp) and isinstance(cur.op, ast.Div):
        if not (isinstance(cur.right, ast.Constant) and isinstance(cur.right.value, str)):
            return None
        parts.append(cur.right.value)
        cur = cur.left
    if isinstance(cur, ast.Constant) and isinstance(cur.value, str):
        parts.append(cur.value)
    elif isinstance(cur, ast.Name) and cur.id in env:
        parts.append(env[cur.id])
    elif _file_parent_depth(cur) == 1:
        parts.append("tools")
    parts.reverse()
    return "/".join(_normalise_repo_path(p) for p in parts if p)


#: A ``tools/...`` path written inside a string.
_TOOLS_PATH_LITERAL = re.compile(r"(?<![\w./-])tools/[\w./-]*\w")


@lru_cache(maxsize=512)
def _module_tools_paths(module_path: Path, text: str) -> frozenset[str]:
    """Repository paths under ``tools/`` a test module names: in a string (a docstring
    excluded) or as a ``/`` path expression."""
    tree = ast.parse(text, filename=str(module_path))
    docs = _docstring_node_ids(tree)
    assigns = [
        (n.targets[0].id, n.value) for n in ast.walk(tree)
        if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name)
    ]
    env: dict[str, str] = {}
    for _ in range(2):
        for name, value in assigns:
            if isinstance(value, ast.BinOp) and isinstance(value.op, ast.Div):
                resolved = _repo_path_div(value, env)
                if resolved is not None:
                    env[name] = resolved
            elif _file_parent_depth(value) == 1:
                env[name] = "tools"
    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docs:
            found.update(m.group(0) for m in _TOOLS_PATH_LITERAL.finditer(node.value.replace("\\", "/")))
        elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
            resolved = _repo_path_div(node, env)
            if resolved and resolved.startswith("tools/"):
                found.add(resolved)
    return frozenset(found)


def _tools_data_tests_for_paths(paths: list[str], root: Path | None = None) -> set[str]:
    """Select ``tools/test_*.py`` modules that name a changed ``tools/`` data file (see
    ``_is_tools_data``): the reader scan ``_source_tests_for_paths`` does for ``src/``."""
    active_root = ROOT if root is None else root
    targets = {p for p in (_normalise_repo_path(path) for path in paths) if _is_tools_data(p)}
    if not targets:
        return set()
    basenames = {PurePosixPath(path).name for path in targets}
    modules: set[str] = set()
    for module_name, module_path, text in _test_module_texts(active_root):
        if module_name in TOOLS_DATA_TEST_EXCLUSIONS:
            continue
        if not any(name in text for name in basenames):
            continue
        if _module_tools_paths(module_path, text) & targets:
            modules.add(module_name)
    return modules


def python_test_modules(paths: list[str]) -> list[str]:
    """Map changed ``tools/``, ``src/``, and ``tests/native/`` paths onto covering test modules.

    A changed ``tools/`` data file (JSON, TOML or ``tools/README.md``) selects the test modules
    that name it, as hosted CI runs the Python job for it."""
    modules: set[str] = set()
    for raw_path in paths:
        path = _normalise_repo_path(raw_path)
        name = PurePosixPath(path).name
        # A shared or differently named module outside tools/ still selects its tests.
        modules.update(SHARED_TOOL_TESTS.get(path, ()))
        if not path.startswith("tools/") or not name.endswith(".py"):
            continue
        if name.startswith("test_"):
            # A deleted test module has nothing to import.
            if (ROOT / path).is_file():
                modules.add("tools." + name[:-3])
            continue
        if (ROOT / f"tools/test_{name}").is_file():
            modules.add(f"tools.test_{name[:-3]}")
    modules.update(_source_tests_for_paths(paths, ROOT))
    modules.update(_tools_data_tests_for_paths(paths, ROOT))
    return sorted(modules)


def _run(
    label: str, command: list[str], *, timeout: int,
    failure_notes: Callable[[str], list[str]] | None = None,
) -> Gate:
    try:
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, timeout=timeout,
        )
    except FileNotFoundError:
        return Gate(label, SKIP, f"{command[0]} is not installed")
    except subprocess.TimeoutExpired:
        return Gate(label, FAIL, f"timed out after {timeout}s: {' '.join(command)}")
    if result.returncode == 0:
        return Gate(label, PASS)
    text = (result.stdout + result.stderr).strip()
    output = text.splitlines()
    detail = " | ".join(output[-6:]) or f"exit {result.returncode}"
    if failure_notes is not None:
        detail += "".join(f"\n    {note}" for note in failure_notes(result.stderr))
    return Gate(label, FAIL, detail, text)


def _python_tool(tool: str, arguments: list[str], *, timeout: int) -> Gate:
    """Run *tool* as a module of the gate interpreter, or report it missing."""
    executable = find_tool(tool)
    if executable:
        return _run(tool, [executable, *arguments], timeout=timeout)
    gate = _run(tool, [GATE_PYTHON, "-m", tool, *arguments], timeout=timeout)
    if gate.status == FAIL and "No module named" in gate.detail:
        return Gate(
            tool, SKIP,
            f"{tool} is not installed for {GATE_PYTHON}; install it or set CONTRIB_PYTHON "
            f"to the interpreter that has it",
        )
    return gate


def ruff_gate(paths: list[str]) -> Gate:
    if not paths:
        return Gate("ruff", NOT_RUN, "no changed Python")
    return _python_tool("ruff", ["check", *paths], timeout=300)


# Match unittest's own failure headings on stderr. Extract the dotted identifier
# without retaining descriptions, exceptions, or optional subtest parameters.
_UNITTEST_FAILURE = re.compile(r"^={70}\n(FAIL|ERROR): \w+ \((\w+(?:\.\w+)+)\)", re.MULTILINE)


def _unittest_failure_notes(stderr: str) -> list[str]:
    plain = re.sub(r"\x1b\[[0-9;]*m", "", stderr)
    failures: dict[str, set[str]] = {}
    for status, name in _UNITTEST_FAILURE.findall(plain):
        failures.setdefault(name, set()).add(status)
    return [f"{'/'.join(sorted(statuses))}: {name}" for name, statuses in failures.items()]


#: The time limit of one unittest process never falls below this.
UNIT_MIN_TIMEOUT = 1800
#: Local seconds allowed per measured hosted second (``tools/ci_test_weights.json``).  Those weights
#: were taken under coverage on hosted runners.  A Windows workstation ran 31 selected modules in one
#: process in 2.7 times their weight; in four parallel processes, on a machine other sessions were
#: also loading, one process took 5.0 times its own, so the limit allows six.
UNIT_TIMEOUT_PER_MEASURED_SECOND = 6
#: Measured seconds of work that justify one more parallel unittest process.
UNIT_SECONDS_PER_PROCESS = 300
#: Environment variable that sets the parallel unittest processes.  The default is one: the modules
#: share one checkout and the workstation's RAM lock grants one slot, and two parallel runs of the
#: #2981 selection each failed a different test that passes alone.
UNIT_JOBS_ENV = "CONTRIB_TEST_JOBS"


@dataclass(frozen=True)
class UnitBatch:
    """One unittest process: its modules, their measured seconds and its time limit."""

    modules: tuple[str, ...]
    planned_seconds: float
    timeout: int


def _unit_timeout(planned_seconds: float) -> int:
    return max(UNIT_MIN_TIMEOUT, math.ceil(UNIT_TIMEOUT_PER_MEASURED_SECOND * planned_seconds))


def _unit_jobs() -> int:
    raw = os.environ.get(UNIT_JOBS_ENV, "").strip()
    if raw:
        try:
            jobs = int(raw)
        except ValueError:
            jobs = 0
        if jobs < 1:
            raise ValueError(f"{UNIT_JOBS_ENV} must be a positive integer, got {raw!r}")
        return jobs
    return 1


def plan_unit_batches(modules: list[str], data: ci_test_shards.WeightData, jobs: int) -> tuple[
    tuple[UnitBatch, ...], tuple[UnitBatch, ...],
]:
    """Split *modules* into parallel unittest processes and the processes that must run alone.

    The parallel processes are packed by the hosted shard planner over the measured weights, one
    per ``UNIT_SECONDS_PER_PROCESS`` of work and at most *jobs*.  A module in a ``separate`` group
    mutates tracked repository state, which hosted runners keep apart on separate checkouts; here
    the checkout is shared, so those modules run in one process of their own after the others.
    Each process's time limit scales with its measured work.
    """
    def weight(module: str) -> float:
        return data.weights.get(module.rsplit(".", 1)[-1], data.default_seconds)

    def batch(group: list[str]) -> UnitBatch:
        planned = round(sum(weight(module) for module in group), 3)
        return UnitBatch(tuple(group), planned, _unit_timeout(planned))

    exclusive_names = frozenset().union(*data.separate) if data.separate else frozenset()
    exclusive = [module for module in modules if module.rsplit(".", 1)[-1] in exclusive_names]
    shared = [module for module in modules if module not in exclusive]
    parallel: list[UnitBatch] = []
    if shared:
        total = sum(weight(module) for module in shared)
        count = max(1, min(jobs, len(shared), math.ceil(total / UNIT_SECONDS_PER_PROCESS)))
        if count == 1:
            parallel.append(batch(shared))
        else:
            by_name = {module.rsplit(".", 1)[-1]: module for module in shared}
            unseparated = ci_test_shards.WeightData(data.default_seconds, data.weights, ())
            plan = ci_test_shards.plan_shards(list(by_name), unseparated, count)
            parallel.extend(batch([by_name[name] for name in shard]) for shard in plan.shards if shard)
    alone = (batch(exclusive),) if exclusive else ()
    return tuple(parallel), alone


def _unit_batch_gate(unit: UnitBatch) -> tuple[Gate, float]:
    started = time.monotonic()
    gate = _run(
        "python-unittest", [GATE_PYTHON, "-m", "unittest", *unit.modules], timeout=unit.timeout,
        failure_notes=_unittest_failure_notes,
    )
    return gate, time.monotonic() - started


def _timeout_detail(unit: UnitBatch) -> str:
    return (f"UNITTEST_TIME_LIMIT: {len(unit.modules)} module(s) with {unit.planned_seconds:g}s of measured "
            f"work did not finish within {unit.timeout}s: {', '.join(unit.modules)}")


def unit_gate(modules: list[str]) -> Gate:
    if not modules:
        return Gate("python-unittest", NOT_RUN, "no matching test module for changed paths")
    try:
        data = ci_test_shards.load_weight_data()
        parallel, alone = plan_unit_batches(modules, data, _unit_jobs())
    except (ci_test_shards.ShardDataError, ValueError) as exc:
        return Gate("python-unittest", FAIL, f"cannot plan the unittest processes: {exc}")
    batches = parallel + alone
    if len(parallel) > 1:
        print(f"  unittest: {len(parallel)} parallel process(es)"
              + (f", then {len(alone[0].modules)} module(s) alone" if alone else ""))
    with ThreadPoolExecutor(max_workers=max(1, len(parallel))) as pool:
        timed = list(pool.map(_unit_batch_gate, parallel))
    timed.extend(_unit_batch_gate(unit) for unit in alone)
    results = [gate for gate, _elapsed in timed]
    if len(batches) > 1:
        for unit, (gate, elapsed) in zip(batches, timed, strict=True):
            print(f"  unittest process: {len(unit.modules)} module(s), {unit.planned_seconds:g}s measured, "
                  f"limit {unit.timeout}s, took {elapsed:.0f}s, {gate.status}")
    failed = [(unit, gate) for unit, gate in zip(batches, results, strict=True) if gate.status != PASS]
    if not failed:
        return Gate("python-unittest", PASS)
    details = []
    for unit, gate in failed:
        detail = _timeout_detail(unit) if gate.detail.startswith("timed out after ") else gate.detail
        if gate.status == FAIL and len(batches) > 1 and not detail.startswith("UNITTEST_TIME_LIMIT"):
            detail = f"[{', '.join(unit.modules)}] {detail}"
        details.append(detail)
    status = FAIL if any(gate.status == FAIL for _unit, gate in failed) else failed[0][1].status
    output = "\n".join(gate.output for _unit, gate in failed if gate.output)
    return Gate("python-unittest", status, "\n".join(details), output)


def markdown_gate(paths: list[str]) -> Gate:
    markdown = [p for p in paths if ci_paths._is_markdown(p)]
    if not markdown:
        return Gate("markdownlint", NOT_RUN, "no changed Markdown")
    npx = find_tool("npx")
    if npx is None:
        return Gate("markdownlint", SKIP, "npx is not installed")
    return _run(
        "markdownlint",
        [npx, "--no-install", "markdownlint-cli2", "--no-globs", *markdown],
        timeout=600,
    )


def build_locations() -> dict[str, str]:
    """The Makefile's ``BUILD_ROOT`` and ``BUILD_DIR``, as make resolves them.

    ``BUILD_ROOT ?= build``, ``GAME_NAME ?= mygame`` and
    ``BUILD_DIR ?= $(BUILD_ROOT)/$(GAME_NAME)``; make takes each from the
    environment when it is set there, and so does this.
    """
    build_root = os.environ.get("BUILD_ROOT") or "build"
    game_name = os.environ.get("GAME_NAME") or "mygame"
    build_dir = os.environ.get("BUILD_DIR") or f"{build_root}/{game_name}"
    return {"build_root": build_root, "build_dir": build_dir}


def vulkan_include_flags() -> tuple[list[str], str]:
    """The Makefile's ``VULKAN_INC_FLAGS``, from the same SDK discovery.

    Returns the flags and, when no SDK was found, the discovery's own reason.
    On Linux the default is no SDK at all: the headers are system headers and
    the compiler finds them without a flag, exactly as the Makefile expects.
    """
    try:
        sdk = discover_vulkan_sdk()
    except VulkanSdkError as error:
        return [], str(error)
    root = sdk.as_posix()
    if os.name == "nt":
        return [f"-I{root}/Include", f"-I{root}/include"], ""
    return [f"-I{root}/include"], ""


def strict_compile_command(compiler: str, source: str, vulkan_flags: list[str]) -> tuple[list[str] | None, str]:
    """The strict compile line for *source*, or ``None`` and why it cannot run.

    It cannot run when the source includes a header that only a build step
    writes and that step has not run here.  Imported upstream code takes its
    vendored build's flags instead (`VENDORED_BUILDS`).
    """
    vendored = vendored_build(source)
    if vendored is not None:
        return [compiler, "-fsyntax-only", *vendored.flags, source], ""
    unit = STRICT_C_UNITS.get(source)
    extra: list[str] = []
    leading = [f"-I{include}" for include in unit.leading_includes] if unit is not None else []
    if unit is not None:
        extra.extend(f"-D{define}" for define in unit.defines)
        extra.extend(f"-I{include}" for include in unit.includes)
        extra.extend(unit.warnings)
        locations = build_locations()
        for header in unit.generated:
            directory = header.directory.format(**locations)
            if not (ROOT / directory / header.name).is_file():
                producer = header.producer.format(**locations)
                return None, (
                    f"needs the generated header {directory}/{header.name}, which only "
                    f"`{producer}` writes"
                )
            flag = f"-I{directory}"
            if flag not in extra:
                extra.append(flag)
    return [compiler, *leading, *STRICT_C_FLAGS, *vulkan_flags, *extra, source], ""


def _missing_sdk(output: str) -> str | None:
    """The SDK a failed compile could not find, if that is why it failed."""
    match = _MISSING_HEADER.search(output)
    if not match:
        return None
    if _REAL_ERROR.search(output):
        # The compiler also reported an error that is not the missing header,
        # so the change is broken whether or not the SDK is installed.
        return None
    header = match.group(1)
    for prefix, sdk in _SDK_HEADER_PREFIXES.items():
        if header.startswith(prefix):
            return f"{sdk} ({header}) is not installed"
    if _PSPSDK_HEADER.fullmatch(header):
        return f"the PSPSDK ({header}) is not installed"
    return None


def c_gate(paths: list[str]) -> Gate:
    sources = [
        p for p in paths
        if PurePosixPath(p).suffix.lower() == ".c" and (ROOT / p).is_file()
    ]
    if not sources:
        headers = [p for p in paths if PurePosixPath(p).suffix.lower() in (".h", ".hpp") and (ROOT / p).is_file()]
        if headers:
            # A header-only change: this gate compiles changed sources only, so the headers
            # are unchecked here and the summary says so (an OK must not read as covering them).
            return Gate("strict-c-compile", NOT_RUN,
                        f"changed header(s) not compiled: {', '.join(headers)}; hosted CI "
                        "(or a local make check) builds them", unchecked=True)
        if ci_paths.classify(paths)["native_runtime"] == "true":
            # A native change with no C file: hosted CI builds it, this local gate compiles sources only.
            return Gate("strict-c-compile", NOT_RUN,
                        "no changed C source; the native_runtime surface changed, which only hosted CI "
                        "(or a local make check) builds", unchecked=True)
        return Gate("strict-c-compile", NOT_RUN, "no changed C source")
    compiler = os.environ.get("CC") or find_tool("gcc") or find_tool("cc")
    if compiler is None:
        return Gate("strict-c-compile", SKIP, "no C compiler on PATH (set CC)")
    vulkan_flags, vulkan_missing = vulkan_include_flags()
    results: list[tuple[str, str, str]] = []
    for source in sources:
        command, reason = strict_compile_command(compiler, source, vulkan_flags)
        if command is None:
            results.append((NOT_RUN, source, reason))
            continue
        gate = _run(f"cc {source}", command, timeout=600)
        if gate.status == FAIL:
            # The whole output: a real error printed before the missing header's last lines still fails.
            sdk = _missing_sdk(gate.output)
            if sdk is not None:
                # A missing SDK header is the environment, not the change: the
                # file was not compiled, so it is a skip and never a pass.
                why = f"{sdk}; {vulkan_missing}" if vulkan_missing and "Vulkan" in sdk else sdk
                results.append((SKIP, source, why))
                continue
        results.append((gate.status, source, gate.detail))
    passed = sum(1 for status, _source, _detail in results if status == PASS)
    for status in (FAIL, SKIP, NOT_RUN):
        named = [f"{source}: {detail}" for found, source, detail in results if found == status]
        if not named:
            continue
        others = [
            f"{found} {source}: {detail}"
            for found, source, detail in results if found not in (PASS, status)
        ]
        summary = " || ".join(named + others)
        return Gate("strict-c-compile", status, f"{summary} || {passed} other file(s) passed"
                    if passed else summary)
    return Gate("strict-c-compile", PASS, f"{len(sources)} file(s)")


def docs_gate(paths: list[str]) -> Gate:
    if not any(ci_paths._is_markdown(p) for p in paths):
        return Gate("documentation-freshness", NOT_RUN, "no changed Markdown")
    return _run("documentation-freshness", [GATE_PYTHON, "tools/lint_docs.py"], timeout=300)


def purity_gate(paths: list[str]) -> Gate:
    """Hosted hygiene runs tools/purity_gate.py on every change; locally, when it can matter."""
    if not any(p.startswith("src/rt/") or p == "tools/purity_gate.py" for p in paths):
        return Gate("core-purity", NOT_RUN, "no changed src/rt/ file")
    return _run("core-purity", [GATE_PYTHON, "tools/purity_gate.py"], timeout=300)


def publication_gate() -> tuple[Gate, Gate]:
    """Publication safety, split into contributor and maintainer obligations."""
    try:
        result = subprocess.run(
            [GATE_PYTHON, "tools/policy_sync.py"],
            cwd=ROOT, capture_output=True, text=True, timeout=600,
        )
    except subprocess.TimeoutExpired:
        return Gate("publication-policy", FAIL, "policy_sync timed out"), Gate(
            "publication-safety", NOT_RUN, "policy classification did not complete",
        )
    if result.returncode != 0:
        return Gate("publication-policy", FAIL, (result.stdout + result.stderr).strip()[-400:]), Gate(
            "publication-safety", NOT_RUN, "policy classification failed",
        )
    policy = Gate("publication-policy", PASS)

    try:
        audit = subprocess.run(
            [GATE_PYTHON, "tools/publish_audit.py", "--tracked-only", "--worktree",
             "--public-scope", "--provenance-self-consistency"],
            cwd=ROOT, capture_output=True, text=True, timeout=1800,
        )
    except subprocess.TimeoutExpired:
        return policy, Gate("publication-safety", FAIL, "publish_audit timed out")
    lines = [line.strip() for line in (audit.stdout + audit.stderr).splitlines() if line.strip()]
    # Only the audit's own ``CODE: path: detail`` lines are findings; its hints (such as the
    # stale-control fix) and summary line are not.
    findings = [line for line in lines if AUDIT_FINDING.match(line)]
    maintainer = [line for line in findings if line.split(":", 1)[0] in MAINTAINER_SIDE_CODES]
    contributor = [line for line in findings if line not in maintainer]
    completed = any(line.startswith("publication audit: ") for line in lines)
    clean = audit.returncode == 0 and any(line.startswith("publication audit: OK") for line in lines)
    if contributor:
        return policy, Gate("publication-safety", FAIL, " || ".join(contributor[:6]))
    if not completed or (not maintainer and not clean):
        # Killed, crashed or silent: an audit that did not report its verdict is never a pass.
        return policy, Gate(
            "publication-safety", FAIL,
            f"publish_audit exited {audit.returncode} without a complete verdict: "
            + (" | ".join(lines[-6:]) or "no output"),
        )
    if maintainer:
        codes = sorted({line.split(":", 1)[0] for line in maintainer})
        return policy, Gate(
            "publication-safety", MAINTAINER_SIDE,
            f"{len(maintainer)} finding(s) a maintainer clears by regenerating the controls: "
            + ", ".join(codes),
        )
    return policy, Gate("publication-safety", PASS)


def run(base: str) -> list[Gate]:
    paths = changed_paths(base)
    routing = ci_paths.classify(paths)
    # A file the branch deleted still routes (its removal can break what remains), but the gates that open
    # each named file (ruff, markdownlint) get only the files that exist; the strict compile filters likewise.
    present = [path for path in paths if (ROOT / path).is_file()]
    python_paths = [
        path for path in present
        if path.endswith(".py") and path.startswith(("tools/", "tests/fixtures/"))
    ]
    print(f"contrib-check: base {base}, {len(paths)} changed path(s)")
    for path in paths:
        print(f"  {path}")
    if not paths:
        return [Gate("changed-paths", NOT_RUN, "nothing changed against the base")]
    print("  routing: " + " ".join(
        f"{key}={routing[key]}"
        for key in ("docs_only", "python_tools", "native_runtime", "markdown")
    ))
    modules = python_test_modules(paths)
    if modules:
        print("  test modules: " + ", ".join(modules))
    gates = [
        ruff_gate(python_paths),
        unit_gate(modules),
        c_gate(paths),
        markdown_gate(present),
        docs_gate(paths),
        purity_gate(paths),
    ]
    gates.extend(publication_gate())
    return gates


def set_gate_python(interpreter: str) -> None:
    """Point the sub-gates at one interpreter for the whole run."""
    global GATE_PYTHON
    GATE_PYTHON = interpreter


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--base", default=os.environ.get("CONTRIB_BASE", "origin/main"),
        help="branch or commit the change is measured against (default: origin/main)",
    )
    parser.add_argument(
        "--python", default=GATE_PYTHON,
        help="interpreter that runs the sub-gates (default: CONTRIB_PYTHON, else this one)",
    )
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    set_gate_python(args.python)

    gates = run(args.base)
    print("")
    for gate in gates:
        detail = f" -- {gate.detail}" if gate.detail else ""
        print(f"  {gate.status:<14} {gate.name}{detail}")
    failed = [gate.name for gate in gates if gate.status == FAIL]
    maintainer = [gate.name for gate in gates if gate.status == MAINTAINER_SIDE]
    skipped = [gate.name for gate in gates if gate.status == SKIP]
    unchecked = [gate.name for gate in gates if gate.status == NOT_RUN and gate.unchecked]
    if maintainer:
        print("\n  a maintainer regenerates PUBLIC_EXPORT.json and the public ledger from the "
              "private trusted ledger (make provenance-refresh); a contributor cannot do that, and these findings are "
              "reported rather than counted as a local failure.")
    if skipped:
        print(f"  SKIPPED because the tool is not installed: {', '.join(skipped)} -- "
              "a skipped gate is not a pass.")
    if unchecked:
        print(f"  NOT CHECKED here, although the change touches it: {', '.join(unchecked)} -- "
              "hosted CI checks it; OK does not cover it.")
    print(f"\ncontrib-check: {'FAIL (' + ', '.join(failed) + ')' if failed else 'OK'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
