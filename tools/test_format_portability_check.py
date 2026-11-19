# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for tools/format_portability_check.py."""

from __future__ import annotations

import os
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock

from tools.format_portability_check import (
    check_format_portability,
    ensure_title_config_header,
    host_long_width,
    main,
    narrow_long_flags,
    resolve_compiler,
)


class TestFormatPortabilityCheck(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.test_root = Path(self.temp_dir.name)
        self.shim_dir = self.test_root / "shim"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_synthetic_bad_uint32_specifier_fails(self) -> None:
        """A synthetic file printing uint32_t with %u must fail the 32-bit newlib check."""
        bad_c = self.test_root / "bad_uint32.c"
        bad_c.write_text(
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "void test_bad(uint32_t val) {\n"
            '    printf("val=%u\\n", val);\n'
            "}\n",
            encoding="utf-8",
        )
        failed, errors = check_format_portability(
            files=[str(bad_c)],
            shim_dir=self.shim_dir,
            narrow_long=False,
        )
        self.assertEqual(failed, 1)
        self.assertEqual(len(errors), 1)
        self.assertIn("error: format", errors[0])

    def test_synthetic_bad_int32_specifier_fails(self) -> None:
        """A synthetic file printing int32_t with %d must fail the 32-bit newlib check."""
        bad_c = self.test_root / "bad_int32.c"
        bad_c.write_text(
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "void test_bad(int32_t val) {\n"
            '    printf("val=%d\\n", val);\n'
            "}\n",
            encoding="utf-8",
        )
        failed, errors = check_format_portability(
            files=[str(bad_c)],
            shim_dir=self.shim_dir,
            narrow_long=False,
        )
        self.assertEqual(failed, 1)
        self.assertEqual(len(errors), 1)
        self.assertIn("error: format", errors[0])

    def test_synthetic_fixed_specifiers_pass(self) -> None:
        """A synthetic file printing uint32_t/int32_t with PRI macros must pass."""
        good_c = self.test_root / "good.c"
        good_c.write_text(
            "#include <errno.h>\n"
            "#include <stdint.h>\n"
            "#include <inttypes.h>\n"
            "#include <stdio.h>\n"
            "void test_good(uint32_t u, int32_t s, size_t n, intmax_t m) {\n"
            '    printf("u=%" PRIu32 " s=%" PRId32 " x=0x%08" PRIx32 "\\n", u, s, u);\n'
            '    printf("cast=%u\\n", (unsigned)u);\n'
            "    /* the shim must carry the C11 limits the core uses */\n"
            '    printf("n=%zu max=%zu ptr=%" PRIdMAX " m=%" PRIdMAX " e=%d\\n", n, (size_t)SIZE_MAX,\n'
            "           (intmax_t)INTPTR_MAX, m, errno);\n"
            "}\n",
            encoding="utf-8",
        )
        failed, errors = check_format_portability(
            files=[str(good_c)],
            shim_dir=self.shim_dir,
            narrow_long=False,
        )
        self.assertEqual(failed, 0, "\n".join(errors))
        self.assertEqual(len(errors), 0)

    def test_cli_invocations(self) -> None:
        """The CLI main() must exit 0 on clean files and 1 on bad files."""
        good_c = self.test_root / "cli_good.c"
        good_c.write_text(
            "#include <stdint.h>\n"
            "#include <inttypes.h>\n"
            "#include <stdio.h>\n"
            "void f(uint32_t x) { printf(\"%\" PRIx32 \"\\n\", x); }\n",
            encoding="utf-8",
        )
        bad_c = self.test_root / "cli_bad.c"
        bad_c.write_text(
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "void f(uint32_t x) { printf(\"%x\\n\", x); }\n",
            encoding="utf-8",
        )

        rc_good = main(["--shim-dir", str(self.shim_dir), "--allow-wide-long", str(good_c)])
        self.assertEqual(rc_good, 0)

        rc_bad = main(["--shim-dir", str(self.shim_dir), "--allow-wide-long", str(bad_c)])
        self.assertEqual(rc_bad, 1)

    def test_missing_compiler_is_the_toolchain_exit_code(self) -> None:
        """A compiler that is not installed is exit 2, the fail-closed contract, never a traceback."""
        good_c = self.test_root / "any.c"
        good_c.write_text("int f(void) { return 0; }\n", encoding="utf-8")
        rc = main(["--compiler", "no-such-c-compiler-7f3a", "--allow-wide-long", str(good_c)])
        self.assertEqual(rc, 2)

    def test_long_width_decides_the_32_bit_flag(self) -> None:
        """An LP64 host compiles the check with -m32; ILP32 and LLP64 hosts need nothing."""
        self.assertEqual(narrow_long_flags(8), ["-m32"])
        self.assertEqual(narrow_long_flags(4), [])
        self.assertIn(host_long_width(resolve_compiler("gcc")), (4, 8))

    def test_compiler_resolution_honors_msys_path(self) -> None:
        ucrt_bin = self.test_root / "custom-ucrt64" / "bin"
        ucrt_bin.mkdir(parents=True)
        gcc = ucrt_bin / "gcc.exe"
        gcc.write_bytes(b"synthetic executable path marker")
        with mock.patch.dict(os.environ, {"MSYS_PATH": str(ucrt_bin)}), \
                mock.patch("tools.format_portability_check.sys", types.SimpleNamespace(platform="win32")), \
                mock.patch("tools.format_portability_check.shutil", types.SimpleNamespace(which=lambda *_: None)):
            self.assertEqual(resolve_compiler("gcc"), str(gcc))

    def test_pointer_sized_specifiers_match_the_shim_intptr_t(self) -> None:
        """PRI*PTR follows intptr_t's type (long long on Win64, int under -m32), not a fixed "l"."""
        good_c = self.test_root / "good_ptr.c"
        good_c.write_text(
            "#include <stdint.h>\n"
            "#include <inttypes.h>\n"
            "#include <stdio.h>\n"
            "void test_ptr(intptr_t s, uintptr_t u) {\n"
            '    printf("s=%" PRIdPTR " u=%" PRIuPTR " x=%" PRIxPTR " X=%" PRIXPTR "\\n", s, u, u, u);\n'
            '    sscanf("1", "%" SCNxPTR, &u);\n'
            "}\n",
            encoding="utf-8",
        )
        failed, errors = check_format_portability(
            files=[str(good_c)],
            shim_dir=self.shim_dir,
            narrow_long=False,
        )
        self.assertEqual(failed, 0, "\n".join(errors))

    def test_the_generic_title_config_header_is_regenerated_when_stale(self) -> None:
        """A header left by an older generator is replaced; an unchanged one keeps its bytes."""
        repo = self.test_root / "repo"
        (repo / "tools").mkdir(parents=True)
        header = repo / "build" / "title-config" / "generic" / "sr_title_config.h"
        header.parent.mkdir(parents=True)
        header.write_text("/* stale output of an older generator */\n", encoding="utf-8")
        fresh = "#ifndef SR_TITLE_CONFIG_GENERATED_H\n#define SR_TITLE_CONFIG_GENERATED_H\n#endif\n"
        (repo / "tools" / "title_runtime_config.py").write_text(
            "import argparse\nfrom pathlib import Path\n"
            "parser = argparse.ArgumentParser()\nparser.add_argument('--output', type=Path)\n"
            f"parser.parse_args().output.write_text({fresh!r}, encoding='utf-8')\n",
            encoding="utf-8",
        )
        self.assertEqual(ensure_title_config_header(repo), header.parent)
        self.assertEqual(header.read_text(encoding="utf-8"), fresh)
        stamp = header.stat().st_mtime_ns
        ensure_title_config_header(repo)
        self.assertEqual(header.stat().st_mtime_ns, stamp)
        self.assertEqual(sorted(path.name for path in header.parent.iterdir()), ["sr_title_config.h"])


if __name__ == "__main__":
    unittest.main()
