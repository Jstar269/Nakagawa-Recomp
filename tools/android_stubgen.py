# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Stub libraries for the on-device Android sysroot, from AOSP symbol files.

The compiler the APK ships (#360) links titles against libc, libm, libdl, liblog,
libandroid and libvulkan. It needs only their exported symbols, not the code: the
device supplies the real libraries at run time. So the sysroot carries stub shared
libraries, built here from the platform's own symbol files (`libc.map.txt` and the
others, pinned in android/toolchain/PROVENANCE.md) instead of being copied from the NDK.

A symbol file is a linker version script with tags in its comments:

    LIBC_Q { # introduced=29
      global:
        foo;            # arm64
        bar;            # var introduced-arm64=30
        baz;            # apex
      local:
        *;
    } LIBC_P;

For one API level and one architecture, a symbol is kept when its block and its own
tags allow both, and dropped when either is for the platform alone (`platform-only`,
`apex`, `systemapi`, `llndk`, `future`, or a block named `*_PRIVATE` or
`*_PLATFORM`). `introduced-<arch>=` wins over `introduced=`, and a symbol's own tag wins
over its block's. `var` makes a data symbol, `weak` a weak one. A symbol tagged
`versioned=N`, or one in a block tagged so, has its version only from API N; below that
it is exported unversioned. A block whose base is not exported names no base.

The output is a C file whose functions and variables have those names, and the
version script that gives them their versions. `clang -shared -nostdlib` builds the
stub from the pair. Written for this project from the documented symbol-file format;
no AOSP build code is used.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ARCHES = ("arm", "arm64", "riscv64", "x86", "x86_64")

# Release names a symbol file may use for an API level.
CODENAMES = {
    "G": 9,
    "I": 14,
    "J": 16,
    "J-MR1": 17,
    "J-MR2": 18,
    "K": 19,
    "L": 21,
    "L-MR1": 22,
    "M": 23,
    "N": 24,
    "N-MR1": 25,
    "O": 26,
    "O-MR1": 27,
    "P": 28,
    "Q": 29,
    "R": 30,
    "S": 31,
    "Sv2": 32,
    "Tiramisu": 33,
    "UpsideDownCake": 34,
    "VanillaIceCream": 35,
    "Baklava": 36,
}
FUTURE_API = 10000

#: Marks a pattern from a nested `extern "C++" { ... };` block (only platform blocks have them).
EXTERN_CXX_TAG = "<extern-c++>"

PLATFORM_TAGS = frozenset({"platform-only", "apex", "systemapi", "llndk", "future"})
PLATFORM_BLOCK_SUFFIXES = ("_PRIVATE", "_PLATFORM")

_BLOCK_OPEN = re.compile(r"^([A-Za-z_][A-Za-z0-9_.]*)\s*\{$")
_BLOCK_CLOSE = re.compile(r"^\}\s*([A-Za-z_][A-Za-z0-9_.]*)?\s*;$")
_SYMBOL = re.compile(r"^([A-Za-z_][A-Za-z0-9_.$]*)\s*;$")
_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class SymbolFileError(ValueError):
    """The symbol file breaks the format; the message names the line."""


@dataclass(frozen=True)
class Symbol:
    name: str
    tags: tuple[str, ...]


@dataclass
class Block:
    name: str
    base: str | None
    tags: tuple[str, ...]
    symbols: list[Symbol] = field(default_factory=list)


@dataclass(frozen=True)
class StubSymbol:
    name: str
    is_var: bool
    is_weak: bool


@dataclass(frozen=True)
class StubVersion:
    name: str | None  # None: the symbols are unversioned at this API level
    base: str | None
    symbols: tuple[StubSymbol, ...]


def parse_api(value: str, where: str) -> int:
    if value.isdigit():
        return int(value)
    if value in CODENAMES:
        return CODENAMES[value]
    if value in ("current", "future"):
        return FUTURE_API
    raise SymbolFileError(f"{where}: unknown API level {value!r}")


def _split_comment(line: str) -> tuple[str, tuple[str, ...]]:
    code, sep, comment = line.partition("#")
    tags = tuple(comment.split()) if sep else ()
    return code.strip(), tags


