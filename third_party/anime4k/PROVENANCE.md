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

## Local deltas

None. The wrapped compute shaders are generated files outside this folder; `python tools/anime4k_port.py
check` fails when they differ from what the generator writes from this file.
