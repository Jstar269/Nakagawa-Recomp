#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Wrap Anime4K's mpv user shaders as Vulkan compute shaders for the movie upscaler.

Anime4K (third_party/anime4k, MIT) ships each upscaler as one mpv user-shader file: a sequence of
passes, each a ``//!`` directive block (BIND, SAVE, WIDTH, HEIGHT, ...) followed by GLSL that
defines ``vec4 hook()`` over mpv's texture macros (``NAME_tex``, ``NAME_pos``, ``NAME_pt``,
``NAME_size``, ``NAME_texOff``). This tool keeps every pass body byte for byte and wraps it in a
compute shader that defines those macros over Vulkan bindings, one shader per pass, plus a C
header describing the pass graph for src/rt/gpu_sdl3vk/anime4k_pass.c.

    python tools/anime4k_port.py generate   # rewrite the generated files
    python tools/anime4k_port.py check      # fail if they are not what generate writes

The generated shaders carry the upstream MIT notice and say they were modified (wrapped).
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "third_party" / "anime4k"
GPU_DIR = ROOT / "src" / "rt" / "gpu_sdl3vk"
SHADER_DIR = GPU_DIR / "shaders"
HEADER = GPU_DIR / "anime4k_passes.h"

#: (upstream file, prefix of the generated shaders and of the C symbols)
SOURCES = (("Anime4K_Upscale_CNN_x2_M.glsl", "anime4k_cnn_x2_m"),)

MAX_BINDS = 8
OUTPUT_BINDING = MAX_BINDS


@dataclass
class Pass:
    desc: str
    binds: list[str]
    save: str
    scale: int  # output size relative to the stage input: 1 or 2
    components: int
    body: list[str] = field(default_factory=list)


def _size_scale(width: str, height: str) -> int:
    """WIDTH/HEIGHT are ``NAME.w`` or ``NAME.w 2 *``; every texture of one stage but the output has
    the input's size, so the scale is the trailing factor."""
    scales = []
    for expr in (width, height):
        parts = expr.split()
        if len(parts) == 1 and re.fullmatch(r"\w+\.[wh]", parts[0]):
            scales.append(1)
        elif len(parts) == 3 and re.fullmatch(r"\w+\.[wh]", parts[0]) and parts[1] == "2" and parts[2] == "*":
            scales.append(2)
        else:
            raise ValueError(f"unsupported size expression {expr!r}")
    if scales[0] != scales[1]:
        raise ValueError("WIDTH and HEIGHT scale differently")
    return scales[0]


def parse(text: str) -> tuple[list[str], list[Pass]]:
    """The licence comment before the first pass, and the passes."""
    lines = text.split("\n")
    first = next(i for i, line in enumerate(lines) if line.startswith("//!DESC"))
    notice = [line for line in lines[:first] if line.startswith("//") or not line.strip()]
    passes: list[Pass] = []
    i = first
    while i < len(lines):
        if not lines[i].startswith("//!DESC"):
            if lines[i].strip():
                raise ValueError(f"line {i + 1}: text outside a pass")
            i += 1
            continue
        directives: dict[str, list[str]] = {}
        while i < len(lines) and lines[i].startswith("//!"):
            key, _, value = lines[i][3:].partition(" ")
            directives.setdefault(key, []).append(value.strip())
            i += 1
        body = []
        while i < len(lines) and not lines[i].startswith("//!DESC"):
            body.append(lines[i])
            i += 1
        while body and not body[-1].strip():
            body.pop()
        unknown = set(directives) - {"DESC", "HOOK", "BIND", "SAVE", "WIDTH", "HEIGHT", "COMPONENTS", "WHEN"}
        if unknown:
            raise ValueError(f"unsupported directives {sorted(unknown)}")
        if directives.get("HOOK") != ["MAIN"]:
            raise ValueError("only passes that hook MAIN are supported")
        binds = directives["BIND"]
        if len(binds) > MAX_BINDS:
            raise ValueError(f"{len(binds)} bound textures; at most {MAX_BINDS}")
        passes.append(Pass(
            desc=directives["DESC"][0],
            binds=binds,
            save=directives["SAVE"][0],
            scale=_size_scale(directives["WIDTH"][0], directives["HEIGHT"][0]),
            components=int(directives.get("COMPONENTS", ["4"])[0]),
            body=body,
        ))
    if passes[-1].save != "MAIN" or passes[-1].scale != 2:
        raise ValueError("the last pass must write MAIN at twice the size")
    return notice, passes


