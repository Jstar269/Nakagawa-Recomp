# SPDX-License-Identifier: GPL-3.0-or-later

"""The title's allocator family runs as written under ``--profile=hst`` (#363).

``--profile=hst`` used to emit four bridges in place of the title's ``_malloc_r``, ``_free_r``,
``_memalign_r`` and ``_realloc_r``, handing each call to a heap the runtime owned.  The four
were retired together, because they share one set of chunk headers, bins and top chunk: the
title's ``_memalign_r`` and ``_realloc_r`` write chunk headers themselves, with header bit 0
meaning PREV_INUSE, so none of them can run as written while another is bridged to a heap
whose header gives that bit another meaning.

The executing fixture below is a project-written allocator family of that shape at the four
addresses.  It is not the title's code and proves nothing about the title's own allocator;
it proves that the generator emits the guest's instructions at those addresses and that an
allocator family that owns its metadata in guest memory then behaves as its instructions say.
"""

from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen
import codegen_hst_stubs
import compat_overrides
import scratch_dir
import test_codegen_entry_semantics as entry
from host_stubs import HST_SIMPLE_STUBS

FREE_R = 0x0000F538
MEMALIGN_R = 0x000101C4
MALLOC_R = 0x00010738
REALLOC_R = 0x00013524
#: The retired bridges, in address order.
ALLOCATOR_FAMILY = (FREE_R, MEMALIGN_R, MALLOC_R, REALLOC_R)
#: A caller of memalign and realloc, so the whole family is reachable from the ELF entry.
ANCHOR = 0x00013700

# --- the fixture's allocator -----------------------------------------------------------------
#
# A chunk is [prev_size][size | PREV_INUSE] followed by its payload, sizes in multiples of 8,
# as in the allocator newlib ships.  Whether a chunk is free is recorded in the header of the
# chunk after it (PREV_INUSE clear, and prev_size valid), never in its own.  The allocator's
# state is two guest words: the top chunk, which every fresh chunk is carved from, and one
# bin, a singly linked list of free chunks through their first payload word.

_STATE = 0x00018000        # +0: the top chunk, +4: the bin's first chunk
_HEAP = 0x00019000
_HEAP_END = 0x0001D000
_STACK = 0x0001F000
_REENT = 0x00018100        # the reentrancy structure's address; the fixture never reads it

_ZERO, _V0, _A0, _A1, _A2 = entry._ZERO, entry._V0, entry._A0, entry._A1, entry._A2
_T0, _T1, _T2, _T3, _T4, _T5, _T6, _T7 = 8, 9, 10, 11, 12, 13, 14, 15
_S0, _S1, _T8, _T9, _SP, _RA = 16, 17, 24, 25, 29, 31
_addiu, _sltiu, _andi, _lw, _sw = entry._addiu, entry._sltiu, entry._andi, entry._lw, entry._sw
_addu, _subu, _jr, _beq, _jal = entry._addu, entry._subu, entry._jr, entry._beq, entry._jal


def _and(rd, rs, rt): return entry._rtype(rs, rt, rd, 0x24)
def _or(rd, rs, rt): return entry._rtype(rs, rt, rd, 0x25)
def _sltu(rd, rs, rt): return entry._rtype(rs, rt, rd, 0x2B)
def _ori(rt, rs, imm): return entry._itype(0x0D, rs, rt, imm)
def _lui(rt, imm): return entry._itype(0x0F, 0, rt, imm)


def _bne(rs, rt, label):
    return lambda pc, at: entry._itype(0x05, rs, rt, (at[label] - pc - 4) >> 2)


def _state(rt):
    """Load the address of the allocator's state."""
    return [_lui(rt, _STATE >> 16), _ori(rt, rt, _STATE & 0xFFFF)]


