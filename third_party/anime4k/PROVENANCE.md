# PROVENANCE.md — `third_party/anime4k/`

Anime4K's CNN x2 (M) upscaler: a small convolutional network, written as an mpv user shader, that
doubles a picture and predicts the detail a bilinear doubling loses. It suits the flat colours and
dark lines of animated and pre-rendered movies. `tools/anime4k_port.py` wraps each of its nine passes,
body unchanged, as a Vulkan compute shader (`src/rt/gpu_sdl3vk/shaders/anime4k_cnn_x2_m_0.comp` to
`anime4k_cnn_x2_m_8.comp`, which keep the MIT notice and say they were modified) and writes the pass
graph to `src/rt/gpu_sdl3vk/anime4k_passes.h`. The native player runs them as the opt-in movie
upscaler (`SR_MOVIE_UPSCALE=anime4k`) on CPU-presented movie frames.

## Upstream source

- Project: Anime4K
- Repository: `https://github.com/bloc97/Anime4K`
- Pin: commit `4029bf701ecaa15f163cdc49cffe5501c1acf410` (2021-09-16), the `v4.0.1` release tag. The
  file's own description names the network version, `Anime4K-v3.2-Upscale-CNN-x2-(M)`. The two
  imported files are unchanged at the default branch's head at the time of import.
- License: MIT. Copyright (c) 2019-2021 bloc97 (the shader's notice) and Copyright (c) 2019 bloc97
  (`LICENSE`). The notice is kept at the top of the shader and in `LICENSE.txt`.
- Import method: `git show <pin>:<path>` from a clone of the repository at the pin, so the bytes are
  the Git blobs, unchanged. Upstream `LICENSE` is stored as `LICENSE.txt`, with its bytes unchanged.

## File manifest

| File | Kind | Upstream path | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `Anime4K_Upscale_CNN_x2_M.glsl` | imported byte-identical | `glsl/Upscale/Anime4K_Upscale_CNN_x2_M.glsl` | `156c6bdd3b80a6c784ddf6655493bb35337cc0ec` | `716e02098a68f0d648761f2b96b4dd139e1cb09b174bb369fca3aa34328fff7e` |
| `LICENSE.txt` | imported byte-identical (upstream `LICENSE`) | `LICENSE` | `140b2c983100845a3ad52e12d20573459d0f484c` | `5bad448b737378e3d0c977ad0d0521fa37ad279a7e76ea9a31d9257eeb6953f5` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a | n/a |

## Derived files

The nine compute shaders are not in this folder. Each is an adapted file: the upstream pass body is
kept as it is, under the MIT notice and a modification note, inside the project's bindings. They are
listed here as LOCAL DELTA rows, like `third_party/xbrz/PROVENANCE.md`. `python tools/anime4k_port.py
check` fails when one differs from what the generator writes from the imported file.

| File | Kind | Upstream source | SHA-256 |
| --- | --- | --- | --- |
| `shaders/anime4k_cnn_x2_m_0.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 0 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x3x3x3`) | `86233a317def05d87d74f76548262706c1aa01db8610a56aacc09d1c0862ff6a` |
| `shaders/anime4k_cnn_x2_m_1.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 1 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x3x3x8`) | `b0f24f9dff7ee14f3d050b6bfbc2c4126a4804804fed26d1e0f90580daebeb13` |
| `shaders/anime4k_cnn_x2_m_2.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 2 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x3x3x8`) | `91a781ceb9775fd0baeb24fa3d2987415e161c26d67e842ca75ce6a6d69746e7` |
| `shaders/anime4k_cnn_x2_m_3.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 3 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x3x3x8`) | `4347216d7d0723d6568a296b3f75f705cf3bb31b2900be4de624b803d53552ae` |
| `shaders/anime4k_cnn_x2_m_4.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 4 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x3x3x8`) | `8f29006e73653e8a9dd883245407b29a4beb29c5d049d1c71ced44428a945f40` |
| `shaders/anime4k_cnn_x2_m_5.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 5 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x3x3x8`) | `49e9a03daeaa0dd357e15fddb9044a7fa1d26b54242cc2451ceafe3d67543484` |
| `shaders/anime4k_cnn_x2_m_6.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 6 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x3x3x8`) | `d4384490b4ee18d2fd3fb8648e91bfae87b366dd3b8fef57c47cdb08d466298b` |
| `shaders/anime4k_cnn_x2_m_7.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 7 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Conv-4x1x1x56`) | `d046874474b356764b1ba663b9d2c4d83ba8f3d45658fb81c366aeb22a0b9bc9` |
| `shaders/anime4k_cnn_x2_m_8.comp` (under `src/rt/gpu_sdl3vk/`) | adapted + LOCAL DELTA: wrapped as a Vulkan compute shader by `tools/anime4k_port.py`; the pass body is unchanged | pass 8 of `Anime4K_Upscale_CNN_x2_M.glsl` (`Anime4K-v3.2-Upscale-CNN-x2-(M)-Depth-to-Space`) | `0d137f6ffc12f18da826c4739ecd5c0d07821b5b155b0cfc658104dc01cee99d` |

## Local deltas

The two imported files above carry none: they are byte-identical. The local deltas are the nine
derived shaders in the table above. The nine `anime4k_cnn_x2_m_*_comp.inc` files in
`src/rt/gpu_sdl3vk/` are their compiled SPIR-V (`tools/shader_embed.py`), and `anime4k_passes.h` there
is the pass graph (`tools/anime4k_port.py`); both are generated from this import, hold upstream text
only as compiled shader code and pass names, and are recomputable.