def shader(source: str, notice: list[str], index: int, p: Pass) -> str:
    out = [
        "#version 450",
        "// SPDX-License-Identifier: MIT",
        *notice,
        "",
        f"// Modified for Nakagawa Recomp: pass {index} ({p.desc}) of third_party/anime4k/{source},",
        "// wrapped as a Vulkan compute shader by tools/anime4k_port.py. The pass body below is",
        "// upstream's, unchanged; only the wrapper around it is new. Do not edit: regenerate.",
        "",
        "layout(local_size_x = 8, local_size_y = 8) in;",
    ]
    for b, name in enumerate(p.binds):
        out.append(f"layout(set = 0, binding = {b}) uniform sampler2D nk_tex{b};  // {name}")
    out += [
        f"layout(set = 0, binding = {OUTPUT_BINDING}, rgba16f) uniform writeonly image2D nk_out;  // {p.save}",
        "layout(push_constant) uniform NkPass { ivec2 out_size; } nk_pc;",
        "vec2 nk_pos;",
    ]
    for b, name in enumerate(p.binds):
        aliases = [name, "HOOKED"] if name == "MAIN" else [name]
        for alias in aliases:
            out += [
                f"#define {alias}_tex(p) texture(nk_tex{b}, (p))",
                f"#define {alias}_pos nk_pos",
                f"#define {alias}_size vec2(textureSize(nk_tex{b}, 0))",
                f"#define {alias}_pt (vec2(1.0) / {alias}_size)",
                f"#define {alias}_texOff(off) {alias}_tex({alias}_pos + {alias}_pt * (off))",
            ]
    out += p.body
    out += [
        "void main() {",
        "    ivec2 p = ivec2(gl_GlobalInvocationID.xy);",
        "    if (any(greaterThanEqual(p, nk_pc.out_size))) return;",
        "    nk_pos = (vec2(p) + 0.5) / vec2(nk_pc.out_size);",
        "    imageStore(nk_out, p, hook());",
        "}",
        "",
    ]
    return "\n".join(out)


def outputs() -> dict[Path, str]:
    files: dict[Path, str] = {}
    head = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "/* Copyright (C) 2026 the Nakagawa Recomp authors */",
        "/* anime4k_passes.h - generated by tools/anime4k_port.py from third_party/anime4k: the pass",
        " * graph of each Anime4K upscaler for src/rt/gpu_sdl3vk/anime4k_pass.c. Do not edit: regenerate.",
        " * A texture index is 0 for the stage input (MAIN) and 1 + k for the output of pass k. */",
        "#ifndef SR_ANIME4K_PASSES_H",
        "#define SR_ANIME4K_PASSES_H",
        "",
        f"#define ANIME4K_MAX_BINDS {MAX_BINDS}",
        f"#define ANIME4K_OUTPUT_BINDING {OUTPUT_BINDING}",
        "",
        "typedef struct {",
        "    int binds;                       /* bound textures */",
        "    int bind[ANIME4K_MAX_BINDS];     /* their texture indexes */",
        "    int scale;                       /* output size over the stage input's: 1 or 2 */",
        "} Anime4kPassDesc;",
        "",
    ]
    for source, prefix in SOURCES:
        text = (UPSTREAM / source).read_text(encoding="utf-8")
        notice, passes = parse(text)
        index_of = {"MAIN": 0}
        head.append(f"/* {source} */")
        head.append(f"#define {prefix.upper()}_PASSES {len(passes)}")
        head.append(f"static const Anime4kPassDesc k_{prefix}[{prefix.upper()}_PASSES] = {{")
        for k, p in enumerate(passes):
            name = f"{prefix}_{k}"
            files[SHADER_DIR / f"{name}.comp"] = shader(source, notice, k, p)
            try:
                idx = [index_of[b] for b in p.binds]
            except KeyError as missing:
                raise ValueError(f"pass {k} binds {missing} before it is written") from None
            padded = idx + [0] * (MAX_BINDS - len(idx))
            head.append(f"    {{ {len(idx)}, {{ {', '.join(map(str, padded))} }}, {p.scale} }},  /* {p.desc} */")
            if p.save != "MAIN":
                index_of[p.save] = k + 1
        head.append("};")
        head.append("")
    head.append("#endif /* SR_ANIME4K_PASSES_H */")
    head.append("")
    files[HEADER] = "\n".join(head)
    return files


def shader_list() -> list[tuple[str, str]]:
    """(source, embedded) pairs, relative to src/rt/gpu_sdl3vk, for tools/shader_embed.py."""
    pairs = []
    for source, prefix in SOURCES:
        _notice, passes = parse((UPSTREAM / source).read_text(encoding="utf-8"))
        for k in range(len(passes)):
            pairs.append((f"shaders/{prefix}_{k}.comp", f"{prefix}_{k}_comp.inc"))
    return pairs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=("generate", "check"))
    args = ap.parse_args()
    stale = []
    for path, text in outputs().items():
        current = path.read_text(encoding="utf-8") if path.is_file() else None
        if current == text:
            continue
        if args.command == "generate":
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"wrote {path.relative_to(ROOT).as_posix()}")
        else:
            stale.append(path.relative_to(ROOT).as_posix())
    if stale:
        print("anime4k_port: stale generated files (run tools/anime4k_port.py generate):", *stale, sep="\n  ")
        return 1
    if args.command == "check":
        print("anime4k_port: generated files are current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