# void *_malloc_r(reent, size): the first bin chunk that is large enough, whole, or a fresh
# chunk carved from the top.  Taking a bin chunk sets PREV_INUSE in the chunk after it.
_MALLOC_R = [
    _addiu(_T0, _A1, 11),
    _addiu(_T9, _ZERO, -8),
    _and(_T0, _T0, _T9),                    # t0: the chunk size for this request
    _sltiu(_T1, _T0, 16),
    _beq(_T1, _ZERO, "m_sized"),
    0,
    _addiu(_T0, _ZERO, 16),
    "m_sized",
    *_state(_T1),
    _addiu(_T2, _T1, 4),                    # t2: the link that names the chunk in t3
    "m_search",
    _lw(_T3, 0, _T2),
    _beq(_T3, _ZERO, "m_top"),
    0,
    _lw(_T4, 4, _T3),
    _and(_T5, _T4, _T9),
    _sltu(_T6, _T5, _T0),
    _beq(_T6, _ZERO, "m_take"),
    0,
    _beq(_ZERO, _ZERO, "m_search"),
    _addiu(_T2, _T3, 8),
    "m_take",
    _lw(_T6, 8, _T3),
    _sw(_T6, 0, _T2),                       # unlink
    _addu(_T7, _T3, _T5),
    _lw(_T6, 4, _T7),
    _ori(_T6, _T6, 1),
    _sw(_T6, 4, _T7),                       # the next chunk's PREV_INUSE
    _jr(_RA),
    _addiu(_V0, _T3, 8),
    "m_top",
    _lw(_T3, 0, _T1),
    _lw(_T4, 4, _T3),
    _and(_T5, _T4, _T9),
    _sltu(_T6, _T5, _T0),
    _beq(_T6, _ZERO, "m_carve"),
    0,
    _jr(_RA),
    _addu(_V0, _ZERO, _ZERO),               # the top is too small: NULL
    "m_carve",
    _andi(_T6, _T4, 1),
    _or(_T6, _T6, _T0),
    _sw(_T6, 4, _T3),                       # the chunk keeps the top's PREV_INUSE
    _addu(_T7, _T3, _T0),
    _subu(_T5, _T5, _T0),
    _ori(_T5, _T5, 1),
    _sw(_T5, 4, _T7),                       # the new top follows a chunk in use
    _sw(_T7, 0, _T1),
    _jr(_RA),
    _addiu(_V0, _T3, 8),
]

# void _free_r(reent, mem): a chunk that borders the top joins it; any other is marked free in
# the header of the chunk after it and goes to the front of the bin.
_FREE_R = [
    _beq(_A1, _ZERO, "f_ret"),
    _addiu(_T9, _ZERO, -8),
    _addiu(_T3, _A1, -8),
    _lw(_T4, 4, _T3),
    _and(_T5, _T4, _T9),
    _addu(_T7, _T3, _T5),                   # t7: the chunk after this one
    *_state(_T1),
    _lw(_T6, 0, _T1),
    _beq(_T7, _T6, "f_top"),
    0,
    _lw(_T6, 4, _T7),
    _and(_T6, _T6, _T9),
    _sw(_T6, 4, _T7),                       # clear its PREV_INUSE
    _sw(_T5, 0, _T7),                       # and give it this chunk's size as prev_size
    _lw(_T6, 4, _T1),
    _sw(_T6, 8, _T3),
    _jr(_RA),
    _sw(_T3, 4, _T1),
    "f_top",
    _lw(_T6, 4, _T7),
    _and(_T6, _T6, _T9),
    _addu(_T6, _T6, _T5),
    _andi(_T4, _T4, 1),
    _or(_T6, _T6, _T4),
    _sw(_T6, 4, _T3),
    _jr(_RA),
    _sw(_T3, 0, _T1),
    "f_ret",
    _jr(_RA),
    0,
]

