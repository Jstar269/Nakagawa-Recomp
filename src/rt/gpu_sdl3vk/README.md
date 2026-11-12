# SDL3 + Vulkan GPU renderer

GPU renderer for the recompiled runtime. It replaces the Win32/GDI presentation path and, with
`SR_GPU_GE=1` (which the player sets for every launch), the software GE rasterizer in `src/rt/ge.c`,
which stays as the fallback. Built from scratch in this
directory — it is the project's own Vulkan backend and does **not** reuse PPSSPP's GPU. (An
earlier PPSSPP-GPU bridge under `src/rt/gpu_vk/` was a frozen experiment and has been removed.)

> **AI disclosure:** this renderer was written with substantial assistance from an LLM
> (Anthropic Claude). It is an original implementation, but it reproduces PSP/GE pixel
> semantics derived from PPSSPP's software renderer (through `ge.c`), so it is GPL-2.0-or-later.
> See [`../../../NOTICE.md`](../../../NOTICE.md). Treat "faithfulness" claims below as
> intent, not an audited guarantee.

## Why this shape

The software GE in `ge.c` is now the reference-correct implementation (clipping, PSP
primitive acceptance, strip cull parity, depth semantics — all validated against PPSSPP's
software renderer and in-game). The GPU renderer is introduced *under* it in phases, so at
every step there is a known-good fallback and an A/B comparison target. `ge.c` stays the
arbiter of GE semantics; this backend starts as a presenter and absorbs stages of the
pipeline one at a time.

## Build / toolchain

- SDL3 (`mingw-w64-ucrt-x86_64-sdl3`) and Vulkan headers + loader
  (`mingw-w64-ucrt-x86_64-vulkan-headers`, `-vulkan-loader`) from MSYS2 UCRT64.
  Use the versions resolved by the supported setup; do not treat a local package version as a
  repository compatibility promise. Link: `-lSDL3 -lvulkan-1`. Deploy `SDL3.dll` beside the exe.
- Shader compiler for Phase 1+: `glslc` from the current Vulkan SDK's `Bin` directory; it is only
  needed when regenerating the checked-in shader headers.
- Built into the game exe by the top-level `Makefile` (the runtime always links this SDL3 +
  Vulkan backend; `-DSR_SDL3VK`). Inside that build, `SR_VIDEO=gdi` falls back to the classic
  Win32/GDI window at runtime.

## Phases

### Phase 0 — presentation (DONE, this directory)

SDL3 window + Vulkan instance/device/swapchain. The software GE keeps rasterizing into
guest VRAM; `gui_present` converts the flipped framebuffer to BGRA once (same loop as the
GDI path) and `sdl3vk_present_rgba()` uploads + blits it to the swapchain (FIFO/vsync,
aspect-correct letterbox). Input moves to SDL3 (keyboard + SDL_Gamepad, same PSP-bit
mapping as gui.c). Deliverable: pixel-identical output to the GDI build at lower CPU cost,
resizable window, fullscreen-capable.

### Phase 1 — GE draw capture → Vulkan rasterization (DONE, `ge_gpu.c`)

Enabled with `SR_GPU_GE=1`. Implementation:

- Capture seam: runtime hooks (`GeGpuHooks` in `src/rt/ge_shared.h`, registered via
  `ge_set_gpu_hooks`) at the top of `raster_tri` and `fill_sprite`. ge.c still does
  vertex decode, T&L, near-clipping, primitive acceptance and cull-order reordering;
  the backend receives finished screen-space primitives. The GDI build registers
  nothing and is bit-identical to before.
- Vulkan side: 512x272 RGBA8 + D16 offscreen target, classic render pass (LOAD/STORE),
  one uber fragment shader (`shaders/psp.frag`: texfunc 0-7 + RGBA/double bits, alpha
  test, fog, minz/maxz discard, clear mode), pipelines cached per
  blend/depth/cull/write-mask key, dynamic viewport/scissor/blend-constants, vertices
  pre-snapped to the 28.4 grid, noperspective interpolation of (u·rw, v·rw, rw) to
  match the software inner loop exactly.
