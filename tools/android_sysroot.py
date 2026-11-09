# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""The on-device compiler's sysroot, built from AOSP bionic (#360).

The compiler the APK ships needs a C sysroot for aarch64-linux-android<api>: the libc and
kernel headers, the crt objects every program links, and stub shared libraries to link
against. The NDK has one, but the project builds its own from a pinned bionic commit, so
nothing from the NDK ships:

- headers: `libc/include`, the kernel's `libc/kernel/uapi` (its `asm-arm64/asm` under
  `aarch64-linux-android/asm`, as the NDK lays it out) and `libc/kernel/android/uapi`;
- crt objects: `crtbegin_dynamic.o`, `crtbegin_so.o`, `crtend_android.o`, `crtend_so.o`
  and `crt_pad_segment.o`, compiled from `libc/arch-common/bionic` by the clang given, with
  `crtbrand.S` (the `.note.android.ident` that names the API level) linked into the two
  crtbegin objects, as bionic's own build does;
- stubs: libc.so, libm.so and libdl.so from their symbol files, through
  tools/android_stubgen.py.

Every input is read from the bionic clone's git objects, not its working tree: a Windows
checkout rewrites line endings, and eight kernel headers differ only in letter case
(`ipt_TTL.h` and `ipt_ttl.h`), so one of each pair is missing there. The output is one tar
archive with fixed timestamps, owners and modes, sorted by path; the device's file system
keeps both names. A Windows compile needs only headers outside those pairs, so the scratch
copy the compiler reads leaves the pairs out.

The clone is data: nothing in it runs, and paths reach the compiler as arguments.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_stubgen  # noqa: E402

BIONIC_URL = "https://android.googlesource.com/platform/bionic"
BIONIC_TAG = "android-16.0.0_r4"
BIONIC_COMMIT = "f22516cbc67e81c13cf943ce02f01b8929a98726"

ARCH = "arm64"
TRIPLE_ARCH = "aarch64"
ASM_DIR = "aarch64-linux-android"

#: (source folder in bionic, destination in the sysroot). Kernel folders for other
#: architectures stay out.
HEADER_TREES = (
    ("libc/include", "usr/include"),
    ("libc/kernel/android/uapi", "usr/include"),
)
UAPI = "libc/kernel/uapi"
UAPI_ARCH = f"{UAPI}/asm-{ARCH}/asm"
UAPI_OTHER_ARCHES = tuple(f"{UAPI}/asm-{a}/" for a in ("arm", "arm64", "riscv", "x86"))

#: What the crt sources include beyond the public headers.
CRT_SOURCES = ("libc/arch-common/bionic", "libc/private", "libc/bionic/libc_init_common.h")

STUBS = (
    ("libc.so", "libc/libc.map.txt"),
    ("libm.so", "libm/libm.map.txt"),
    ("libdl.so", "libdl/libdl.map.txt"),
)
NOTICES = ("libc/NOTICE", "libm/NOTICE", "libdl/NOTICE")


class SysrootError(RuntimeError):
    """The inputs are not the pinned ones, or a build step failed; the message says which."""


@dataclass(frozen=True)
class Entry:
    data: bytes
    executable: bool = False