# void *_memalign_r(reent, alignment, size), as newlib's: allocate size + alignment + 16, and
# when the payload is not aligned, split the chunk by writing both headers itself and free the
# leading part.  The header it writes for the aligned chunk carries PREV_INUSE.
_MEMALIGN_R = [
    _addiu(_SP, _SP, -0x20),
    _sw(_RA, 0x1C, _SP),
    _sw(_S0, 0x18, _SP),
    _sw(_A1, 0x10, _SP),
    _addu(_A1, _A2, _A1),
    _jal("malloc_r"),
    _addiu(_A1, _A1, 16),
    _beq(_V0, _ZERO, "a_out"),
    _lw(_T0, 0x10, _SP),                    # t0: the alignment
    _addiu(_T1, _T0, -1),
    _addu(_T2, _V0, _T1),
    _subu(_T3, _ZERO, _T0),
    _and(_T2, _T2, _T3),                    # the first aligned address at or after the payload
    _beq(_T2, _V0, "a_out"),
    _addu(_S0, _T2, _ZERO),                 # s0: the aligned payload
    _addiu(_T4, _V0, -8),                   # t4: the chunk malloc returned
    _addiu(_T5, _S0, -8),                   # t5: the aligned chunk
    _subu(_T6, _T5, _T4),                   # t6: the leading part's size
    _sltiu(_T7, _T6, 16),
    _beq(_T7, _ZERO, "a_lead"),
    0,
    _addu(_S0, _S0, _T0),                   # too small to be a chunk: the next aligned address
    _addu(_T5, _T5, _T0),
    _addu(_T6, _T6, _T0),
    "a_lead",
    _lw(_T7, 4, _T4),
    _addiu(_T8, _ZERO, -8),
    _and(_T9, _T7, _T8),
    _subu(_T9, _T9, _T6),
    _ori(_T9, _T9, 1),
    _sw(_T9, 4, _T5),                       # the aligned chunk: the rest, PREV_INUSE set
    _andi(_T7, _T7, 1),
    _or(_T7, _T7, _T6),
    _sw(_T7, 4, _T4),                       # the leading chunk, shortened
    _jal("free_r"),
    _addiu(_A1, _T4, 8),
    _addu(_V0, _S0, _ZERO),
    "a_out",
    _lw(_RA, 0x1C, _SP),
    _lw(_S0, 0x18, _SP),
    _jr(_RA),
    _addiu(_SP, _SP, 0x20),
]

# void *_realloc_r(reent, mem, size): a chunk that is large enough stays; one that borders the
# top grows into it by rewriting both headers; any other moves: malloc, copy, free.
_REALLOC_R = [
    _addiu(_SP, _SP, -0x20),
    _sw(_RA, 0x1C, _SP),
    _sw(_S0, 0x18, _SP),
    _sw(_S1, 0x14, _SP),
    _addu(_S0, _A1, _ZERO),                 # s0: the old payload
    _addiu(_T0, _A2, 11),
    _addiu(_T9, _ZERO, -8),
    _and(_T0, _T0, _T9),
    _sltiu(_T1, _T0, 16),
    _beq(_T1, _ZERO, "r_sized"),
    0,
    _addiu(_T0, _ZERO, 16),
    "r_sized",
    _addiu(_T3, _S0, -8),
    _lw(_T4, 4, _T3),
    _and(_T5, _T4, _T9),                    # t5: the old chunk size
    _sltu(_T6, _T5, _T0),
    _beq(_T6, _ZERO, "r_keep"),
    0,
    _addu(_T7, _T3, _T5),
    *_state(_T1),
    _lw(_T6, 0, _T1),
    _bne(_T7, _T6, "r_move"),
    0,
    _lw(_T6, 4, _T7),
    _and(_T6, _T6, _T9),                    # t6: the top's size
    _subu(_T8, _T0, _T5),                   # t8: the growth
    _sltu(_T2, _T6, _T8),
    _bne(_T2, _ZERO, "r_move"),
    0,
    _andi(_T4, _T4, 1),
    _or(_T4, _T4, _T0),
    _sw(_T4, 4, _T3),
    _addu(_T7, _T3, _T0),
    _subu(_T6, _T6, _T8),
    _ori(_T6, _T6, 1),
    _sw(_T6, 4, _T7),
    _sw(_T7, 0, _T1),
    "r_keep",
    _beq(_ZERO, _ZERO, "r_done"),
    _addu(_V0, _S0, _ZERO),
    "r_move",
    _addiu(_S1, _T5, -8),                   # s1: the bytes the old chunk held
    _jal("malloc_r"),
    _addu(_A1, _A2, _ZERO),
    _beq(_V0, _ZERO, "r_done"),
    _addu(_T0, _ZERO, _ZERO),
    "r_copy",
    _beq(_T0, _S1, "r_copied"),
    _addu(_T1, _S0, _T0),
    _lw(_T2, 0, _T1),
    _addu(_T1, _V0, _T0),
    _sw(_T2, 0, _T1),
    _beq(_ZERO, _ZERO, "r_copy"),
    _addiu(_T0, _T0, 4),
    "r_copied",
    _sw(_V0, 0x10, _SP),
    _jal("free_r"),
    _addu(_A1, _S0, _ZERO),
    _lw(_V0, 0x10, _SP),
    "r_done",
    _lw(_RA, 0x1C, _SP),
    _lw(_S0, 0x18, _SP),
    _lw(_S1, 0x14, _SP),
    _jr(_RA),
    _addiu(_SP, _SP, 0x20),
]