- **Guest VRAM stays authoritative**: at every flush point (GE list END, framebuffer
  retarget, texture-from-VRAM, CLUT-from-VRAM, present, software fallback) the target
  is rendered and written back to guest VRAM + the software z-buffer in the native
  format (16-bit round trip is lossless). Anything unsupported — lines, points,
  doubled-alpha blend factors, min/max/absdiff blend equations, two distinct FIX
  constants, partial-byte write masks — flushes and falls back to the software
  rasterizer in correct order. Presentation/snapshot code is unchanged.
- Not reproduced on GPU: ordered dithering (±4 LSB on 16-bit targets). Fixed-function
  blends differ from the software blender's truncating integer products by at most 2 LSB
  (analytic bound: truncation of two products plus round-to-nearest). Shader blends are
  bit-exact to `ge.c` for non-overlapping primitives. Measured by the
  `gpu-coherence-selftest` blend-parity matrix (726 factor/equation states on synthetic
  draws). Overlapping primitives inside one batch (#2109) depend on the destination-read
  path below.
- Destination reads (shader blends and the stencil test, #2109). On the snapshot path every
  primitive of a batch reads one copy of the target taken before the batch, so a primitive
  drawn over an earlier one of the same batch blends against the pre-batch pixels, not
  against what that primitive stored. A stencil state that reads its own writes therefore
  starts a new batch, with a new copy, wherever a primitive's box meets the earlier ones. A
  write-once stencil state is the exception (`stencil_write_once`: the first fragment to pass
  at a pixel stores a stencil that rejects every later one, with no depth write that could
  change which fragment passes): the pixel is that one fragment over the pre-batch
  destination, so its primitives share the batch, drawn last to first, and match `ge.c` with
  one copy. On the interlock path (`VK_EXT_fragment_shader_interlock`
  with `fragmentShaderPixelInterlock` and `fragmentStoresAndAtomics`) such batches run
  `psp_frag_rmw.inc`, `psp.frag` built with `-DSR_DST_INTERLOCK`. Each fragment loads the
  target as a storage image, blends, and stores it inside a pixel-ordered interlock, so
  overlapping primitives match `ge.c`. These batches record in a depth-only render pass with
  the colour image in `GENERAL`. The other batches of the op keep the fixed-function pass, with
  a barrier at each switch, and the op takes no snapshot copy. The variant tests depth early.
  A run of these batches that holds one that writes depth and may still kill a fragment in the
  shader (the alpha test, the color test, the transform-mode depth range when it is narrower
  than 0–65535, or a stencil-fail KEEP) tests and writes depth in the shader instead, inside the
  interlock and after those kills: the run's box of the D16 depth image is copied into a storage
  buffer before its pass and back after it, bit for bit, and its pipelines test no depth. With
  depth=32 (#2051) the depth image is D32, which that copy does not pack, so such an op keeps
  the snapshot path. An op
  keeps the snapshot path when its destination-reading batch textures from its own target (the
  destination copy is its texture), and every op into a widescreen wing canvas does (it has no
  RMW objects). The host twin (#2050) does not change the path: the image is drawn as in a
  twin-off run, and the twin's pass of an interlock op records the same runs into the twin, which
  it loads and stores as a storage image in place of the twin snapshot. The `submit-batch` and
  `gpu-timestamps` cells hold the
  snapshot path on every device, since they count and time snapshot copies. The interlock path is
  taken where the device has it; `SR_GPU_DST_READ` forces a path. On the snapshot path the
  device, images, render passes and default shader are made exactly as before. The
  `gpu-coherence-selftest` dst-read cells check both paths against `ge.c`.
- Source-premultiplied states (src factor 2·α or 1−2α with a dst factor that reads the
  source colour, and min/max with those src factors) go through the shader or drop the
  premultiply, because fixed-function blending would otherwise read the premultiplied
  colour that `ge.c` never uses.
- The R6 colour product in `ge_raster_ref.c` (SPEC_ASSUMPTION until the #343 P8/P9 oracle
  cells run) is not the software blender's product. On the same synthetic draws both the
  GPU and `ge.c` differ from R6 by at most 2 LSB, so R6 is not a parity target until a
  hardware cell settles which product the PSP uses.
- A/B harness: run twice with `SR_FBSNAP=1` (once `SR_GPU_GE=0`), then
  `python tools/ppmdiff.py dirA dirB`. Title/menu sequence matches pixel-exactly
  except dithering. Measured GE cost on the intro: ~1700ms/60-frames software →
  ~330ms/60-frames GPU (~5x).

### Phase 2 — textures on GPU (DONE, folded into Phase 1)

- Texture cache keyed by (addr, fmt, dims, bufw, swizzle, wrap, filter) + a sparse FNV
  content hash (CLUT RAM + format word included for CLUT4/CLUT8). Decode goes through
  `ge_decode_tex_rgba()` in ge.c — the exact software sampler path — so swizzle/CLUT
  behaviour is reference-correct by construction. 256 entries, full-drop eviction.
- Invalidation is the content hash ("hash on use", PPSSPP-style). Render-to-texture
  works because texturing from VRAM forces a flush+writeback first, then the hash
  sees the new pixels.

### Phase 3 — full GE on GPU / cleanup

- T&L in the vertex shader (#318), opt-in with `SR_GPU_TNL=1`. ge.c decodes the vertices of a
  transform-mode triangle draw (morphing included) and hands them, with the draw's matrices,
  fog and light constants, to `hook_tnl_prim`; `shaders/psp_tnl.vert` then skins, transforms,
  fogs, lights (ambient and diffuse, directional and point) and projects them. ge.c offers a
  draw only when a clip-space bound shows that no vertex is clipped, dropped or non-finite, and
  only in states the shader repeats exactly: no specular or powered-diffuse light while ge.c's
  lighting uses the C library's `powf` (the default), texture map mode 0, gouraud shading, no
  clear mode, every
  float input zero or of magnitude 2^-40..2^40. The shader evaluates ge.c's arithmetic in C's
  order with `precise` (Vulkan rounds add and multiply correctly; the reference has no fused
  multiply-add) and divides and takes square roots exactly in integer arithmetic, subnormal
  operands included, so its vertices reach the rasterizer with the bits the CPU T&L gives them.
  Intermediate values can be subnormal even with every input in range (the squared length of
  a small transformed normal), and the CPU keeps them, so the shader declares DenormPreserve:
  the stage runs only on a device that enables `VK_KHR_shader_float_controls` with
  `shaderDenormPreserveFloat32`. On other devices, NVIDIA's among them, ge.c keeps T&L and
  `SR_GPU_TNL=1` says so once. Everything else, and a draw the fragment state sends to
  software, keeps ge.c's T&L. The `tnl-parity` cells of `gpu-coherence-selftest` compare the
  two routes byte for byte (on a device without the feature, every cell on ge.c's route).
  `ge_tnl_reason_counts` counts each draw ge.c keeps under the first reason that rules the route
  out (clear, flat, diagnostic, texgen, matrix, viewport, texcoord, fog, specular, light, alloc,
  vertex, clip, backend). With `SR_GESTAT=1` as well, each stat window from the first
  transform-mode triangle draw prints that window's counts as
  `GESTAT-TNL f=N taken=N cpu=N: clear=N flat=N ... backend=N`, every reason in that order.
  `ge-tnl-census-selftest` drives one draw per reason through ge.c and checks the line over two
  windows. Left:
  texgen, flat shading, specular light by default, sprites, lines, points, clipped draws, and
  devices that flush fp32 subnormals.
- `ge_powf`, the specular and powered-diffuse power, is one text in ge.c and `psp_tnl.vert`
  (between the `BEGIN ge_pow helpers` and `END ge_powf` markers): 32-bit integer arithmetic
  only, table-driven log2 and exp2 in 64-bit fixed point, correctly rounded. With
  `ge_pow_set_exact(1)` ge.c's lighting uses it and the GPU T&L takes specular and
  powered-diffuse lights; it stays off until it is measured against the PSP (#1732, the
  `ge-light` oracle case). `tools/test_shader_embed.py` keeps the two copies identical and runs
  the shader's copy as C against ge.c's, and the `specular-ge-powf` and `light-4-ge-powf`
  parity cells compare the routes on a device that runs the T&L stage.
- Not done: render-to-texture chains (the hangar uses fbp swaps), depth readbacks if any game code
  reads zbuf, small-tri rasterization rules (top-left bias already documented in
  raster_tri) via conservative settings.
- The GDI presenter and the software GE stay in the build. The software GE is never deleted:
  it is the documentation of GE semantics.

## Files

- `sdl3vk.h` — C ABI used by `gui.c` (`-DSR_SDL3VK`); also exports the shared Vulkan
  device handles (`sdl3vk_get_vk`). Frame capture is armed and reported through the
  presenter-neutral `src/rt/fbcap.h`, not through this header.
- `sdl3vk.c` — Phase 0: window, device, swapchain, upload/blit presenter, SDL input,
  validation messenger, and the present-truthful frame capture.
- `ge_gpu.h` / `ge_gpu.c` — Phase 1+2: capture, batching, pipeline/texture caches,
  flush + guest-VRAM writeback. Hooks defined in `src/rt/ge_shared.h`.
- `pipeline_cache.h` — the on-disk Vulkan pipeline cache (#2149): the file's header (GPU,
  driver, pipeline cache UUID and shader set), its checks, and the atomic write. `ge_gpu.c`
  loads it at start and saves it at exit; `pipeline_cache_selftest.c` checks every refusal.
- `shaders/psp.vert`, `shaders/psp.frag` — compiled by glslc (`GLSLC` in the Makefile)
  into `psp_{vert,frag}.inc` at build time.
- `shaders/psp_tnl.vert` — the GPU transform-and-lighting vertex stage (`SR_GPU_TNL=1`, #318), a GLSL
  translation of ge.c's T&L; compiled the same way into `psp_tnl_vert.inc`.
- `tools/ppmdiff.py`, `tools/ppm2png.py` (repo root) — A/B snapshot comparison.

## Present-truthful frame capture

The old `sdl3vk_capture_swapchain_ppm` was an invalid acquisition: it could read a stale or
UNDEFINED-layout image and published a PPM under a `.png` name. It is gone. In its place:

- Capture state and the P6 publisher are presenter-neutral and live in `src/rt/fbcap.{h,c}`:
  `sr_capture_arm(path)` arms a capture for the *next* present, `sr_capture_result()`,
  `sr_capture_cancel()` and `sr_capture_source_label()` expose and resolve the outcome, and the
  publisher writes an exact P6 `.ppm` (verified header, atomic temp+rename). The offscreen and
  GDI presenters in `gui.c` publish their converted frame through the same service, so a
  capture means the same thing whichever presenter showed the frame.
- This presenter's part is the readback. When a capture is armed, `cap_record` runs inside the
  presenting command buffer, immediately after the blit, so the buffer content is exactly what
  was presented — no extra acquire, no layout discard, tight `w*4` pitch, no UNDEFINED
  transitions anywhere on the capture path. The `VkBuffer` is created from its own
  `VkBufferMemoryRequirements` (HOST_VISIBLE+HOST_COHERENT with a non-coherent fallback) and
  released by `sdl3vk_shutdown`. Once the frame is known to have reached the presentation
  engine, the readback is handed to `fbcap.c` with its source label (`cpu-framebuffer` or
  `gpu-render-target`) and byte order.
- Slot policy (which env var owns the next present, paths, FBDUMP exit status) lives in
  `src/rt/fbcap_policy.{h,c}`; the hle.c integration is `fbcap_arm_for_present`.
- `sdl3vk_capture_selftest()` (standalone binary via the Makefile `gpu-capture-selftest` target)
  exercises policy, CPU- and GPU-source captures, cancel, scale, and byte-checks the published
  PPMs; under `SR_VULKAN_VALIDATION=1` it also asserts zero validation-layer errors. It also
  checks present pacing (below) against a fake display that holds every image. Exit 77
  means SKIP (Vulkan unavailable).
- Present pacing: while the scheduler paces the guest to real time (the default), a game-frame
  present never waits for the presentation engine (`sdl3vk_set_present_nowait`, set by
  `gui_present`). The present polls its frame slot's fence and acquires with timeout 0; when a
  FIFO (VSync) display still holds every image, or the frame slot's earlier submission is still
  pending (under MAILBOX, a GPU frames behind), it returns `SDL3VK_PRESENT_DISPLAY_BUSY`. The
  window keeps the frame it has, and the guest's VBLANK clock goes on. These frames count as
  `vblanks.present_busy` in the perf JSON (apart from the output cap's `present_skips`), and
  shutdown prints how many there were, by cause. A frame not shown is not recorded either, so a
  VSync-on video recording repeats the previous picture at that VBLANK. Two kinds of present
  still wait: a frame an armed file or evidence capture is waiting for (the picture probe is not
  one), and every present in a deterministic-clock or turbo run (`SR_DETERMINISTIC_CLOCK=1`,
  `SR_NOVBPACE=1`), which has no real-time clock to protect and keeps every frame. `fbcap-selftest` covers the presenter-neutral service itself
  without a GPU.

## Widescreen Fill wings (#1881)

Off by default; the contract is `docs/ENHANCEMENT_CONTRACT.md` section 10.8. ge.c decides which
primitives reach a display framebuffer's wing canvas and moves their vertices there; this backend
draws them through `hook_wing_tri` / `hook_wing_sprite`, which run the ordinary `hook_tri` /
`hook_sprite` with `s_wing_dst` set. That records into `s_wbatch` instead of `s_batch`, with the
canvas columns as the scissor's x and the guest scissor's rows, and `submit_pending` submits those
batches after the target's own, so every resource a pending batch names is released at the same
boundaries as before. A canvas (`WingEnt`) is a 512x272 colour and depth pair at render scale,
the size of every target, so both wings of up to 256 columns each fit (32:9 uses 240). It is never
in `s_tgts` or `s_deps`: no readback, upload, coherence or texture path reaches it. It is cleared
whenever ge.c clears its own (`ge_wide_epoch`), which ge.c does on the first draw into the
framebuffer after it was shown. A state this backend would draw in software is dropped from the
wings with no flush or readback: `wing_drop` counts it and names each cause once on stderr, and the
wing hook returns 0, so ge.c does not mark the canvas drawn. `gegpu_wide_read` reads a canvas back
for `gpu-coherence-selftest`'s `wide-fill` case, which compares it with the software rasterizer's.

With Fill on, `gegpu_present` blits the left wing, the shown image and the right wing into one
(480 + 2D) x 272 image at the shown image's scale (`wide_compose`, on the GE chain ahead of the
presenter's work) and presents that; wings the frame did not draw are black. The window fits it
by its own shape (`sdl3vk_present_plan`). The software renderer's picture is composed in
`src/rt/gui.c` and presented with `sdl3vk_present_rgba_ex`.

## Present and swapchain failures (#1377)

Every state-changing Vulkan result on the present, pause-overlay and swapchain paths is checked.
A failed present returns -1 and resolves an armed capture as failed. Whether the renderer can
go on decides the transition:

- Nothing is lost when an acquire fails with out-of-memory (no image was acquired), or a present
  fails before it is enqueued: `recover_unenqueued_present` waits for the frame and replaces its
  semaphore, and the swapchain is rebuilt, which hands back the image the present still held.
  The next present tries again.
- The renderer turns terminal (`sdl3vk_renderer_terminal()`; every later present returns -1 at
  once) when the device or surface is lost, a fence wait fails (the frame's work may still run),
  or any step between a successful acquire and the submit fails (the image stays acquired and its
  semaphore stays to be signalled, and neither can be handed back without a present). It also
  turns terminal when the swapchain cannot be rebuilt: the queue-idle wait fails,
  `vkCreateSwapchainKHR` fails (the old swapchain is retired anyway, so it is destroyed), or
  its images cannot be read.
- A suboptimal acquire has acquired its image: the frame is presented and the present, which
  reports it again, rebuilds the swapchain.
- Only `sdl3vk_shutdown` leaves the terminal state, and a later `sdl3vk_init` starts afresh.

`sdl3vk_lifecycle_selftest` (inside `gpu-capture-selftest`) checks two kinds of fault with the
validation layer on, and expects no leaked object and no validation error from either:

- an init failure at each step;
- a failure at each checked present step, through a one-shot present-step fault seam.

## Render scale and render targets read as textures (#2052)

At a render scale above 1x (`SR_GPU_SCALE`, the player's Internal Render Resolution) every render
target and its depth buffer are allocated at that scale. Guest VRAM keeps the 1x frame: a CPU
readback takes the top-left device pixel of each guest pixel (`readback-rows`). Effects that draw
into one target and read it back as a texture keep the scale in these cases:

- **A target sampled as a texture** is bound directly: the draw samples the target's own image at
  render scale, with no readback and no decode. A draw that samples the target it renders into
  reads a snapshot copy of it. Sample positions keep the PSP's lattice in guest texels
  (`psp.frag`):
  - Linear filtering steps in sixteenths of a guest texel and blends the device texels around
    each position. A bloom-like pass into a quarter-size buffer, sampled back with linear
    filtering, keeps its render-scale detail (`rtt-scale` in `gpu-coherence-selftest`, at 1x, 2x
    and 4x). At 3x a device texel is not a whole number of sixteenths (16/3), so the sample
    positions fall unevenly across device texels and the detail is finer than 1x but uneven.
  - Nearest filtering takes the centre of each guest texel, as the PSP does, so a nearest read of
    a target shows one device texel per guest texel at any render scale.
  - The texture's own width and height bound the read, as on the PSP: each texel index is clamped
    or wrapped at them (the texture's clamp bits), not at the edge of the target's image, so a
    texture inside a larger target never shows the target's pixels beyond it (`rtt-edge`, clamped
    and repeated, linear and nearest). `psp.frag` gets the size in `texenv.a` and `fogcol.a`, the
    clamp bits in `cfg.y` bits 24 and 25 and the minification and magnification filters in bits
    26 and 27, lanes the per-fragment level of detail uses only for a mipmapped read, which is
    never bound directly. Each fragment picks its filter from its footprint per guest pixel, as
    `ge.c` does. With different minification and magnification filters the shader fetches every
    sample itself (for nearest the device texel at the guest texel's centre, as below; for linear
    the four taps), since a Vulkan sampler may judge the footprint another way. A linear tap pair that straddles a repeated texture's seam is
    fetched and blended in the shader, wrapped in device texels. Rows of the texture below the
    target's 272 rows read the image's last row.
  - A texture that starts inside the target, at a pixel-aligned offset, is read through that
    offset (`texsize.zw`): `rtt-scale-offset` runs the bloom-like pass with its buffer as a
    sub-rectangle of a 512-wide target.
- **A CPU read of a target a draw sampled** returns its top-left device samples in the target's own
  format, the readback-rows contract: 1x pixels, the software renderer's except where an edge
  passes between a guest pixel's centre and its top-left device sample (`rtt-scale` reads its
  quarter-size buffer back after sampling it).
- **A block transfer between targets** copies at render scale (`hook_xfer`).

A texture is a render-target read when its address lies inside the span of a colour target the
GPU has drawn (one whose image is valid): from the target's base address, its stride times 272
rows (`target_containing` in `ge_gpu.c`). The direct
bind then needs all of these: the texture format equals the target's pixel format, the texture is
not swizzled and mipmapping is not active (a mipmap filter with a maximum level above 0), its
buffer width equals the target's stride (or is 0), the texture address is pixel-aligned inside the
target, and the target's image holds the guest's latest CPU writes. Any other read of a render
target reads the target back to guest VRAM and decodes it there at 1x, the way the GE drew it, so
the effect shows 1x detail at any render scale. Those reads include:

- a read in any texture format other than the target's pixel format: CLUT4, CLUT8, CLUT16,
  CLUT32, DXT, or another direct-colour format (a 16-bit target read as 8888 or as another 16-bit
  format, an 8888 target read as 16-bit);
- a swizzled read;
- a read with mipmapping active;
- a read with another buffer width;
- a read of a target whose CPU writes could not be copied into its image.

Depth is matched by address like any texture. A depth buffer read as a texture decodes guest VRAM
at 1x when its address lies outside every colour target's span. When the texture's address lies
inside both a colour target's span and a depth buffer the renderer holds (its stride times 272 rows
of 16-bit values from its base; `depth_containing`: the texture's start address decides, as for
colour targets), the read takes whichever of the two buffers was written last. Each change to a
target's image takes the next value of one count (`cseq`), a batch that writes depth stamps its
depth buffer with its own (`zseq`), and queued batches are submitted first. Depth written after the
target last changed makes it a depth read: it commits what a CPU read would (the targets' drawn
pixels in the span, then the whole depth buffer) and decodes guest VRAM at 1x, through the depth
buffer's own address or the 0x04200000 linear view alike (`rtt-depth-alias`). Otherwise the
target's colour is the newer content and the rules above apply, so a title that leaves its depth
address on a colour buffer it draws (and writes no depth there) keeps the direct bind
(`rtt-depth-alias` draws the colour target over the depth buffer's rows and reads it again). The
choice is per buffer, not per pixel: a depth buffer some batch wrote after the colour target
changed is read whole, rows that batch did not reach included, as a CPU read of it is; and a
colour target that changed after the depth was written is bound whole, its rows where depth was
written later than its last draw there included (those rows show the target's colour where the
PSP's VRAM holds depth).

`SR_GPU_NORTT=1` sends every render-target read through that 1x path, for bisecting.

### Why the other reads stay at 1x (#2279)

Each read below decodes guest VRAM at 1x at every render scale, by decision. The format, CLUT,
swizzle, width and alignment cases are not plain samples of the target's image: each reinterprets
the target's memory, which is what the 1x decode does exactly, as the GE did. The mipmapped and
depth cases have their own reasons below.

**No scaled form.** The texture's texel grid is not the target's pixel grid, so a texel has no
device samples of its own:

- an 8888 target read as a 16-bit format or CLUT16 (a texel is half a pixel), and a 16-bit target
  read as 8888 or CLUT32 (a texel spans two pixels);
- CLUT4 and CLUT8 (a texel is a nibble or a byte of a pixel);
- DXT (a 4 x 4 block of texels is 8 or 16 bytes of pixels);
- an address that is not on a pixel boundary of the target (each texel straddles two pixels).

**A scaled form that redoes the reinterpretation per device sample.** Here each texel is one
pixel of the target, so a scaled read could exist, but the shader would have to repeat, for every
filter tap, what the decoder does once per texel:

- a 16-bit target read as another 16-bit format (5650, 5551, 4444): repack each tap's bits from
  the target's format into the texture's before filtering;
- CLUT16 of a 16-bit target, CLUT32 of an 8888 target: look each tap up in the palette, a lookup
  that runs only in the texture decoder;
- a swizzled read, or a read with another buffer width: find the target pixel that holds each
  tap's texel (a swizzled texture takes the buffer's 16-byte by 8-row blocks in another order;
  another width puts texture rows across the target's rows), and filter only between texels,
  whose neighbours are not the target's neighbouring pixels.

The renderer keeps these at 1x: each would be a separate shader path for a read that
reinterprets memory, while the 1x read is already exact. A title whose effect shows the 1x detail
through one of them is the case for adding that path, under its own issue.

**Mipmapped** (a mipmap filter and a highest level above 0). The texture cache builds the whole
chain from guest VRAM: its other levels are separate 1x buffers, and a target image has no mip
levels. A level 0 at render scale beside 1x levels would filter between levels of unrelated
detail.

**Depth outside every colour target.** The depth image is an attachment the renderer never
samples; a scaled read would sample it and repack each depth value into the texture's format.
The read goes through the GE's CPU-read path (`sr_ge_vram_read_prepare`), which commits the GPU's
depth to guest VRAM first, and decodes there at 1x, through the depth address (swizzled storage),
the 0x04200000 linear view and the 0x04600000 view alike. Like any texture outside every colour
target, that 1x decode may then be upscaled by texture scaling when it is on.

`rtt-1x-reads` in `gpu-coherence-selftest` runs thirteen of these reads at 1x, 2x and 4x: an 8888
target read as 4444, CLUT8, CLUT4, DXT1, CLUT32, swizzled, mipmapped and at half its buffer width;
a 5650 target read as 5551 and as 8888; and Z, the depth buffer all three share, through the
three views. None binds a
target's image, each depth read commits depth the GPU wrote since the last commit, and the reading
target's guest VRAM equals the software renderer's exactly. A control read in the target's own
format binds its image directly. The other cases, among them an unaligned read, CLUT16, DXT3 and
DXT5, the other pairs of formats, and swizzled, mipmapped or other-width reads of a 16-bit target,
rest on the reasons above alone.

## Environment variables

- `SR_GPU_GE=1` — enable the GPU rasterizer (SDL3 build only; default off).
- `SR_GPU_SCALE=1..4` — the integer render scale (`SR_RESOLUTION_SCALE` is the other spelling);
  default 1. See [Render scale and render targets read as textures](#render-scale-and-render-targets-read-as-textures-2052).
- `SR_GPU_NORTT=1` — read every render target used as a texture back through guest VRAM at 1x,
  instead of binding the target image (debug bisect).
- `SR_GPU_LOG=1` — one line per flush (reason, batches, verts, target).
- `SR_GPU_SUBMIT_BATCH=1..128` — recorded GE ops (render batches, destination snapshot
  copies, ring uploads) that share one `vkQueueSubmit`; default 128, out-of-range values keep
  the default, and `8` restores the schedule used before #827. A submission granularity, not
  an ordering boundary: all GE work stays on one queue with its barriers recorded in the
  stream, and every CPU-visible consumer (readback, presentation, capture, destruction,
  software fallback) closes the open command buffer itself. A larger limit trades CPU/GPU
  overlap inside one command buffer for fewer submits and fence waits.
- `SR_GPU_SYNC_SUBMIT=1` — exact synchronous fallback: one op per submit, each waited on.
- `SR_GPU_DST_READ=snapshot|interlock` — the destination-read path (#2109), read before the
  device is made. Unset, the interlock is taken where the device has it; an unknown value says
  so once and gets the same default. A forced path the device lacks is refused with its reason,
  and the snapshot path runs.
- `SR_GPU_READBACK_FULL=1` — copy the whole scaled target on a readback to guest memory, the
  reference path for the default copy of the rows guest memory keeps (#323).
- `SR_GPU_PIPELINE_CACHE=0` — run without the pipeline cache in `<data root>/cache/gegpu`
  (on by default; `on` or `1` keep it, any other value is refused). One start line says
  whether the file was loaded, absent or discarded and why; one exit line says whether it was
  saved and how long pipeline creation took.
- `SR_VIDEO=gdi` — Phase-0 fallback to the classic GDI window.
- `SR_VULKAN_VALIDATION=1` — enable the `VK_LAYER_KHRONOS_validation` messenger (fail-closed
  if the layer is requested but missing). See `docs/DEBUGGING.md` for FBSNAP/FBDUMP capture
  controls.

## PSP/GE semantics that MUST carry over (hard-won, see ge.c comments)

1. Primitive acceptance: whole-triangle drops on out-of-grid / z-range vertices
   (oor/ozp/ozn, depth-clamp on/off variants) happen **before** rasterization.
2. Near clip only against z >= -w; clip-created vertices outside the 4096-grid kill their
   sub-triangle (PPSSPP `clip_interpolate` rule).
3. Culling is submission-order: rasterizer keeps positive-winding only; cull mode reverses
   order, strips flip per parity. Map to VK_CULL_MODE with care (or pre-flip on CPU).
4. Depth: 16-bit, often inverted range (viewport zscale < 0), GEQUAL tests, minz/maxz is a
   per-pixel *discard* range, not a clamp.
5. CLUT-format quirks, swizzled textures, color-double texfunc bit, alpha 0..0xFF with
   0x80 = 1.0 semantics in blending.
