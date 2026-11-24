# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The hand-written HST stub bodies in ``codegen_hst_stubs`` reach the generated C unchanged.

``tools/codegen.py`` used to hold these bodies inline; they are data now.  These tests pin the
shape of that data and prove, through the production ``codegen.main`` path over the shared
profile-isolation specimen, that ``--profile=hst`` emits each body verbatim and that
``--profile=none`` emits none of them.

The module also keeps the retired bodies retired.  The last of them, the native copy and fill
bodies, have an executing fixture here: project-written copy and fill routines at the four
addresses, generated under both profiles, compiled and run.  It is not the title's code and
proves nothing about the title's own routines; it proves that the generator emits the guest's
instructions at those addresses and that they then behave as the instructions say, where the
retired bodies behaved as a host ``memmove`` and ``memset``.
"""

import subprocess
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import codegen  # noqa: E402
import codegen_hst_stubs  # noqa: E402
import compat_overrides  # noqa: E402
import scratch_dir  # noqa: E402
import test_codegen_entry_semantics as entry  # noqa: E402
import test_codegen_profile_isolation as isolation  # noqa: E402
from host_stubs import HST_SIMPLE_STUBS  # noqa: E402

STUBS = codegen_hst_stubs.HST_CUSTOM_STUBS


class StubDataShape(unittest.TestCase):
    def test_each_body_defines_the_function_for_its_own_address(self):
        for addr, text in STUBS.items():
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertTrue(text.startswith(f"void f_{addr:08x}(CpuState *s) {{"), text[:80])
                self.assertTrue(text.endswith("}"))
                self.assertNotIn("@ADDR@", text)

    def test_no_address_has_two_bodies(self):
        self.assertEqual(set(STUBS) & set(HST_SIMPLE_STUBS), set())


class StubsReachTheGeneratedC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        extra = {addr: isolation._body_leaf() for addr in STUBS}
        words = isolation._build_specimen_words(extra)
        cls.hst = isolation._codegen_functions(words, "hst")
        cls.none = isolation._codegen_functions(words, "none")

    def test_hst_emits_each_body_verbatim(self):
        for addr, text in STUBS.items():
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertEqual(self.hst[f"f_{addr:08x}"], text)

    def test_profile_none_emits_none_of_them(self):
        for addr in STUBS:
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertNotIn("custom stub", self.none[f"f_{addr:08x}"])
                self.assertNotIn(f"f_{addr:08x}_real", self.none)


#: Where --profile=hst emitted a native copy (sr_native_memmove) in place of the title's own.
COPY_BYTES = 0x00011090
COPY_WORDS = 0x000110DC
#: Where it emitted a native fill (sr_native_memset).  The first is six instructions before
#: the second.
FILL = 0x000114A8
FILL_PATTERN = 0x000114C0
#: The retired copy and fill bodies, in address order.
COPY_AND_FILL = (COPY_BYTES, COPY_WORDS, FILL, FILL_PATTERN)

#: Hand-written bodies retired under #363: the guest's own code runs at each address.
RETIRED = (0x000011B0, 0x0000260C, 0x0000FE3C, 0x00015EA0, 0x000149A8) + COPY_AND_FILL


class RetiredStubsTranslateAsWritten(unittest.TestCase):
    """A retired address gets the same translated body under --profile=hst as under --profile=none."""

    @classmethod
    def setUpClass(cls):
        words = isolation._build_specimen_words({addr: isolation._body_leaf() for addr in RETIRED})
        cls.hst = isolation._codegen_functions(words, "hst")
        cls.none = isolation._codegen_functions(words, "none")

    def test_no_retired_address_has_a_hand_written_body(self):
        self.assertEqual(set(RETIRED) & set(STUBS), set())
        self.assertEqual(set(RETIRED) & set(HST_SIMPLE_STUBS), set())

    def test_hst_translates_each_retired_address_as_written(self):
        for addr in RETIRED:
            with self.subTest(addr=f"0x{addr:08x}"):
                name = f"f_{addr:08x}"
                self.assertNotIn("custom stub", self.hst[name])
                self.assertEqual(self.hst[name], self.none[name])


#: The main_RunGameLoop scheduler wrapper, retired under #363: the worker thread's own loop runs.
GAME_LOOP = 0x000468C8


class GameLoopTranslatesAsWritten(unittest.TestCase):
    """profile=hst emits 0x000468c8 exactly as profile=none does: no wrapper, no renamed body."""

    @classmethod
    def setUpClass(cls):
        words = isolation._build_specimen_words({GAME_LOOP: isolation._body_leaf()})
        cls.hst = isolation._codegen_functions(words, "hst")
        cls.none = isolation._codegen_functions(words, "none")

    def test_no_hand_written_body_or_wrapper(self):
        self.assertNotIn(GAME_LOOP, STUBS)
        self.assertNotIn(GAME_LOOP, HST_SIMPLE_STUBS)
        self.assertFalse(hasattr(codegen_hst_stubs, "HST_GAME_LOOP_WRAPPER"))

    def test_hst_translates_the_game_loop_as_written(self):
        name = f"f_{GAME_LOOP:08x}"
        self.assertNotIn(f"{name}_real", self.hst)
        self.assertNotIn("custom stub", self.hst[name])
        self.assertEqual(self.hst[name], self.none[name])


# --- copy and fill routines at the addresses of the retired native bodies (#363) -------------
#
# Four project-written routines, each a plain reading of what a C library's copy or fill does
# one byte or one word at a time.  They differ from a host memmove and memset where guest code
# may: a forward copy whose destination overlaps the source ahead of it repeats what it has
# already written, a fill may take a word and store it a byte at a time, and each leaves its
# pointer and count registers where its loop ended.

_ZERO, _V0, _A0, _A1, _A2 = entry._ZERO, entry._V0, entry._A0, entry._A1, entry._A2
_T0, _T1, _T2, _T3, _SP, _RA = entry._T0, entry._T1, entry._T2, entry._T3, entry._SP, entry._RA
_addiu, _sltiu, _andi, _lw, _sw = entry._addiu, entry._sltiu, entry._andi, entry._lw, entry._sw
_lbu, _sb, _addu, _sll, _jr = entry._lbu, entry._sb, entry._addu, entry._sll, entry._jr
_beq, _jal = entry._beq, entry._jal


def _or(rd, rs, rt): return entry._rtype(rs, rt, rd, 0x25)
def _srl(rd, rt, shamt): return entry._rtype(0, rt, rd, 0x02, shamt)


def _bne(rs, rt, label):
    return lambda pc, at: entry._itype(0x05, rs, rt, (at[label] - pc - 4) >> 2)


# void *copy_bytes(void *dst, const void *src, size_t n): forwards, a byte at a time.
_COPY_BYTES = [
    _addu(_V0, _A0, _ZERO),
    _addu(_T1, _A1, _A2),                   # t1: the end of the source
    "b_loop",
    _beq(_A1, _T1, "b_done"),
    0,
    _lbu(_T2, 0, _A1),
    _addiu(_A1, _A1, 1),
    _sb(_T2, 0, _A0),
    _beq(_ZERO, _ZERO, "b_loop"),
    _addiu(_A0, _A0, 1),
    "b_done",
    _jr(_RA),
    0,
]

# void *copy_words(void *dst, const void *src, size_t n): forwards, whole words while four
# bytes remain, then the rest a byte at a time.
_COPY_WORDS = [
    _addu(_V0, _A0, _ZERO),
    "w_loop",
    _sltiu(_T0, _A2, 4),
    _bne(_T0, _ZERO, "w_tail"),
    0,
    _lw(_T2, 0, _A1),
    _addiu(_A1, _A1, 4),
    _sw(_T2, 0, _A0),
    _addiu(_A0, _A0, 4),
    _beq(_ZERO, _ZERO, "w_loop"),
    _addiu(_A2, _A2, -4),
    "w_tail",
    _beq(_A2, _ZERO, "w_done"),
    0,
    _lbu(_T2, 0, _A1),
    _addiu(_A1, _A1, 1),
    _sb(_T2, 0, _A0),
    _addiu(_A0, _A0, 1),
    _beq(_ZERO, _ZERO, "w_tail"),
    _addiu(_A2, _A2, -1),
    "w_done",
    _jr(_RA),
    0,
]

# void *fill(void *dst, int c, size_t n): the byte c, repeated into a word, and then the code
# that follows it in memory: these six instructions end where fill_pattern begins.
_FILL = [
    _addu(_V0, _A0, _ZERO),
    _andi(_A1, _A1, 0xFF),
    _sll(_T0, _A1, 8),
    _or(_A1, _A1, _T0),
    _sll(_T0, _A1, 16),
    _or(_A1, _A1, _T0),
]

# void *fill_pattern(void *dst, uint32_t pattern, size_t n): the pattern's bytes in memory
# order, repeated, a byte at a time.  The branch's delay slot runs on both of its outcomes.
_FILL_PATTERN = [
    _addu(_V0, _A0, _ZERO),
    _addu(_T1, _A0, _A2),                   # t1: the end of the destination
    "p_loop",
    _beq(_A0, _T1, "p_done"),
    _srl(_T2, _A1, 8),
    _sb(_A1, 0, _A0),
    _sll(_T3, _A1, 24),
    _or(_A1, _T2, _T3),                     # the pattern, turned right by one byte
    _beq(_ZERO, _ZERO, "p_loop"),
    _addiu(_A0, _A0, 1),
    "p_done",
    _jr(_RA),
    0,
]

#: A caller of the four, so each is reachable from the ELF entry.
_COPY_AND_FILL_ANCHOR = 0x00011500
_ANCHOR = [
    _addiu(_SP, _SP, -0x10),
    _sw(_RA, 0x0C, _SP),
    _jal("copy_bytes"), 0,
    _jal("copy_words"), 0,
    _jal("fill"), 0,
    _jal("fill_pattern"), 0,
    _lw(_RA, 0x0C, _SP),
    _jr(_RA),
    _addiu(_SP, _SP, 0x10),
]


def _copy_and_fill_fixture() -> dict[int, int]:
    at = {"copy_bytes": COPY_BYTES, "copy_words": COPY_WORDS, "fill": FILL,
          "fill_pattern": FILL_PATTERN}
    words: dict[int, int] = {}
    for base, program in ((COPY_BYTES, _COPY_BYTES), (COPY_WORDS, _COPY_WORDS), (FILL, _FILL),
                          (FILL_PATTERN, _FILL_PATTERN), (_COPY_AND_FILL_ANCHOR, _ANCHOR)):
        body = entry._assemble(base, program, at)
        assert not set(body) & set(words), f"the body at 0x{base:08x} overlaps another"
        words.update(body)
    assert FILL + 4 * len(_FILL) == FILL_PATTERN, "fill must run on into fill_pattern"
    return words


_BUF = 0x00018000
_STACK = 0x0001F000


def _copy_and_fill_harness() -> str:
    return f"""\
{entry._executing_harness_support("copyfill_funcs.h")}
static CpuState cpu;
static void call(void (*fn)(CpuState *), uint32_t a0, uint32_t a1, uint32_t a2) {{
    cpu.r[4] = a0; cpu.r[5] = a1; cpu.r[6] = a2;
    cpu.r[31] = 0x12345678u;
    fn(&cpu); sr_tail_drain(&cpu);
    if (cpu.r[29] != 0x{_STACK:08x}u || cpu.r[31] != 0x12345678u || cpu.r[16] != 0x5a5a5a5au) {{
        printf("a call lost sp, ra or s0\\n");
        exit(1);
    }}
}}
static uint32_t put(uint32_t addr, const char *text) {{
    memcpy(at(addr, (uint32_t)strlen(text) + 1u), text, strlen(text) + 1u);
    return addr;
}}
static const char *text(uint32_t addr) {{ return (const char *)at(addr, 64u); }}
/* The registers a call leaves, relative to its destination `dst` and source `src`. */
static void show(const char *what, uint32_t dst, uint32_t src) {{
    printf("%s v0=dst+%u a0=dst+%u", what, cpu.r[2] - dst, cpu.r[4] - dst);
    if (src) printf(" a1=src+%u", cpu.r[5] - src); else printf(" a1=%08x", cpu.r[5]);
    printf(" a2=%u", cpu.r[6]);
}}
static void bytes(uint32_t addr, uint32_t n) {{
    printf(" bytes=");
    for (uint32_t i = 0; i < n; i++) printf("%02x", test_mem_r8(addr + i));
    printf("\\n");
}}

