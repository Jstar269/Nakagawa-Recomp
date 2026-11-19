/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_scale.h - Texture scaling (xBRZ) and the smart 2D filter: the CPU half of the Vulkan GE
 * renderer's host-side texture enhancement (#1273). ge_gpu.c owns every Vulkan object; this
 * module owns the settings, the scaling jobs and their worker threads, the VRAM budget for
 * scaled copies, and the classifier that tells a pixel-mapped 2D draw from a 3D one.
 *
 * What it touches: only host copies. A job scales a copy of a texture already decoded on the
 * host (after the CLUT lookup), on a worker thread, into host memory; ge_gpu.c uploads the
 * result into a second Vulkan image beside the unscaled one. Guest memory, the CLUT, texture
 * readbacks and render targets are never read or written here.
 *
 * Off is authentic: both features take effect only in an Ace Mode run (g_ace_on, ace.h), whose
 * launch line the launcher always sets, so an authentic run can never turn them on. With them
 * off no thread starts and nothing is allocated, unless the texture dump (an Enhanced or Ace run
 * too) hands the workers its own work jobs (below), which read only the host copies they are
 * given and write only the dump's files.
 *
 * Settings, taken once when the renderer starts (nk_tex_scale_init):
 *   texscale=off | 2 | 3 | 4 | 5 | 6   the launch line's xBRZ factor (ace.h); a missing key is off
 *   smart2d=off | on                   the launch line's nearest filtering for pixel-mapped 2D draws
 *   SR_TEX_SCALE_BUDGET_MB  1..8192    VRAM for scaled copies (default: an eighth of the device's
 *                                      largest device-local heap, 512 to 2048; 512 when unknown)
 * Any other value refuses the request: both stay off and one TEX_SCALE line names the reason. No
 * environment variable turns either feature on: SR_TEX_SCALE and SR_SMART_2D are retired, and a
 * run that has one set stops at start (ace.h, ace_retired_env_named).
 *
 * Both features are guest-visible (docs/ENHANCEMENT_CONTRACT.md, the section 11.3 entry "Texture
 * Scaling and the Smart 2D Filter"): each present commits the frame, scaled copies and nearest 2D
 * draws included, to guest VRAM. They are admitted under section 11.2, off by default (the entry
 * lists the evidence still owed), and a run that turns one on says at start that it is
 * guest-visible.
 */
#ifndef NK_TEX_SCALE_H
#define NK_TEX_SCALE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_TEX_SCALE_MAX 6
#define NK_TEX_SCALE_DEFAULT_BUDGET_MB 512u
/* The default budget a device gets (nk_tex_scale_budget_for_vram): an eighth of its largest
 * device-local heap, within these bounds. */
#define NK_TEX_SCALE_VRAM_BUDGET_MIN_MB NK_TEX_SCALE_DEFAULT_BUDGET_MB
#define NK_TEX_SCALE_VRAM_BUDGET_MAX_MB 2048u
/* A scaled copy is at most this many texels on a side; a larger texture takes the largest
 * factor that fits, and none below 2. */
#define NK_TEX_SCALE_MAX_DIM 2048
/* Scaling jobs waiting, running or finished at once; a texture that finds the queue full is
 * drawn unscaled and offered again on a later draw. */
#define NK_TEX_SCALE_QUEUE 64
/* Host memory those jobs may hold together: sources, results and the scratch a running scale makes
 * (nk_tex_scale_job_bytes; a 512x512 texture at 4x counts about 34 MiB). A submit past it is refused
 * like a full queue, except that one job is always taken. */
#define NK_TEX_SCALE_HOST_MB 128u
/* Worker threads the jobs run on (nk_tex_scale_worker_count). */
#define NK_TEX_SCALE_WORKERS_MIN 2
#define NK_TEX_SCALE_WORKERS_MAX 8

typedef struct NkTexScaleConfig {
    int factor;            /* 0 off, 2..6 */
    int smart2d;           /* 0 off, 1 on */
    uint64_t budget_bytes; /* VRAM budget for scaled copies */
} NkTexScaleConfig;

/* Check the three settings: the factor (0 off, or 2 to NK_TEX_SCALE_MAX), smart 2D (0 or 1) and
 * the budget's text (NULL or "" is the default). Returns 1 when they are valid (all-off included),
 * 0 with `why` filled when one is refused; `out` is then all off. Pure: no state. */
int nk_tex_scale_parse(int factor, int smart2d, const char *budget_mb,
                       NkTexScaleConfig *out, char *why, size_t why_cap);

/* The default scaled-copy budget for a device whose largest device-local heap holds
 * `device_local_bytes`: an eighth of it in whole MiB, at least NK_TEX_SCALE_VRAM_BUDGET_MIN_MB and
 * at most NK_TEX_SCALE_VRAM_BUDGET_MAX_MB; 0 (unknown) gives NK_TEX_SCALE_DEFAULT_BUDGET_MB. A
 * 10 GiB card gets 1280 MiB. Pure. */
uint64_t nk_tex_scale_budget_for_vram(uint64_t device_local_bytes);
/* The renderer's device: its largest device-local heap, in bytes (0 = unknown, the state at
 * start). Set before nk_tex_scale_init; the default budget is derived from it from then on. */
void nk_tex_scale_set_device_vram(uint64_t device_local_bytes);
/* The budget a feature that shares the scaled-copy budget takes when SR_TEX_SCALE_BUDGET_MB set
 * none: nk_tex_scale_budget_for_vram of the device set above. */
uint64_t nk_tex_scale_default_budget_bytes(void);

/* Make the launch line's request (`factor` and `smart2d`, from ace_effective()) the run's
 * configuration, only when `enhanced` is nonzero (an Enhanced or Ace run); SR_TEX_SCALE_BUDGET_MB
 * is read then, and without it the budget is nk_tex_scale_default_budget_bytes(). Prints the TEX_SCALE lines when either feature is on or the request was refused;
 * a refusal also writes its reason to `refused` (when given; "" otherwise), and leaves both off.
 * Returns the configuration in force. */
const NkTexScaleConfig *nk_tex_scale_init(int enhanced, int factor, int smart2d, char *refused,
                                          size_t refused_cap);
/* The start line naming a configuration that is on as guest-visible under its contract entry.
 * Writes it to `out` (without a newline) and returns its length when either feature is on;
 * returns 0 and writes an empty string when both are off. */
size_t nk_tex_scale_guest_visible_line(const NkTexScaleConfig *cfg, char *out, size_t cap);
/* The lines the last nk_tex_scale_init printed, each ending in a newline ("" for
 * none), for the self-test. */
const char *nk_tex_scale_start_lines(void);
/* Tests: set the configuration directly (no environment, no line). */
void nk_tex_scale_configure(const NkTexScaleConfig *cfg);
const NkTexScaleConfig *nk_tex_scale_config(void);

/* The factor a w x h texture takes under factor `want`: want, or the largest factor whose
 * scaled size fits NK_TEX_SCALE_MAX_DIM, or 0 when none of 2 or more fits. */
int nk_tex_scale_factor_for(int want, int w, int h);

/* ---- budget: VRAM held by scaled copies ---------------------------------------------- */
int nk_tex_scale_budget_take(uint64_t bytes);   /* 1 when it fits and is now held */
void nk_tex_scale_budget_give(uint64_t bytes);
uint64_t nk_tex_scale_budget_used(void);

/* ---- jobs ----------------------------------------------------------------------------- */
/* How many workers a host with `logical_cpus` logical CPUs starts: every CPU but two (one for the
 * guest's thread, which also records the GE's work, one for audio and the graphics driver), at least
 * NK_TEX_SCALE_WORKERS_MIN and at most NK_TEX_SCALE_WORKERS_MAX; a count of 0 or less (unknown)
 * gives the minimum. The count changes only how fast copies arrive, never what a job makes, and the
 * memory the jobs hold stays under NK_TEX_SCALE_HOST_MB whatever it is: a job is admitted with every
 * buffer it makes while it runs (nk_tex_scale_job_bytes), so jobs running at once never pass the cap
 * (the one exception is a lone job larger than the cap, which no PSP texture's is). Pure. */
int nk_tex_scale_worker_count(int logical_cpus);
/* The host bytes a scale of a w x h texture by `factor` is counted as from submit until its result
 * is released: the source copy, the result, and the padded source and padded result
 * nk_tex_scale_image makes while it runs. 0 for a factor or size submit refuses. Pure. */
uint64_t nk_tex_scale_job_bytes(int w, int h, int factor);
/* The workers started for the jobs now (0 before the first job and after nk_tex_scale_shutdown). */
int nk_tex_scale_workers(void);
/* Queue a scale of `rgba` (w x h, R in the low byte, as ge.c's decoder writes) by `factor`.
 * `clamp_u`/`clamp_v` say how the texture is sampled past its edges (1 clamp, 0 repeat), so the
 * scaled edges continue the way the GPU will read them. The pixels are copied before the call
 * returns. `serial` identifies the content (the caller's generation number), `hash` its
 * content hash; both come back with the result. Returns 1 when queued, 0 when the queue is
 * full or memory ran out (the texture is drawn unscaled). */
int nk_tex_scale_submit(uint64_t serial, uint64_t hash, const uint32_t *rgba, int w, int h,
                        int factor, int clamp_u, int clamp_v);
/* Queue a finished image: `rgba` is already (w*factor) x (h*factor) (R in the low byte), as a
 * texture-replacement file is (texrepl.h). It is copied and comes back from nk_tex_scale_take
 * like a scaled result, with `serial` and `hash`; no scaling and no worker are involved. Refused
 * like nk_tex_scale_submit when the queue or the host-memory cap has no room. */
int nk_tex_scale_submit_ready(uint64_t serial, uint64_t hash, const uint32_t *rgba, int w, int h,
                              int factor);
/* Whether a submit of a w x h texture at `factor` would be accepted now: queue room, and its host
 * memory within the cap. The same test nk_tex_scale_submit makes. */
int nk_tex_scale_can_submit(int w, int h, int factor);

typedef struct NkTexScaleResult {
    uint64_t serial, hash;
    int w, h, factor;      /* source size and factor; the pixels are w*factor x h*factor */
    uint32_t *rgba;        /* R in the low byte; owned by the result */
} NkTexScaleResult;

/* Take one finished scale job, oldest first. Returns 1 and fills `out` (release it with
 * nk_tex_scale_release), or 0 when none is finished. Never waits, and never returns a work job. */
int nk_tex_scale_take(NkTexScaleResult *out);
void nk_tex_scale_release(NkTexScaleResult *res);

/* ---- work jobs: other host work on the same workers ---------------------------------------
 * The texture dump (ge_gpu.c) encodes and writes its files here, off the guest thread. A work job
 * shares the scale jobs' NK_TEX_SCALE_QUEUE slots and NK_TEX_SCALE_HOST_MB cap: `host_bytes` is
 * what it holds until taken. `run(ctx)` is called once, on a worker thread (with sync on, or when
 * no worker could start, on the submitting thread before submit returns); the job is then done
 * and waits for nk_tex_scale_take_work. `ctx` stays the caller's: nothing here reads or frees it,
 * and the caller must not write it between a submit that returned 1 and the take that returns it
 * (reading what the job never writes is safe: the dump compares a queued job's path).
 * Submit returns 1 when queued, 0 when the queue or the cap has no room (counted in
 * work_refused; the caller keeps `ctx`). Nothing waits: with hold on, a work job stays queued like
 * a scale. The owner takes every work job before nk_tex_scale_shutdown, which says so on stderr
 * when one is left. */
typedef void (*NkTexScaleWorkFn)(void *ctx);
int nk_tex_scale_submit_work(NkTexScaleWorkFn run, void *ctx, uint64_t host_bytes);
/* Whether a work job holding `host_bytes` would be accepted now; the same test submit makes, so a
 * single submitting thread told there is room is never refused. */
int nk_tex_scale_can_submit_work(uint64_t host_bytes);
/* Take one finished work job of `run`'s, oldest first: 1 and *ctx, or 0 when none is finished.
 * Never waits; a job of another function is left for its own owner. */
int nk_tex_scale_take_work(NkTexScaleWorkFn run, void **ctx);
/* `run`'s work jobs queued or running (not finished yet). */
int nk_tex_scale_work_pending(NkTexScaleWorkFn run);
/* Wait at most `timeout_ms` for `run`'s queued and running work jobs to finish: 1 when none is
 * left, 0 when the time ran out first (a job still queued or running). A stop that ends the run
 * waits this way, so a worker that never finishes cannot hold the exit forever. With hold on, a
 * queued job does not start: let the hold go first. */
int nk_tex_scale_wait_work(NkTexScaleWorkFn run, uint32_t timeout_ms);
/* Whether the calling thread is one of the workers (a job's own code): such a caller must never
 * wait for the workers. */
int nk_tex_scale_on_worker(void);

/* Tests and capture runs: with sync on, nk_tex_scale_submit scales on the calling thread
 * and the result is ready before it returns; with hold on, workers start no new job (a
 * queued job stays queued). */
void nk_tex_scale_set_sync(int on);
void nk_tex_scale_set_hold(int on);
/* Tests: make every later start fail at one step, so its fallback runs: NK_TEX_SCALE_FAULT_SYNC (a
 * condition variable is not made), NK_TEX_SCALE_FAULT_THREAD (no worker thread starts), or
 * NK_TEX_SCALE_FAULT_NONE. And the mutexes and conditions this file holds now (0 when stopped). */
enum { NK_TEX_SCALE_FAULT_NONE, NK_TEX_SCALE_FAULT_SYNC, NK_TEX_SCALE_FAULT_THREAD };
void nk_tex_scale_set_start_fault(int fault);
int nk_tex_scale_sync_objects(void);
/* Wait until every queued job has finished (tests and shutdown). */
void nk_tex_scale_wait_idle(void);
/* Stop the workers and free every queued or finished job. A worker still inside a work job is not
 * waited for (its owner's bounded wait has given up on it): it is left to finish on its own,
 * touches nothing here afterwards, and is counted in workers_left and said on stderr. */
void nk_tex_scale_shutdown(void);

typedef struct NkTexScaleStats {
    uint64_t submitted, scaled, refused_full, taken, discarded;
    uint64_t work_submitted, work_done, work_refused;   /* work jobs */
    uint64_t scale_ns;          /* worker time spent scaling */
    uint64_t waits;             /* nk_tex_scale_wait_idle and nk_tex_scale_wait_work calls */
    uint64_t workers_left;      /* workers a stop left inside a work job, not waited for */
    uint64_t budget_used, budget_peak;
} NkTexScaleStats;
void nk_tex_scale_stats(NkTexScaleStats *out);
/* The exit line, when either feature was on. */
void nk_tex_scale_report(void);

/* The pure scale a job runs: rgba w x h -> out (w*factor x h*factor), both R in the low byte.
 * The source is extended by two texels on each side the way it is sampled (clamped edge or
 * wrapped repeat) before xBRZ runs, and the extension is cut off again, so a repeating
 * texture tiles without a seam. Returns 0 on a bad factor or no memory. */
int nk_tex_scale_image(const uint32_t *rgba, int w, int h, int factor, int clamp_u, int clamp_v,
                       uint32_t *out);

/* ---- soft shadows (Ace part shadow=soft, docs/ENHANCEMENT_CONTRACT.md section 10.23) ------- */
/* A title's blob shadow is a small textured quad whose alpha is a hard-edged disc. Ace's soft
 * shadows draw such a texture, when its package declares it, from a host-made copy with a softer
 * alpha edge, through the same scaled-copy path as a texture replacement. The copy is made here,
 * on the CPU, in integer arithmetic, so every renderer that draws it samples the same texels. */
#define NK_TEX_SOFT_MAX_DIM 64      /* only textures of at most 64x64 (one mip level) are hashed */
#define NK_TEX_SOFT_FACTOR 4        /* the factor the renderer makes its copies at */
#define NK_TEX_SOFT_FACTOR_MIN 2
#define NK_TEX_SOFT_FACTOR_MAX 4
#define NK_TEX_SOFT_EDGE_MIN 1
#define NK_TEX_SOFT_EDGE_MAX 4

/* The softened copy of `src` (w x h, R in the low byte) into `dst` ((w*factor) x (h*factor)).
 * Accepts 1 <= w, h <= NK_TEX_SOFT_MAX_DIM, factor NK_TEX_SOFT_FACTOR_MIN..MAX and edge_texels
 * NK_TEX_SOFT_EDGE_MIN..MAX; returns 0 for anything else, or when scratch memory runs out (dst is
 * then untouched), and 1 when dst is written.
 *
 * Each axis samples the source bilinearly at the centre of each output texel: X maps to source
 * coordinate (X + 0.5) / factor - 0.5, between texels floor() and floor() + 1, each clamped to
 * the texture (clamp-to-edge), weighted in steps of 1 / (2 * factor).
 *   Colour: R, G and B are that bilinear value, rounded half up to 8 bits.
 *   Alpha: the bilinear value, kept unrounded, then a separable box blur of radius
 *     r = edge_texels * factor output texels (a (2r + 1)-texel window on each axis, the window
 *     clamped to the image), rounded half up to 8 bits once, at the end.
 * Only integers are used: the same input gives the same bytes on every host.
 *
 * Interior: an output texel at (X, Y) keeps alpha a exactly when every source texel within
 * Chebyshev distance edge_texels + 1 of source texel (X / factor, Y / factor) (integer division,
 * neighbours clamped to the texture) has alpha a. The flat interior of a disc keeps its alpha,
 * and so does the flat outside.
 * Mass: when every source texel within edge_texels + 1 texels of the border has alpha 0, the
 * output's alpha sum differs from factor * factor times the source's by at most half the number
 * of output texels (one rounding per texel; the filters themselves lose and add nothing). */
int nk_tex_soften_alpha_edge(const uint32_t *src, uint32_t w, uint32_t h, uint32_t factor,
                             uint32_t edge_texels, uint32_t *dst);

/* The digest that names a soft-shadow texture: HMAC-SHA-256 (RFC 2104, vfpu_tables.h) under the
 * package's 32-byte `key` of the message
 *     "soft shadow texture v1" and its terminating zero byte (23 bytes),
 *     width as u16 little-endian, height as u16 little-endian,
 *     the decoded level-0 texels, row by row, each as the four bytes R, G, B, A.
 * Width and height sit inside the message, so a published digest gives away no size, palette or
 * texel. Returns 0 (digest untouched) for a size outside 1..NK_TEX_SOFT_MAX_DIM, else 1. Pure. */
int nk_tex_soft_shadow_digest(const uint8_t key[32], const uint32_t *rgba, uint32_t w, uint32_t h,
                              uint8_t digest[32]);

/* The authoring aid of SR_ACE_SHADOW_CANDIDATES=1 (contract section 10.23): with the part on, the
 * renderer prints one line for each distinct content it hashes, so a package author can find the
 * digest a shadow texture's texture_digest must hold. The line, without its newline, is
 *     SOFT_SHADOW: candidate <w>x<h> digest=<64 lowercase hex digits>
 * with w and h in decimal. Returns its length, or 0 (out set to "" when cap > 0) when it does not
 * fit in cap bytes with its terminating zero, or a pointer is NULL. Pure. */
size_t nk_tex_soft_candidate_line(char *out, size_t cap, uint32_t w, uint32_t h, const uint8_t digest[32]);

/* The digests already listed, so each distinct content is printed once per run. */
#define NK_TEX_SOFT_CANDIDATES_MAX 1024u
typedef struct {
    uint32_t count;
    uint8_t digest[NK_TEX_SOFT_CANDIDATES_MAX][32];
} NkTexSoftCandidates;
/* 1 the first time `digest` is offered (it is recorded), 0 when it was offered before, and -1 when
 * it is new but the table already holds NK_TEX_SOFT_CANDIDATES_MAX digests (not recorded). */
int nk_tex_soft_candidate_first(NkTexSoftCandidates *seen, const uint8_t digest[32]);

/* ---- smart 2D filter ------------------------------------------------------------------- */
/* Whether a draw maps texels to pixels 1:1: one texel per guest pixel along each axis (either
 * direction), no rotation, no perspective, and every pixel centre on a texel centre or at most
 * 1/32 texel above it, never below (the linear rule floors, so a centre just below one would
 * take part of the texel before). Such a draw samples the same texels under nearest and linear
 * filtering at the PSP's own resolution; above it, nearest keeps each texel crisp where linear would blur it across the
 * scaled pixels. Coordinates are guest pixels and texels.
 *
 * A sprite: corners (x0, y0) and (x1, y1) with texel coordinates (u0, v0) at the left/top
 * edge and (u1, v1) at the right/bottom edge. */
int nk_smart2d_sprite(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1);
/* A triangle: three vertices, screen x/y and texel u/v, with their 1/w (rw). A 1:1 draw has
 * the same rw at all three vertices, bit for bit (no perspective), and an affine map of +/-1 texel per pixel on each axis. */
int nk_smart2d_triangle(const float x[3], const float y[3], const float u[3], const float v[3],
                        const float rw[3]);

#ifdef __cplusplus
}
#endif

#endif