_ANCHOR = [
    _addiu(_SP, _SP, -0x10),
    _sw(_RA, 0x0C, _SP),
    _jal("memalign_r"),
    0,
    _jal("realloc_r"),
    0,
    _lw(_RA, 0x0C, _SP),
    _jr(_RA),
    _addiu(_SP, _SP, 0x10),
]


def _allocator_fixture() -> dict[int, int]:
    at = {"free_r": FREE_R, "memalign_r": MEMALIGN_R, "malloc_r": MALLOC_R,
          "realloc_r": REALLOC_R}
    words: dict[int, int] = {}
    for base, program in ((FREE_R, _FREE_R), (MEMALIGN_R, _MEMALIGN_R), (MALLOC_R, _MALLOC_R),
                          (REALLOC_R, _REALLOC_R), (ANCHOR, _ANCHOR)):
        body = entry._assemble(base, program, at)
        assert not set(body) & set(words), f"the body at 0x{base:08x} overlaps another"
        words.update(body)
    return words


def _allocator_harness() -> str:
    return f"""\
{entry._executing_harness_support("allocator_funcs.h")}
static CpuState cpu;
static uint32_t call(void (*fn)(CpuState *), uint32_t a1, uint32_t a2) {{
    cpu.r[4] = 0x{_REENT:08x}u; cpu.r[5] = a1; cpu.r[6] = a2;
    cpu.r[31] = 0x12345678u;
    fn(&cpu); sr_tail_drain(&cpu);
    if (cpu.r[29] != 0x{_STACK:08x}u || cpu.r[31] != 0x12345678u ||
        cpu.r[16] != 0x5a5a5a5au || cpu.r[17] != 0x6b6b6b6bu) {{
        printf("a call lost sp, ra, s0 or s1\\n");
        exit(1);
    }}
    return cpu.r[2];
}}
#define MALLOC(size) call(f_{MALLOC_R:08x}, (size), 0u)
#define FREE(mem) ((void)call(f_{FREE_R:08x}, (mem), 0u))
#define MEMALIGN(alignment, size) call(f_{MEMALIGN_R:08x}, (alignment), (size))
#define REALLOC(mem, size) call(f_{REALLOC_R:08x}, (mem), (size))
#define HEAD(mem) test_mem_r32((mem) - 4u)       /* the chunk's size | PREV_INUSE */
#define PREV_SIZE(mem) test_mem_r32((mem) - 8u)
#define TOP() test_mem_r32(0x{_STATE:08x}u)
#define BIN() test_mem_r32(0x{_STATE + 4:08x}u)

int main(void) {{
    sr_register_all();
    cpu.r[16] = 0x5a5a5a5au; cpu.r[17] = 0x6b6b6b6bu; cpu.r[29] = 0x{_STACK:08x}u;
    /* One top chunk spans the heap; nothing precedes it, so its PREV_INUSE is set. */
    test_mem_w32(0x{_STATE:08x}u, 0x{_HEAP:08x}u);
    test_mem_w32(0x{_HEAP + 4:08x}u, 0x{_HEAP_END - _HEAP:08x}u | 1u);

    uint32_t a = MALLOC(24u), b = MALLOC(40u), c = MALLOC(24u);
    printf("malloc a=%05x b=%05x c=%05x heads=%x,%x,%x top=%05x\\n",
           a, b, c, HEAD(a), HEAD(b), HEAD(c), TOP());

    /* free records b as free in c's header and puts it in the bin. */
    FREE(b);
    printf("free b: c.head=%x c.prev_size=%x bin=%05x\\n", HEAD(c), PREV_SIZE(c), BIN());

    /* malloc takes b's chunk back out of the bin and marks it in use in c's header. */
    uint32_t d = MALLOC(36u);
    printf("malloc d=%05x c.head=%x bin=%05x\\n", d, HEAD(c), BIN());

    /* memalign splits a fresh chunk itself and frees the leading part through free. */
    uint32_t m = MEMALIGN(64u, 100u);
    printf("memalign m=%05x mod64=%u head=%x prev_size=%x bin=%05x top=%05x\\n",
           m, m % 64u, HEAD(m), PREV_SIZE(m), BIN(), TOP());

    /* realloc of a, which another chunk follows, moves it and frees the old chunk. */
    for (uint32_t i = 0; i < 24u; i += 4u) test_mem_w32(a + i, 0xa5000000u + i);
    uint32_t r = REALLOC(a, 200u);
    uint32_t kept = 1u;
    for (uint32_t i = 0; i < 24u; i += 4u) kept &= test_mem_r32(r + i) == 0xa5000000u + i;
    printf("realloc r=%05x kept=%u head=%x bin=%05x a.fd=%05x d.head=%x d.prev_size=%x\\n",
           r, kept, HEAD(r), BIN(), test_mem_r32(a), HEAD(d), PREV_SIZE(d));

    /* realloc of r, which borders the top, grows in place. */
    uint32_t g = REALLOC(r, 400u);
    for (uint32_t i = 0; i < 24u; i += 4u) kept &= test_mem_r32(g + i) == 0xa5000000u + i;
    printf("realloc g=%05x kept=%u head=%x top=%05x top.head=%x\\n",
           g, kept, HEAD(g), TOP(), test_mem_r32(TOP() + 4u));

    /* malloc reuses the chunk memalign freed, the second in the bin. */
    uint32_t e = MALLOC(64u);
    printf("malloc e=%05x m.head=%x bin=%05x a.fd=%05x\\n", e, HEAD(m), BIN(), test_mem_r32(a));
    return 0;
}}
"""


