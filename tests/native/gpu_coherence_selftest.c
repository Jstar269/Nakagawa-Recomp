// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#define _POSIX_C_SOURCE 200809L   /* setenv under -std=c11 */

#include "ace.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include "gpu_sdl3vk/sdl3vk.h"
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <vulkan/vulkan.h>

/* Standalone support required by recomp.h and the production GE object. The test owns a
 * synthetic guest arena and never consumes game binaries, captures, or private inputs. */
uint8_t *g_mem;
uint32_t g_sr_debug;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count;
int g_sr_metadata_watch;
int g_sr_heap_watch;
int g_hle_depth;
CpuState *s_cpu;
int g_sr_last_writer_enabled;
uint32_t g_sr_store_context_pc;
unsigned g_sr_store_context_count;
unsigned g_sr_store_context_limit;
uint32_t g_sr_mem_watch_context_pc;
unsigned g_sr_mem_watch_context_limit;
unsigned g_sr_mem_watch_context_count;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value;
int g_sr_store_context_mem_gpr;
uint32_t g_sr_store_context_mem_offset;
unsigned g_sr_store_context_mem_words;

void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label) {
    (void)start; (void)end; (void)label;
}
void sr_add_value_watch(uint32_t value, const char *label) {
    (void)value; (void)label;
}
void sr_debug_init_watches(void) {}
void sr_last_writer_reset(void) {}
int sr_find_last_writer(uint32_t addr, uint32_t width,
                        uint32_t *write_addr, uint32_t *write_width,
                        uint32_t *value, uint32_t *pc) {
    (void)addr; (void)width; (void)write_addr; (void)write_width;
    (void)value; (void)pc;
    return 0;
}

void sr_oor(uint32_t addr, uint32_t value, int store) {
    fprintf(stderr, "gpu coherence selftest: out-of-range %s addr=0x%08x value=0x%08x\n",
            store ? "write" : "read", addr, value);
}

void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}

void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)addr; (void)width; (void)pc;
}

uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }

/* Host-mapped buffer memory selection (#1296), pure and device-free so it runs where Vulkan
 * does not. The table is an RTX 3080's: type 3 is visible+coherent but uncached, type 4 adds
 * HOST_CACHED. A readback must take type 4; an upload keeps the first visible+coherent type;
 * a readback with no cached+coherent type keeps the old choice. */
static int host_mem_type_selection_ok(void) {
    const uint32_t vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t rtx3080[6] = {
        0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vis,
        vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | vis,
    };
    const uint32_t cached_noncoherent[3] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, vis, 0u,
    };
    int ok = 1;
    ok &= gegpu_pick_host_mem_type(rtx3080, 6u, 0x3fu, 1) == 4u;
    ok &= gegpu_pick_host_mem_type(rtx3080, 6u, 0x3fu, 0) == 3u;
    ok &= gegpu_pick_host_mem_type(rtx3080, 6u, 0x2fu, 1) == 3u;   /* type 4 not allowed */
    ok &= gegpu_pick_host_mem_type(rtx3080, 6u, 0x07u, 1) == UINT32_MAX;
    ok &= gegpu_pick_host_mem_type(cached_noncoherent, 3u, 0x7u, 1) == 1u;
    if (!ok) fprintf(stderr, "gpu coherence selftest: FAIL host buffer memory type selection\n");
    return ok;
}

/* `path` made absolute against the current folder (the folder need not exist yet). */
static int absolute_path(const char *path, char *out, size_t n) {
#ifdef _WIN32
    return _fullpath(out, path, n) != NULL;
#else
    if (path[0] == '/') return snprintf(out, n, "%s", path) < (int)n;
    char cwd[3072];
    return getcwd(cwd, sizeof cwd) && snprintf(out, n, "%s/%s", cwd, path) < (int)n;
#endif
}

/* `value` "unset" removes the variable. */
static int set_env(const char *name, const char *value) {
    const int unset = strcmp(value, "unset") == 0;
#ifdef _WIN32
    return _putenv_s(name, unset ? "" : value) == 0;
#else
    return (unset ? unsetenv(name) : setenv(name, value, 1)) == 0;
#endif
}

