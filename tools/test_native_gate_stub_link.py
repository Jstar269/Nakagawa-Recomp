# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Keep the headless codegen-gate link surface synchronized with the runtime."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from makefile_text import read_makefile  # noqa: E402
import codegen_gate  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "src" / "rt"
CI_WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
MAKEFILE = ROOT / "Makefile"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

_SRC_TOKEN = re.compile(r"\b(?:src|tests/native)/[\w./-]+\.(?:c|cpp)\b")

# Hosted-CI inline builds whose source lists duplicate a canonical Makefile
# target recipe. PR #118 shipped a runtime dependency (guest_interp.c) that the
# Makefile target picked up but the inline CI recipe silently omitted, failing
# hosted link gates when draft suppression was in place. Each mapping below is
# asserted source-list-equal so a new runtime dependency cannot be added to one
# surface and forgotten in the other.
_CI_BUILD_TO_MAKE_TARGET = {
    "vfpu_interp_selftest": "vfpu-interp-selftest",
    "fp_convert_selftest": "fp-convert-selftest",
    "vfpu_tables_selftest": "vfpu-tables-selftest",
    "watchpoints_file_selftest": "watchpoints-file-selftest",
}


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestGateStubLink(unittest.TestCase):
    """Link the headless driver exactly as tools/codegen_gate.py builds it.

    The source list and flags come from codegen_gate.build_traced_driver itself,
    not from a copy here, so a TU dropped from (or a flag missing in) the real
    gate fails this test. Only the codegen step is replaced: it writes an empty
    registration pair instead of translating an ELF.
    """

    def _build_gate_driver(self, work: Path) -> tuple[str | None, list[str], subprocess.CompletedProcess]:
        assert CC is not None
        link: list[tuple[list[str], subprocess.CompletedProcess]] = []

        def fake_run(cmd, **kw):
            cmd = [str(part) for part in cmd]
            if any(Path(part).name == "codegen.py" for part in cmd):
                gen = Path(cmd[-1])
                gen.write_text('#include "recomp.h"\nvoid sr_register_all(void) {}\n', encoding="ascii")
                gen.with_name(gen.stem + "_0.c").write_text(
                    '#include "recomp.h"\nvoid sr_register_chunk_0(void) {}\n', encoding="ascii"
                )
                return 0
            result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)
            if Path(cmd[0]).name != Path(sys.executable).name:
                link.append((cmd, result))
            return result.returncode

        overrides = {"CC": CC, "CG_EXTRA_OBJS": "", "CG_EXTRA_CFLAGS": ""}
        with mock.patch.object(codegen_gate, "run", fake_run), mock.patch.dict(os.environ, overrides):
            driver = codegen_gate.build_traced_driver("synthetic.elf", str(work), dict(os.environ))
        self.assertEqual(len(link), 1, "codegen_gate must invoke the compiler exactly once")
        argv, result = link[0]
        return driver, argv, result

    def test_same_headless_runtime_link_inputs_include_stale_detector(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gate_stub_link_") as td:
            work = Path(td)
            driver, argv, result = self._build_gate_driver(work)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIsNotNone(driver)
            assert driver is not None
            self.assertTrue(Path(driver).is_file())
            linked = {Path(part).name for part in argv if part.endswith(".c")}
            # Every unit the gate's driver link names (codegen_gate.build_traced_driver), so a link
            # that loses one, domain_mode.c beside guest_interp.c included, fails here by name.
            for unit in ("gen.c", "gen_0.c", "recomp.c", "guest_interp.c", "cpu_lle.c", "domain_mode.c",
                         "stale_code.c", "vfpu_tables.c", "driver.c", "nk_shared_helpers.c", "ace.c",
                         "videorec.c", "title_config.c", "gate_stub.c"):
                self.assertIn(unit, linked, f"codegen_gate's driver link no longer names {unit}")
            includes = [Path(argv[i + 1]).resolve() for i, part in enumerate(argv[:-1]) if part == "-I"]
            self.assertIn(RT.resolve(), includes)
            self.assertIn((work / "title-config").resolve(), includes)

    def test_headless_driver_link_fails_without_title_config(self) -> None:
        """Negative control: the real gate argv without title_config.c fails with an undefined reference."""
        with tempfile.TemporaryDirectory(prefix="gate_stub_link_neg_") as td:
            _, argv, result = self._build_gate_driver(Path(td))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            without = [part for part in argv if Path(part).name != "title_config.c"]
            self.assertEqual(len(without), len(argv) - 1)
            result = subprocess.run(without, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, "link should have failed without title_config.c")
            self.assertIn("sr_title_config_fallback_entry", result.stderr + result.stdout)

    def test_headless_driver_link_fails_without_config_include(self) -> None:
        """Negative control: the real gate argv without the config include dir fails to compile."""
        with tempfile.TemporaryDirectory(prefix="gate_stub_link_neg_inc_") as td:
            work = Path(td)
            _, argv, result = self._build_gate_driver(work)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            config_dir = (work / "title-config").resolve()
            index = next(
                i for i, part in enumerate(argv[:-1])
                if part == "-I" and Path(argv[i + 1]).resolve() == config_dir
            )
            without = argv[:index] + argv[index + 2:]
            result = subprocess.run(without, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, "compilation should have failed without sr_title_config.h")
            self.assertIn("sr_title_config.h", result.stderr + result.stdout)

def _logical_lines(text: str) -> list[str]:
    """Join backslash-newline continuations so one command is one line."""
    return text.replace("\\\n", " ").splitlines()


def _ci_inline_sources(build_name: str) -> set[str]:
    """Source paths in the hosted-CI inline build of build/<build_name>."""
    joined = _logical_lines(CI_WORKFLOW.read_text(encoding="utf-8"))
    # "-o build/<name>" must be its own argument, not a prefix of a longer
    # name (e.g. vfpu_interp_selftest vs vfpu_interp_selftest_san).
    output_arg = re.compile(rf"-o build/{re.escape(build_name)}(?:\s|$)")
    sources: set[str] = set()
    for line in joined:
        stripped = line.strip()
        if not stripped.startswith(("gcc ", "g++ ", "cc ", "clang ")):
            continue
        if output_arg.search(stripped):
            sources |= set(_SRC_TOKEN.findall(stripped))
    return sources


def _make_target_sources(target: str) -> set[str]:
    """Literal source paths in the canonical Makefile target's recipe."""
    lines = read_makefile(ROOT).splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(rf"^{re.escape(target)}:(?:\s|$)", line):
            start = i + 1
            break
    assert start is not None, f"Makefile target not found: {target}"
    recipe: list[str] = []
    for line in lines[start:]:
        if not line.startswith("\t"):
            break
        recipe.append(line[1:])
    joined = "\n".join(recipe).replace("\\\n", " ")
    return set(_SRC_TOKEN.findall(joined))


class TestInlineCiSelftestLinkSync(unittest.TestCase):
    """The hosted-CI inline selftest builds must match their Makefile targets.

    These pairs duplicate the same link contract in two places. When they drift,
    the Makefile target keeps passing locally while the hosted substantive gate
    fails (or vice versa), which is exactly how PR #118 hid a missing
    guest_interp.c behind the earlier draft-suppressed jobs.
    """

    def test_inline_ci_selftest_source_lists_match_makefile_targets(self) -> None:
        for build_name, target in sorted(_CI_BUILD_TO_MAKE_TARGET.items()):
            # Some selftests ship both a plain and a sanitizer CI build, some
            # only one; require that every variant which exists matches the
            # canonical target, and that at least one variant exists.
            variants = [
                name
                for name in (build_name, f"{build_name}_san")
                if _ci_inline_sources(name)
            ]
            with self.subTest(make_target=target):
                self.assertTrue(
                    variants,
                    f"No inline CI gcc/g++ build found for build/{build_name}(_san)",
                )
            for name in variants:
                with self.subTest(ci_build=name, make_target=target):
                    self.assertEqual(_ci_inline_sources(name), _make_target_sources(target))



# A program that links the run recorder (it rides with the presenter, SDL3VK_SRCS) but no
# scheduler: it registers the clock it was given (or none) and asks to record.
_RECORDER_CLOCK_HARNESS = """\
#include "videorec.h"
#include <stdio.h>
#include <string.h>

static int paced_clock(void) { return 0; }
static int deterministic_clock(void) { return 1; }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (strcmp(argv[1], "paced") == 0) sr_videorec_set_clock(paced_clock);
    else if (strcmp(argv[1], "deterministic") == 0) sr_videorec_set_clock(deterministic_clock);
    else if (strcmp(argv[1], "none") != 0) return 2;
    int active = sr_videorec_active();
    printf("active=%d\\n", active);
    fflush(stdout);
    if (active) (void)sr_videorec_finish_status("the clock-seam harness ended the run", 0);
    return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestRecorderClockSeam(unittest.TestCase):
    """The recorder records only on the clock the runtime's main() hands it (#2080).

    videorec.c links with the presenter, so programs that run no scheduler (the GPU
    selftests, ge-replay) link it too. With no clock set, a requested recording is
    refused for that reason; with one set, the recorder follows it. This builds the
    real videorec.c, not its standalone-selftest variant.
    """

    _tmp: tempfile.TemporaryDirectory[str]
    work: Path
    exe: Path
    build_error: str

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="videorec_clock_seam_")
        cls.work = Path(cls._tmp.name)
        src = cls.work / "harness.c"
        src.write_text(_RECORDER_CLOCK_HARNESS, encoding="ascii")
        cls.exe = cls.work / ("harness.exe" if os.name == "nt" else "harness")
        assert CC is not None
        built = subprocess.run(
            [CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", f"-I{RT}",
             str(src), str(RT / "videorec.c"), "-o", str(cls.exe)],
            capture_output=True, text=True,
        )
        cls.build_error = "" if built.returncode == 0 else built.stdout + built.stderr

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def _run(self, mode: str) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
        self.assertEqual(self.build_error, "", self.build_error)
        out = self.work / mode
        env = dict(os.environ, SR_VIDEOREC_DIR=str(out), SR_VIDEOREC_CODEC="raw")
        # The clock comes from the registered function, never from this key.
        env["SR_DETERMINISTIC_CLOCK"] = "1"
        result = subprocess.run([str(self.exe), mode], env=env, capture_output=True, text=True,
                                timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = json.loads((out / "record.json").read_text(encoding="utf-8"))
        return result, record

    def _reason(self, record: dict[str, object]) -> str:
        reason = record["reason"]
        if not isinstance(reason, str):
            self.fail(f"record.json's reason is not a string: {record!r}")
        return reason

    def test_no_clock_refuses_because_no_scheduler_runs(self) -> None:
        result, record = self._run("none")
        self.assertIn("active=0", result.stdout)
        self.assertEqual(record["status"], "REFUSED")
        self.assertIn("runs no scheduler", self._reason(record))
        self.assertIn("VIDEOREC: not recording: this program runs no scheduler", result.stderr)

    def test_a_paced_clock_refuses_for_the_clock(self) -> None:
        result, record = self._run("paced")
        self.assertIn("active=0", result.stdout)
        self.assertEqual(record["status"], "REFUSED")
        self.assertIn("SR_DETERMINISTIC_CLOCK=1", self._reason(record))
        self.assertNotIn("runs no scheduler", self._reason(record))

    def test_a_deterministic_clock_starts_the_recording(self) -> None:
        result, record = self._run("deterministic")
        self.assertIn("active=1", result.stdout)
        self.assertNotEqual(record["status"], "REFUSED", record)


if __name__ == "__main__":
    unittest.main()