# What the instructions above produce, worked by hand from them.  Chunk sizes are the request
# plus 4, rounded up to 8: 24 -> 0x20, 40 -> 0x30, 36 -> 0x28, 180 -> 0xb8, 200 -> 0xd0,
# 400 -> 0x198, 64 -> 0x48.
_ALLOCATOR_OUTCOME = """\
malloc a=19008 b=19028 c=19058 heads=21,31,21 top=19070
free b: c.head=20 c.prev_size=30 bin=19020
malloc d=19028 c.head=21 bin=00000
memalign m=190c0 mod64=0 head=70 prev_size=48 bin=19070 top=19128
realloc r=19130 kept=1 head=d1 bin=19000 a.fd=19070 d.head=30 d.prev_size=20
realloc g=19130 kept=1 head=199 top=192c0 top.head=3d41
malloc e=19078 m.head=71 bin=19000 a.fd=00000"""


@unittest.skipUnless(entry.CC, "TOOLCHAIN_MISSING: gcc is required for the generated-C execution regression")
class RetailAllocatorFamilyRunsAsWrittenTests(unittest.TestCase):
    """The four allocator entry points run their own instructions under every profile.

    The fixture's memalign writes PREV_INUSE into a chunk header and frees the part before
    it, its realloc rewrites the top chunk's header, and its malloc then reuses what both
    freed: each depends on the others reading the same headers, bin and top in guest memory.
    The test fails while the profile emits anything but the guest's words at one of the four
    addresses, and if a pointer, a header, the bin or the top comes back different from what
    the instructions produce.  The harness's dispatch is a stand-in, so this checks the
    generated code, not the runtime's dispatch."""

    def _run(self, profile: str, work: Path) -> tuple[dict[int, str], str]:
        assert entry.CC is not None
        words = _allocator_fixture()
        elf = work / "allocator.elf"
        generated = work / "allocator.c"
        elf.write_bytes(entry._synthetic_elf(words, ANCHOR))
        result = subprocess.run(
            [sys.executable, str(entry.CODEGEN), str(elf), str(generated),
             f"--profile={profile}"],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        chunks = sorted(work.glob("allocator_*.c"))
        text = "\n".join(path.read_text(encoding="ascii") for path in chunks)
        bodies = {}
        for addr in ALLOCATOR_FAMILY:
            start = text.index(f"void f_{addr:08x}(CpuState *s)")
            body = text[start:text.index("\nvoid ", start + 1)]
            self.assertNotIn("custom stub", body,
                             f"profile={profile} emits a hand-written body at 0x{addr:08x}")
            self.assertNotIn("sr_newlib_", body,
                             f"profile={profile} bridges 0x{addr:08x} to the runtime's heap")
            self.assertIn(f"sr_begin(s, 0x{addr:08x}u, 0x{words[addr]:08x}u)", body,
                          f"profile={profile} does not translate the guest's first word")
            bodies[addr] = body
        self.assertIn(f"f_{MALLOC_R:08x}(s)", bodies[MEMALIGN_R], "memalign does not call malloc")
        self.assertIn(f"f_{FREE_R:08x}(s)", bodies[MEMALIGN_R], "memalign does not call free")
        self.assertIn(f"f_{MALLOC_R:08x}(s)", bodies[REALLOC_R], "realloc does not call malloc")
        self.assertIn(f"f_{FREE_R:08x}(s)", bodies[REALLOC_R], "realloc does not call free")
        (work / "recomp.h").write_text(entry._FORMATTER_RECOMP_H, encoding="ascii")
        (work / "harness.c").write_text(_allocator_harness(), encoding="ascii")
        exe = work / "allocator.exe"
        compiled = subprocess.run(
            [entry.CC, "-std=c11", "-O0", "-I", str(work), str(work / "harness.c"),
             str(generated), *(str(path) for path in chunks), "-lm", "-o", str(exe)],
            capture_output=True, text=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
        return bodies, ran.stdout.strip().replace("\r\n", "\n")

    def test_the_guest_allocator_family_runs_under_both_profiles(self):
        bodies = {}
        for profile in ("hst", "none"):
            with self.subTest(profile=profile), \
                    scratch_dir.TemporaryDirectory(prefix="allocator_") as tmp:
                bodies[profile], outcome = self._run(profile, Path(tmp))
                self.assertEqual(outcome, _ALLOCATOR_OUTCOME)
        self.assertEqual(bodies.get("hst"), bodies.get("none"),
                         "an allocator entry point diverged between profiles")


class RetiredAllocatorBridgeTests(unittest.TestCase):
    """No table names an allocator address, and nothing the generator emits names the heap
    the bridges used."""

    def test_no_allocator_address_has_a_hand_written_body(self):
        manifest = {item["address"] for item in compat_overrides.CODEGEN_CUSTOM_STUBS}
        for addr in ALLOCATOR_FAMILY:
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertNotIn(addr, codegen_hst_stubs.HST_CUSTOM_STUBS)
                self.assertNotIn(addr, HST_SIMPLE_STUBS)
                self.assertNotIn(addr, codegen.GUEST_PATCHES)
                self.assertNotIn(addr, manifest)

    def test_no_emitted_body_calls_the_runtime_heap(self):
        for addr, text in codegen_hst_stubs.HST_CUSTOM_STUBS.items():
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertNotIn("sr_newlib_", text)
        self.assertNotIn("sr_newlib_", (ROOT / "tools" / "codegen.py").read_text(encoding="utf-8"))

    def test_public_wrappers_translate_retail_callees(self):
        class FakeElf:
            def __init__(self, words):
                self.words = words

            def read_at_vaddr(self, addr, size):
                word = self.words.get(addr)
                return word.to_bytes(4, "little") if word is not None else None

        def jal(target):
            return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)

        # Reduced control-flow fixtures for the retail public wrappers:
        # malloc -> _getreent -> _malloc_r and free -> _getreent -> _free_r.
        elf = FakeElf({
            0x104B0: jal(0x0FE3C),
            0x104B4: 0,
            0x104B8: jal(0x10738),
            0x104BC: 0,
            0x104C0: 0x03E00008,
            0x104C4: 0,
            0x104E0: jal(0x0FE3C),
            0x104E4: 0,
            0x104E8: jal(0x0F538),
            0x104EC: 0,
            0x104F0: 0x03E00008,
            0x104F4: 0,
        })
        ranges = [(0x0F000, 0x10800)]
        known = {0x0F538, 0x0FE3C, 0x104B0, 0x104E0, 0x10738}
        malloc_text = "\n".join(codegen.emit_function(elf, 0x104B0, ranges, known))
        free_text = "\n".join(codegen.emit_function(elf, 0x104E0, ranges, known))
        self.assertIn("f_0000fe3c(s);", malloc_text)
        self.assertIn("f_00010738(s);", malloc_text)
        self.assertIn("f_0000fe3c(s);", free_text)
        self.assertIn("f_0000f538(s);", free_text)


if __name__ == "__main__":
    unittest.main()