/* `--scale N`, `--case NAME`, `--readback-full 1` and `--gpu-timestamps VALUE|unset` set SR_GPU_SCALE,
 * SR_GPU_COHERENCE_CASE, SR_GPU_READBACK_FULL and SR_GPU_TIMESTAMPS before the backend starts;
 * `--twin 1|native` and `--twin-ref PATH` set SR_GPU_TWIN_COLOR (the host twin, #2050) and
 * SR_GPU_TWIN_REF (a file the twin cases write in a twin-off run and compare against in a
 * twin-on run). The twin is an Ace Mode feature, so `--twin` also turns Ace Mode on (with no
 * Ace item of its own), unless `--authentic` keeps it off to show that an authentic run
 * ignores the switch. `--twin-key on` makes that Ace line carry `twin=on` and sets no variable:
 * the twin-key case then checks the launch line's own request, in both renderers (with
 * `--twin 0|off|native` the variable overrides the line and the case checks that).
 * `--twin-key off` keeps the line at `twin=off`; with `--twin 1|native` the twin-key case then checks
 * that a twin the variable turned on is reported as running. `--pipeline-cache DIR` turns the pipeline cache (#2149) on and makes DIR,
 * as an absolute path, the data root (SR_USER_DATA_ROOT), so the cache lives in DIR/cache/gegpu
 * as it would under a launcher's root; without it the harness turns the cache off, so a
 * selftest never reads or writes the user's cache folder.
 * `--dst-read snapshot|interlock` sets SR_GPU_DST_READ (#2109), which sdl3vk_init reads before it
 * makes the device. `--depth 32` turns Ace Mode on with the line's depth=32 (the 32-bit host depth
 * buffer, #2051), and `--depth-ref PATH` sets SR_GPU_DEPTH_REF, the file host-depth writes in a
 * key-off run and compares against with the key on.
 * `--ssaa 2|4` turns Ace Mode on with that supersampling factor (#2073; it runs the host twin), and
 * `--ssaa-ref PATH` sets SR_GPU_SSAA_REF (ssaa-edge's reference: a run without supersampling writes
 * it, a supersampled run at the same scale checks it). `--ssaa-budget-mb N`, `--ssaa-max-dim N` and
 * `--ssaa-expect F` force the size check's limits and name the factor ssaa-limit expects.
 * In the sharp game text build (SR_GPU_GLYPH_SELFTEST), `--glyphs on|off|absent` picks the Ace line's
 * glyphs key (on by default; absent writes no key), and `--scene-hash` draws one scene and prints its
 * hash instead of running the cells; `--authentic` (no Ace line) implies it.
 * Recipes pass arguments, not a `VAR=value` prefix, which cmd.exe does not accept. */