def parse_symbol_file(text: str, source: str = "<symbol file>") -> list[Block]:
    """Parse the blocks of a symbol file, keeping every tag for the filter."""
    blocks: list[Block] = []
    current: Block | None = None
    scope: str | None = None
    in_extern = False
    for number, raw in enumerate(text.splitlines(), start=1):
        where = f"{source}:{number}"
        code, tags = _split_comment(raw)
        if not code:
            continue
        if in_extern:
            # Mangled-name patterns of a nested extern "C++" { ... }; block.
            if code == "};":
                in_extern = False
            elif code.endswith(";"):
                assert current is not None
                current.symbols.append(Symbol(name=code[:-1].strip(), tags=(*tags, EXTERN_CXX_TAG)))
            else:
                raise SymbolFileError(f"{where}: expected a pattern ending in ';', found {code!r}")
            continue
        if current is not None and code == 'extern "C++" {':
            if scope != "global":
                raise SymbolFileError(f"{where}: extern \"C++\" is outside a global scope")
            in_extern = True
            continue
        if current is None:
            match = _BLOCK_OPEN.match(code)
            if not match:
                raise SymbolFileError(f"{where}: expected a version block, found {code!r}")
            current = Block(name=match.group(1), base=None, tags=tags)
            scope = None
            continue
        close = _BLOCK_CLOSE.match(code)
        if close:
            current.base = close.group(1)
            blocks.append(current)
            current = None
            continue
        if code in ("global:", "local:"):
            scope = code[:-1]
            continue
        if scope == "local":
            if code != "*;":
                raise SymbolFileError(f"{where}: a local scope may only hide everything ('*;')")
            continue
        match = _SYMBOL.match(code)
        if not match:
            raise SymbolFileError(f"{where}: expected 'name;', found {code!r}")
        if scope != "global":
            raise SymbolFileError(f"{where}: symbol {match.group(1)!r} is outside a global scope")
        current.symbols.append(Symbol(name=match.group(1), tags=tags))
    if current is not None:
        raise SymbolFileError(f"{source}: block {current.name!r} is not closed")
    if in_extern:
        raise SymbolFileError(f"{source}: an extern \"C++\" block is not closed")
    names = [block.name for block in blocks]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise SymbolFileError(f"{source}: version blocks defined twice: {', '.join(duplicates)}")
    return blocks


def _tag_value(tags: tuple[str, ...], key: str) -> str | None:
    prefix = key + "="
    values = [tag[len(prefix):] for tag in tags if tag.startswith(prefix)]
    if len(values) > 1:
        raise SymbolFileError(f"tag {key}= given twice in {' '.join(tags)!r}")
    return values[0] if values else None


def _introduced(tags: tuple[str, ...], arch: str, where: str) -> int | None:
    value = _tag_value(tags, f"introduced-{arch}")
    if value is None:
        value = _tag_value(tags, "introduced")
    return None if value is None else parse_api(value, where)


def _arch_allows(tags: tuple[str, ...], arch: str) -> bool:
    named = [tag for tag in tags if tag in ARCHES]
    return not named or arch in named


def _is_platform(tags: tuple[str, ...]) -> bool:
    return any(tag in PLATFORM_TAGS for tag in tags)