int main(void) {{
    const uint32_t b = 0x{_BUF:08x}u;
    sr_register_all();
    cpu.r[16] = 0x5a5a5a5au; cpu.r[29] = 0x{_STACK:08x}u;

    /* A copy between separate spans moves n bytes and no more. */
    put(b, "0123456789abcdef"); put(b + 0x40u, "................");
    call(f_{COPY_BYTES:08x}, b + 0x41u, b + 3u, 6u);
    show("copy_bytes apart", b + 0x41u, b + 3u); printf(" text=%s\\n", text(b + 0x40u));

    /* Forwards into its own source, one byte ahead: each byte read is the one just written. */
    put(b + 0x80u, "ABCDEFGH");
    call(f_{COPY_BYTES:08x}, b + 0x81u, b + 0x80u, 6u);
    show("copy_bytes ahead", b + 0x81u, b + 0x80u); printf(" text=%s\\n", text(b + 0x80u));

    /* Two words and three bytes. */
    put(b + 0xc0u, "0123456789abcdef"); put(b + 0x100u, "................");
    call(f_{COPY_WORDS:08x}, b + 0x100u, b + 0xc0u, 11u);
    show("copy_words apart", b + 0x100u, b + 0xc0u); printf(" text=%s\\n", text(b + 0x100u));

    /* Forwards into its own source, one word ahead: the second word read is the first written. */
    put(b + 0x140u, "abcdefghijkl");
    call(f_{COPY_WORDS:08x}, b + 0x144u, b + 0x140u, 8u);
    show("copy_words ahead", b + 0x144u, b + 0x140u); printf(" text=%s\\n", text(b + 0x140u));

    /* fill keeps the low byte of c and runs on into fill_pattern. */
    put(b + 0x180u, "................");
    call(f_{FILL:08x}, b + 0x182u, 0x000001a5u, 5u);
    show("fill", b + 0x182u, 0u); bytes(b + 0x180u, 9u);

    /* fill_pattern entered at its own address stores all four bytes of its word in turn. */
    put(b + 0x1c0u, "................");
    call(f_{FILL_PATTERN:08x}, b + 0x1c1u, 0x44332211u, 6u);
    show("fill_pattern", b + 0x1c1u, 0u); bytes(b + 0x1c0u, 8u);

    /* Nothing to do: no byte moves, and v0 is still the destination. */
    call(f_{COPY_WORDS:08x}, b + 0x100u, b + 0xc0u, 0u);
    show("copy_words none", b + 0x100u, b + 0xc0u); printf(" text=%s\\n", text(b + 0x100u));
    call(f_{FILL_PATTERN:08x}, b + 0x1c1u, 0x44332211u, 0u);
    show("fill_pattern none", b + 0x1c1u, 0u); bytes(b + 0x1c0u, 8u);
    return 0;
}}
"""


# What the instructions above produce, worked by hand from them.  0x2e is '.', the byte each
# destination holds before its call.  A host memmove at the two copy addresses would leave
# "AABCDEFH" and "abcdabcdefgh" in the two overlapping cases, and a host memset at
# fill_pattern six bytes of 0x11; the retired bodies also left a0, a1 and a2 as the caller
# set them.
_COPY_AND_FILL_OUTCOME = """\
copy_bytes apart v0=dst+0 a0=dst+6 a1=src+6 a2=6 text=.345678.........
copy_bytes ahead v0=dst+0 a0=dst+6 a1=src+6 a2=6 text=AAAAAAAH
copy_words apart v0=dst+0 a0=dst+11 a1=src+11 a2=0 text=0123456789a.....
copy_words ahead v0=dst+0 a0=dst+8 a1=src+8 a2=0 text=abcdabcdabcd
fill v0=dst+0 a0=dst+5 a1=a5a5a5a5 a2=5 bytes=2e2ea5a5a5a5a52e2e
fill_pattern v0=dst+0 a0=dst+6 a1=22114433 a2=6 bytes=2e1122334411222e
copy_words none v0=dst+0 a0=dst+0 a1=src+0 a2=0 text=0123456789a.....
fill_pattern none v0=dst+0 a0=dst+0 a1=44332211 a2=0 bytes=2e1122334411222e"""


@unittest.skipUnless(entry.CC, "TOOLCHAIN_MISSING: gcc is required for the generated-C execution regression")
class RetiredCopyAndFillBodiesRunAsWrittenTests(unittest.TestCase):
    """The four copy and fill addresses run their own instructions under every profile (#363).

    --profile=hst used to emit a host memmove at 0x00011090 and 0x000110dc and a host memset
    at 0x000114c0 and 0x000114a8 in place of the title's own routines.  The fixture puts a
    byte copy, a word copy, and a fill whose head runs on into a pattern fill at those
    addresses.  It fails while the profile emits anything but the guest's words at one of
    them, and if a copied or filled byte, the return value or a register the loops advance
    comes back different from what the instructions produce: the overlapping copies and the
    pattern fill are the cases a host memmove or memset answers differently.  The harness's
    dispatch and drain are stand-ins, so this checks the generated code, not the runtime's."""

    def _run(self, profile: str, work: Path) -> tuple[dict[int, str], str]:
        assert entry.CC is not None
        words = _copy_and_fill_fixture()
        elf = work / "copyfill.elf"
        generated = work / "copyfill.c"
        elf.write_bytes(entry._synthetic_elf(words, _COPY_AND_FILL_ANCHOR))
        result = subprocess.run(
            [sys.executable, str(entry.CODEGEN), str(elf), str(generated),
             f"--profile={profile}"],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        chunks = sorted(work.glob("copyfill_*.c"))
        text = "\n".join(path.read_text(encoding="ascii") for path in chunks)
        bodies = {}
        for addr in COPY_AND_FILL:
            start = text.index(f"void f_{addr:08x}(CpuState *s)")
            body = text[start:text.index("\nvoid ", start + 1)]
            self.assertNotIn("custom stub", body,
                             f"profile={profile} emits a hand-written body at 0x{addr:08x}")
            self.assertNotIn("sr_native_mem", body,
                             f"profile={profile} emits a host copy or fill at 0x{addr:08x}")
            self.assertIn(f"sr_begin(s, 0x{addr:08x}u, 0x{words[addr]:08x}u)", body,
                          f"profile={profile} does not translate the guest's first word")
            bodies[addr] = body
        (work / "recomp.h").write_text(entry._FORMATTER_RECOMP_H, encoding="ascii")
        (work / "harness.c").write_text(_copy_and_fill_harness(), encoding="ascii")
        exe = work / "copyfill.exe"
        compiled = subprocess.run(
            [entry.CC, "-std=c11", "-O0", "-I", str(work), str(work / "harness.c"),
             str(generated), *(str(path) for path in chunks), "-lm", "-o", str(exe)],
            capture_output=True, text=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
        return bodies, ran.stdout.strip().replace("\r\n", "\n")

    def test_the_guest_copy_and_fill_routines_run_under_both_profiles(self):
        bodies = {}
        for profile in ("hst", "none"):
            with self.subTest(profile=profile), \
                    scratch_dir.TemporaryDirectory(prefix="copyfill_") as tmp:
                bodies[profile], outcome = self._run(profile, Path(tmp))
                self.assertEqual(outcome, _COPY_AND_FILL_OUTCOME)
        self.assertEqual(bodies.get("hst"), bodies.get("none"),
                         "a copy or fill entry point diverged between profiles")


class RetiredCopyAndFillTableTests(unittest.TestCase):
    """No table names a copy or fill address, and the generator emits neither host helper."""

    def test_no_copy_or_fill_address_has_a_hand_written_body(self):
        manifest = {item["address"] for item in compat_overrides.CODEGEN_CUSTOM_STUBS}
        for addr in COPY_AND_FILL:
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertNotIn(addr, STUBS)
                self.assertNotIn(addr, HST_SIMPLE_STUBS)
                self.assertNotIn(addr, codegen.GUEST_PATCHES)
                self.assertNotIn(addr, manifest)

    def test_the_generator_emits_no_host_copy_or_fill(self):
        for addr, text in STUBS.items():
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertNotIn("sr_native_mem", text)
        for name in ("codegen.py", "codegen_hst_stubs.py"):
            source = (TOOLS / name).read_text(encoding="utf-8")
            with self.subTest(source=name):
                self.assertNotIn("sr_native_memmove(", source)
                self.assertNotIn("sr_native_memset(", source)


if __name__ == "__main__":
    unittest.main()