int main(int argc, char **argv) {
    int twin_arg = 0, twin_key = 0, twin_key_on = 0, authentic = 0, repl_only = 0, pcache_arg = 0, depth_arg = 0;
    int ssaa_arg = 0, ssaa_expect = 0, scene_hash = 0;
    unsigned long long ssaa_budget_mb = 0;
    unsigned ssaa_dim = 0;
    const char *glyphs_key = "on";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--authentic") == 0) { authentic = 1; continue; }
        if (strcmp(argv[i], "--scene-hash") == 0) { scene_hash = 1; continue; }
        if (strcmp(argv[i], "--glyphs") == 0) {
            if (i + 1 >= argc || (strcmp(argv[i + 1], "on") != 0 && strcmp(argv[i + 1], "off") != 0 &&
                                  strcmp(argv[i + 1], "absent") != 0)) {
                fprintf(stderr, "gpu coherence selftest: --glyphs takes on, off or absent\n");
                return 2;
            }
            glyphs_key = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--twin-key") == 0) {
            if (i + 1 >= argc || (strcmp(argv[++i], "on") != 0 && strcmp(argv[i], "off") != 0)) {
                fprintf(stderr, "gpu coherence selftest: --twin-key takes on or off\n");
                return 2;
            }
            twin_key = 1;
            twin_key_on = strcmp(argv[i], "on") == 0;
            continue;
        }
        if (strcmp(argv[i], "--repl-only") == 0) { repl_only = 1; continue; }
        if (strcmp(argv[i], "--twin") == 0) twin_arg = 1;
        if (strcmp(argv[i], "--depth") == 0) {
            if (i + 1 >= argc || strcmp(argv[++i], "32") != 0) {
                fprintf(stderr, "gpu coherence selftest: --depth takes 32\n");
                return 2;
            }
            depth_arg = 1;
            continue;
        }
        if (strcmp(argv[i], "--ssaa") == 0 || strcmp(argv[i], "--ssaa-budget-mb") == 0 ||
            strcmp(argv[i], "--ssaa-max-dim") == 0 || strcmp(argv[i], "--ssaa-expect") == 0) {
            char *end = NULL;
            const unsigned long long v = i + 1 < argc ? strtoull(argv[i + 1], &end, 10) : 0;
            if (i + 1 >= argc || !end || *end || end == argv[i + 1]) {
                fprintf(stderr, "gpu coherence selftest: %s needs a number\n", argv[i]);
                return 2;
            }
            if (strcmp(argv[i], "--ssaa") == 0) ssaa_arg = (int)v;
            else if (strcmp(argv[i], "--ssaa-budget-mb") == 0) ssaa_budget_mb = v;
            else if (strcmp(argv[i], "--ssaa-max-dim") == 0) ssaa_dim = (unsigned)v;
            else ssaa_expect = (int)v;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--pipeline-cache") == 0) {
            char root[4096];
            if (i + 1 >= argc || !absolute_path(argv[++i], root, sizeof root) || !set_env("SR_USER_DATA_ROOT", root)) {
                fprintf(stderr, "gpu coherence selftest: --pipeline-cache needs a folder it can name absolutely\n");
                return 2;
            }
            pcache_arg = 1;
            continue;
        }
        const char *name = strcmp(argv[i], "--scale") == 0 ? "SR_GPU_SCALE"
                         : strcmp(argv[i], "--case") == 0  ? "SR_GPU_COHERENCE_CASE"
                         : strcmp(argv[i], "--readback-full") == 0 ? "SR_GPU_READBACK_FULL"
                         : strcmp(argv[i], "--tex-scale") == 0 ? "SR_TEX_SCALE"
                         : strcmp(argv[i], "--gpu-timestamps") == 0 ? "SR_GPU_TIMESTAMPS"
                         : strcmp(argv[i], "--twin") == 0 ? "SR_GPU_TWIN_COLOR"
                         : strcmp(argv[i], "--twin-ref") == 0 ? "SR_GPU_TWIN_REF"
                         : strcmp(argv[i], "--depth-ref") == 0 ? "SR_GPU_DEPTH_REF"
                         : strcmp(argv[i], "--ssaa-ref") == 0 ? "SR_GPU_SSAA_REF"
                         : strcmp(argv[i], "--dst-read") == 0 ? "SR_GPU_DST_READ" : NULL;
        if (!name || i + 1 >= argc || !set_env(name, argv[++i])) {
            fprintf(stderr, "usage: gpu_coherence_selftest [--scale N] [--case scaled-upload|mip-lod]\n");
            return 2;
        }
    }
    if (!set_env("SR_GPU_PIPELINE_CACHE", pcache_arg ? "on" : "off")) return 2;
    (void)repl_only;   /* read only in the texture scaling build */
    (void)scene_hash;  /* read only in the sharp game text build */
    (void)glyphs_key;
    if (!host_mem_type_selection_ok()) return 1;
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;
    sr_perf_init();
#ifdef SR_GPU_ACE_SELFTEST
    /* Ace Mode with 16x texture filtering, read before the device is made (src/rt/ace.h). */
    if (!ace_init_spec("v1;af=16;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;twin=off", NULL)) {
        fprintf(stderr, "gpu ace selftest: FAIL the Ace line was refused\n");
        free(arena);
        return 1;
    }
#else
    {
        char spec[192];
        snprintf(spec, sizeof spec, "v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;twin=%s%s%s",
                 twin_key && twin_key_on ? "on" : "off", depth_arg ? ";depth=32" : "",
                 ssaa_arg == 4 ? ";ssaa=4" : ssaa_arg == 2 ? ";ssaa=2" : "");
        if ((twin_arg || twin_key || depth_arg || ssaa_arg) && !authentic && !ace_init_spec(spec, NULL)) {
            fprintf(stderr, "gpu coherence selftest: FAIL the Ace line for the host twin, depth=32 or "
                    "supersampling was refused\n");
            free(arena);
            return 1;
        }
    }
#endif
#ifdef SR_GPU_TEXSCALE_SELFTEST
    /* Texture scaling and the smart 2D filter take effect only in Ace Mode (tex_scale.h). The
     * factor defaults to 2; `--tex-scale N` picks another. */
