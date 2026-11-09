# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""The on-device toolchain's own code (#360).

android/toolchain/nk_ld_launcher.c is built with the host compiler and started next to a
stand-in libnk_lld.so, a small C program that records the argv it was given and exits with
the status the test asks for. The launcher reads /proc/self/exe, so this runs on Linux (WSL
included) and is skipped elsewhere.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / "android" / "toolchain" / "nk_ld_launcher.c"

STAND_IN = r"""
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    FILE *record = fopen(getenv("NK_LD_RECORD"), "w");
    if (record == NULL) return 99;
    for (int i = 0; i < argc; i++) fprintf(record, "%s\n", argv[i]);
    fclose(record);
    return atoi(getenv("NK_LD_STATUS"));
}
"""


@unittest.skipUnless(sys.platform.startswith("linux"), "the launcher reads /proc/self/exe")
class LdLauncherTests(unittest.TestCase):
    _tmp: tempfile.TemporaryDirectory[str]
    folder: Path
    launcher: Path
    stand_in: Path

    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which(os.environ.get("CC", "cc")) or shutil.which("gcc")
        if compiler is None:
            raise unittest.SkipTest("no C compiler")
        cls._tmp = tempfile.TemporaryDirectory(prefix="nk-ld-launcher-")
        root = Path(cls._tmp.name)
        cls.folder = root / "lib" / "arm64"
        cls.folder.mkdir(parents=True)
        cls.launcher = cls.folder / "libnk_ld.so"
        flags = ["-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror"]
        subprocess.run([compiler, *flags, "-o", str(cls.launcher), str(LAUNCHER)], check=True)
        (root / "stand_in.c").write_bytes(STAND_IN.encode())
        cls.stand_in = root / "stand_in"
        subprocess.run([compiler, *flags, "-o", str(cls.stand_in), str(root / "stand_in.c")], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def setUp(self) -> None:
        self.record = self.folder / "record.txt"
        self.record.unlink(missing_ok=True)
        self.lld = self.folder / "libnk_lld.so"
        self.lld.unlink(missing_ok=True)

    def install_stand_in(self) -> None:
        shutil.copy2(self.stand_in, self.lld)

    def run_launcher(self, *args: str, status: int = 0, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ, NK_LD_RECORD=str(self.record), NK_LD_STATUS=str(status))
        return subprocess.run(
            [str(self.launcher), *args], env=env, cwd=cwd, capture_output=True, text=True, check=False
        )

    def recorded(self) -> list[str]:
        return self.record.read_text().split("\n")[:-1]

    def test_lld_starts_as_ld_lld_with_every_argument_unchanged(self) -> None:
        self.install_stand_in()
        args = ("-o", "out file", "--version-script=a.map", "", "x.o")
        result = self.run_launcher(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded(), ["ld.lld", *args])

    def test_lld_is_found_beside_the_launcher_from_any_working_directory(self) -> None:
        self.install_stand_in()
        result = self.run_launcher("x.o", cwd=Path(self._tmp.name))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded(), ["ld.lld", "x.o"])

    def test_the_exit_status_is_lld_s(self) -> None:
        self.install_stand_in()
        self.assertEqual(self.run_launcher("x.o", status=3).returncode, 3)

    def test_a_missing_lld_returns_127_and_names_it(self) -> None:
        result = self.run_launcher("x.o")
        self.assertEqual(result.returncode, 127)
        self.assertIn(f"libnk_ld: cannot start {self.lld}", result.stderr)
        self.assertFalse(self.record.exists())


if __name__ == "__main__":
    unittest.main()