def select(blocks: list[Block], api: int, arch: str, unversioned: bool = False) -> list[StubVersion]:
    """The versions and symbols a stub for `api` on `arch` exports, in file order.

    With *unversioned*, the same symbols are exported without any version: the NDK ships
    liblog, libandroid and libvulkan that way (their platform build marks them unversioned
    for every released API level), although their symbol files name versions.
    """
    if arch not in ARCHES:
        raise SymbolFileError(f"unknown architecture {arch!r}; expected one of {', '.join(ARCHES)}")
    versions: list[StubVersion] = []
    seen: dict[str, str] = {}
    for block in blocks:
        where = f"block {block.name}"
        if block.name.endswith(PLATFORM_BLOCK_SUFFIXES) or _is_platform(block.tags):
            continue
        if not _arch_allows(block.tags, arch):
            continue
        block_introduced = _introduced(block.tags, arch, where)
        if block_introduced is not None and block_introduced > api:
            continue
        block_versioned = _tag_value(block.tags, "versioned")
        kept: list[StubSymbol] = []
        bare: list[StubSymbol] = []
        for symbol in block.symbols:
            if _is_platform(symbol.tags) or not _arch_allows(symbol.tags, arch):
                continue
            introduced = _introduced(symbol.tags, arch, f"{where}, symbol {symbol.name}")
            if introduced is None:
                introduced = block_introduced
            if introduced is not None and introduced > api:
                continue
            if EXTERN_CXX_TAG in symbol.tags:
                raise SymbolFileError(
                    f"{where}: the extern \"C++\" pattern {symbol.name!r} would be exported, "
                    "and a C stub cannot define it"
                )
            if not _IDENT.match(symbol.name):
                raise SymbolFileError(f"{where}: {symbol.name!r} cannot be a C identifier in a stub")
            if symbol.name in seen:
                raise SymbolFileError(
                    f"{where}: {symbol.name!r} is already exported by {seen[symbol.name]}"
                )
            seen[symbol.name] = block.name
            stub = StubSymbol(symbol.name, "var" in symbol.tags, "weak" in symbol.tags)
            versioned = _tag_value(symbol.tags, "versioned") or block_versioned
            if unversioned or (
                versioned is not None and api < parse_api(versioned, f"{where}, symbol {symbol.name}")
            ):
                bare.append(stub)
            else:
                kept.append(stub)
        if kept or not bare:
            versions.append(StubVersion(block.name, block.base, tuple(kept)))
        if bare:
            versions.append(StubVersion(None, None, tuple(bare)))
    return versions


def render_stub_c(versions: list[StubVersion], library: str) -> str:
    lines = [
        f"/* Stub {library} generated by tools/android_stubgen.py: symbols only, no code. */",
        "/* The device's own library provides the definitions at run time. */",
        "",
    ]
    for version in versions:
        for symbol in version.symbols:
            weak = "__attribute__((weak)) " if symbol.is_weak else ""
            if symbol.is_var:
                lines.append(f"{weak}int {symbol.name} = 0;")
            else:
                lines.append(f"{weak}void {symbol.name}(void) {{}}")
    return "\n".join(lines) + "\n"


def render_version_script(versions: list[StubVersion]) -> str:
    # No "local: *;": the stub defines only the listed symbols, and a wildcard would also
    # hide the ones that stay unversioned at this API level.
    lines: list[str] = []
    emitted: set[str] = set()
    for version in versions:
        if version.name is None:
            continue
        lines.append(f"{version.name} {{")
        if version.symbols:
            lines.append("  global:")
            lines.extend(f"    {symbol.name};" for symbol in version.symbols)
        base = version.base if version.base in emitted else None
        lines.append(f"}} {base};" if base else "};")
        emitted.add(version.name)
    return "\n".join(lines) + "\n" if lines else ""


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("symbol_file", type=Path, help="an AOSP *.map.txt")
    parser.add_argument("--api", required=True, help="API level, a number or a release name")
    parser.add_argument("--arch", required=True, choices=ARCHES)
    parser.add_argument("--library", required=True, help="the stub's name, such as libc.so")
    parser.add_argument("--out-c", required=True, type=Path)
    parser.add_argument("--out-version-script", required=True, type=Path)
    parser.add_argument("--unversioned", action="store_true",
                        help="export the symbols without versions (liblog, libandroid, libvulkan)")
    args = parser.parse_args(argv)
    try:
        api = parse_api(args.api, "--api")
        text = args.symbol_file.read_text(encoding="utf-8")
        versions = select(parse_symbol_file(text, str(args.symbol_file)), api, args.arch,
                          unversioned=args.unversioned)
    except (OSError, SymbolFileError) as error:
        print(f"android_stubgen: {error}", file=sys.stderr)
        return 1
    args.out_c.write_bytes(render_stub_c(versions, args.library).encode("utf-8"))
    args.out_version_script.write_bytes(render_version_script(versions).encode("utf-8"))
    count = sum(len(version.symbols) for version in versions)
    print(f"android_stubgen: {args.library}: {count} symbols for API {api} on {args.arch}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