#ifdef SR_GPU_GLYPH_SELFTEST
    /* Sharp game text (glyph_recon.h) alone: Ace Mode with no texture scaling, and the line's glyphs key
     * on unless `--glyphs off|absent` says otherwise. `--authentic` leaves Ace Mode off. */
    char glyph_line[ACE_SPEC_MAX];
    snprintf(glyph_line, sizeof glyph_line,
             "v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;twin=off%s",
             strcmp(glyphs_key, "on") == 0 ? ";glyphs=on" : strcmp(glyphs_key, "off") == 0 ? ";glyphs=off" : "");
    if ((!authentic && !ace_init_spec(glyph_line, NULL)) ||
        !set_env("SR_TEX_SCALE", "off") || !set_env("SR_SMART_2D", "off")) {
        fprintf(stderr, "gpu glyph selftest: FAIL the Ace line or the settings were refused\n");
        free(arena);
        return 1;
    }
#else
    if (repl_only) {
        /* SR_TEX_REPLACE alone (the documented default use): no texture scaling, no smart 2D, and
         * the pack written and named before the renderer reads it. */
        if (!ace_init_spec("v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;twin=off", NULL) ||
            !set_env("SR_TEX_SCALE", "off") || !set_env("SR_SMART_2D", "off") ||
            !gegpu_texrepl_only_prepare(SR_SELFTEST_BUILD_ROOT)) {
            fprintf(stderr, "gpu texscale selftest: FAIL the Ace line, the settings or the synthetic pack were refused\n");
            free(arena);
            return 1;
        }
    } else if (!ace_init_spec("v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;twin=off", NULL) ||
        (!getenv("SR_TEX_SCALE") && !set_env("SR_TEX_SCALE", "2")) || !set_env("SR_SMART_2D", "on")) {
        fprintf(stderr, "gpu texscale selftest: FAIL the Ace line or the settings were refused\n");
        free(arena);
        return 1;
    }
#endif
#endif

    gegpu_selftest_ssaa_limits(ssaa_budget_mb << 20, ssaa_dim, ssaa_expect);
    if (!sdl3vk_init("Nakagawa GPU coherence selftest") || !gegpu_init()) {
        fprintf(stderr, "gpu coherence selftest: SKIP (Vulkan initialization unavailable)\n");
        sdl3vk_shutdown();
        free(arena);
        return 77;
    }

    int ok;
#if defined(SR_GPU_ACE_SELFTEST)
    ok = gegpu_ace_af_selftest();
    if (ok == 77) {
        gegpu_shutdown();
        sdl3vk_shutdown();
        free(arena);
        return 77;
    }
#elif defined(SR_GPU_SNAPSHOT_SYNC_SELFTEST)
    ok = gegpu_snapshot_sync_selftest();
#elif defined(SR_GPU_GLYPH_SELFTEST)
    ok = authentic || scene_hash ? gegpu_glyph_scene_selftest() : gegpu_glyph_selftest();
#elif defined(SR_GPU_TEXSCALE_SELFTEST)
    ok = repl_only ? gegpu_texrepl_only_selftest() : gegpu_texscale_selftest();
#else
    ok = gegpu_coherence_selftest();
    if (ok == 77) {   /* --case gpu-timestamps on a queue without timestamp queries */
        gegpu_shutdown();
        sdl3vk_shutdown();
        free(arena);
        return 77;
    }
#endif
#if !defined(SR_GPU_ACE_SELFTEST) && !defined(SR_GPU_TEXSCALE_SELFTEST)
    /* Ace Mode off (A2, src/rt/ace.h): every cell above ran with no anisotropic sampler,
     * and the texture pool kept its MAX_TEX sets. */
    {
        GeGpuAceStats ace;
        gegpu_ace_stats(&ace);
        if (ace.af != 0 || ace.af_samplers != 0u || ace.tex_pool_sets == 0u) {
            fprintf(stderr, "gpu coherence selftest: FAIL ace-off af=%d af_samplers=%llu pool=%u\n",
                    ace.af, ace.af_samplers, ace.tex_pool_sets);
            ok = 0;
        } else {
            printf("gpu coherence selftest: PASS ace-off af=0 af_samplers=0 pool=%u\n",
                   ace.tex_pool_sets);
        }
    }
#endif
    gegpu_shutdown();
    sdl3vk_shutdown();
    free(arena);
    if (!ok) return 1;
#if defined(SR_GPU_ACE_SELFTEST)
    puts("gpu ace selftest: OK");
#elif defined(SR_GPU_SNAPSHOT_SYNC_SELFTEST)
    puts("gpu snapshot sync selftest: OK");
#elif defined(SR_GPU_GLYPH_SELFTEST)
    puts("gpu glyph selftest: OK");
#elif defined(SR_GPU_TEXSCALE_SELFTEST)
    puts("gpu texscale selftest: OK");
#else
    puts("gpu coherence selftest: OK");
#endif
    return 0;
}