def _git(bionic: Path, *args: str) -> bytes:
    command = ["git", "-c", "core.autocrlf=false", "-C", str(bionic), *args]
    result = subprocess.run(command, capture_output=True, check=False)
    if result.returncode != 0:
        raise SysrootError(f"git {' '.join(args)}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def read_bionic(bionic: Path, commit: str = BIONIC_COMMIT) -> dict[str, bytes]:
    """The files the sysroot needs, from the clone's objects at *commit*."""
    head = _git(bionic, "rev-parse", "--verify", f"{commit}^{{commit}}").decode().strip()
    if head != commit:
        raise SysrootError(f"{bionic} resolves {commit} to {head}")
    paths = [tree for tree, _ in HEADER_TREES] + [UAPI, *CRT_SOURCES]
    paths += [map_file for _, map_file in STUBS] + list(NOTICES)
    archive = _git(bionic, "archive", "--format=tar", commit, "--", *paths)
    files: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar.getmembers():
            if member.isfile():
                extracted = tar.extractfile(member)
                assert extracted is not None
                files[member.name] = extracted.read()
            elif not member.isdir():
                raise SysrootError(f"{member.name}: only files and folders are expected in bionic")
    return files


def header_entries(files: dict[str, bytes]) -> dict[str, Entry]:
    """The sysroot's usr/include, laid out as the NDK lays it out."""
    out: dict[str, Entry] = {}

    def put(dest: str, data: bytes, source: str) -> None:
        if dest in out:
            raise SysrootError(f"{source} and an earlier file both map to {dest}")
        out[dest] = Entry(data)

    for path, data in sorted(files.items()):
        for tree, dest in HEADER_TREES:
            if path.startswith(tree + "/"):
                put(f"{dest}/{path[len(tree) + 1:]}", data, path)
        if path.startswith(UAPI_ARCH + "/"):
            put(f"usr/include/{ASM_DIR}/asm/{path[len(UAPI_ARCH) + 1:]}", data, path)
        elif path.startswith(UAPI + "/") and not path.startswith(UAPI_OTHER_ARCHES):
            put(f"usr/include/{path[len(UAPI) + 1:]}", data, path)
    return out


def case_collisions(paths: list[str]) -> set[str]:
    seen: dict[str, list[str]] = {}
    for path in paths:
        seen.setdefault(path.lower(), []).append(path)
    return {path for group in seen.values() if len(group) > 1 for path in group}


def _write_tree(root: Path, entries: dict[str, bytes], skip: set[str]) -> None:
    for path, data in entries.items():
        if path in skip:
            continue
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def _run(command: list[str], what: str) -> None:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SysrootError(f"{what} failed ({result.returncode}): {result.stderr.strip()[:2000]}")


def build_objects(files: dict[str, bytes], headers: dict[str, Entry], clang: Path, api: int,
                  scratch: Path) -> dict[str, Entry]:
    """crt objects and stub libraries, compiled by *clang* for aarch64-linux-android<api>."""
    skip = case_collisions(list(headers))
    sysroot = scratch / "sysroot"
    _write_tree(sysroot, {p: e.data for p, e in headers.items()}, skip)
    src = scratch / "bionic"
    _write_tree(src, {p: d for p, d in files.items() if p.startswith(CRT_SOURCES)}, set())
    target = f"--target={TRIPLE_ARCH}-linux-android{api}"
    common = [
        str(clang), target, f"--sysroot={sysroot}", "-c", "-O2", "-fPIC",
        "-fno-stack-protector", "-Wall", "-Werror", "-Wno-gcc-compat",
        f"-I{src / 'libc'}", f"-I{src / 'libc' / 'bionic'}", f"-I{src / 'libc' / 'private'}",
        f"-DPLATFORM_SDK_VERSION={api}",
        f"-ffile-prefix-map={scratch}=/sysroot-build",
    ]
    crt = src / "libc" / "arch-common" / "bionic"
    objects = scratch / "obj"
    objects.mkdir()

    def compile_one(name: str, source: str) -> Path:
        out = objects / name
        _run([*common, "-o", str(out), str(crt / source)], f"compiling {source}")
        return out

    def partial_link(name: str, *inputs: Path) -> Path:
        out = objects / name
        _run([str(clang), target, "-fuse-ld=lld", "-nostdlib", "-r", "-o", str(out),
              *map(str, inputs)], f"linking {name}")
        return out

    brand = compile_one("crtbrand.o", "crtbrand.S")
    built = {
        "crtbegin_dynamic.o": partial_link(
            "crtbegin_dynamic.o", compile_one("crtbegin.o", "crtbegin.c"), brand),
        "crtbegin_so.o": partial_link(
            "crtbegin_so.o", compile_one("crtbegin_so_part.o", "crtbegin_so.c"), brand),
        "crtend_android.o": compile_one("crtend_android.o", "crtend.S"),
        "crtend_so.o": compile_one("crtend_so.o", "crtend_so.S"),
        "crt_pad_segment.o": compile_one("crt_pad_segment.o", "crt_pad_segment.S"),
    }
    lib_dir = f"usr/lib/{TRIPLE_ARCH}-linux-android/{api}"
    out = {f"{lib_dir}/{name}": Entry(path.read_bytes()) for name, path in built.items()}

    for library, map_file in STUBS:
        text = files[map_file].decode("utf-8")
        versions = android_stubgen.select(
            android_stubgen.parse_symbol_file(text, map_file), api, ARCH)
        stem = library.removesuffix(".so")
        c_file, script = objects / f"{stem}.c", objects / f"{stem}.map"
        c_file.write_bytes(android_stubgen.render_stub_c(versions, library).encode())
        script.write_bytes(android_stubgen.render_version_script(versions).encode())
        stub = objects / library
        _run([str(clang), target, "-shared", "-nostdlib", "-fuse-ld=lld", "-fPIC", "-O2",
              f"-Wl,-soname,{library}", f"-Wl,--version-script={script}", "-Wl,--build-id=sha1",
              f"-ffile-prefix-map={scratch}=/sysroot-build", "-o", str(stub), str(c_file)],
             f"linking the {library} stub")
        out[f"{lib_dir}/{library}"] = Entry(stub.read_bytes(), executable=True)
    return out


def notice_entries(files: dict[str, bytes]) -> dict[str, Entry]:
    return {f"NOTICES/bionic/{path}": Entry(files[path]) for path in NOTICES}


def write_archive(entries: dict[str, Entry], out: Path) -> str:
    """A tar of *entries*, sorted, with fixed metadata; returns its SHA-256."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        folders: set[str] = set()
        for path in sorted(entries):
            # Every parent folder once, outermost first ("." is the archive root, not an entry).
            for parent in reversed(list(PurePosixPath(path).parents)[:-1]):
                name = parent.as_posix()
                if name not in folders:
                    folders.add(name)
                    info = tarfile.TarInfo(name)
                    info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, 0
                    tar.addfile(info)
            entry = entries[path]
            info = tarfile.TarInfo(path)
            info.size, info.mtime = len(entry.data), 0
            info.mode = 0o755 if entry.executable else 0o644
            tar.addfile(info, io.BytesIO(entry.data))
    data = buffer.getvalue()
    out.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bionic", required=True, type=Path,
                        help=f"a clone of {BIONIC_URL} holding {BIONIC_TAG} ({BIONIC_COMMIT[:12]})")
    parser.add_argument("--clang", required=True, type=Path, help="the clang that builds the objects")
    parser.add_argument("--api", type=int, default=29)
    parser.add_argument("--out", required=True, type=Path, help="the sysroot tar to write")
    args = parser.parse_args(argv)
    try:
        files = read_bionic(args.bionic)
        headers = header_entries(files)
        with tempfile.TemporaryDirectory(prefix="nk-sysroot-") as tmp:
            objects = build_objects(files, headers, args.clang, args.api, Path(tmp))
        entries = {**headers, **objects, **notice_entries(files)}
        digest = write_archive(entries, args.out)
    except (OSError, SysrootError, android_stubgen.SymbolFileError) as error:
        print(f"android_sysroot: {error}", file=sys.stderr)
        return 1
    collisions = sorted(case_collisions(list(headers)))
    print(f"android_sysroot: {args.out}: {len(entries)} files, sha256 {digest}")
    print(f"android_sysroot: {len(collisions)} headers differ from another only in case; the tar keeps all")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
