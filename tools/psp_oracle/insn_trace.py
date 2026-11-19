# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Real-PSP instruction-trace differential (#312).

The fixtures in ``tests/fixtures/psp_oracle/insn_trace_fixtures.h`` run one instruction at a
time on a PSP (``CASE=insn-trace``, ``probe_insn_trace.c``) and through Nakagawa's own
code generator. This module turns both into v2 traces (``tools/TRACE_FORMAT.md``) and
compares them with ``tracediff.strict_hardware_diff``, which names the first record that
differs.

    insn_trace.py psp <host0-log> <out-dir> --model M --firmware F
        one PSP_HARDWARE trace per fixture from the probe's host0 log
    insn_trace.py local <out-dir> --source-commit C --model M --firmware F [--fixture ID]
        one LOCAL_COSIM trace per fixture: tools/codegen.py on the fixture's words, the
        headless traced driver of tools/codegen_gate.py, the fixture's initial state
    insn_trace.py compare <psp.trace> <local.trace>
        exit 0 on STRICT_V2_AGREEMENT, 1 on the first divergence, 2 on a rejected input

Both sides record, per executed instruction (delay slots included, an annulled likely
delay slot not), the traced registers that changed (r1..r25, r29..r31, hi, lo, selected
f0..f3/fcr31 and v0..v3/v32) and the store the instruction made. r26..r28 are never
traced: the probe's trampoline owns them and no fixture names them. The separate
``CASE=insn-trace-exception`` fixture faults in a branch delay slot; the PSPLink runner
retains its EPC/Cause/BadVAddr/Status frame in the case teardown report.
"""

from __future__ import annotations
from typing import Any

import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
FIXTURES_H = ROOT / "tests" / "fixtures" / "psp_oracle" / "insn_trace_fixtures.h"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import tracediff  # noqa: E402

FIXTURE_ID = "insn-trace"
TEST_ID = "PSP-TRACE-001"
EXIT_SYSCALL_WORD = 0x0008430C  # syscall 0x210c: the gate build's stop (tools/gate_stub.c)
TRACED_GPRS = tuple(range(1, 26)) + (29, 30, 31)
TRACED_FPRS = tuple(range(4))
TRACED_VFPU = (0, 1, 2, 3, 32)
TRACE_STATE_NAMES = tuple(
    [f"r{i}" for i in TRACED_GPRS] + ["hi", "lo"]
    + [f"f{i}" for i in TRACED_FPRS] + ["fcr31"]
    + [f"v{i}" for i in TRACED_VFPU]
)
RECORD_WORDS = 2 + len(TRACE_STATE_NAMES) + 2   # pc, op, selected state, store address/value
STORE_BITS = {0x28: 8, 0x29: 16, 0x2B: 32}     # sb, sh, sw: the store width follows from the op
END_REACHED = 0
# Host-side bound on one traced driver run (a few dozen steps). It is generous because a newly
# built executable can take seconds to start on a loaded host.
LOCAL_TRACE_TIMEOUT_SECONDS = 30.0


class InsnTraceError(ValueError):
    """An input does not satisfy the instruction-trace contract."""


def parse_fixtures(path: Path = FIXTURES_H) -> dict:
    """Read the fixtures header: constants, word arrays and the fixture table."""
    text = path.read_text(encoding="utf-8")
    consts = {name: int(value, 16) for name, value in
              re.findall(r"#define (NT_[A-Z0-9_]+) (0x[0-9a-fA-F]+)u", text)}
    arrays = {}
    for name, body in re.findall(r"static const uint32_t (nt_\w+)_words\[\] = \{(.*?)\};", text, re.S):
        arrays[name] = [(int(word, 16), asm.strip()) for word, asm in
                        re.findall(r"(0x[0-9a-fA-F]{8})u,\s*/\*\s*(.*?)\s*\*/", body)]
    fixtures = []
    table_start = text.find("static const NtFixture nt_fixtures")
    if table_start < 0:
        raise InsnTraceError("fixture table not found")
    table = text[table_start:]
    for fid, base, count, array in re.findall(
            r'\{"([\w-]+)", (0x[0-9a-fA-F]+)u, (\d+)u, (nt_\w+)_words\}', table):
        if array not in arrays:
            raise InsnTraceError(f"{fid}: fixture words array {array}_words not found")
        words = arrays[array]
        if len(words) != int(count):
            raise InsnTraceError(f"{fid}: table count {count} != {len(words)} words")
        fixtures.append({"id": fid, "base": int(base, 16), "words": [w for w, _ in words],
                         "asm": [a for _, a in words]})
    count_match = re.search(r"#define NT_FIXTURE_COUNT (\d+)u", text)
    if count_match is None:
        raise InsnTraceError("NT_FIXTURE_COUNT define not found")
    declared = int(count_match.group(1))
    if declared != len(fixtures):
        raise InsnTraceError(f"NT_FIXTURE_COUNT {declared} != {len(fixtures)} table rows")
    return {"consts": consts, "fixtures": fixtures}


def initial_state(consts: dict) -> dict:
    """The register state every fixture starts from (the header's contract)."""
    regs = {f"r{i}": (0x01010101 * i) & 0xFFFFFFFF for i in range(1, 32)}
    regs["r4"] = consts["NT_DATA_BASE"]
    regs["r29"] = consts["NT_DATA_BASE"] + 0x800
    for i in (26, 27, 28):
        regs[f"r{i}"] = 0
    regs["hi"] = consts["NT_HI_INIT"]
    regs["lo"] = consts["NT_LO_INIT"]
    regs.update({f"f{i}": 0 for i in range(32)})
    regs["f0"] = consts["NT_FPU_F0_INIT"]
    regs["f1"] = consts["NT_FPU_F1_INIT"]
    regs["f2"] = consts["NT_FPU_F2_INIT"]
    regs["f3"] = consts["NT_FPU_F3_INIT"]
    regs["fcr31"] = consts["NT_FCR_INIT"]
    regs.update({f"v{i}": 0 for i in range(128)})
    vfpu_init_names = (
        "NT_VFPU_S000_INIT", "NT_VFPU_S001_INIT", "NT_VFPU_S002_INIT",
        "NT_VFPU_S003_INIT", "NT_VFPU_S200_INIT",
    )
    for index, const_name in zip(TRACED_VFPU, vfpu_init_names, strict=True):
        regs[f"v{index}"] = consts[const_name]
    return regs


def image_sha256(fixture: dict) -> str:
    """binary_sha256 of a fixture: its words, little-endian (the executed guest image)."""
    return hashlib.sha256(b"".join(struct.pack("<I", w) for w in fixture["words"])).hexdigest()


def words_fnv(words) -> int:
    """FNV-1a over the words' little-endian bytes, as the probe reports it."""
    h = 0x811C9DC5
    for word in words:
        for byte in struct.pack("<I", word):
            h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def fixture_elf(fixture: dict) -> bytes:
    """A fixed-address ELF32 image of the fixture's words followed by the exit syscall."""
    import analyze  # noqa: PLC0415 - use the code generator's shared ELF builder

    text = b"".join(struct.pack("<I", w) for w in fixture["words"] + [EXIT_SYSCALL_WORD])
    base = fixture["base"]
    image = bytearray(analyze._fixed_address_elf(
        base,
        text,
        len(text),
        [(b".text", 1, 0x6, base, 0, len(text))],
    ))
    # The trace gate starts at the fixture address and maps its executable
    # segment as RX; the shared analyzer writer's defaults describe a full
    # RWX image with no entry point.
    struct.pack_into("<I", image, 24, base)
    struct.pack_into("<I", image, 64, base)
    struct.pack_into("<I", image, 76, 5)
    return bytes(image)


def v2_header(source_tier: str, fixture: dict, steps: int, *, source_commit: str,
              model: str, firmware: str) -> str:
    return (f"# psp-recomp trace v2 source_tier={source_tier} fixture_id={FIXTURE_ID} "
            f"cell_id={fixture['id']} binary_sha256={image_sha256(fixture)} "
            f"source_commit={source_commit} model={model} firmware={firmware} "
            f"start_pc=0x{fixture['base']:08x} steps={steps} complete=1")


def _state_tokens(before: dict, after: dict) -> list[str]:
    return [f"{name}=0x{after[name]:08x}" for name in TRACE_STATE_NAMES
            if after[name] != before[name]]


def psp_records(log_text: str, fixture: dict, consts: dict) -> tuple[list[str], str]:
    """Step lines for one fixture from the probe log, and the log's source commit."""
    meta_lines = [line for line in log_text.splitlines()
                  if line.startswith("NAKAGAWA_PSP_META ")]
    if len(meta_lines) != 1:
        raise InsnTraceError("the log must contain exactly one NAKAGAWA_PSP_META line")
    meta = re.search(
        r"^NAKAGAWA_PSP_META .*source_commit=([0-9a-fA-F]{40}(?:[0-9a-fA-F]{24})?)(?![0-9a-fA-F])",
        meta_lines[0],
    )
    if not meta:
        raise InsnTraceError("the log has no NAKAGAWA_PSP_META line with a source commit")
    fid = fixture["id"]
    records = {}
    for line in log_text.splitlines():
        m = re.match(rf"NAKAGAWA_PSP_TEST schema=1 test_id={TEST_ID} case_id={re.escape(fid)}-(\S+) "
                     r"status=(\S+) result=0x([0-9a-f]{8})((?: out\d+=0x[0-9a-f]{8})*)$", line)
        if m:
            key = m.group(1)
            if key in records:
                raise InsnTraceError(f"{fid}: duplicate record {key}")
            records[key] = (m.group(2), int(m.group(3), 16),
                            [int(v, 16) for v in re.findall(r"=0x([0-9a-f]{8})", m.group(4))])
    if "init" not in records or "end" not in records:
        raise InsnTraceError(f"{fid}: the log has no init or end record")
    init = records["init"][2]
    if len(init) != 4 + len(TRACE_STATE_NAMES):
        raise InsnTraceError(f"{fid}: the init record is malformed")
    if init[0] != fixture["base"] or init[1] != len(fixture["words"]):
        raise InsnTraceError(f"{fid}: the probe ran a different fixture (base/count)")
    if init[2] != words_fnv(fixture["words"]):
        raise InsnTraceError(f"{fid}: the probe's words differ from the header (FNV)")
    status, result, end = records["end"]
    if status != "PASS" or result != END_REACHED:
        raise InsnTraceError(
            f"{fid}: the probe did not reach the end address "
            f"(status {status}, result 0x{result:08x})"
        )
    if len(end) != 4:
        raise InsnTraceError(f"{fid}: end record is malformed")
    steps = end[0]
    names = TRACE_STATE_NAMES
    state = dict(zip(names, init[4:], strict=True))
    expected = initial_state(consts)
    if any(state[n] != expected[n] for n in names) or init[3] != consts["NT_DATA_BASE"]:
        raise InsnTraceError(f"{fid}: the probe's initial state differs from the header contract")
    lines = []
    for step in range(steps):
        key = f"s{step:04d}"
        if key not in records or len(records[key][2]) != RECORD_WORDS:
            raise InsnTraceError(f"{fid}: step record {key} is missing or malformed")
        out = records[key][2]
        after = dict(zip(names, out[2:2 + len(names)], strict=True))
        tokens = _state_tokens(state, after)
        addr, value = out[-2:]
        kind = STORE_BITS.get(out[1] >> 26, 0)
        if kind:
            width = {8: 2, 16: 4, 32: 8}[kind]
            tokens.append(f"m{kind}[0x{addr:08x}]=0x{value:0{width}x}")
        lines.append(f"{step} pc=0x{out[0]:08x} op=0x{out[1]:08x}" + "".join(f" {t}" for t in tokens))
        state = after
    return lines, meta.group(1)


def local_records(fixture: dict, consts: dict, workdir: Path, mutate=None) -> list[str]:
    """Step lines for one fixture from Nakagawa's generated code (the traced gate driver)."""
    import codegen_gate  # noqa: PLC0415 - tools/ is on sys.path above

    workdir.mkdir(parents=True, exist_ok=True)
    elf = workdir / f"{fixture['id']}.elf"
    elf.write_bytes(fixture_elf(fixture))
    init = workdir / "init.trace"
    state = initial_state(consts)
    init.write_text("# init " + " ".join(f"{k}=0x{v:08x}" for k, v in state.items()) + "\n",
                    encoding="utf-8", newline="\n")
    env = codegen_gate.gate_env()
    drv = codegen_gate.build_traced_driver(str(elf), str(workdir), env, mutate=mutate)
    if drv is None:
        raise InsnTraceError(f"{fixture['id']}: the traced driver did not build")
    out = workdir / "local.trace"
    # The exit syscall at the end address is the only way the gate build's sr_syscall is
    # reached (tools/gate_stub.c); its SR_PPM_DUMP side file is the proof that the
    # generated code ran to the end address rather than stopping anywhere else.
    marker = workdir / "exit-reached.ppm"
    env = dict(env, SR_PPM_DUMP=str(marker))
    try:
        proc = subprocess.run(
            [drv, str(elf), str(init), str(out)],
            capture_output=True,
            text=True,
            env=env,
            timeout=LOCAL_TRACE_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise InsnTraceError(
            f"{fixture['id']}: traced driver timed out after {LOCAL_TRACE_TIMEOUT_SECONDS:g}s"
        ) from exc
    if proc.returncode != 0 or not out.exists():
        raise InsnTraceError(f"{fixture['id']}: the traced driver failed ({proc.returncode})")
    if not marker.exists():
        raise InsnTraceError(f"{fixture['id']}: the generated code did not reach the end address")
    end = fixture["base"] + 4 * len(fixture["words"])
    lines: list[Any] = []
    for raw in out.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            continue
        parts = raw.split()
        if int(parts[1][3:], 16) == end:
            break
        kept = [p for p in parts[3:] if not re.fullmatch(r"r2[678]=.*", p)]
        lines.append(" ".join([str(len(lines))] + parts[1:3] + kept))
    return lines


def write_trace(path: Path, header: str, lines: list[str]) -> None:
    path.write_text(header + "\n" + "".join(line + "\n" for line in lines), encoding="utf-8",
                    newline="\n")


def _local_source_problem(source_commit: str) -> str | None:
    try:
        head = subprocess.run(
            ["git", "rev-parse", "--verify", "HEAD"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=normal"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return f"source checkout could not be verified ({type(exc).__name__})"
    if head.returncode != 0 or status.returncode != 0:
        return "source checkout could not be verified by Git"
    if status.stdout.strip():
        return "source worktree is dirty"
    if head.stdout.strip().casefold() != source_commit.casefold():
        return "source commit does not match the clean checkout HEAD"
    return None


def compare(psp_trace: str, local_trace: str) -> int:
    try:
        divergence = tracediff.strict_hardware_diff(psp_trace, local_trace)
    except (tracediff.HardwareTraceError, OSError, UnicodeDecodeError) as exc:
        print(f"REJECTED: {exc}")
        return 2
    if divergence is None:
        print("STRICT_V2_AGREEMENT: PSP_HARDWARE == LOCAL_COSIM")
        return 0
    step, pc, detail = divergence
    print(f"FIRST_DIVERGENCE: step {step}, pc {pc}: {detail}")
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    psp = sub.add_parser("psp")
    psp.add_argument("log")
    psp.add_argument("out")
    local = sub.add_parser("local")
    local.add_argument("out")
    local.add_argument("--source-commit", required=True)
    local.add_argument("--fixture", action="append")
    for p in (psp, local):
        p.add_argument("--model", required=True)
        p.add_argument("--firmware", required=True)
    cmp_ = sub.add_parser("compare")
    cmp_.add_argument("psp")
    cmp_.add_argument("local")
    args = parser.parse_args(argv)
    if args.command == "compare":
        return compare(args.psp, args.local)
    if args.command == "local":
        source_problem = _local_source_problem(args.source_commit)
        if source_problem:
            print(f"REJECTED: {source_problem}", file=sys.stderr)
            return 2
    try:
        data = parse_fixtures(FIXTURES_H)
    except (InsnTraceError, OSError, UnicodeError, ValueError, KeyError) as exc:
        detail = str(exc) if isinstance(exc, InsnTraceError) else type(exc).__name__
        print(f"REJECTED: fixture header could not be parsed ({detail})", file=sys.stderr)
        return 2
    if args.command == "local":
        fixture_ids = {fixture["id"] for fixture in data["fixtures"]}
        unknown = sorted(set(args.fixture or ()) - fixture_ids)
        if unknown:
            print(f"REJECTED: unknown fixture id(s): {', '.join(unknown)}", file=sys.stderr)
            return 2
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    try:
        if args.command == "psp":
            for stale in out.glob("*.psp.trace"):
                stale.unlink()
            text = Path(args.log).read_text(encoding="utf-8")
            converted: list[tuple[dict, list[str], str]] = []
            for fixture in data["fixtures"]:
                lines, commit = psp_records(text, fixture, data["consts"])
                converted.append((fixture, lines, commit))
            for fixture, lines, commit in converted:
                header = v2_header("PSP_HARDWARE", fixture, len(lines), source_commit=commit,
                                   model=args.model, firmware=args.firmware)
                write_trace(out / f"{fixture['id']}.psp.trace", header, lines)
                print(f"{fixture['id']}: {len(lines)} PSP_HARDWARE records")
            return 0
        selected = [
            fixture for fixture in data["fixtures"]
            if not args.fixture or fixture["id"] in args.fixture
        ]
        for fixture in selected:
            (out / f"{fixture['id']}.local.trace").unlink(missing_ok=True)
        converted_local: list[tuple[dict, list[str]]] = []
        for fixture in selected:
            with tempfile.TemporaryDirectory(dir=os.environ.get("NK_INSN_TRACE_TMP")) as tmp:
                lines = local_records(fixture, data["consts"], Path(tmp))
            converted_local.append((fixture, lines))
        for fixture, lines in converted_local:
            header = v2_header("LOCAL_COSIM", fixture, len(lines), source_commit=args.source_commit,
                               model=args.model, firmware=args.firmware)
            write_trace(out / f"{fixture['id']}.local.trace", header, lines)
            print(f"{fixture['id']}: {len(lines)} LOCAL_COSIM records")
        return 0
    except InsnTraceError as exc:
        print(f"REJECTED: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
