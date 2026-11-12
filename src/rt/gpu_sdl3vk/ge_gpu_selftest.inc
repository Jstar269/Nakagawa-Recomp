// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-11-01.
// See NOTICE.md for upstream lineage and modification provenance.

/*
 * ge_gpu's self-tests (SR_GPU_COHERENCE_SELFTEST): the coherence, replay and corpus cases that
 * run ge_gpu.c's real paths against a headless Vulkan device.
 *
 * This is not a translation unit: ge_gpu.c includes it, inside its SR_GPU_COHERENCE_SELFTEST
 * guard and at the point where the code used to sit, because the tests reach the file-local
 * state (render targets, batches, readback bookkeeping) directly. Build it only through ge_gpu.c.
 */

typedef struct CoherenceCase {
    const char *name;
    uint32_t fmt, stride;
    uint32_t dirty_offset, dirty_bytes;
    int pending_batch;
    int pending_readback;
    int dirty_alias;
} CoherenceCase;

static void coherence_fill_raw(uint8_t *dst, uint32_t pixels, uint32_t bpp, uint32_t raw) {
    for (uint32_t i = 0; i < pixels; i++) memcpy(dst + i * bpp, &raw, bpp);
}

static void twin_identity_check(const char *when);

static const char *s_twin_case = "";      /* the running cell, for twin identity messages */

static void coherence_reset_targets(void) {
    if (s_nbatch) (void)submit_pending();
    twin_identity_check("before a case frees its targets");   /* the host twin (#2050) */
    s_twin_case = "";
    s_cur = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used) target_destroy(&s_tgts[i]);
    s_tmpl_ok = 0;
}

static uint32_t coherence_g1(uint32_t fmt) {
    static const uint32_t value[4] = { 0x4a69u, 0xca69u, 0xa73cu, 0x50607080u };
    return value[fmt & 3];
}

static uint32_t coherence_g0(uint32_t fmt) {
    static const uint32_t value[4] = { 0x1357u, 0x9357u, 0x2468u, 0x10203040u };
    return value[fmt & 3];
}

static uint32_t coherence_gbatch(uint32_t fmt) {
    static const uint32_t value[4] = { 0x6e25u, 0xee25u, 0xbb55u, 0xaa55aa55u };
    return value[fmt & 3];
}

/* The fixture's image is newer than guest VRAM in every row, as if the GPU had drawn them all. */
static void coherence_image_newer(Target *t) {
    t->clean_gen = 0;
    t->draw_x0 = 0;
    t->draw_y0 = 0;
    t->draw_x1 = FB_W;
    t->draw_y1 = FB_H;
}

static int coherence_queue_g1_batch(uint32_t base, uint32_t stride, uint32_t fmt) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = fmt;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535;
    s_ge->clear = 1;
    s_ge->clear_mode = 0x301;
    uint32_t g_batch = coherence_gbatch(fmt);
    uint32_t rgba = fb_unpack(g_batch, fmt);
    GeVtx p0 = { 10.0f, 10.0f, 1.0f, 1.0f, 0, 0, 1.0f,
                 rgba & 0xff, (rgba >> 8) & 0xff, (rgba >> 16) & 0xff, rgba >> 24 };
    GeVtx p1 = p0;
    p1.x = 20.0f;
    p1.y = 20.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_nbatch) return 0;
    return 1;
}

/* A CPU load after a completed GE list must materialize only the GPU-owned
 * surface it touches. This drives the production GeGpuHooks and recomp.h scalar
 * accessors; it does not call the explicit snapshot API. */
static int coherence_run_scalar_vram_read(uint32_t depth_base) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t color_offset = (10u * stride + 10u) * 4u;
    const uint32_t depth_offset = (10u * stride + 10u) * 2u;
    /* The GE XORs bits 6 and 13 of the absolute address (PSP-measured absolute form). */
    const uint32_t depth_swizzled = (depth_base + depth_offset) ^ 0x2040u;
    const uint32_t depth_linear_alias = 0x04200000u +
                                        (depth_base - 0x04000000u) + depth_offset;
    coherence_reset_targets();
    g_sr_vram_read_pending = 0;
    for (int i = 0; i < MAX_DEP; i++) {
        if (s_deps[i].used) {
            s_deps[i].guest_dirty = 0;
            s_deps[i].cpu_dirty = 1;
        }
    }
    memset(SR_HOST(base), 0, stride * FB_H * 4u);
    memset(SR_HOST(depth_base), 0, 0x44000u + 0x2040u);

    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = 3u;
    s_ge->zbp = depth_base;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    s_ge->ztest_enable = 1;
    s_ge->ztest = 1u;
    s_ge->zwrite_disable = 0;
    GeVtx p0 = { 10.0f, 10.0f, (float)0x1111u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x40, 0x50, 0x60, 0xff };
    GeVtx p1 = p0;
    p1.x = 12.0f;
    p1.y = 12.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_nbatch) {
        fprintf(stderr, "gpu coherence selftest: failed to queue scalar-read sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    uint32_t before = 1u;
    memcpy(&before, SR_HOST(base + color_offset), sizeof before);
    if (before != 0u || !g_sr_vram_read_pending) {
        fprintf(stderr,
                "gpu coherence selftest: list end materialized color early or missed pending ownership\n");
        coherence_reset_targets();
        return 0;
    }

    uint64_t lazy_color_start = SDL_GetTicksNS();
    uint32_t color = MEM_R32(base + color_offset);
    uint64_t lazy_color_ns = SDL_GetTicksNS() - lazy_color_start;
    if (color != 0x00605040u) {
        fprintf(stderr,
                "gpu coherence selftest: scalar framebuffer load (depth base 0x%08x) returned 0x%08x, expected 0x00605040\n",
                depth_base, color);
        coherence_reset_targets();
        return 0;
    }
    uint64_t lazy_depth_start = SDL_GetTicksNS();
    uint16_t depth = MEM_R16(depth_swizzled);
    uint64_t lazy_depth_ns = SDL_GetTicksNS() - lazy_depth_start;
    uint16_t depth_linear = MEM_R16(depth_linear_alias);
    if (depth != 0x1111u || depth_linear != 0x1111u) {
        fprintf(stderr,
                "gpu coherence selftest: depth scalar loads returned base=%04x mirror=%04x, expected 1111\n",
                depth, depth_linear);
        coherence_reset_targets();
        return 0;
    }

    enum { ACCESSOR_BENCH_ITERATIONS = 500000u };
    volatile uint32_t accessor_sink = 0u;
    uint64_t check_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) {
        sr_guest_vram_read_prepare_bytes(0x08010000u + ((i & 1023u) * 4u), 4u);
        accessor_sink += i + 1u;
    }
    uint64_t check_ns = SDL_GetTicksNS() - check_start;
    uint64_t loop_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) accessor_sink += i + 1u;
    uint64_t loop_ns = SDL_GetTicksNS() - loop_start;
    uint64_t scalar_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++)
        accessor_sink += MEM_R32(0x08010000u + ((i & 1023u) * 4u));
    uint64_t scalar_ns = SDL_GetTicksNS() - scalar_start;
    uint64_t raw_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) {
        uint32_t value;
        memcpy(&value, SR_HOST_LINEAR(0x08010000u + ((i & 1023u) * 4u)), sizeof value);
        accessor_sink += value;
    }
    uint64_t raw_ns = SDL_GetTicksNS() - raw_start;
    (void)accessor_sink;

    MEM_W16(depth_swizzled, 0xFFFFu);
    MEM_W32(base + color_offset, 0xFF112233u);
    s_ge->ztest = 4u;                    /* less: the CPU depth store must make this pass */
    s_ge->blend_enable = 1;
    s_ge->blend_mode = 3u | (10u << 4); /* source alpha zero, destination fixed B */
    s_ge->blend_fixb = 0x00FFFFFFu;
    GeVtx q0 = { 10.0f, 10.0f, (float)0x4444u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x01, 0x02, 0x03, 0xff };
    GeVtx q1 = q0;
    q1.x = 12.0f;
    q1.y = 12.0f;
    if (!hook_sprite(&q0, &q1, 0)) {
        fprintf(stderr, "gpu coherence selftest: CPU-written targets were not drawable\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t hybrid_submit_start = SDL_GetTicksNS();
    if (!submit_pending()) {
        fprintf(stderr, "gpu coherence selftest: CPU-written target batch did not submit\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t hybrid_submit_ns = SDL_GetTicksNS() - hybrid_submit_start;
    uint64_t hybrid_color_start = SDL_GetTicksNS();
    uint32_t color_after_cpu_write = MEM_R32(base + color_offset);
    uint64_t hybrid_color_ns = SDL_GetTicksNS() - hybrid_color_start;
    uint64_t hybrid_depth_start = SDL_GetTicksNS();
    uint16_t depth_after_cpu_write = MEM_R16(depth_swizzled);
    uint64_t hybrid_depth_ns = SDL_GetTicksNS() - hybrid_depth_start;
    if (color_after_cpu_write != 0xFF112233u || depth_after_cpu_write != 0x4444u) {
        fprintf(stderr,
                "gpu coherence selftest: post-store draw returned color=%08x depth=%04x, expected ff112233/4444\n",
                color_after_cpu_write, depth_after_cpu_write);
        coherence_reset_targets();
        return 0;
    }
    puts("gpu coherence selftest: PASS cpu_stores_consumed_by_next_gpu_draw");

    s_ge->ztest = 1u;
    s_ge->blend_enable = 0;
    GeVtx e0 = { 14.0f, 14.0f, (float)0x5555u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x70, 0x60, 0x50, 0xff };
    GeVtx e1 = e0;
    e1.x = 16.0f;
    e1.y = 16.0f;
    if (!hook_sprite(&e0, &e1, 0)) {
        fprintf(stderr, "gpu coherence selftest: could not queue eager-sync candidate draw\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_submit_start = SDL_GetTicksNS();
    if (!submit_pending()) {
        fprintf(stderr, "gpu coherence selftest: eager-sync candidate did not submit\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_submit_ns = SDL_GetTicksNS() - eager_submit_start;
    uint64_t eager_materialize_start = SDL_GetTicksNS();
    if (!target_readback(s_cur) || !depth_readback_to_guest(s_cur->dep)) {
        fprintf(stderr, "gpu coherence selftest: eager-sync candidate materialization failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_materialize_ns = SDL_GetTicksNS() - eager_materialize_start;
    printf("gpu coherence selftest: VRAM timing ns lazy-first-touch color=%" PRIu64
           " depth=%" PRIu64 " hybrid-listend-submit=%" PRIu64
           " hybrid-first-touch color=%" PRIu64 " depth=%" PRIu64
           " eager-listend-submit=%" PRIu64 " eager-sync-materialize=%" PRIu64
           " pending-check-extra=%" PRIu64 "/%u calls scalar-load=%" PRIu64
           " raw-load=%" PRIu64 " scalar-extra=%" PRIu64 "\n",
           lazy_color_ns, lazy_depth_ns, hybrid_submit_ns,
           hybrid_color_ns, hybrid_depth_ns, eager_submit_ns,
           eager_materialize_ns,
           check_ns > loop_ns ? (check_ns - loop_ns) / ACCESSOR_BENCH_ITERATIONS : 0u,
           ACCESSOR_BENCH_ITERATIONS, scalar_ns / ACCESSOR_BENCH_ITERATIONS,
           raw_ns / ACCESSOR_BENCH_ITERATIONS,
           scalar_ns > raw_ns ? (scalar_ns - raw_ns) / ACCESSOR_BENCH_ITERATIONS : 0u);
    puts("gpu coherence selftest: PASS scalar_cpu_read color+depth after list-end");
    coherence_reset_targets();
    return 1;
}

/* Establish issue 145's split generations without duplicating coherence: production
 * target_upload() creates GPU G1, the harness restores stale guest G0, then a real CPU
 * byte write flows through sr_gpu_vram_dirty(), production reacquire, and readback. */
static int coherence_run_case(const CoherenceCase *tc) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t bpp = tc->fmt == 3 ? 4u : 2u;
    uint32_t pixels = tc->stride * FB_H;
    uint32_t target_bytes = pixels * bpp;
    if (!tc->dirty_bytes || tc->dirty_offset + tc->dirty_bytes > target_bytes) return 0;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    uint8_t *payload = (uint8_t *)malloc(tc->dirty_bytes);
    if (!expected || !payload) { free(expected); free(payload); return 0; }
    uint32_t g1 = coherence_g1(tc->fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    coherence_fill_raw(expected, pixels, bpp, g1);

    Target *t = target_color_acquire(fba, tc->stride, tc->fmt, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu coherence selftest [%s]: could not establish GPU G1\n", tc->name);
        free(expected); free(payload); return 0;
    }
    coherence_fill_raw(guest, pixels, bpp, coherence_g0(tc->fmt));
    coherence_image_newer(t);

    if (tc->pending_batch) {
        if (!coherence_queue_g1_batch(base, tc->stride, tc->fmt)) {
            fprintf(stderr, "gpu coherence selftest [%s]: could not queue production batch\n", tc->name);
            free(expected); free(payload); return 0;
        }
        uint32_t g_batch = coherence_gbatch(tc->fmt);
        for (uint32_t py = 10; py < 20; py++) {
            for (uint32_t px = 10; px < 20; px++) {
                uint32_t b_offset = (py * tc->stride + px) * bpp;
                memcpy(expected + b_offset, &g_batch, bpp);
            }
        }
    }

    ReadbackSlot *stale = NULL;
    if (tc->pending_readback) {
        if (!target_prepare_present(t)) {
            fprintf(stderr, "gpu coherence selftest [%s]: async readback setup failed\n", tc->name);
            free(expected); free(payload); return 0;
        }
        for (int i = 0; i < READBACK_FRAMES; i++)
            if (s_readback[i].pending && s_readback[i].image == t->img) {
                stale = &s_readback[i]; break;
            }
        if (!stale) {
            fprintf(stderr, "gpu coherence selftest [%s]: no pending async slot\n", tc->name);
            free(expected); free(payload); return 0;
        }
    }

    for (uint32_t i = 0; i < tc->dirty_bytes; i++) payload[i] = (uint8_t)(0xd1u + i * 37u);
    memcpy(guest + tc->dirty_offset, payload, tc->dirty_bytes);
    memcpy(expected + tc->dirty_offset, payload, tc->dirty_bytes);
    uint32_t dirty_addr = base + tc->dirty_offset;
    if (tc->dirty_alias) dirty_addr += 0x40000000u;
    sr_gpu_vram_dirty(dirty_addr, tc->dirty_bytes);

    if (!t->cpu_n) {
        fprintf(stderr, "gpu coherence selftest [%s]: dirty hook did not hold the write\n", tc->name);
        free(expected); free(payload); return 0;
    }
    if (stale) {
        if (stale->commit || readback_finish(stale, 1, 1) < 0 ||
            memcmp(guest + tc->dirty_offset, payload, tc->dirty_bytes) != 0) {
            fprintf(stderr, "gpu coherence selftest [%s]: stale async completion overwrote G2\n",
                    tc->name);
            free(expected); free(payload); return 0;
        }
    }

    t = target_color_acquire(fba, tc->stride, tc->fmt, 1);
    if (!t || !target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: production reacquire/readback failed\n",
                tc->name);
        free(expected); free(payload); return 0;
    }

    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr,
                "gpu coherence selftest [%s]: byte %zu expected=%02x actual=%02x\n",
                tc->name, mismatch, expected[mismatch], guest[mismatch]);
        free(expected); free(payload); return 0;
    }
    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           tc->name, tc->fmt, tc->stride, s_scale);
    free(expected);
    free(payload);
    return 1;
}

static int coherence_run_clean_case(const char *name, uint32_t fmt, uint32_t stride,
                                     uint32_t dirty_offset, uint32_t dirty_bytes) {
    const uint32_t fba = 0x00120000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t bpp = fmt == 3 ? 4u : 2u;
    uint32_t pixels = stride * FB_H;
    uint32_t target_bytes = pixels * bpp;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    uint8_t *payload = (uint8_t *)malloc(dirty_bytes);
    if (!expected || !payload) { free(expected); free(payload); return 0; }

    uint32_t g1 = coherence_g1(fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    coherence_fill_raw(expected, pixels, bpp, g1);

    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: clean_gen initial setup failed\n", name);
        free(expected); free(payload); return 0;
    }

    for (uint32_t i = 0; i < dirty_bytes; i++) payload[i] = (uint8_t)(0xb5u + i * 19u);
    memcpy(guest + dirty_offset, payload, dirty_bytes);
    memcpy(expected + dirty_offset, payload, dirty_bytes);

    sr_gpu_vram_dirty(base + dirty_offset, dirty_bytes);

    if (t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: clean_gen did not advance to render_gen\n", name);
        free(expected); free(payload); return 0;
    }

    uint64_t pre_rb = s_cnt_readback;
    if (!target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: target_readback failed\n", name);
        free(expected); free(payload); return 0;
    }
    if (s_cnt_readback != pre_rb) {
        fprintf(stderr, "gpu coherence selftest [%s]: readback performed unnecessary copy for clean target\n", name);
        free(expected); free(payload); return 0;
    }

    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr, "gpu coherence selftest [%s]: guest VRAM mismatch at byte %zu\n", name, mismatch);
        free(expected); free(payload); return 0;
    }

    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           name, fmt, stride, s_scale);
    free(expected); free(payload);
    return 1;
}

/* Many small CPU writes into a resident target between two GE uses are held and applied
 * together (target_note_cpu_write, target_sync_cpu_writes), not one image patch each. The GPU
 * holds G1 while guest VRAM holds G0, so a byte the image takes from the wrong side shows.
 * 1024 adjacent 4-byte stores fill two rows and merge into one span; 48 unaligned 2-byte
 * stores in other rows split pixels and outnumber the spans, so one group is applied early.
 * The readback must hold the written bytes, G1 everywhere else (the GPU side of every split
 * pixel included), with far fewer patches than writes. */
static int coherence_run_cpu_write_coalesce(void) {
    const char *name = "cpu-write-coalesce";
    const uint32_t fba = 0x00150000u, base = 0x04000000u | fba;
    const uint32_t fmt = 3u, stride = 512u, bpp = 4u, pixels = stride * FB_H;
    const uint32_t target_bytes = pixels * bpp;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    if (!expected) return 0;
    coherence_fill_raw(guest, pixels, bpp, coherence_g1(fmt));
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) { free(expected); return 0; }
    memcpy(expected, guest, target_bytes);              /* the image: G1 */
    coherence_fill_raw(guest, pixels, bpp, coherence_g0(fmt));
    coherence_image_newer(t);

    const unsigned long long notes0 = s_replay_stats.cpu_write_notes;
    const unsigned long long syncs0 = s_replay_stats.cpu_write_syncs;
    const unsigned uploads0 = s_cnt_upload;
    for (uint32_t i = 0; i < 1024u; i++) {             /* rows 40 and 41, every pixel */
        const uint32_t off = (40u * stride) * bpp + i * 4u;
        for (uint32_t k = 0; k < 4u; k++) guest[off + k] = expected[off + k] = (uint8_t)(0x31u + i + k);
        sr_gpu_vram_dirty(base + off, 4u);
    }
    for (uint32_t i = 0; i < 48u; i++) {               /* one 2-byte store per row, mid-pixel */
        const uint32_t off = ((100u + i) * stride + 7u * i) * bpp + 1u;
        guest[off] = expected[off] = (uint8_t)(0xa0u + i);
        guest[off + 1u] = expected[off + 1u] = (uint8_t)(0x50u + i);
        sr_gpu_vram_dirty(base + off, 2u);
    }
    const unsigned long long notes = s_replay_stats.cpu_write_notes - notes0;
    if (!target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: readback failed\n", name);
        free(expected); return 0;
    }
    const unsigned long long syncs = s_replay_stats.cpu_write_syncs - syncs0;
    const unsigned uploads = s_cnt_upload - uploads0;
    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr, "gpu coherence selftest [%s]: byte %zu expected=%02x actual=%02x\n",
                name, mismatch, expected[mismatch], guest[mismatch]);
        free(expected); return 0;
    }
    if (notes != 1072u || syncs != 2u || uploads > 1u + 48u) {
        fprintf(stderr, "gpu coherence selftest [%s]: notes=%llu syncs=%llu uploads=%u\n",
                name, notes, syncs, uploads);
        free(expected); return 0;
    }
    printf("gpu coherence selftest: PASS %-22s writes=%llu groups=%llu patches=%u\n",
           name, notes, syncs, uploads);
    coherence_reset_targets();
    free(expected);
    return 1;
}

static int coherence_run_full_cover_case(void) {
    const char *name = "full-cover-stale";
    const uint32_t fba = 0x00130000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t fmt = 3, stride = 512, bpp = 4;
    uint32_t pixels = stride * FB_H;
    uint32_t target_bytes = pixels * bpp;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    if (!expected) return 0;

    uint32_t g1 = coherence_g1(fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) { free(expected); return 0; }

    coherence_image_newer(t);

    for (uint32_t i = 0; i < target_bytes; i++) {
        guest[i] = (uint8_t)(0x77u + i * 13u);
        expected[i] = guest[i];
    }
    sr_gpu_vram_dirty(base, target_bytes);

    if (!target_sync_cpu_writes(t) || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: full cover write failed to advance clean_gen\n", name);
        free(expected); return 0;
    }

    if (!target_readback(t) || memcmp(guest, expected, target_bytes) != 0) {
        fprintf(stderr, "gpu coherence selftest [%s]: target readback verification failed\n", name);
        free(expected); return 0;
    }

    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           name, fmt, stride, s_scale);
    free(expected);
    return 1;
}

static int coherence_run_overlap_case(void) {
    const uint32_t fba_a = 0x000f0000u, fba_b = 0x00110000u;
    const uint32_t base_a = 0x04000000u | fba_a, base_b = 0x04000000u | fba_b;
    const uint32_t stride = FB_W, bpp = 4, bytes = stride * FB_H * bpp;
    const uint32_t g1a = 0x31527394u, g1b = 0x426384a5u, g0 = 0x10203040u;
    const uint32_t dirty_addr = base_b + 123u;
    const uint8_t payload[7] = { 0xe1, 0x17, 0xc3, 0x49, 0xa5, 0x6b, 0x8d };
    coherence_reset_targets();
    uint8_t *ga = (uint8_t *)SR_HOST(base_a), *gb = (uint8_t *)SR_HOST(base_b);
    uint8_t *ea = (uint8_t *)malloc(bytes), *eb = (uint8_t *)malloc(bytes);
    if (!ea || !eb) { free(ea); free(eb); return 0; }

    coherence_fill_raw(ga, stride * FB_H, bpp, g1a);
    Target *a = target_color_acquire(fba_a, stride, 3, 1);
    coherence_fill_raw(gb, stride * FB_H, bpp, g1b);
    Target *b = target_color_acquire(fba_b, stride, 3, 1);
    if (!a || !b) { free(ea); free(eb); return 0; }
    coherence_fill_raw(ga, (bytes + (fba_b - fba_a)) / bpp, bpp, g0);
    coherence_fill_raw(ea, stride * FB_H, bpp, g1a);
    coherence_fill_raw(eb, stride * FB_H, bpp, g1b);
    coherence_image_newer(a);
    coherence_image_newer(b);
    memcpy(SR_HOST(dirty_addr), payload, sizeof(payload));
    memcpy(ea + (dirty_addr - base_a), payload, sizeof(payload));
    memcpy(eb + (dirty_addr - base_b), payload, sizeof(payload));
    sr_gpu_vram_dirty(dirty_addr, sizeof(payload));

    int ok = a->gpu_valid && b->gpu_valid && target_readback(a) &&
             memcmp(SR_HOST(base_a), ea, bytes) == 0 && target_readback(b) &&
             memcmp(SR_HOST(base_b), eb, bytes) == 0;
    if (!ok) fprintf(stderr, "gpu coherence selftest [overlapping-targets]: preservation failed\n");
    else printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
                "overlapping-targets", stride, s_scale);
    free(ea); free(eb);
    return ok;
}

/* The GE has no buffer height, and titles keep data in VRAM below a small render target: vertices
 * read a word at a time, or another target. Only the rows the GPU drew are the GPU's. A read of any
 * other row leaves the queued batches queued and needs no readback, and a readback leaves every
 * other row of VRAM alone. */
static int coherence_run_undrawn_rows(void) {
    const char *name = "undrawn-rows";
    const uint32_t stride = 256u, row = stride * 4u, fmt = 3u;
    const uint32_t fba_a = 0x00100000u, base_a = 0x04000000u | fba_a;
    const uint32_t fba_b = fba_a + 100u * row, base_b = 0x04000000u | fba_b;
    const uint32_t data = base_a + 200u * row + 16u, data_word = 0x12345678u;
    const uint32_t pixel = coherence_gbatch(fmt), offset = 15u * row + 15u * 4u;
    coherence_reset_targets();
    memset(SR_HOST(base_a), 0, 372u * row);
    memcpy(SR_HOST(data), &data_word, sizeof data_word);
    g_sr_vram_read_pending = 0;

    /* Rows 10 to 19 of `a` drawn: a read of what lies below them is plain VRAM. */
    int ok = coherence_queue_g1_batch(base_a, stride, fmt);
    ok = submit_pending() && ok;
    Target *a = target_find_by_fba(fba_a);
    const unsigned long readbacks = s_cnt_readback;
    ok = ok && a && a->clean_gen != a->render_gen && a->draw_y0 <= 10 && a->draw_y1 >= 20 &&
         a->draw_y1 < 100;
    uint32_t value = MEM_R32(data);
    if (!ok || value != data_word || s_cnt_readback != readbacks || a->clean_gen == a->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: read below the drawn rows read back the "
                        "target (value=0x%08x readbacks=%lu rows=%d..%d gen=%llu clean=%llu)\n", name, value,
                s_cnt_readback - readbacks, a ? a->draw_y0 : -1, a ? a->draw_y1 : -1,
                a ? (unsigned long long)a->render_gen : 0ull, a ? (unsigned long long)a->clean_gen : 0ull);
        coherence_reset_targets();
        return 0;
    }
    /* Queued rows count too, and only they do. */
    ok = coherence_queue_g1_batch(base_a, stride, fmt);
    value = MEM_R32(data);
    const int kept_queued = s_nbatch != 0;
    uint32_t drawn = MEM_R32(base_a + offset);
    if (!ok || value != data_word || !kept_queued || s_nbatch != 0 || drawn != pixel) {
        fprintf(stderr, "gpu coherence selftest [%s]: queued rows: kept=%d pending=%u drawn=0x%08x\n",
                name, kept_queued, (unsigned)s_nbatch, drawn);
        coherence_reset_targets();
        return 0;
    }
    /* `b` lies in rows of `a` the GPU never drew there: a readback of `a` keeps `b`'s pixels. */
    ok = coherence_queue_g1_batch(base_b, stride, fmt);
    ok = submit_pending() && ok;
    ok = ok && MEM_R32(base_b + offset) == pixel;
    ok = ok && coherence_queue_g1_batch(base_a, stride, fmt);
    ok = submit_pending() && ok;
    ok = ok && MEM_R32(base_a + offset) == pixel;
    uint32_t kept = 0u;
    memcpy(&kept, SR_HOST(base_b + offset), sizeof kept);
    if (!ok || kept != pixel) {
        fprintf(stderr, "gpu coherence selftest [%s]: the readback of a target overwrote the "
                        "target below it (0x%08x, expected 0x%08x)\n", name, kept, pixel);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           name, fmt, stride, s_scale);
    coherence_reset_targets();
    return 1;
}

/* A title may keep its palettes in the stride padding of its own render target: a 16-bit target
 * at stride 512 that the GPU draws 480 pixels wide leaves x = 480 to 511 of every row to the CPU.
 * A CLUT load, or a CPU read, of those columns reads plain guest VRAM, so it must neither submit
 * the queued batches nor read the target back, however many drawn rows lie around it. A palette
 * under the pixels the GPU drew is read back, and the load gets those pixels. */
static int coherence_draw_480_wide(uint32_t base, uint32_t stride, uint32_t fmt) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = fmt;
    s_ge->zbp = 0x04180000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = 479;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535;
    s_ge->clear = 1;
    s_ge->clear_mode = 0x301;
    uint32_t rgba = fb_unpack(coherence_gbatch(fmt), fmt);
    GeVtx p0 = { 0.0f, 0.0f, 1.0f, 1.0f, 0, 0, 1.0f,
                 rgba & 0xff, (rgba >> 8) & 0xff, (rgba >> 16) & 0xff, rgba >> 24 };
    GeVtx p1 = p0;
    p1.x = 480.0f;
    p1.y = (float)FB_H;
    return hook_sprite(&p0, &p1, 0) && s_nbatch;
}

/* Load `blocks` 32-byte blocks of 5551 palette from `clut` with a GE list. */
static void coherence_load_clut(uint32_t clut, uint32_t blocks) {
    const uint32_t list_addr = 0x00100800u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0xB0, clut & 0x00FFFFFFu);
    DL_EMIT(0xB1, (clut & 0xFF000000u) >> 8);
    DL_EMIT(0xC5, 1u | (0xFFu << 8));                   /* 5551, shift 0, mask 0xFF */
    DL_EMIT(0xC4, blocks);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);
}

static int coherence_run_clut_stride_padding(void) {
    const char *name = "clut-stride-padding";
    const uint32_t stride = 512u, fmt = 1u, row = stride * 2u, bytes = 64u;
    const uint32_t fba = 0x00100000u, base = 0x04000000u | fba;
    const uint32_t pad = base + 5u * row + 480u * 2u;   /* row 5, x = 480 to 511 */
    const uint32_t drawn = base + 6u * row;             /* row 6, x = 0 to 31 */
    const uint16_t pixel = (uint16_t)coherence_gbatch(fmt);
    uint8_t palette[64];
    for (uint32_t i = 0; i < bytes; i++) palette[i] = (uint8_t)(0x3du + i * 7u);
    coherence_reset_targets();
    memset(SR_HOST(base), 0, FB_H * row);
    sr_gpu_vram_dirty(base, FB_H * row);
    g_sr_vram_read_pending = 0;

    /* Every row drawn 480 wide, then the palette written by the CPU into the padding. */
    int ok = coherence_draw_480_wide(base, stride, fmt);
    ok = submit_pending() && ok;
    Target *t = target_find_by_fba(fba);
    memcpy(SR_HOST(pad), palette, bytes);
    sr_gpu_vram_dirty(pad, bytes);
    ok = ok && t && t->gpu_valid && t->clean_gen != t->render_gen;
    /* Queue more drawing, so a needless overlap would also submit it. */
    ok = ok && coherence_draw_480_wide(base, stride, fmt);
    const unsigned long readbacks = s_cnt_readback;
    coherence_load_clut(pad, bytes / 32u);
    const int kept_queued = s_nbatch != 0;
    const uint32_t word = MEM_R32(pad + 8u);
    uint32_t want_word = 0;
    memcpy(&want_word, palette + 8, sizeof want_word);
    if (!ok || !kept_queued || s_cnt_readback != readbacks || !t || t->clean_gen == t->render_gen ||
        memcmp(s_ge->clutram, palette, bytes) != 0 || word != want_word) {
        fprintf(stderr, "gpu coherence selftest [%s]: a palette in the stride padding read back "
                        "the target (ok=%d queued=%d readbacks=%lu palette=%s word=0x%08x)\n", name,
                ok, kept_queued, s_cnt_readback - readbacks,
                memcmp(s_ge->clutram, palette, bytes) == 0 ? "match" : "MISMATCH", word);
        coherence_reset_targets();
        return 0;
    }
    /* A palette under drawn pixels is read back, queued batches included. */
    coherence_load_clut(drawn, bytes / 32u);
    int under = s_nbatch == 0 && s_cnt_readback == readbacks + 1u && t->clean_gen == t->render_gen;
    for (uint32_t i = 0; under && i < bytes / 2u; i++) {
        uint16_t v;
        memcpy(&v, s_ge->clutram + i * 2u, sizeof v);
        under = v == pixel;
    }
    if (!under) {
        fprintf(stderr, "gpu coherence selftest [%s]: a palette under drawn pixels was not read "
                        "back (pending=%u readbacks=%lu)\n", name, (unsigned)s_nbatch,
                s_cnt_readback - readbacks);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           name, fmt, stride, s_scale);
    coherence_reset_targets();
    return 1;
}

/* A title that never writes depth may leave zbp on its own colour buffer: a 16-bit depth surface
 * at stride 512 covers the first 136 rows of a 32-bit one. Draws that do not write depth leave the
 * depth surface as it was loaded, so it is not newer than guest VRAM, and a CPU access to those
 * rows must not write it back over the colour pixels the GPU drew there. */
static int coherence_run_depth_alias_no_zwrite(void) {
    const char *name = "depth-alias-no-zwrite";
    const uint32_t fba = 0x00100000u, base = 0x04000000u | fba, stride = 512u;
    const uint32_t bytes = stride * FB_H * 4u, probe = (5u * stride + 20u) * 4u;
    coherence_reset_targets();
    g_sr_vram_read_pending = 0;
    for (int i = 0; i < MAX_DEP; i++) {
        if (s_deps[i].used) {
            s_deps[i].guest_dirty = 0;
            s_deps[i].cpu_dirty = 1;
        }
    }
    coherence_fill_raw(SR_HOST(base), stride * FB_H, 4u, coherence_g0(3u));
    uint8_t *expected = malloc(bytes);
    if (!expected) return 0;

    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = 3u;
    s_ge->zbp = base;                      /* depth aliases rows 0..135 of the colour buffer */
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    GeVtx p0 = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
    GeVtx p1 = p0;
    p1.x = 480.0f;
    p1.y = (float)FB_H;
    int ok = hook_sprite(&p0, &p1, 0) && s_nbatch && submit_pending();
    Target *t = target_find_by_fba(fba);
    DepthEnt *d = t ? t->dep : NULL;
    ok = ok && t && d && target_readback(t) && t->clean_gen == t->render_gen;
    if (ok) memcpy(expected, SR_HOST(base), bytes);
    uint32_t drawn = 0u;
    if (ok) memcpy(&drawn, expected + probe, sizeof drawn);
    if (!ok || drawn == coherence_g0(3u)) {
        fprintf(stderr, "gpu coherence selftest [%s]: setup failed (target=%p depth=%p drawn=0x%08x)\n",
                name, (void *)t, (void *)d, drawn);
        free(expected);
        coherence_reset_targets();
        return 0;
    }
    const int depth_newer = d->guest_dirty;
    const uint32_t loaded = MEM_R32(base + probe);
    const int kept = memcmp(SR_HOST(base), expected, bytes) == 0;
    uint32_t first_bad = UINT32_MAX;
    for (uint32_t off = 0; !kept && off < bytes; off += 4u)
        if (memcmp(SR_HOST(base + off), expected + off, 4u) != 0) { first_bad = off; break; }
    free(expected);
    if (depth_newer || loaded != drawn || !kept) {
        fprintf(stderr, "gpu coherence selftest [%s]: a draw without depth writes marked depth newer "
                        "(%d) and a CPU load returned 0x%08x, expected 0x%08x; first changed colour row %d\n",
                name, depth_newer, loaded, drawn,
                first_bad == UINT32_MAX ? -1 : (int)(first_bad / (stride * 4u)));
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n", name, stride, s_scale);
    coherence_reset_targets();
    return 1;
}

static int coherence_run_tflt_descriptor_capacity(void) {
    const uint32_t stride = FB_W;
    coherence_reset_targets();
    for (unsigned i = 0; i < MAX_TGT; i++) {
        uint32_t fba = 0x00020000u + i * 0x00010000u;
        Target *target = target_color_acquire(fba, stride, 3u, 1);
        if (!target) {
            fprintf(stderr,
                    "gpu coherence selftest [TFLT-descriptors]: target %u/%u exhausted the fixed sampler pool\n",
                    i + 1u, MAX_TGT);
            coherence_reset_targets();
            return 0;
        }
        for (unsigned filter = 0; filter < TEX_FILTER_VARIANTS; filter++) {
            if (!target->set[filter]) {
                fprintf(stderr,
                        "gpu coherence selftest [TFLT-descriptors]: target %u lacks sampler variant %u\n",
                        i, filter);
                coherence_reset_targets();
                return 0;
            }
        }
    }
    coherence_reset_targets();
    printf("gpu coherence selftest: PASS TFLT descriptor-capacity targets=%u variants=%u sets=%u\n",
           MAX_TGT, TEX_FILTER_VARIANTS, FIXED_FILTER_DESCRIPTOR_SETS);
    return 1;
}

typedef struct SpriteEndpoint {
    float x, y, u, v;
} SpriteEndpoint;

typedef struct SpriteExpected {
    const char *name;
    SpriteEndpoint p0, p1;
    int x, y, w, h;
    int u, v, du, dv;
} SpriteExpected;

typedef struct RawSpriteVtx {
    float u, v;
    float x, y, z;
} RawSpriteVtx;

/* Synthetic through-mode sprites.  The expected table is deliberately independent of
 * the interpolation code: it names the complete texel sequence from the screen-space
 * minimum corner. UV is linear between the vertex on each edge and every pixel samples
 * it at its centre (PSP-GE-SAMPLE-001), so a pixel shows texel floor(u at x + 0.5):
 * reverse-x-scaled's first pixel reads 8 - 0.5 * 2 = 7. */
static const SpriteExpected k_sprite_expected[] = {
    { "normal",               { 10.0f,  10.0f, 0.0f, 0.0f }, { 14.0f,  14.0f, 4.0f, 4.0f },
      10,  10, 4, 4, 0, 0,  1,  1 },
    { "reverse-x",            { 14.0f,  20.0f, 0.0f, 0.0f }, { 10.0f,  24.0f, 4.0f, 4.0f },
      10,  20, 4, 4, 3, 0, -1,  1 },
    { "reverse-y",            { 10.0f,  34.0f, 0.0f, 0.0f }, { 14.0f,  30.0f, 4.0f, 4.0f },
      10,  30, 4, 4, 0, 3,  1, -1 },
    { "reverse-xy",           { 14.0f,  44.0f, 0.0f, 0.0f }, { 10.0f,  40.0f, 4.0f, 4.0f },
      10,  40, 4, 4, 3, 3, -1, -1 },
    { "one-pixel",            { 20.0f,  50.0f, 2.0f, 2.0f }, { 21.0f,  51.0f, 3.0f, 3.0f },
      20,  50, 1, 1, 2, 2,  0,  0 },
    { "one-pixel-reverse-x",  { 21.0f,  52.0f, 2.0f, 2.0f }, { 20.0f,  53.0f, 3.0f, 3.0f },
      20,  52, 1, 1, 2, 2,  0,  0 },
    { "reverse-x-offset",     { 14.0f,  60.0f, 2.0f, 3.0f }, { 10.0f,  64.0f, 6.0f, 7.0f },
      10,  60, 4, 4, 5, 3, -1,  1 },
    { "reverse-x-scaled",     { 14.0f,  70.0f, 0.0f, 0.0f }, { 10.0f,  74.0f, 8.0f, 4.0f },
      10,  70, 4, 4, 7, 0, -2,  1 },
    { "reverse-x-subpixel",   { 14.75f, 80.75f, 0.0f, 0.0f }, { 10.75f, 84.75f, 4.0f, 4.0f },
      10,  80, 4, 4, 3, 0, -1,  1 },
    { "reverse-x-far-edge",   { 38.0f,  90.0f, 0.0f, 0.0f }, { 30.0f,  92.0f, 8.0f, 2.0f },
      30,  90, 8, 2, 7, 0, -1,  1 },
    { "one-high-reverse-y",   { 40.0f, 101.0f, 1.0f, 2.0f }, { 44.0f, 100.0f, 5.0f, 3.0f },
      40, 100, 4, 1, 1, 2,  1,  0 },
    { "one-pixel-reverse-xy", { 51.0f, 111.0f, 2.0f, 2.0f }, { 50.0f, 110.0f, 3.0f, 3.0f },
      50, 110, 1, 1, 2, 2,  0,  0 },
};

static uint32_t *s_sprite_gpu_pixels;
static size_t s_sprite_gpu_bytes;

static uint32_t coherence_sprite_texel(int u, int v) {
    uint32_t r = (uint32_t)(u + 1) * 25u;
    uint32_t g = (uint32_t)(v + 1) * 25u;
    /* Default GE alpha write state keeps the destination alpha byte at zero. */
    return 0x00640000u | (g << 8) | r;
}

static int coherence_verify_sprite_output(const char *path, const uint32_t *fb,
                                          uint32_t stride) {
    for (size_t i = 0; i < sizeof(k_sprite_expected) / sizeof(k_sprite_expected[0]); i++) {
        const SpriteExpected *tc = &k_sprite_expected[i];
        for (int dy = 0; dy < tc->h; dy++) {
            for (int dx = 0; dx < tc->w; dx++) {
                int u = tc->u + dx * tc->du;
                int v = tc->v + dy * tc->dv;
                uint32_t want = coherence_sprite_texel(u, v);
                uint32_t got = fb[(tc->y + dy) * stride + tc->x + dx];
                if (got != want) {
                    fprintf(stderr,
                            "gpu coherence selftest [sprite-%s/%s]: pixel=(%d,%d) "
                            "texel=(%d,%d) expected=%08x actual=%08x\n",
                            path, tc->name, tc->x + dx, tc->y + dy, u, v, want, got);
                    return 0;
                }
            }
        }
        /* Prove the rectangle is endpoint-exclusive on all four sides. */
        for (int dx = 0; dx < tc->w; dx++) {
            if (fb[(tc->y - 1) * stride + tc->x + dx] != 0 ||
                fb[(tc->y + tc->h) * stride + tc->x + dx] != 0) {
                fprintf(stderr, "gpu coherence selftest [sprite-%s/%s]: y edge leaked\n",
                        path, tc->name);
                return 0;
            }
        }
        for (int dy = 0; dy < tc->h; dy++) {
            if (fb[(tc->y + dy) * stride + tc->x - 1] != 0 ||
                fb[(tc->y + dy) * stride + tc->x + tc->w] != 0) {
                fprintf(stderr, "gpu coherence selftest [sprite-%s/%s]: x edge leaked\n",
                        path, tc->name);
                return 0;
            }
        }
    }
    return 1;
}

static uint32_t coherence_prepare_sprite_fixture(uint32_t base, uint32_t tex_base,
                                                 uint32_t stride) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            uint32_t r = (uint32_t)(u + 1) * 25u;
            uint32_t g = (uint32_t)(v + 1) * 25u;
            tex[v * 8 + u] = 0xff640000u | (g << 8) | r;
        }
    }
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    uint32_t sprite_count = (uint32_t)(sizeof(k_sprite_expected) /
                                       sizeof(k_sprite_expected[0]));
    for (uint32_t i = 0; i < sprite_count; i++) {
        const SpriteExpected *tc = &k_sprite_expected[i];
        vtx[i * 2] = (RawSpriteVtx){ tc->p0.u, tc->p0.v, tc->p0.x, tc->p0.y, 0.0f };
        vtx[i * 2 + 1] =
            (RawSpriteVtx){ tc->p1.u, tc->p1.v, tc->p1.x, tc->p1.y, 0.0f };
    }

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8 | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3 << 8) | 3);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, 0);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3 | (1 << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3 << 0) | (3 << 7) | (1 << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6 << 16) | (sprite_count * 2u));
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_run_sprite_semantics(void) {
    const uint32_t fba = 0x00160000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);

    uint64_t spr_before = s_cnt_spr;
    size_t sprite_count = sizeof(k_sprite_expected) / sizeof(k_sprite_expected[0]);
    uint32_t list_addr = coherence_prepare_sprite_fixture(base, tex_base, stride);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);

    if (s_cnt_spr != spr_before + sprite_count) {
        fprintf(stderr, "gpu coherence selftest [sprite-semantics]: hook_sprite call count mismatch\n");
        return 0;
    }

    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride, .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [sprite-semantics]: gegpu_sync_guest_fb failed\n");
        return 0;
    }

    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_sprite_output("gpu", fb, stride)) return 0;

    s_sprite_gpu_bytes = stride * FB_H * sizeof(*fb);
    free(s_sprite_gpu_pixels);
    s_sprite_gpu_pixels = (uint32_t *)malloc(s_sprite_gpu_bytes);
    if (!s_sprite_gpu_pixels) {
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    memcpy(s_sprite_gpu_pixels, fb, s_sprite_gpu_bytes);

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-exact-gpu", stride, s_scale);
    return 1;
}

static int coherence_run_software_sprite_semantics(void) {
    const uint32_t fba = 0x00160000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);

    uint32_t list_addr = coherence_prepare_sprite_fixture(base, tex_base, stride);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);

    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_sprite_output("software", fb, stride)) {
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    size_t bytes = stride * FB_H * sizeof(*fb);
    if (!s_sprite_gpu_pixels || s_sprite_gpu_bytes != bytes) {
        fprintf(stderr, "gpu coherence selftest [sprite-parity]: missing GPU reference\n");
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    size_t mismatch = 0;
    while (mismatch < bytes && ((const uint8_t *)s_sprite_gpu_pixels)[mismatch] == guest[mismatch])
        mismatch++;
    if (mismatch != bytes) {
        fprintf(stderr,
                "gpu coherence selftest [sprite-parity]: byte=%zu gpu=%02x software=%02x\n",
                mismatch, ((const uint8_t *)s_sprite_gpu_pixels)[mismatch], guest[mismatch]);
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    free(s_sprite_gpu_pixels);
    s_sprite_gpu_pixels = NULL;
    s_sprite_gpu_bytes = 0;

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-exact-software", stride, s_scale);
    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-sw-gpu-parity", stride, s_scale);
    return 1;
}

/* ---- raster bounds: the scissor and the target's stride, not the 480-pixel screen (#2193) ----
 *
 * #2193 states that a PSP draws anywhere inside the scissor on a target up to 512 pixels
 * wide: an offscreen render target, or a display buffer's stride padding. This cell checks
 * that the two renderers agree on that bound, not what a PSP draws; stopping at a stride of
 * 481 to 511 is unmeasured on a PSP. One list draws three bands reaching
 * x = 512 on an 8888 target pre-filled with a sentinel: a clear (rows 0-89), a sprite (rows
 * 90-179, x from 400) and two triangles (rows 180-271, x from 400). The Vulkan backend and
 * ge.c each run it, and each must draw every column up to the scissor's x2 or the stride's
 * last column, whichever is less, leave the rest of each row alone, and agree byte for byte.
 * The 480-pixel scissor on a 512 stride is the screen case: columns 480-511 keep the
 * sentinel and columns 0-479 hold what they held before this bound changed. A 256 stride
 * under that scissor is the narrow case: a column past the stride would be the next row's
 * pixel in guest memory, so each renderer must stop at column 255, and the row past the
 * target (checked in every case) must keep the sentinel. */
typedef struct CohWideVtx { uint32_t c; float x, y, z; } CohWideVtx;

enum { COH_WIDE_CLEAR = 0xff2040c0u, COH_WIDE_SPRITE = 0xff30a050u, COH_WIDE_TRI = 0xffc06030u };
static const uint32_t k_coh_wide_bg = 0xa5a5a5a5u;

static uint32_t coherence_wide_raster_list(uint32_t base, uint32_t stride, int scis_x2) {
    const uint32_t vaddr = 0x00102000u;
    CohWideVtx *v = (CohWideVtx *)SR_HOST(vaddr);
    const CohWideVtx verts[] = {
        { COH_WIDE_CLEAR, 0.0f, 0.0f, 0.0f },     { COH_WIDE_CLEAR, 512.0f, 90.0f, 0.0f },
        { COH_WIDE_SPRITE, 400.0f, 90.0f, 0.0f }, { COH_WIDE_SPRITE, 512.0f, 180.0f, 0.0f },
        { COH_WIDE_TRI, 400.0f, 180.0f, 0.0f },   { COH_WIDE_TRI, 400.0f, 272.0f, 0.0f },
        { COH_WIDE_TRI, 512.0f, 180.0f, 0.0f },   { COH_WIDE_TRI, 512.0f, 180.0f, 0.0f },
        { COH_WIDE_TRI, 400.0f, 272.0f, 0.0f },   { COH_WIDE_TRI, 512.0f, 272.0f, 0.0f },
    };
    memcpy(v, verts, sizeof verts);

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x1E, 0);
    for (uint32_t cmd = 0x1Du; cmd <= 0x28u; cmd++)   /* cull, fog, dither ... logic op: all off */
        if (cmd != 0x1Eu) DL_EMIT(cmd, 0);
    DL_EMIT(0xE8, 0);
    DL_EMIT(0xE9, 0);
    DL_EMIT(0x15, 0);                                    /* the drawing region: the whole target */
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);                                    /* the scissor, to x2 */
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (uint32_t)scis_x2);
    DL_EMIT(0x12, (7u << 2) | (3u << 7) | (1u << 23));   /* 8888 colour, float position, through */
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0xD3, 1u | (3u << 8));                       /* clear mode: colour and alpha */
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0x04, (3u << 16) | 6u);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

/* The pixel (x, y) must hold: its band's colour inside the bound, the sentinel outside. The
 * sprite and triangles keep the sentinel's alpha (the stencil byte), so only RGB is theirs. */
static int coherence_wide_raster_verify(const char *path, const char *name, const uint32_t *fb,
                                        uint32_t stride, int last_col) {
    for (int y = 0; y <= FB_H; y++) {                   /* row FB_H: the one past the target */
        for (int x = 0; x < (int)stride; x++) {
            uint32_t got = fb[(uint32_t)y * stride + (uint32_t)x], want = k_coh_wide_bg, mask = ~0u;
            if (x <= last_col && y < FB_H) {
                if (y < 90) want = COH_WIDE_CLEAR;
                else if (x >= 400) { want = y < 180 ? COH_WIDE_SPRITE : COH_WIDE_TRI; mask = 0x00ffffffu; }
            }
            if ((got & mask) != (want & mask)) {
                fprintf(stderr, "gpu coherence selftest [%s]: %s pixel=(%d,%d) got=%08x want=%08x "
                                "(last column %d)\n", name, path, x, y, got, want, last_col);
                return 0;
            }
        }
    }
    return 1;
}

static int coherence_run_wide_raster_one(const char *name, uint32_t stride, int scis_x2) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const size_t bytes = (size_t)stride * (FB_H + 1u) * 4u;   /* the target and the row past it */
    const int last_col = scis_x2 < (int)stride - 1 ? scis_x2 : (int)stride - 1;
    uint32_t *fb = (uint32_t *)SR_HOST(base);

    coherence_reset_targets();
    for (size_t i = 0; i < bytes / 4u; i++) fb[i] = k_coh_wide_bg;
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    const unsigned long spr_before = s_cnt_spr, tri_before = s_cnt_tri;
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_wide_raster_list(base, stride, scis_x2), 0);
    /* the visible width, never past the stride (a narrower one is refused) */
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = stride < 480u ? stride : 480u, .height = 272 };
    if (s_cnt_spr != spr_before + 2u || s_cnt_tri != tri_before + 2u ||
        gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) {
        fprintf(stderr, "gpu coherence selftest [%s]: the GPU took %lu sprite(s) and %lu triangle(s), "
                        "want 2 and 2, or the sync failed\n", name, s_cnt_spr - spr_before, s_cnt_tri - tri_before);
        coherence_reset_targets();
        return 0;
    }
    int ok = coherence_wide_raster_verify("gpu", name, fb, stride, last_col);
    uint32_t *gpu = (uint32_t *)malloc(bytes);
    if (!gpu) { coherence_reset_targets(); return 0; }
    memcpy(gpu, fb, bytes);
    coherence_reset_targets();

    for (size_t i = 0; i < bytes / 4u; i++) fb[i] = k_coh_wide_bg;
    ge_set_gpu_hooks(NULL);
    ge_run_list(coherence_wide_raster_list(base, stride, scis_x2), 0);
    ge_set_gpu_hooks(&k_hooks);
    ok = coherence_wide_raster_verify("software", name, fb, stride, last_col) && ok;
    size_t mismatch = 0;
    while (mismatch < bytes && ((const uint8_t *)gpu)[mismatch] == ((const uint8_t *)fb)[mismatch])
        mismatch++;
    if (mismatch != bytes) {
        fprintf(stderr, "gpu coherence selftest [%s]: GPU/software mismatch at byte=%zu pixel=(%zu,%zu) "
                        "gpu=%02x software=%02x\n", name, mismatch, (mismatch / 4u) % stride,
                (mismatch / 4u) / stride, ((const uint8_t *)gpu)[mismatch], ((const uint8_t *)fb)[mismatch]);
        ok = 0;
    }
    free(gpu);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scissor_x2=%d scale=%d\n",
               name, stride, scis_x2, s_scale);
    return ok;
}

/* Depth writes stay inside the depth buffer (#2193). The depth surface is zbw columns by 272
 * rows; column x of row y is guest address (y * zbw + x), so a depth write past zbw would be
 * the next row's pixel, or on row 271 one past the buffer. One list on an 8888 target with
 * stride fbw and a depth buffer with stride zbw, depth pre-filled with a sentinel: a depth
 * clear (depth A) of the whole 512x272, a depth-writing sprite (test ALWAYS, depth B) on rows
 * 0-9 at x 256-511, and under a scissor of x 0-99 a depth clear (depth C) of rows 200-271.
 * Every pixel drawn is inside the colour target and the depth buffer, so the depth buffer must
 * hold C in rows 200-271 of columns 0-99, A in the rest of the columns both buffers have, and
 * the sentinel in any column past the colour target's stride and in the row past the buffer.
 * The Vulkan backend and ge.c each run it and must agree byte for byte. A 512-stride target
 * with a 256-wide depth buffer is the case where the sprite lies past zbw; a 256-stride target
 * with a 512-wide depth buffer the case where the draws reach past the colour target. */
static void coherence_reset_depths(void);

enum { COH_WIDE_DEPTH_A = 0x4000u, COH_WIDE_DEPTH_B = 0x0100u, COH_WIDE_DEPTH_C = 0x7777u,
       COH_WIDE_DEPTH_SENT = 0x1234u };

static uint32_t coherence_wide_depth_list(uint32_t base, uint32_t fbw, uint32_t zbase, uint32_t zbw) {
    const uint32_t vaddr = 0x00102000u;
    CohWideVtx *v = (CohWideVtx *)SR_HOST(vaddr);
    const CohWideVtx verts[] = {
        { COH_WIDE_CLEAR, 0.0f, 0.0f, (float)COH_WIDE_DEPTH_A },
        { COH_WIDE_CLEAR, 512.0f, 272.0f, (float)COH_WIDE_DEPTH_A },
        { COH_WIDE_SPRITE, 256.0f, 0.0f, (float)COH_WIDE_DEPTH_B },
        { COH_WIDE_SPRITE, 512.0f, 10.0f, (float)COH_WIDE_DEPTH_B },
        { COH_WIDE_CLEAR, 0.0f, 200.0f, (float)COH_WIDE_DEPTH_C },
        { COH_WIDE_CLEAR, 512.0f, 272.0f, (float)COH_WIDE_DEPTH_C },
    };
    memcpy(v, verts, sizeof verts);

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, fbw | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0x9E, zbase & 0x00FFFFFFu);
    DL_EMIT(0x9F, zbw | ((zbase & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);
    for (uint32_t cmd = 0x1Du; cmd <= 0x28u; cmd++)   /* cull, fog, ... depth test, logic op: all off */
        DL_EMIT(cmd, 0);
    DL_EMIT(0xE8, 0);
    DL_EMIT(0xE9, 0);
    DL_EMIT(0xD6, 0);                                    /* depth range: all of it */
    DL_EMIT(0xD7, 0xFFFFu);
    DL_EMIT(0xE7, 0);                                    /* depth writes on */
    DL_EMIT(0xDE, 1);                                    /* depth test: ALWAYS */
    DL_EMIT(0x15, 0);                                    /* the drawing region: the whole 512x272 */
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0x12, (7u << 2) | (3u << 7) | (1u << 23));   /* 8888 colour, float position, through */
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0xD3, 1u | (4u << 8));                       /* clear mode: depth only */
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 1);                                    /* depth test on */
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0x23, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | 99u);             /* the scissor, to x 99 */
    DL_EMIT(0xD3, 1u | (4u << 8));
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

/* Guest depth pixel `linear` (y * zbw + x), at the address both renderers' depth surfaces use. */
static uint16_t *coherence_wide_depth_px(uint32_t zbase, uint32_t linear) {
    return (uint16_t *)SR_HOST_LINEAR((zbase + linear * 2u) ^ 0x2040u);
}

static void coherence_wide_depth_fill(uint32_t zbase, uint32_t zbw, uint32_t span) {
    const uint16_t sent = COH_WIDE_DEPTH_SENT;
    for (uint32_t i = 0; i < zbw * (FB_H + 1u); i++) memcpy(coherence_wide_depth_px(zbase, i), &sent, 2);
    sr_gpu_vram_dirty(zbase, span);
    ge_zbuf_invalidate_guest_span(zbase, span);
}

static int coherence_wide_depth_verify(const char *path, const char *name, uint32_t zbase,
                                       uint32_t fbw, uint32_t zbw) {
    const uint32_t cols = fbw < zbw ? fbw : zbw;
    for (uint32_t i = 0; i < zbw * (FB_H + 1u); i++) {
        const uint32_t x = i % zbw, y = i / zbw;
        uint16_t want = COH_WIDE_DEPTH_SENT, got;
        if (y < FB_H && x < cols) want = y >= 200u && x < 100u ? COH_WIDE_DEPTH_C : COH_WIDE_DEPTH_A;
        memcpy(&got, coherence_wide_depth_px(zbase, i), 2);
        if (got != want) {
            fprintf(stderr, "gpu coherence selftest [%s]: %s depth pixel=(%u,%u) got=%04x want=%04x "
                            "(stride %u, depth stride %u)\n", name, path, (unsigned)x, (unsigned)y,
                    got, want, (unsigned)fbw, (unsigned)zbw);
            return 0;
        }
    }
    return 1;
}

static int coherence_run_wide_raster_depth(const char *name, uint32_t fbw, uint32_t zbw) {
    const uint32_t base = 0x04000000u | 0x00160000u, zbase = 0x04000000u;
    /* the depth buffer, the row past it, and the 0x2040 swizzle's reach */
    const uint32_t span = zbw * (FB_H + 1u) * 2u + 0x2040u;
    const size_t fb_bytes = (size_t)fbw * FB_H * 4u;

    coherence_reset_depths();
    memset(SR_HOST(base), 0, fb_bytes);
    sr_gpu_vram_dirty(base, (uint32_t)fb_bytes);
    coherence_wide_depth_fill(zbase, zbw, span);
    const unsigned long spr_before = s_cnt_spr;
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_wide_depth_list(base, fbw, zbase, zbw), 0);
    int ok = s_cnt_spr == spr_before + 3u && submit_pending();
    for (int i = 0; ok && i < MAX_DEP; i++)
        if (s_deps[i].used) ok = depth_readback_to_guest(&s_deps[i]);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [%s]: the GPU took %lu sprite(s), want 3, or the depth "
                        "readback failed\n", name, s_cnt_spr - spr_before);
        coherence_reset_depths();
        return 0;
    }
    ok = coherence_wide_depth_verify("gpu", name, zbase, fbw, zbw);
    uint8_t *gpu = (uint8_t *)malloc(span);
    if (!gpu) { coherence_reset_depths(); return 0; }
    memcpy(gpu, SR_HOST_LINEAR(zbase), span);
    coherence_reset_depths();

    memset(SR_HOST(base), 0, fb_bytes);
    sr_gpu_vram_dirty(base, (uint32_t)fb_bytes);
    coherence_wide_depth_fill(zbase, zbw, span);
    ge_set_gpu_hooks(NULL);
    ge_run_list(coherence_wide_depth_list(base, fbw, zbase, zbw), 0);
    const int materialized = ge_zbuf_materialize_current();
    ge_set_gpu_hooks(&k_hooks);
    if (!materialized) {
        fprintf(stderr, "gpu coherence selftest [%s]: ge.c's depth surface was not written back\n", name);
        ok = 0;
    }
    ok = coherence_wide_depth_verify("software", name, zbase, fbw, zbw) && ok;
    const uint8_t *sw = (const uint8_t *)SR_HOST_LINEAR(zbase);
    uint32_t mismatch = 0;
    while (mismatch < span && gpu[mismatch] == sw[mismatch]) mismatch++;
    if (mismatch != span) {
        fprintf(stderr, "gpu coherence selftest [%s]: GPU/software depth mismatch at byte=%u gpu=%02x "
                        "software=%02x\n", name, (unsigned)mismatch, gpu[mismatch], sw[mismatch]);
        ok = 0;
    }
    free(gpu);
    sr_gpu_vram_dirty(base, (uint32_t)fb_bytes);
    sr_gpu_vram_dirty(zbase, span);
    ge_zbuf_invalidate_guest_span(zbase, span);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u depth_stride=%u scale=%d\n",
               name, (unsigned)fbw, (unsigned)zbw, s_scale);
    return ok;
}

static int coherence_run_wide_raster(void) {
    int ok = coherence_run_wide_raster_one("wide-raster-512", 512u, 511);
    ok = coherence_run_wide_raster_one("wide-raster-screen", 512u, 479) && ok;
    ok = coherence_run_wide_raster_one("wide-raster-stride", 500u, 511) && ok;
    ok = coherence_run_wide_raster_one("wide-raster-narrow", 256u, 479) && ok;
    ok = coherence_run_wide_raster_depth("wide-raster-depth", 512u, 256u) && ok;
    ok = coherence_run_wide_raster_depth("wide-raster-depth-narrow", 256u, 512u) && ok;
    return ok;
}

/* ---- linearly filtered sprite: nothing from outside the sampled region may bleed ----
 *
 * The sprite-exact fixtures above run with GE_TEXFILTER=0 (NEAREST), so they pin the
 * endpoint-exclusive rectangle but say nothing about the filter window. This fixture
 * samples texel rows 1..4 of a texture whose row 0 is pure white, and demands that a
 * linearly filtered 1:1 sprite reproduce rows 1..4 exactly: an integer texel coordinate
 * names that texel's centre, so the filter weight is 0 and the outside row cannot
 * contribute. A half-texel window error shows up here as a white 50/50 wash on the
 * sprite's first row and column -- the one-pixel line this fixture exists to catch.
 * Both backends run the same list and the results must also agree byte for byte. */
#define COH_LIN_RECT_X 100
#define COH_LIN_RECT_Y 100
#define COH_LIN_RECT_W 4
#define COH_LIN_RECT_H 4
#define COH_LIN_TEX_U0 1u
#define COH_LIN_TEX_V0 1u
#define COH_LIN_OUTSIDE_ROW 0xFFFFFFFFu   /* pure white: no sampled texel uses 255 */

static uint32_t coherence_linear_texel(unsigned u, unsigned v) {
    if (v == 0u) return COH_LIN_OUTSIDE_ROW;          /* outside the sampled region */
    if (v > 4u) return 0xFF000000u;                    /* below it */
    return 0xFF000000u | (0x40u << 16) | ((v * 40u) << 8) | (u * 40u);
}

/* The default GE alpha-write state leaves the destination alpha byte at zero (see
 * coherence_sprite_texel), so the RGB triple is what this fixture compares. */
#define COH_LIN_RGB 0x00FFFFFFu

static uint32_t coherence_prepare_linear_sprite_fixture(uint32_t tex_base) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (unsigned v = 0; v < 8u; v++)
        for (unsigned u = 0; u < 8u; u++)
            tex[v * 8u + u] = coherence_linear_texel(u, v);
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ (float)COH_LIN_TEX_U0, (float)COH_LIN_TEX_V0,
                             (float)COH_LIN_RECT_X, (float)COH_LIN_RECT_Y, 0.0f };
    vtx[1] = (RawSpriteVtx){ (float)(COH_LIN_TEX_U0 + COH_LIN_RECT_W),
                             (float)(COH_LIN_TEX_V0 + COH_LIN_RECT_H),
                             (float)(COH_LIN_RECT_X + COH_LIN_RECT_W),
                             (float)(COH_LIN_RECT_Y + COH_LIN_RECT_H), 0.0f };

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8 | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3 << 8) | 3);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, 1);   /* GE_TEXFILTER = LINEAR: the filter window is what is under test */
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3 | (1 << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3 << 0) | (3 << 7) | (1 << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6 << 16) | 2u);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_verify_linear_sprite(const char *path, const uint32_t *fb,
                                          uint32_t stride) {
    for (int dy = 0; dy < COH_LIN_RECT_H; dy++) {
        for (int dx = 0; dx < COH_LIN_RECT_W; dx++) {
            uint32_t want = coherence_linear_texel(COH_LIN_TEX_U0 + (unsigned)dx,
                                                   COH_LIN_TEX_V0 + (unsigned)dy) & COH_LIN_RGB;
            uint32_t got = fb[(COH_LIN_RECT_Y + dy) * stride + COH_LIN_RECT_X + dx] & COH_LIN_RGB;
            if (got != want) {
                fprintf(stderr,
                        "gpu coherence selftest [linear-%s]: pixel=(%d,%d) texel=(%u,%u) "
                        "expected=%06x actual=%06x%s\n",
                        path, COH_LIN_RECT_X + dx, COH_LIN_RECT_Y + dy,
                        COH_LIN_TEX_U0 + (unsigned)dx, COH_LIN_TEX_V0 + (unsigned)dy,
                        want, got,
                        (got & COH_LIN_RGB) == (COH_LIN_OUTSIDE_ROW & COH_LIN_RGB)
                            ? " -- the texel row OUTSIDE the sampled region bled in" : "");
                return 0;
            }
        }
    }
    /* The rectangle stays endpoint-exclusive: a filter that reached outward would also
     * paint the neighbouring rows. */
    for (int dx = 0; dx < COH_LIN_RECT_W; dx++) {
        if (fb[(COH_LIN_RECT_Y - 1) * stride + COH_LIN_RECT_X + dx] != 0 ||
            fb[(COH_LIN_RECT_Y + COH_LIN_RECT_H) * stride + COH_LIN_RECT_X + dx] != 0) {
            fprintf(stderr, "gpu coherence selftest [linear-%s]: y edge leaked\n", path);
            return 0;
        }
    }
    return 1;
}

static int coherence_run_linear_sprite(void) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);

    uint32_t list_addr = coherence_prepare_linear_sprite_fixture(tex_base);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);

    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride, .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [linear-gpu]: gegpu_sync_guest_fb failed\n");
        return 0;
    }
    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_linear_sprite("gpu", fb, stride)) return 0;

    size_t bytes = stride * FB_H * sizeof(*fb);
    uint8_t *reference = (uint8_t *)malloc(bytes);
    if (!reference) {
        fprintf(stderr, "gpu coherence selftest [linear-gpu]: no reference buffer\n");
        return 0;
    }
    memcpy(reference, guest, bytes);

    memset(guest, 0, stride * FB_H * 4u);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (!coherence_verify_linear_sprite("software", fb, stride)) { free(reference); return 0; }
    size_t mismatch = 0;
    while (mismatch < bytes && reference[mismatch] == guest[mismatch]) mismatch++;
    if (mismatch != bytes) {
        fprintf(stderr, "gpu coherence selftest [linear-parity]: byte=%zu gpu=%02x software=%02x\n",
                mismatch, reference[mismatch], guest[mismatch]);
        free(reference);
        return 0;
    }
    free(reference);

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "linear-no-region-bleed", stride, s_scale);
    return 1;
}

/* Distinct GE minification/magnification filters must survive the production sampler path.
 * The two cases deliberately render at 2x magnification and 2x minification; the selected
 * filter is NEAREST in each case, so an alternating texture must contain no blended colors. */
#define COH_TFLT_RECT_X 120
#define COH_TFLT_RECT_Y 120

static uint32_t coherence_tflt_texel(unsigned u, unsigned v) {
    (void)v;
    return (u & 1u) ? 0xFFFFFFFFu : 0xFF000000u;
}

static uint32_t coherence_prepare_tflt_fixture(uint32_t tex_base, uint32_t tflt, int primitive_type,
                                               int rect_w, int rect_h,
                                               float u0, float v0, float u_span, float v_span) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (unsigned v = 0; v < 8u; v++)
        for (unsigned u = 0; u < 8u; u++)
            tex[v * 8u + u] = coherence_tflt_texel(u, v);
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ u0, v0, (float)COH_TFLT_RECT_X,
                             (float)COH_TFLT_RECT_Y, 0.0f };
    vtx[1] = (RawSpriteVtx){ u0 + u_span, v0 + v_span,
                             (float)(COH_TFLT_RECT_X + rect_w),
                             (float)(COH_TFLT_RECT_Y + rect_h), 0.0f };
    if (primitive_type == 3) {
        vtx[2] = (RawSpriteVtx){ u0, v0 + v_span,
                                 (float)COH_TFLT_RECT_X,
                                 (float)(COH_TFLT_RECT_Y + rect_h), 0.0f };
    }

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8u | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3u << 8) | 3u);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, tflt);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, ((uint32_t)primitive_type << 16) | (primitive_type == 3 ? 3u : 2u));
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_verify_tflt(const char *path, const uint32_t *fb, uint32_t stride,
                                 int rect_w, int rect_h, int expect_linear) {
    int found_blended = 0;
    for (int y = 0; y < rect_h; y++) {
        for (int x = 0; x < rect_w; x++) {
            uint32_t rgb = fb[(COH_TFLT_RECT_Y + y) * stride + COH_TFLT_RECT_X + x] & 0x00FFFFFFu;
            if (rgb != 0x000000u && rgb != 0xFFFFFFu) found_blended = 1;
            if (!expect_linear && rgb != 0x000000u && rgb != 0xFFFFFFu) {
                fprintf(stderr,
                        "gpu coherence selftest [TFLT-%s]: pixel=(%d,%d) unexpectedly blended: %06x\n",
                        path, COH_TFLT_RECT_X + x, COH_TFLT_RECT_Y + y, rgb);
                return 0;
            }
        }
    }
    if (expect_linear && !found_blended) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: expected linear filtering, found only texels\n",
                path);
        return 0;
    }
    return 1;
}

static int coherence_run_tflt_case(const char *name, int primitive_type, uint32_t tflt,
                                   int rect_w, int rect_h,
                                   float u0, float v0, float u_span, float v_span,
                                   int expect_linear) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    uint32_t list_addr = coherence_prepare_tflt_fixture(tex_base, tflt, primitive_type,
                                                        rect_w, rect_h,
                                                        u0, v0, u_span, v_span);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: GPU readback failed\n", name);
        coherence_reset_targets();
        return 0;
    }
    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_tflt("gpu", fb, stride, rect_w, rect_h, expect_linear)) {
        coherence_reset_targets();
        return 0;
    }
    size_t bytes = stride * FB_H * sizeof(*fb);
    uint8_t *reference = (uint8_t *)malloc(bytes);
    if (!reference) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: no reference buffer\n", name);
        coherence_reset_targets();
        return 0;
    }
    memcpy(reference, guest, bytes);
    memset(guest, 0, stride * FB_H * 4u);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (!coherence_verify_tflt("software", fb, stride, rect_w, rect_h, expect_linear)) {
        free(reference);
        coherence_reset_targets();
        return 0;
    }
    size_t pixel_count = bytes / sizeof(*fb);
    size_t mismatch = pixel_count;
    const uint32_t *gpu = (const uint32_t *)reference;
    for (size_t pixel = 0; pixel < pixel_count; pixel++) {
        int differs = 0;
        if (!expect_linear) {
            differs = gpu[pixel] != fb[pixel];
        } else {
            /* Vulkan filtering and the integer software lerp can round by one U8 step. */
            for (unsigned shift = 0; shift < 32; shift += 8) {
                int gpu_channel = (int)((gpu[pixel] >> shift) & 0xFFu);
                int sw_channel = (int)((fb[pixel] >> shift) & 0xFFu);
                int delta = gpu_channel - sw_channel;
                if (delta < -1 || delta > 1) differs = 1;
            }
        }
        if (differs) { mismatch = pixel; break; }
    }
    if (mismatch != pixel_count) {
        fprintf(stderr,
                "gpu coherence selftest [TFLT-%s]: GPU/software mismatch at byte=%zu pixel=(%zu,%zu) gpu=%08x software=%08x\n",
                name, mismatch * sizeof(*fb), mismatch % stride, mismatch / stride,
                gpu[mismatch], fb[mismatch]);
        free(reference);
        coherence_reset_targets();
        return 0;
    }
    free(reference);
    coherence_reset_targets();
    printf("gpu coherence selftest: PASS TFLT %-22s mode=0x%03x\n", name, tflt);
    return 1;
}

static int coherence_run_tflt_software_line(const char *name, uint32_t tflt,
                                            int rect_w, int rect_h,
                                            float u0, float u_span, int expect_linear) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    uint32_t list_addr = coherence_prepare_tflt_fixture(tex_base, tflt, 1,
                                                        rect_w, rect_h,
                                                        u0, 0.0f, u_span, 0.0f);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    int ok = coherence_verify_tflt("software-line", (const uint32_t *)guest, stride,
                                   rect_w, rect_h, expect_linear);
    ge_set_gpu_hooks(&k_hooks);
    coherence_reset_targets();
    if (!ok) return 0;
    printf("gpu coherence selftest: PASS TFLT %-22s mode=0x%03x\n", name, tflt);
    return 1;
}

static int coherence_run_tflt_filters(void) {
    int ok = 1;
    /* min=linear, mag=nearest; this sprite is magnified, so nearest must win. */
    if (!coherence_run_tflt_case("mag-nearest", 6, 0x001u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 0))
        ok = 0;
    /* min=nearest, mag=linear; this sprite is minified, so nearest must win. */
    if (!coherence_run_tflt_case("min-nearest", 6, 0x100u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 0))
        ok = 0;
    /* The complementary cases prove linear filtering still wins when its field is selected. */
    if (!coherence_run_tflt_case("mag-linear", 6, 0x100u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_case("min-linear", 6, 0x001u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 1))
        ok = 0;
    /* The triangle path uses the per-pixel texture planes rather than sprite steps. */
    if (!coherence_run_tflt_case("triangle-mag-nearest", 3, 0x001u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-min-nearest", 3, 0x100u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-mag-linear", 3, 0x100u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-min-linear", 3, 0x001u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 1))
        ok = 0;
    /* Lines are software-rasterized for this fixture; exercise their DDA footprint too. */
    if (!coherence_run_tflt_software_line("line-mag-nearest", 0x001u, 16, 2,
                                          0.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-min-nearest", 0x100u, 4, 2,
                                          0.25f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-mag-linear", 0x100u, 16, 2,
                                          0.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-min-linear", 0x001u, 4, 2,
                                          0.25f, 8.0f, 1))
        ok = 0;
    return ok;
}

/* ---- texture mip levels (#1373) ----------------------------------------------------------
 *
 * The Vulkan backend samples TEXMODE/TEXLEVEL mip levels from one Vulkan mip chain per cached
 * texture and must choose what ge.c tex_detail16 and ge_tex_level_pick choose. Each case draws one through-mode sprite
 * with the Vulkan backend and then with the software GE, and compares the framebuffers:
 * exactly for nearest-level cases, and within the one-U8-step tolerance of the TFLT linear
 * cases for a blend of two levels. Every texel names its level and position, so a wrong
 * level, a level decoded without its own buffer width, or a level coordinate one texel off
 * changes a pixel. The UV start of 0.25 texel keeps every sample off a texel edge at every
 * level, and the footprints (1/2, 2 and 4 texels per pixel) give exact levels of detail.
 * Software/Vulkan agreement is not a PSP measurement; the level rules both paths share are
 * PSP-GE-MIP-001's (hle_thread_selftest.c test_ge_mip_cells_match_psp checks ge.c against
 * the PSP). The automatic and slope cases draw on the GPU, which picks their level per
 * fragment in psp.frag (F_LODFRAG); each case also checks that only a level list a halving
 * chain cannot hold takes the software rasterizer. */
#define COH_MIP_TEX   0x04040000u          /* level L at COH_MIP_TEX + L * 0x1000 */
#define COH_MIP_ALT1  0x04045000u          /* a second level 1 (4x4) for the cache-key case */
#define COH_MIP_LIST  0x00100800u
#define COH_MIP_RESET 0x00100C00u
#define COH_MIP_VERTS 0x00101800u
#define COH_MIP_X     120
#define COH_MIP_Y     120

static uint32_t coherence_mip_texel(unsigned level, unsigned u, unsigned v) {
    uint32_t r = level * 64u + u * 8u + v, g = 255u - r, b = 40u * level + 30u;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* The second level 1 differs from the first in every colour bit. */
static uint32_t coherence_mip_alt_texel(unsigned u, unsigned v) {
    return coherence_mip_texel(1u, u, v) ^ 0x00FFFFFFu;
}

/* Levels 0-3 of an 8x8 8888 texture. Level 1 keeps its 4 texels per row in an 8-wide buffer
 * with magenta beyond them, which only a decode that ignores the level's own buffer width
 * reaches; the second level 1 is a plain 4x4. Levels 2 and 3 use buffer width 4: a linear
 * texture's rows are whole 16-byte lines (ge_tex_row_bytes). */
static unsigned coherence_mip_bufw(unsigned level) {
    return level <= 1u ? 8u : 4u;
}

static void coherence_mip_fill(void) {
    for (unsigned level = 0; level < 4u; level++) {
        unsigned size = 8u >> level, bufw = coherence_mip_bufw(level);
        uint32_t addr = COH_MIP_TEX + level * 0x1000u;
        uint32_t *tex = (uint32_t *)SR_HOST(addr);
        for (unsigned v = 0; v < size; v++)
            for (unsigned u = 0; u < bufw; u++)
                tex[v * bufw + u] = u < size ? coherence_mip_texel(level, u, v) : 0xFFFF00FFu;
        sr_gpu_vram_dirty(addr, size * bufw * 4u);
    }
    uint32_t *alt = (uint32_t *)SR_HOST(COH_MIP_ALT1);
    for (unsigned v = 0; v < 4u; v++)
        for (unsigned u = 0; u < 4u; u++) alt[v * 4u + u] = coherence_mip_alt_texel(u, v);
    sr_gpu_vram_dirty(COH_MIP_ALT1, 4u * 4u * 4u);
}

typedef struct CohMipCase {
    const char *name;
    uint32_t tflt;              /* TEXFILTER (magnification field in bits 8+) */
    uint32_t max_level;         /* TEXMODE highest level */
    uint32_t mode, bias16;      /* TEXLEVEL mode, and its bias in 1/16 (8-bit two's complement) */
    int rect;                   /* sprite side in pixels; the UV span is 8 texels */
    int tolerance;              /* per-channel U8 tolerance: 1 for a blend of two levels */
    int level;                  /* level the first pixel samples, or -1 for a blend */
    int alt_level1;             /* level 1 is the second (plain 4x4) level 1 */
    uint32_t level1_log2;       /* level 1 TEXSIZE log2 on both axes; 0 = 2, the halving chain */
    float slope;                /* TEXLODSLOPE */
    int falls_back;             /* the draw takes the software rasterizer */
} CohMipCase;

/* The GE list of one case: framebuffer 0x04160000, levels 0-3, then one sprite when `draw`
 * is set (otherwise the state alone, for direct hook draws). */
static uint32_t coherence_mip_list(const CohMipCase *c, int draw) {
    if (draw) {
        RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_MIP_VERTS);
        vtx[0] = (RawSpriteVtx){ 0.25f, 0.25f, (float)COH_MIP_X, (float)COH_MIP_Y, 0.0f };
        vtx[1] = (RawSpriteVtx){ 8.25f, 8.25f, (float)(COH_MIP_X + c->rect),
                                 (float)(COH_MIP_Y + c->rect), 0.0f };
    }
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_LIST);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0x9E, 0x00080000u);                         /* depth buffer clear of the levels */
    DL_EMIT(0x9F, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    for (uint32_t level = 0; level < 4u; level++) {
        uint32_t addr = COH_MIP_TEX + level * 0x1000u, bufw = coherence_mip_bufw(level);
        uint32_t log2 = 3u - level;
        if (level == 1u) {
            addr = c->alt_level1 ? COH_MIP_ALT1 : addr;
            bufw = c->alt_level1 ? 4u : 8u;
            log2 = c->level1_log2 ? c->level1_log2 : 2u;
        }
        DL_EMIT(0xA0 + level, addr & 0x00FFFFFFu);                        /* TEXADDRn */
        DL_EMIT(0xA8 + level, bufw | ((addr & 0xFF000000u) >> 8));         /* TEXBUFWIDTHn */
        DL_EMIT(0xB8 + level, (log2 << 8) | log2);                         /* TEXSIZEn */
    }
    DL_EMIT(0xC2, c->max_level << 16);                  /* TEXMODE: highest level, unswizzled */
    DL_EMIT(0xC8, c->mode | ((c->bias16 & 0xFFu) << 16));                  /* TEXLEVEL */
    { uint32_t slope_bits; memcpy(&slope_bits, &c->slope, 4);
      DL_EMIT(0xD0, slope_bits >> 8); }                                     /* TEXLODSLOPE */
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, c->tflt);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));                      /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));  /* float UV and position, through */
    if (draw) {
        DL_EMIT(0x01, COH_MIP_VERTS & 0x00FFFFFFu);
        DL_EMIT(0x04, (6u << 16) | 2u);                 /* one sprite */
    }
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return COH_MIP_LIST;
}

/* Texture sample points measured on a PSP-3000 (PSP-GE-SAMPLE-001, issue #282). A 32x32 8888
 * texture shows texel (u, v) as R = 8u, G = 8v, so a pixel's R and G name its texel and, under
 * bilinear filtering, its blend. Four through-mode cells from the record, each 8x8 pixels:
 *   A  triangle pair, bilinear, 1:1 at integer UV 8..16: every pixel is exactly its texel;
 *   B  the same with nearest filtering;
 *   C  triangle pair, nearest, UV 8.5..16.5: texel 9 + dx, 9 + dy;
 *   D  sprite, bilinear, 2x magnified (UV 8..12): the first pixel blends texel 7 at 4/16
 *      and texel 8 at 12/16 on each axis, the second texel 8 at 12/16 and texel 9 at 4/16.
 * Before the rule was measured, a 1:1 triangle blended half of the next texel under bilinear
 * filtering (a glyph's last row picked up the next glyph's first) and showed the next texel
 * under nearest. Both the Vulkan path and the software rasterizer are checked. */
#define COH_SAMPLE_TEX   0x04048000u
#define COH_SAMPLE_LIST  0x00102000u
#define COH_SAMPLE_VERTS 0x00102800u
static int coherence_sample_expect(int cell, int dx, int dy, uint32_t *want) {
    int r, g;
    if (cell == 0 || cell == 1) { r = 8 * (8 + dx); g = 8 * (8 + dy); }
    else if (cell == 2) { r = 8 * (9 + dx); g = 8 * (9 + dy); }
    else {
        static const int first_two[2] = { (56 * 4 + 64 * 12) / 16, (64 * 12 + 72 * 4) / 16 };
        if (dx > 1 || dy > 1) return 0;
        r = first_two[dx]; g = first_two[dy];
    }
    *want = 0xFF000000u | ((uint32_t)g << 8) | (uint32_t)r;
    return 1;
}

static int coherence_run_psp_sample_points(void) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(COH_SAMPLE_TEX);
    for (unsigned v = 0; v < 32u; v++)
        for (unsigned u = 0; u < 32u; u++) tex[v * 32u + u] = 0xFF000000u | ((8u * v) << 8) | (8u * u);
    sr_gpu_vram_dirty(COH_SAMPLE_TEX, 32u * 32u * 4u);

    static const struct { int x, type; float u0, u1; uint32_t tflt; } cells[4] = {
        { 100, 3, 8.0f, 16.0f, 0x101u }, { 110, 3, 8.0f, 16.0f, 0x000u },
        { 120, 3, 8.5f, 16.5f, 0x000u }, { 130, 6, 8.0f, 12.0f, 0x101u },
    };
    const int y0 = 10;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_SAMPLE_VERTS);
    uint32_t *dl = (uint32_t *)SR_HOST(COH_SAMPLE_LIST);
    int p = 0, nv = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, COH_SAMPLE_TEX & 0x00FFFFFFu);
    DL_EMIT(0xA8, 32u | ((COH_SAMPLE_TEX & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (5u << 8) | 5u);
    DL_EMIT(0xC2, 0);
    DL_EMIT(0xC8, 0);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC7, (1u << 8) | 1u);                         /* clamp u and v */
    DL_EMIT(0xC9, 3u | (1u << 8));                         /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));     /* float UV and position, through */
    for (int c = 0; c < 4; c++) {
        float x0 = (float)cells[c].x, x1 = x0 + 8.0f, ya = (float)y0, yb = ya + 8.0f;
        float u0 = cells[c].u0, u1 = cells[c].u1;
        uint32_t first = COH_SAMPLE_VERTS + (uint32_t)nv * (uint32_t)sizeof(RawSpriteVtx);
        DL_EMIT(0xC6, cells[c].tflt);
        DL_EMIT(0xCB, 0);                                  /* TEXFLUSH */
        if (cells[c].type == 3) {                          /* TL TR BL, BL TR BR */
            vtx[nv++] = (RawSpriteVtx){ u0, u0, x0, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u0, x1, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u0, u1, x0, yb, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u0, u1, x0, yb, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u0, x1, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u1, x1, yb, 0.0f };
            DL_EMIT(0x01, first & 0x00FFFFFFu);
            DL_EMIT(0x04, (3u << 16) | 6u);
        } else {
            vtx[nv++] = (RawSpriteVtx){ u0, u0, x0, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u1, x1, yb, 0.0f };
            DL_EMIT(0x01, first & 0x00FFFFFFu);
            DL_EMIT(0x04, (6u << 16) | 2u);
        }
    }
    DL_EMIT(0xC6, 0);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT

    uint32_t *gpu = (uint32_t *)malloc(bytes);
    if (!gpu) return 0;
    coherence_reset_targets();
    memset(fb, 0, bytes);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(COH_SAMPLE_LIST, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    int ok = gegpu_sync_guest_fb(&desc);
    if (!ok) fprintf(stderr, "gpu coherence selftest [psp-sample-points]: GPU readback failed\n");
    if (ok) {
        memcpy(gpu, fb, bytes);
        memset(fb, 0, bytes);
        ge_set_gpu_hooks(NULL);
        ge_run_list(COH_SAMPLE_LIST, 0);
        ge_set_gpu_hooks(&k_hooks);
    }
    static const char *const names[4] = { "tri-linear-1to1", "tri-nearest-1to1",
                                          "tri-nearest-half", "spr-linear-mag2" };
    for (int c = 0; ok && c < 4; c++)
        for (int dy = 0; ok && dy < 8; dy++)
            for (int dx = 0; ok && dx < 8; dx++) {
                uint32_t want;
                if (!coherence_sample_expect(c, dx, dy, &want)) continue;
                size_t at = (size_t)(y0 + dy) * stride + (size_t)(cells[c].x + dx);
                uint32_t sw = fb[at] & 0x00FFFFFFu, gp = gpu[at] & 0x00FFFFFFu;
                want &= 0x00FFFFFFu;
                if (sw != want || gp != want) {
                    fprintf(stderr, "gpu coherence selftest [psp-sample-points/%s]: pixel=(%d,%d) "
                            "software=%06x gpu=%06x, want %06x (PSP-GE-SAMPLE-001)\n",
                            names[c], dx, dy, sw, gp, want);
                    ok = 0;
                }
            }
    free(gpu);
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS %-22s 4 cells (PSP-GE-SAMPLE-001)\n", "psp-sample-points");
    return ok;
}

/* Textured sprites at render scale 1 to 4, over a 16x16 gradient texture (clamped).
 * - A nearest-filtered sprite at one texel per pixel, either direction, shows the PSP's texel
 *   for each guest pixel (PSP-GE-SAMPLE-001: floor of the coordinate at the pixel centre) in
 *   every device pixel of that pixel, whatever its origin: on a texel edge, half a texel off
 *   (+0.5, PSP-GE-SAMPLE-001's "+0.5-texel" mapping) or a quarter off. Guest VRAM gets that
 *   texel at every scale.
 * - Every other sprite samples at each device pixel's own centre, as a triangle does, except
 *   that a sample in an edge pixel's outer half takes that pixel's centre (uv-edge-clamp): a
 *   minified nearest one, and linear ones (exact within 2 of the filtered value). Guest VRAM
 *   gets the top-left device sample.
 * Before this case hook_sprite shifted every sprite's coordinate by 0.5 - 0.5 / scale of a
 * texel for every device sample. That put a texel boundary inside each block of a 1:1 sprite
 * (HST's button text at 4x), and without it a half-texel nearest origin read one texel low.
 * At scale 1 every cell is the PSP's own sampling. gpu-coherence-selftest also runs this case
 * alone at render scales 2, 3 and 4. */
static int coherence_read_device(VkImage img, VkImageAspectFlags aspect, VkImageLayout *layout,
                                 VkImageLayout after, uint32_t bpp, void *out);

typedef struct { float a0, w; } SprAxis;      /* coordinate at the first pixel edge, per pixel */

/* The 0-255 channel a device sample at device index d of a sprite axis of n pixels shows. Above
 * 1x a sample in an edge pixel's outer half takes that pixel's centre coordinate (the edge clamp,
 * uv-edge-clamp); at 1x every sample is a centre. */
static float coherence_spr_expect(SprAxis ax, int nearest, uint32_t d, uint32_t s, uint32_t n) {
    float at = ((float)d + 0.5f) / (float)s;
    if (s > 1u) at = fminf(fmaxf(at, 0.5f), (float)n - 0.5f);
    const float pos = ax.a0 + at * ax.w;
    if (!nearest) return 16.0f * (pos - 0.5f);
    if (fabsf(fabsf(ax.w) - 1.0f) < 1e-6f) {                   /* 1:1: the PSP's texel */
        const uint32_t g = d / s;
        return 16.0f * floorf(ax.a0 + ((float)g + 0.5f) * ax.w);
    }
    return 16.0f * floorf(pos);
}

typedef struct { int x, n; uint32_t tflt; float u0, u1, v0, v1; const char *name; } SprCell;

/* Every sample stays inside texels 0..15, so clamping never decides a value. */
static const SprCell k_spr_cells[] = {
    { 100, 12, 0x000u, 2.0f, 14.0f, 2.0f, 14.0f, "nearest-1to1" },
    { 120, 12, 0x000u, 2.5f, 14.5f, 2.5f, 14.5f, "nearest-half-origin" },
    { 140, 12, 0x000u, 2.25f, 14.25f, 2.75f, 14.75f, "nearest-quarter-origin" },
    { 160, 12, 0x000u, 2.0f, 14.0f, 14.5f, 2.5f, "nearest-flipped-v" },
    { 180, 6, 0x000u, 2.25f, 14.25f, 2.25f, 14.25f, "nearest-minified" },
    { 200, 12, 0x101u, 2.0f, 14.0f, 2.0f, 14.0f, "linear-1to1" },
    { 220, 12, 0x101u, 2.5f, 14.5f, 2.5f, 14.5f, "linear-half-origin" },
    { 240, 12, 0x101u, 2.0f, 14.0f, 14.5f, 2.5f, "linear-flipped-v" },
};
#define SPR_NEAREST_1TO1 4   /* the first four cells: nearest, one texel per pixel */

/* The 16x16 gradient texture and a display list drawing `cells` as through-mode sprites at row y0 of
 * the 8888 target at fba (stride 512), through ge.c's interpreter and the production hooks. */
static void coherence_sprite_list(const SprCell *cells, int ncells, int y0, uint32_t fba, uint32_t stride);
/* sprite-scaled's checks of the device image `dev` (at the render scale; freed here) and guest VRAM. */
static int coherence_sprite_check(const SprCell *cells, int ncells, int y0, const uint32_t *fb, uint32_t stride,
                                  uint32_t *dev, int ok);

static int coherence_run_sprite_scaled(void) {
    const uint32_t fba = 0x00160000u, base = 0x04000000u | fba, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    const SprCell *cells = k_spr_cells;
    const int ncells = (int)(sizeof(k_spr_cells) / sizeof(k_spr_cells[0])), y0 = 10;
    coherence_sprite_list(cells, ncells, y0, fba, stride);

    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev) return 0;
    coherence_reset_targets();
    memset(fb, 0, bytes);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(COH_SAMPLE_LIST, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    int ok = gegpu_sync_guest_fb(&desc);
    Target *t = ok ? target_find_by_fba(fba) : NULL;
    ok = t != NULL && coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev);
    return coherence_sprite_check(cells, ncells, y0, fb, stride, dev, ok);
}

static void coherence_sprite_list(const SprCell *cells, int ncells, int y0, uint32_t fba, uint32_t stride) {
    uint32_t *tex = (uint32_t *)SR_HOST(COH_SAMPLE_TEX);
    for (unsigned v = 0; v < 16u; v++)
        for (unsigned u = 0; u < 16u; u++) tex[v * 16u + u] = 0xFF000000u | ((16u * v) << 8) | (16u * u);
    sr_gpu_vram_dirty(COH_SAMPLE_TEX, 16u * 16u * 4u);
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_SAMPLE_VERTS);
    uint32_t *dl = (uint32_t *)SR_HOST(COH_SAMPLE_LIST);
    int p = 0, nv = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, fba);
    DL_EMIT(0x9D, stride | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, COH_SAMPLE_TEX & 0x00FFFFFFu);
    DL_EMIT(0xA8, 16u | ((COH_SAMPLE_TEX & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (4u << 8) | 4u);
    DL_EMIT(0xC2, 0);
    DL_EMIT(0xC8, 0);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC7, (1u << 8) | 1u);                         /* clamp u and v */
    DL_EMIT(0xC9, 3u | (1u << 8));                         /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));     /* float UV and position, through */
    for (int c = 0; c < ncells; c++) {
        uint32_t first = COH_SAMPLE_VERTS + (uint32_t)nv * (uint32_t)sizeof(RawSpriteVtx);
        DL_EMIT(0xC6, cells[c].tflt);
        DL_EMIT(0xCB, 0);                                  /* TEXFLUSH */
        vtx[nv++] = (RawSpriteVtx){ cells[c].u0, cells[c].v0, (float)cells[c].x, (float)y0, 0.0f };
        vtx[nv++] = (RawSpriteVtx){ cells[c].u1, cells[c].v1, (float)(cells[c].x + cells[c].n),
                                    (float)(y0 + cells[c].n), 0.0f };
        DL_EMIT(0x01, first & 0x00FFFFFFu);
        DL_EMIT(0x04, (6u << 16) | 2u);
    }
    DL_EMIT(0xC6, 0);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
}

static int coherence_sprite_check(const SprCell *cells, int ncells, int y0, const uint32_t *fb, uint32_t stride,
                                  uint32_t *dev, int ok) {
    const uint32_t s = (uint32_t)s_scale;
    if (!ok) fprintf(stderr, "gpu coherence selftest [sprite-scaled]: draw or device read failed\n");
    int bad = 0;
    for (int c = 0; ok && c < ncells; c++) {
        const int nearest = cells[c].tflt == 0u;
        const float n = (float)cells[c].n;
        const SprAxis ax = { cells[c].u0, (cells[c].u1 - cells[c].u0) / n };
        const SprAxis ay = { cells[c].v0, (cells[c].v1 - cells[c].v0) / n };
        const int exact_readback = nearest && fabsf(fabsf(ax.w) - 1.0f) < 1e-6f &&
                                   fabsf(fabsf(ay.w) - 1.0f) < 1e-6f;
        /* Linear: the filter's weight and rounding are within 2 of the exact value; the old
         * shift moved it by 16 * (0.5 - 0.5 / scale), 4 at scale 2 and 6 at 4. */
        const float tol = nearest ? 0.0f : 2.0f;
        for (uint32_t dy = 0; dy < (uint32_t)cells[c].n * s; dy++) {
            for (uint32_t dx = 0; dx < (uint32_t)cells[c].n * s; dx++) {
                const float wr = coherence_spr_expect(ax, nearest, dx, s, (uint32_t)cells[c].n);
                const float wg = coherence_spr_expect(ay, nearest, dy, s, (uint32_t)cells[c].n);
                const uint32_t X = (uint32_t)cells[c].x * s + dx, Y = (uint32_t)y0 * s + dy;
                const uint32_t got = dev[(size_t)Y * SCL_W + X];
                const float er = fabsf((float)(got & 0xFFu) - wr);
                const float eg = fabsf((float)((got >> 8) & 0xFFu) - wg);
                if ((er > tol || eg > tol) && bad++ < 8)
                    fprintf(stderr, "gpu coherence selftest [sprite-scaled/%s]: scale=%u device pixel "
                            "(%u,%u) r=%u g=%u, want %.2f %.2f\n", cells[c].name, s, dx, dy,
                            got & 0xFFu, (got >> 8) & 0xFFu, wr, wg);
            }
        }
        /* Guest VRAM: the PSP's texel where the device image holds it in every device pixel,
         * otherwise the top-left device sample of each pixel. At scale 1 both are the PSP's. */
        for (uint32_t gy = 0; gy < (uint32_t)cells[c].n; gy++) {
            for (uint32_t gx = 0; gx < (uint32_t)cells[c].n; gx++) {
                const uint32_t got = fb[(size_t)(y0 + gy) * stride + (size_t)cells[c].x + gx] & 0x00FFFFFFu;
                uint32_t want;
                if (exact_readback) {
                    const uint32_t r = (uint32_t)floorf(ax.a0 + ((float)gx + 0.5f) * ax.w);
                    const uint32_t g = (uint32_t)floorf(ay.a0 + ((float)gy + 0.5f) * ay.w);
                    want = ((16u * g) << 8) | (16u * r);
                } else {
                    const uint32_t X = ((uint32_t)cells[c].x + gx) * s, Y = ((uint32_t)y0 + gy) * s;
                    want = dev[(size_t)Y * SCL_W + X] & 0x00FFFFFFu;
                }
                if (got != want && bad++ < 16)
                    fprintf(stderr, "gpu coherence selftest [sprite-scaled/%s/guest]: scale=%u pixel "
                            "(%u,%u) = %06x, want %06x\n", cells[c].name, s, gx, gy, got, want);
            }
        }
    }
    if (bad) {
        fprintf(stderr, "gpu coherence selftest [sprite-scaled]: %d pixel(s) differ at scale %u\n", bad, s);
        ok = 0;
    }
    free(dev);
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS %-22s %d cells scale=%u\n", "sprite-scaled", ncells, s);
    return ok;
}

/* uv-edge-clamp: through-mode rectangles whose texture coordinate reaches the texture's edge at
 * the rectangle's edge, at 16 texels over 12 pixels, as LittleBigPlanet's loading screen draws its
 * planet (a 128x64 texture over 220x52 pixels, flipped, linear, repeat). The texture's red channel
 * follows its row and green its column: 0 in texel 0, 255 in texel 15, 128 between. At 1x each edge
 * pixel's centre samples inside the edge texel (the PSP's own picture). Above 1x the outer device
 * samples of an edge pixel lie past that centre, and with linear filtering and repeat wrapping they
 * blended in the texture's opposite edge, a line along the rectangle's edge. Every device sample of
 * every cell must show the model below: the coordinate at that device sample's position, clamped
 * above 1x to the coordinates the 1x pixel centres sample, filtered the way psp.frag filters (weights
 * in 1/16), within 2.
 * - strips (two triangles each) and a sprite, flipped and not, linear with repeat and clamp
 *   wrapping, and nearest with repeat;
 * - a strip whose u also changes with y (as a rotated rectangle's does) keeps every device sample
 *   where it falls: it gets no clamp;
 * - strips with fractional edges (n + 0.5, a pixel centre on the edge, and n + 0.25 and n + 0.75),
 *   flipped in v or u, linear and nearest: the range follows the PSP's top-left rule, which covers
 *   a centre on the left or top edge and not one on the right or bottom edge, so a rule that
 *   breaks the tie the other way moves the range by a pixel and fails. These are checked over a
 *   ring one guest pixel wide around them too: device pixels the strip does not cover at this
 *   scale must keep the cleared target.
 * At 1x no primitive carries a clamp range at all (checked on the vertices hook_sprite writes), so
 * the 1x picture is exactly the one before the clamp existed. */
typedef struct { int sprite; uint32_t tflt, wrap; float ul, ur, vt, vb, shear; const char *name; } EdgeCell;

static float coh_edge_texel(int i, int clampwrap) {
    if (clampwrap) i = i < 0 ? 0 : i > 15 ? 15 : i;
    else i &= 15;
    return i == 0 ? 0.0f : i == 15 ? 255.0f : (float)(16 * i);   /* a ramp between the dark and bright edges */
}

/* One channel at coordinate c (texels), as psp.frag samples it: rounded to 1/4096, then nearest
 * takes texel floor(c) and linear blends floor(c - 0.5) and the next texel by a weight truncated
 * to 1/16. */
static float coh_edge_sample(double c, int nearest, int clampwrap) {
    c = floor(c * 4096.0 + 0.5) / 4096.0;
    if (nearest) return coh_edge_texel((int)floor(c), clampwrap);
    const double f = c - 0.5, i0 = floor(f), w = floor((f - i0) * 16.0) / 16.0;
    return (float)((1.0 - w) * coh_edge_texel((int)i0, clampwrap) + w * coh_edge_texel((int)i0 + 1, clampwrap));
}

/* The coordinate along one axis of n pixels from c0 at its first edge to c1 at its last, at
 * device index d of render scale s; above 1x clamped to the first and last pixel centres'. */
static double coh_edge_coord(double c0, double c1, uint32_t n, uint32_t d, uint32_t s, int clamp_it) {
    const double per = (c1 - c0) / (double)n;
    const double at = ((double)d + 0.5) / (double)s;
    double c = c0 + at * per;
    if (clamp_it && s > 1u) {
        const double a = (double)(float)(c0 + 0.5 * per), b = (double)(float)(c1 - 0.5 * per);
        const double lo = a < b ? a : b, hi = a < b ? b : a;
        c = c < lo ? lo : c > hi ? hi : c;
    }
    return c;
}

typedef struct { float x0, x1, y0, y1; uint32_t tflt; float ul, ur, vt, vb; const char *name; } FracCell;

/* The coordinate at guest position q on an axis from e0 (coordinate c0) to e1 (c1), above 1x
 * clamped to the coordinates of the first and last pixel centres k + 0.5 the PSP covers there:
 * e0 <= k + 0.5 < e1, a centre on the first edge covered and one on the last not. The centres
 * are counted one by one here, independently of the renderer's closed form. */
static double coh_frac_coord(double e0, double e1, double c0, double c1, double q, uint32_t s) {
    const double slope = (c1 - c0) / (e1 - e0);
    double c = c0 + (q - e0) * slope;
    if (s > 1u) {
        double first = 0.0, last = -1.0;
        int any = 0;
        for (int k = (int)floor(e0) - 1; k <= (int)ceil(e1) + 1; k++) {
            const double ck = (double)k + 0.5;
            if (ck >= e0 && ck < e1) {
                if (!any) first = ck;
                last = ck;
                any = 1;
            }
        }
        if (any) {
            const double a = (double)(float)(c0 + (first - e0) * slope);
            const double b = (double)(float)(c0 + (last - e0) * slope);
            const double lo = a < b ? a : b, hi = a < b ? b : a;
            c = c < lo ? lo : c > hi ? hi : c;
        }
    }
    return c;
}

static int coherence_run_uv_edge_clamp(void) {
    const char *name = "uv-edge-clamp";
    const uint32_t fba = 0x00160000u, base = 0x04000000u | fba, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(COH_SAMPLE_TEX);
    for (unsigned v = 0; v < 16u; v++)
        for (unsigned u = 0; u < 16u; u++)
            tex[v * 16u + u] = 0xFF000000u | ((uint32_t)coh_edge_texel((int)u, 0) << 8) |
                               (uint32_t)coh_edge_texel((int)v, 0);
    sr_gpu_vram_dirty(COH_SAMPLE_TEX, 16u * 16u * 4u);

    static const EdgeCell cells[] = {
        { 0, 0x101u, 0x000u, 0.0f, 16.0f, 16.0f, 0.0f, 0.0f, "strip-linear-repeat-flipped-v" },
        { 0, 0x101u, 0x000u, 16.0f, 0.0f, 0.0f, 16.0f, 0.0f, "strip-linear-repeat-flipped-u" },
        { 0, 0x101u, 0x101u, 0.0f, 16.0f, 16.0f, 0.0f, 0.0f, "strip-linear-clamp" },
        { 0, 0x000u, 0x000u, 0.0f, 16.0f, 16.0f, 0.0f, 0.0f, "strip-nearest-repeat" },
        { 1, 0x101u, 0x000u, 0.0f, 16.0f, 16.0f, 0.0f, 0.0f, "sprite-linear-repeat" },
        { 0, 0x101u, 0x000u, 0.0f, 16.0f, 16.0f, 0.0f, 3.0f, "strip-sheared-u-free" },
        /* Linear minification and nearest magnification, so the filter shows the footprint. 64
         * texels over 12 pixels in u stay minified at every scale up to 4; 32 over 12 is minified
         * at 1x and 2x (2.67 and 1.33 texels per device pixel) and magnified at 3x and 4x. The
         * clamped bands must filter as the rest of the primitive does: at 2x a half-clamped quad
         * whose footprint came from the clamped coordinate would drop to 0.67 and magnify. */
        { 0, 0x001u, 0x000u, 0.0f, 64.0f, 16.0f, 0.0f, 0.0f, "strip-minified-u" },
        { 0, 0x001u, 0x000u, 0.0f, 32.0f, 16.0f, 0.0f, 0.0f, "strip-minified-u-to-2x" },
    };
    const int ncells = (int)(sizeof(cells) / sizeof(cells[0]));
    /* Strips with fractional edges, 16 texels over 12 pixels, linear unless named nearest. An edge
     * on n + 0.5 puts a pixel centre exactly on it (the top-left rule's tie); n + 0.25 and n + 0.75
     * put the first and last covered centres a quarter and three quarters of a pixel inside. */
    static const FracCell fcells[] = {
        { 20.5f, 32.5f, 70.5f, 82.5f, 0x101u, 0.0f, 16.0f, 16.0f, 0.0f, "strip-ties-flipped-v" },
        { 40.25f, 52.25f, 70.75f, 82.75f, 0x101u, 0.0f, 16.0f, 16.0f, 0.0f, "strip-quarter-edges" },
        { 60.75f, 72.75f, 70.25f, 82.25f, 0x101u, 0.0f, 16.0f, 16.0f, 0.0f, "strip-three-quarter-edges" },
        { 80.5f, 92.5f, 70.5f, 82.5f, 0x000u, 0.0f, 16.0f, 16.0f, 0.0f, "strip-ties-nearest" },
        { 100.5f, 112.5f, 70.5f, 82.5f, 0x101u, 16.0f, 0.0f, 0.0f, 16.0f, "strip-ties-flipped-u" },
    };
    const int nfrac = (int)(sizeof(fcells) / sizeof(fcells[0]));
    const int y0 = 40, n = 12;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_SAMPLE_VERTS);
    uint32_t *dl = (uint32_t *)SR_HOST(COH_SAMPLE_LIST);
    int p = 0, nv = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, fba);
    DL_EMIT(0x9D, stride | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, COH_SAMPLE_TEX & 0x00FFFFFFu);
    DL_EMIT(0xA8, 16u | ((COH_SAMPLE_TEX & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (4u << 8) | 4u);
    DL_EMIT(0xC2, 0);
    DL_EMIT(0xC8, 0);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC9, 3u | (1u << 8));                         /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));     /* float UV and position, through */
    for (int c = 0; c < ncells; c++) {
        const EdgeCell *e = &cells[c];
        const float x0 = (float)(20 + 20 * c), x1 = x0 + (float)n, ya = (float)y0, yb = ya + (float)n;
        uint32_t first = COH_SAMPLE_VERTS + (uint32_t)nv * (uint32_t)sizeof(RawSpriteVtx);
        DL_EMIT(0xC6, e->tflt);
        DL_EMIT(0xC7, e->wrap);
        DL_EMIT(0xCB, 0);                                  /* TEXFLUSH */
        if (e->sprite) {
            vtx[nv++] = (RawSpriteVtx){ e->ul, e->vt, x0, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ e->ur, e->vb, x1, yb, 0.0f };
            DL_EMIT(0x01, first & 0x00FFFFFFu);
            DL_EMIT(0x04, (6u << 16) | 2u);
        } else {   /* bottom-left, bottom-right, top-left, top-right, as LittleBigPlanet orders them */
            vtx[nv++] = (RawSpriteVtx){ e->ul + e->shear, e->vb, x0, yb, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ e->ur + e->shear, e->vb, x1, yb, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ e->ul, e->vt, x0, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ e->ur, e->vt, x1, ya, 0.0f };
            DL_EMIT(0x01, first & 0x00FFFFFFu);
            DL_EMIT(0x04, (4u << 16) | 4u);
        }
    }
    DL_EMIT(0xC7, 0);   /* the fractional cells: repeat wrapping */
    for (int c = 0; c < nfrac; c++) {
        const FracCell *f = &fcells[c];
        uint32_t first = COH_SAMPLE_VERTS + (uint32_t)nv * (uint32_t)sizeof(RawSpriteVtx);
        DL_EMIT(0xC6, f->tflt);
        DL_EMIT(0xCB, 0);
        vtx[nv++] = (RawSpriteVtx){ f->ul, f->vb, f->x0, f->y1, 0.0f };
        vtx[nv++] = (RawSpriteVtx){ f->ur, f->vb, f->x1, f->y1, 0.0f };
        vtx[nv++] = (RawSpriteVtx){ f->ul, f->vt, f->x0, f->y0, 0.0f };
        vtx[nv++] = (RawSpriteVtx){ f->ur, f->vt, f->x1, f->y0, 0.0f };
        DL_EMIT(0x01, first & 0x00FFFFFFu);
        DL_EMIT(0x04, (4u << 16) | 4u);
    }
    DL_EMIT(0xC6, 0);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT

    const uint32_t s = (uint32_t)s_scale;
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev) return 0;
    coherence_reset_targets();
    memset(fb, 0, bytes);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(COH_SAMPLE_LIST, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    int ok = gegpu_sync_guest_fb(&desc);
    Target *t = ok ? target_find_by_fba(fba) : NULL;
    ok = t != NULL && coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev);
    if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: draw or device read failed\n", name);
    int bad = 0;
    for (int c = 0; ok && c < ncells; c++) {
        const EdgeCell *e = &cells[c];
        /* The filter the sampler picks from the primitive's footprint: texels per device pixel on
         * its steeper axis, minification above 1 (TEXFILTER bits 0-2), magnification otherwise
         * (bits 8-10). Every other cell uses one filter for both. */
        const double rho = fmax(fabs((double)e->ur - e->ul), fabs((double)e->vb - e->vt)) / ((double)n * s);
        const uint32_t filt = rho > 1.0 ? (e->tflt & 7u) : ((e->tflt >> 8) & 7u);
        const int nearest = filt == 0u, clampwrap = e->wrap != 0u, separable = e->shear == 0.0f;
        int cell_bad = 0;
        for (uint32_t dy = 0; dy < (uint32_t)n * s; dy++) {
            for (uint32_t dx = 0; dx < (uint32_t)n * s; dx++) {
                double u = coh_edge_coord(e->ul, e->ur, (uint32_t)n, dx, s, separable);
                const double v = coh_edge_coord(e->vt, e->vb, (uint32_t)n, dy, s, separable);
                if (!separable) u += (((double)dy + 0.5) / (double)s) * (double)e->shear / (double)n;
                const float wr = coh_edge_sample(v, nearest, clampwrap);   /* red: the row */
                const float wg = coh_edge_sample(u, nearest, clampwrap);   /* green: the column */
                const uint32_t X = (uint32_t)(20 + 20 * c) * s + dx, Y = (uint32_t)y0 * s + dy;
                const uint32_t got = dev[(size_t)Y * SCL_W + X];
                const float er = fabsf((float)(got & 0xFFu) - wr);
                const float eg = fabsf((float)((got >> 8) & 0xFFu) - wg);
                if (er > 2.0f || eg > 2.0f) {
                    if (cell_bad++ < 4)
                        fprintf(stderr, "gpu coherence selftest [%s/%s]: scale=%u device pixel (%u,%u) "
                                "r=%u g=%u, want %.2f %.2f (u=%.4f v=%.4f)\n", name, e->name, s, dx, dy,
                                got & 0xFFu, (got >> 8) & 0xFFu, wr, wg, u, v);
                }
            }
        }
        bad += cell_bad;
    }
    /* The fractional cells, over their box and one guest pixel around it: a device pixel the strip
     * covers at this scale shows the model, any other the cleared target. A device pixel centre
     * exactly on an edge at this scale is skipped: which side covers it is the device rasteriser's
     * own tie rule, not the PSP's. */
    for (int c = 0; ok && c < nfrac; c++) {
        const FracCell *f = &fcells[c];
        const int nearest = f->tflt == 0u;
        int cell_bad = 0;
        const int gx0 = (int)floor(f->x0) - 1, gx1 = (int)ceil(f->x1) + 1;
        const int gy0 = (int)floor(f->y0) - 1, gy1 = (int)ceil(f->y1) + 1;
        for (uint32_t Y = (uint32_t)gy0 * s; Y < (uint32_t)gy1 * s; Y++) {
            const double py = ((double)Y + 0.5) / (double)s;
            if (py == f->y0 || py == f->y1) continue;
            for (uint32_t X = (uint32_t)gx0 * s; X < (uint32_t)gx1 * s; X++) {
                const double qx = ((double)X + 0.5) / (double)s;
                if (qx == f->x0 || qx == f->x1) continue;
                const uint32_t got = dev[(size_t)Y * SCL_W + X];
                const int covered = qx >= f->x0 && qx < f->x1 && py >= f->y0 && py < f->y1;
                float wr = 0.0f, wg = 0.0f;
                int fail;
                if (covered) {
                    const double u = coh_frac_coord(f->x0, f->x1, f->ul, f->ur, qx, s);
                    const double v = coh_frac_coord(f->y0, f->y1, f->vt, f->vb, py, s);
                    wr = coh_edge_sample(v, nearest, 0);
                    wg = coh_edge_sample(u, nearest, 0);
                    fail = fabsf((float)(got & 0xFFu) - wr) > 2.0f ||
                           fabsf((float)((got >> 8) & 0xFFu) - wg) > 2.0f;
                } else {
                    fail = got != 0u;
                }
                if (fail && cell_bad++ < 4)
                    fprintf(stderr, "gpu coherence selftest [%s/%s]: scale=%u device pixel (%u,%u) "
                            "(guest %.3f,%.3f) %s r=%u g=%u, want %s %.2f %.2f\n", name, f->name, s, X, Y,
                            qx, py, covered ? "covered" : "outside", got & 0xFFu, (got >> 8) & 0xFFu,
                            covered ? "" : "the cleared target, not", wr, wg);
            }
        }
        bad += cell_bad;
    }
    /* At 1x a primitive carries no clamp range; above 1x a through-mode textured sprite does. */
    if (ok) {
        s_ge->tex_enable = 1;
        GeVtx p0 = { 300.0f, 40.0f, 0.0f, 1.0f, 0.0f, 16.0f, 1.0f, 0xff, 0xff, 0xff, 0xff };
        GeVtx p1 = p0;
        p1.x = 312.0f; p1.y = 52.0f; p1.u = 16.0f; p1.v = 0.0f;
        ok = hook_sprite(&p0, &p1, 0) && s_nverts > 0u;
        const float *box = ok ? s_vcpu[s_nverts - 1u].box : NULL;
        const int free_box = box && box[0] > box[2] && box[1] > box[3];
        if (ok && (s == 1u) != free_box) {
            fprintf(stderr, "gpu coherence selftest [%s]: scale=%u the sprite's clamp range is "
                    "(%.4f,%.4f)-(%.4f,%.4f): want %s\n", name, s, box[0], box[1], box[2], box[3],
                    s == 1u ? "none at 1x" : "a range above 1x");
            ok = 0;
        }
        if (ok) ok = submit_pending();
    }
    if (bad) {
        fprintf(stderr, "gpu coherence selftest [%s]: %d device pixel(s) differ at scale %u\n", name, bad, s);
        ok = 0;
    }
    free(dev);
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS %-22s %d cells scale=%u\n", name, ncells + nfrac, s);
    return ok;
}

/* Put TEXMODE, TEXLEVEL and TEXFILTER back to level 0 only through the list path, so the
 * later fixtures start from the state they assume and the state serial moves with it. */
static void coherence_mip_reset_state(void) {
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_RESET);
    dl[0] = 0xC2u << 24;
    dl[1] = 0xC8u << 24;
    dl[2] = 0xC6u << 24;
    dl[3] = 0x0Fu << 24;
    dl[4] = 0x0Cu << 24;
    ge_run_list(COH_MIP_RESET, 0);
}

/* The texel the first pixel shows: the sprite's UV runs 0.25..8.25 level-0 texels across
 * `rect` pixels, and the pixel samples it at its centre (PSP-GE-SAMPLE-001), so it reads
 * s = 0.25 + 0.5 * 8 / rect, which is s / 2^level in the level. Every case with an expected
 * level filters that level by the nearest rule, floor(s). */
static uint32_t coherence_mip_first_texel(const CohMipCase *c) {
    float s = (0.25f + 4.0f / (float)c->rect) / (float)(1u << (unsigned)c->level);
    unsigned size = 8u >> (unsigned)c->level, t = (unsigned)s;
    if (t >= size) t = size - 1u;
    return c->level == 1 && c->alt_level1 ? coherence_mip_alt_texel(t, t)
                                          : coherence_mip_texel((unsigned)c->level, t, t);
}

static int coherence_run_mip_case(const CohMipCase *c) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u, pixels = bytes / 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *gpu = (uint32_t *)malloc(bytes);
    if (!gpu) {
        fprintf(stderr, "gpu coherence selftest [mip-%s]: no reference buffer\n", c->name);
        return 0;
    }
    uint32_t list = coherence_mip_list(c, 1);
    coherence_reset_targets();
    memset(fb, 0, bytes);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    ge_set_gpu_hooks(&k_hooks);
    const unsigned long long fallback0 = s_replay_stats.fallback_prims;
    ge_run_list(list, 0);
    const unsigned long long fallbacks = s_replay_stats.fallback_prims - fallback0;
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    int ok = gegpu_sync_guest_fb(&desc);
    if (!ok) fprintf(stderr, "gpu coherence selftest [mip-%s]: GPU readback failed\n", c->name);
    if (ok && (fallbacks != 0u) != (c->falls_back != 0)) {
        fprintf(stderr, "gpu coherence selftest [mip-%s]: %llu software fallback primitive(s), want %s\n",
                c->name, fallbacks, c->falls_back ? "the software rasterizer" : "the GPU");
        ok = 0;
    }
    if (ok) {
        memcpy(gpu, fb, bytes);
        memset(fb, 0, bytes);
        ge_set_gpu_hooks(NULL);
        ge_run_list(list, 0);
        ge_set_gpu_hooks(&k_hooks);
    }
    uint32_t first = (uint32_t)(COH_MIP_Y * (int)stride + COH_MIP_X);
    if (ok && c->level >= 0) {
        uint32_t want = coherence_mip_first_texel(c) & 0x00FFFFFFu;
        if ((fb[first] & 0x00FFFFFFu) != want || (gpu[first] & 0x00FFFFFFu) != want) {
            fprintf(stderr,
                    "gpu coherence selftest [mip-%s]: first pixel gpu=%06x software=%06x, want "
                    "level %d texel %06x\n", c->name, gpu[first] & 0x00FFFFFFu,
                    fb[first] & 0x00FFFFFFu, c->level, want);
            ok = 0;
        }
    }
    for (size_t px = 0; ok && px < pixels; px++) {
        int differs = 0;
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            int delta = (int)((gpu[px] >> shift) & 0xFFu) - (int)((fb[px] >> shift) & 0xFFu);
            if (delta < -c->tolerance || delta > c->tolerance) differs = 1;
        }
        if (differs) {
            fprintf(stderr,
                    "gpu coherence selftest [mip-%s]: GPU/software mismatch at pixel=(%zu,%zu) "
                    "gpu=%08x software=%08x\n",
                    c->name, px % stride, px / stride, gpu[px], fb[px]);
            ok = 0;
        }
    }
    free(gpu);
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS mip %-22s mode=%u bias=%d/16 max=%u tflt=0x%03x\n",
               c->name, c->mode, (int)(int8_t)(uint8_t)c->bias16, c->max_level, c->tflt);
    return ok;
}

/* A notified CPU write into mip level 1 between two same-state draws must reach the second
 * draw: hook_vram_dirty has to see the level's own source range, not only level 0's, or the
 * state template keeps the old chain (the texture-dirty-same-state rule, for a level). */
static int coherence_run_mip_level_dirty_same_state(void) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u, rgb = 0x00FFFFFFu;
    const uint32_t level1 = COH_MIP_TEX + 0x1000u;
    const CohMipCase c = { "level-dirty", 0x004u, 3u, 1u, 16u, 8, 0, 1, 0, 0, 0.0f, 0 };
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(level1);
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_mip_list(&c, 0), 0);      /* the state, no draw */
    GeVtx p0 = { (float)COH_MIP_X, (float)COH_MIP_Y, 1.0f, 1.0f, 0.25f, 0.25f, 1.0f,
                 255, 255, 255, 255 };
    GeVtx p1 = { (float)(COH_MIP_X + 8), (float)(COH_MIP_Y + 8), 1.0f, 1.0f, 8.25f, 8.25f, 1.0f,
                 255, 255, 255, 255 };
    int ok = hook_sprite(&p0, &p1, 0);
    for (unsigned v = 0; v < 4u; v++)
        for (unsigned u = 0; u < 4u; u++) tex[v * 8u + u] = coherence_mip_alt_texel(u, v);
    sr_gpu_vram_dirty(level1, 4u * 8u * 4u);
    p0.x += 16.0f; p1.x += 16.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    uint32_t left = fb[COH_MIP_Y * stride + COH_MIP_X] & rgb;
    uint32_t right = fb[COH_MIP_Y * stride + COH_MIP_X + 16] & rgb;
    uint32_t want_left = coherence_mip_texel(1u, 0u, 0u) & rgb;
    uint32_t want_right = coherence_mip_alt_texel(0u, 0u) & rgb;
    if (!ok || left != want_left || right != want_right) {
        fprintf(stderr,
                "gpu coherence selftest [mip-level-dirty]: draws=%d left=%06x (want %06x) "
                "right=%06x (want %06x)\n", ok, left, want_left, right, want_right);
        ok = 0;
    }
    coherence_mip_fill();                           /* level 1 back to its first contents */
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS mip %-22s old=%06x new=%06x\n",
                   "level-dirty-same-state", left, right);
    return ok;
}

/* ---- texture byte shadow of a mip chain (#1931) -------------------------------------------
 *
 * A notified write over a whole chain that leaves its bytes as they were (a title's whole-cache
 * writeback, once a frame) must not decode the chain again: the byte shadow covers every level,
 * as it does a single-level texture. Each case draws a chain three times at one constant level
 * and pixel (level texel (8, 0)): the first draw decodes, the second follows a notified write
 * over every level with the bytes unchanged and must reuse the shadow, and the third follows a
 * change that must decode again. A byte change goes to bytes 16-63 of row 0 of one level, which
 * the content hash does not sample (a level of 4 KiB or more: tex_hash reads 16 of every 64
 * bytes), so only the shadow's comparison of that level can find it. The palette case changes
 * the palette entry every texel uses instead. Levels lie one after another from `addr`. */
#define COH_SHADOW_CLUT 0x085C0000u
#define COH_SHADOW_INDEX 6u

typedef struct {
    const char *name;
    uint32_t addr;          /* level 0; main RAM or VRAM */
    uint32_t log2;          /* level 0 is 2^log2 texels square */
    uint32_t fmt;           /* 3 = 8888, 5 = CLUT8 through the palette at COH_SHADOW_CLUT */
    uint32_t max_level;
    uint32_t show;          /* the constant level the draws sample */
    int change;             /* level whose unsampled bytes the third draw changes; -1 = palette */
} CohShadowCase;

static uint32_t coh_shadow_level(const CohShadowCase *c, uint32_t level, uint32_t *bytes) {
    uint32_t addr = c->addr, bpp = c->fmt == 3u ? 4u : 1u;
    for (uint32_t l = 0; l <= level; l++) {
        uint32_t size = 1u << (c->log2 - l);
        *bytes = size * size * bpp;
        if (l < level) addr += *bytes;
    }
    return addr;
}

/* Distinct opaque colours for the levels and the palette entries (ABGR). */
static uint32_t coh_shadow_colour(uint32_t level) {
    return 0xFF000000u | (0x50u << 16) | ((0xC0u - 0x30u * level) << 8) | (0x30u + 0x40u * level);
}
static uint32_t coh_shadow_entry(uint32_t j) { return 0xFF000000u | ((j * 0x00030507u) & 0x00FFFFFFu); }

/* Every texel of `level` shows `colour` (8888), or palette index COH_SHADOW_INDEX (CLUT8). */
static void coh_shadow_fill(const CohShadowCase *c, uint32_t level, uint32_t colour) {
    uint32_t bytes, addr = coh_shadow_level(c, level, &bytes);
    if (c->fmt == 3u) {
        uint32_t *tex = (uint32_t *)SR_HOST(addr);
        for (uint32_t i = 0; i < bytes / 4u; i++) tex[i] = colour;
    } else {
        memset(SR_HOST(addr), (int)COH_SHADOW_INDEX, bytes);
    }
    sr_gpu_vram_dirty(addr, bytes);
}

/* The chain's state through the list path; with a palette, it is loaded from COH_SHADOW_CLUT. */
static void coh_shadow_state(const CohShadowCase *c) {
    const CohMipCase mc = { c->name, 0x004u, c->max_level, 1u, c->show * 16u, 8, 0, (int)c->show,
                            0, 0, 0.0f, 0 };
    ge_run_list(coherence_mip_list(&mc, 0), 0);
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_RESET);
    int p = 0;
    for (uint32_t l = 0; l <= c->max_level; l++) {
        uint32_t bytes, addr = coh_shadow_level(c, l, &bytes), log2 = c->log2 - l;
        dl[p++] = ((0xA0u + l) << 24) | (addr & 0x00FFFFFFu);                         /* TEXADDRn */
        dl[p++] = ((0xA8u + l) << 24) | (1u << log2) | ((addr & 0xFF000000u) >> 8);  /* TEXBUFWIDTHn */
        dl[p++] = ((0xB8u + l) << 24) | (log2 << 8) | log2;                          /* TEXSIZEn */
    }
    dl[p++] = (0xC3u << 24) | c->fmt;
    if (c->fmt != 3u) {
        dl[p++] = (0xB0u << 24) | (COH_SHADOW_CLUT & 0x00FFFFFFu);
        dl[p++] = (0xB1u << 24) | ((COH_SHADOW_CLUT & 0xFF000000u) >> 8);
        dl[p++] = (0xC5u << 24) | 3u | (0xFFu << 8);    /* 8888 entries, shift 0, mask 0xFF */
        dl[p++] = (0xC4u << 24) | 32u;                  /* load 256 entries */
    }
    dl[p++] = 0x0Fu << 24;
    dl[p++] = 0x0Cu << 24;
    ge_run_list(COH_MIP_RESET, 0);
}

/* Draw `d` of a case at level texel (8, 0), submitted; returns the decodes it took, or -1. */
static long coh_shadow_draw(const CohShadowCase *c, int d) {
    const float scale = (float)(1u << c->show), x = (float)(COH_MIP_X + 16 * d);
    GeVtx p0 = { x, (float)COH_MIP_Y, 1.0f, 1.0f, 8.0f * scale + 0.25f, 0.25f, 1.0f,
                 255, 255, 255, 255 };
    GeVtx p1 = { x + 8.0f, (float)(COH_MIP_Y + 8), 1.0f, 1.0f, 9.0f * scale + 0.25f,
                 scale + 0.25f, 1.0f, 255, 255, 255, 255 };
    unsigned long before = s_cnt_texup;
    if (!hook_sprite(&p0, &p1, 0) || !submit_pending()) return -1;
    return (long)(s_cnt_texup - before);
}

static int coherence_run_mip_shadow_case(const CohShadowCase *c) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u, rgb = 0x00FFFFFFu;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *clut = (uint32_t *)SR_HOST(COH_SHADOW_CLUT);
    const uint32_t changed = 0xFF40C020u;
    for (uint32_t l = 0; l <= c->max_level; l++) coh_shadow_fill(c, l, coh_shadow_colour(l));
    for (uint32_t j = 0; j < 256u; j++) clut[j] = coh_shadow_entry(j);
    sr_gpu_vram_dirty(COH_SHADOW_CLUT, 256u * 4u);
    const uint32_t first = c->fmt == 3u ? coh_shadow_colour(c->show) : clut[COH_SHADOW_INDEX];
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    coh_shadow_state(c);

    long decodes[3];
    uint64_t avoided0 = s_cnt_shadow_avoided;
    decodes[0] = coh_shadow_draw(c, 0);
    for (uint32_t l = 0; l <= c->max_level; l++) {          /* bytes unchanged */
        uint32_t bytes, addr = coh_shadow_level(c, l, &bytes);
        sr_gpu_vram_dirty(addr, bytes);
    }
    decodes[1] = coh_shadow_draw(c, 1);
    if (c->change >= 0) {
        uint32_t bytes, addr = coh_shadow_level(c, (uint32_t)c->change, &bytes);
        uint32_t *tex = (uint32_t *)SR_HOST(addr);
        for (uint32_t u = 4u; u < 16u; u++) tex[u] = changed;      /* row 0 bytes 16-63 */
        sr_gpu_vram_dirty(addr + 16u, 48u);
    } else {
        clut[COH_SHADOW_INDEX] = changed;
        sr_gpu_vram_dirty(COH_SHADOW_CLUT, 256u * 4u);
        coh_shadow_state(c);                                        /* reloads the palette */
    }
    decodes[2] = coh_shadow_draw(c, 2);
    const uint64_t avoided = s_cnt_shadow_avoided - avoided0;
    int ok = decodes[0] >= 0 && decodes[1] >= 0 && decodes[2] >= 0 && s_cur && target_readback(s_cur);
    uint32_t px[3];
    for (int d = 0; d < 3; d++) px[d] = fb[COH_MIP_Y * stride + COH_MIP_X + 16u * (uint32_t)d] & rgb;
    if (!ok || px[0] != (first & rgb) || px[1] != (first & rgb) || px[2] != (changed & rgb) ||
        decodes[0] != 1 || decodes[1] != 0 || decodes[2] != 1 || avoided != 1u) {
        fprintf(stderr,
                "gpu coherence selftest [mip-shadow %s]: draws=%d pixels=%06x %06x %06x (want "
                "%06x %06x %06x) decodes=%ld %ld %ld (want 1 0 1) shadow reuses=%llu (want 1)%s\n",
                c->name, ok, px[0], px[1], px[2], first & rgb, first & rgb, changed & rgb,
                decodes[0], decodes[1], decodes[2], (unsigned long long)avoided,
                decodes[1] ? " -- an unchanged chain decoded again" :
                px[2] != (changed & rgb) ? " -- the shadow missed the change" : "");
        ok = 0;
    }
    coherence_mip_reset_state();
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS mip shadow %-15s decodes=1 0 1 shadow-reuses=1\n",
                   c->name);
    return ok;
}

/* An upload that fails partway leaves the image's contents undefined: copying level 0 discards
 * every level, and a chain stays in its transfer layout. Nothing may reuse that image until a
 * decode succeeds, even when the guest puts back exactly the bytes the shadow and the content
 * hash still describe. The selftest fails the upload right after level 0 has been copied
 * (s_selftest_upload_fail_after). Level 0 turns from one colour to another, the failing draw
 * follows, the old colour comes back, and the next draw must decode and show it; reusing the
 * image would show the new colour, or nothing defined at all. */
static int coherence_run_texture_upload_failure(const CohShadowCase *c) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u, rgb = 0x00FFFFFFu;
    const uint32_t old_colour = coh_shadow_colour(0), new_colour = 0xFF40C020u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    for (uint32_t l = 0; l <= c->max_level; l++) coh_shadow_fill(c, l, coh_shadow_colour(l));
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    coh_shadow_state(c);

    long decodes[3];
    decodes[0] = coh_shadow_draw(c, 0);
    coh_shadow_fill(c, 0, new_colour);
    s_selftest_upload_fail_after = 0u;
    decodes[1] = coh_shadow_draw(c, 1);
    const int failed = s_selftest_upload_fail_after == UINT32_MAX;
    s_selftest_upload_fail_after = UINT32_MAX;
    coh_shadow_fill(c, 0, old_colour);
    decodes[2] = coh_shadow_draw(c, 2);
    int ok = decodes[0] >= 0 && decodes[1] >= 0 && decodes[2] >= 0 && s_cur && target_readback(s_cur);
    const uint32_t last = fb[COH_MIP_Y * stride + COH_MIP_X + 32u] & rgb;
    if (!ok || !failed || last != (old_colour & rgb) || decodes[0] != 1 || decodes[1] != 0 ||
        decodes[2] != 1) {
        fprintf(stderr,
                "gpu coherence selftest [texture-upload-failure %s]: draws=%d injected=%d last=%06x "
                "(want %06x) decodes=%ld %ld %ld (want 1 0 1)%s\n",
                c->name, ok, failed, last, old_colour & rgb, decodes[0], decodes[1], decodes[2],
                decodes[2] == 0 ? " -- the partly uploaded image was reused" : "");
        ok = 0;
    }
    coherence_mip_reset_state();
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS texture-upload-failure %-12s decodes=1 0 1\n", c->name);
    return ok;
}

static int coherence_run_mip_shadow(void) {
    static const CohShadowCase shadow[] = {
        { "vram-level-1", 0x04050000u, 6u, 3u, 1u, 1u, 1 },
        { "ram-level-0",  0x08560000u, 7u, 3u, 1u, 0u, 0 },
        { "ram-level-2",  0x08580000u, 7u, 3u, 2u, 2u, 2 },
        { "ram-palette",  0x085A0000u, 7u, 5u, 1u, 1u, -1 },
    };
    static const CohShadowCase failure[] = {
        { "single-level", 0x08560000u, 7u, 3u, 0u, 0u, 0 },
        { "chain",        0x08580000u, 7u, 3u, 1u, 0u, 0 },
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(shadow) / sizeof(shadow[0]); i++)
        if (!coherence_run_mip_shadow_case(&shadow[i])) ok = 0;
    for (size_t i = 0; i < sizeof(failure) / sizeof(failure[0]); i++)
        if (!coherence_run_texture_upload_failure(&failure[i])) ok = 0;
    return ok;
}

/* One GPU draw through the hooks, after the state of `c`: a size x size quad at (COH_MIP_X,
 * COH_MIP_Y) whose texel coordinates run from 0.25 by `span` texels across it (`vspan` down
 * it, or `span` when 0), at 1/w = `rw`
 * everywhere (texel coordinates pre-multiplied by it, as hook_tri receives them), or a
 * through-mode sprite when `persp` is 0. Returns the level the pixel one in from the corner
 * sampled (red >> 6 of coherence_mip_texel), or -1. */
static int coherence_mip_hook_level(const CohMipCase *c, int persp, float rw, int size, float span,
                                    float vspan) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_mip_list(c, 0), 0);      /* the state, no draw */
    s_ge->minz = 0u;
    s_ge->maxz = 65535u;
    const float x0 = (float)COH_MIP_X, y0 = (float)COH_MIP_Y, x1 = x0 + (float)size, y1 = y0 + (float)size;
    const float u0 = 0.25f, u1 = 0.25f + span, v1 = 0.25f + (vspan > 0.0f ? vspan : span);
    const float q = persp ? rw : 1.0f;
    GeVtx a = { x0, y0, 1.0f, q, u0 * q, u0 * q, 1.0f, 255, 255, 255, 255 };
    GeVtx b = { x1, y0, 1.0f, q, u1 * q, u0 * q, 1.0f, 255, 255, 255, 255 };
    GeVtx d = { x0, y1, 1.0f, q, u0 * q, v1 * q, 1.0f, 255, 255, 255, 255 };
    GeVtx e = { x1, y1, 1.0f, q, u1 * q, v1 * q, 1.0f, 255, 255, 255, 255 };
    const unsigned long long fallback0 = s_replay_stats.fallback_prims;
    int ok = persp ? hook_tri(&a, &b, &e, 1) && hook_tri(&a, &e, &d, 1) : hook_sprite(&a, &e, 0);
    ok = ok && s_replay_stats.fallback_prims == fallback0 && s_cur && target_readback(s_cur);
    if (!ok) return -1;
    const uint32_t px = fb[(COH_MIP_Y + 1) * stride + COH_MIP_X + 1];
    const uint32_t r = px & 0xFFu, g = (px >> 8) & 0xFFu;
    return g == 255u - r ? (int)(r >> 6) : -1;
}

/* Per-fragment levels of detail through the hooks (#1373): psp.frag takes q from the
 * interpolated 1/w in slope mode, and in automatic mode the footprint from the quotient
 * rule raster_tri uses, (d(u/w) - u d(1/w)) / (1/w), per PSP pixel. Only the level is
 * checked, so this also runs alone at SR_GPU_SCALE=2 and 4 (SR_GPU_COHERENCE_CASE=mip-lod),
 * where a device pixel's footprint is 1/scale of a PSP pixel's. */
static int coherence_run_mip_lod(void) {
    static const struct {
        const char *name;
        CohMipCase c;
        int persp; float rw; int size; float span; int level; float vspan;
    } cells[] = {
        /* w = 2, slope 1: 16 + log16(1) - log16(1/2) = 32, level 2, whatever the footprint. */
        { "persp-slope", { "persp-slope", 0x004u, 3u, 2u, 0u, 0, 0, 0, 0, 0, 1.0f, 0 }, 1, 0.5f, 16, 8.0f, 2, 0.0f },
        /* w = 2, 16 texels over 4 pixels: footprint 4, level 2. Leaving d(u/w) undivided by
         * 1/w reads footprint 2, level 1. */
        { "persp-auto", { "persp-auto", 0x004u, 3u, 0u, 0u, 0, 0, 0, 0, 0, 0.0f, 0 }, 1, 0.5f, 4, 16.0f, 2, 0.0f },
        { "persp-auto-mag", { "persp-auto-mag", 0x004u, 3u, 0u, 0u, 0, 0, 0, 0, 0, 0.0f, 0 }, 1, 0.5f, 16, 8.0f, 0, 0.0f },
        /* Through mode, 8 texels over 4 pixels: footprint 2, level 1. */
        { "through-auto", { "through-auto", 0x004u, 3u, 0u, 0u, 0, 0, 0, 0, 0, 0.0f, 0 }, 0, 1.0f, 4, 8.0f, 1, 0.0f },
        /* The larger axis: 2 texels across and 8 down 4 pixels is footprint 2, level 1. */
        { "through-auto-v", { "through-auto-v", 0x004u, 3u, 0u, 0u, 0, 0, 0, 0, 0, 0.0f, 0 }, 0, 1.0f, 4, 2.0f, 1,
          8.0f },
    };
    int ok = 1;
    coherence_mip_fill();
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        const int level = coherence_mip_hook_level(&cells[i].c, cells[i].persp, cells[i].rw,
                                                   cells[i].size, cells[i].span, cells[i].vspan);
        if (level != cells[i].level) {
            fprintf(stderr, "gpu coherence selftest [mip-lod %s]: level %d at scale %d, want %d\n",
                    cells[i].name, level, s_scale, cells[i].level);
            ok = 0;
        } else {
            printf("gpu coherence selftest: PASS mip-lod %-18s level=%d scale=%d\n", cells[i].name,
                   level, s_scale);
        }
    }
    coherence_mip_reset_state();
    coherence_reset_targets();
    return ok;
}

/* A TEXLODSLOPE write between two draws of an otherwise identical state must reach the second
 * draw: the per-draw state template is keyed by the slope too (StateSnap). Slope 1 is level 1
 * and slope 2 level 2 in through mode. */
static int coherence_run_mip_slope_change(void) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u, rgb = 0x00FFFFFFu;
    const CohMipCase c = { "slope-change", 0x004u, 3u, 2u, 0u, 16, 0, 1, 0, 0, 1.0f, 0 };
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    coherence_mip_fill();
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_mip_list(&c, 0), 0);      /* the state, no draw */
    GeVtx p0 = { (float)COH_MIP_X, (float)COH_MIP_Y, 1.0f, 1.0f, 0.25f, 0.25f, 1.0f,
                 255, 255, 255, 255 };
    GeVtx p1 = { (float)(COH_MIP_X + 16), (float)(COH_MIP_Y + 16), 1.0f, 1.0f, 8.25f, 8.25f, 1.0f,
                 255, 255, 255, 255 };
    int ok = hook_sprite(&p0, &p1, 0);
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_RESET);
    const float two = 2.0f;
    uint32_t bits;
    memcpy(&bits, &two, 4);
    dl[0] = (0xD0u << 24) | (bits >> 8);            /* TEXLODSLOPE 2 */
    dl[1] = 0x0Fu << 24;
    dl[2] = 0x0Cu << 24;
    ge_run_list(COH_MIP_RESET, 0);
    p0.x += 32.0f; p1.x += 32.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    const uint32_t left = fb[COH_MIP_Y * stride + COH_MIP_X] & rgb;
    const uint32_t right = fb[COH_MIP_Y * stride + COH_MIP_X + 32] & rgb;
    const uint32_t want_left = coherence_mip_texel(1u, 0u, 0u) & rgb;
    const uint32_t want_right = coherence_mip_texel(2u, 0u, 0u) & rgb;
    if (!ok || left != want_left || right != want_right) {
        fprintf(stderr,
                "gpu coherence selftest [mip-slope-change]: draws=%d left=%06x (want %06x) "
                "right=%06x (want %06x)\n", ok, left, want_left, right, want_right);
        ok = 0;
    }
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS mip %-22s old=%06x new=%06x\n",
                   "slope-change-same-state", left, right);
    return ok;
}

/* Multi-CLUT mip levels (#2031). A CLUT8 chain of four levels whose level-L texel in column u
 * holds index 0x10 + 4 L + (u & 3), through a 512-entry 4444 CLUT whose entry k is 0xF000 | k,
 * so a pixel names the entry it read. With TEXMODE bit 8 set, level L reads entry t + 256 L
 * wrapped in CLUT RAM (ge.c clut_index, PSP-GE-MIP-002); the Vulkan backend decodes each level
 * through that lookup. Each cell draws one through-mode sprite at a constant level on the GPU
 * and with the software GE, and the two framebuffers must be equal, with the first pixel the
 * entry the rule names. off-level-1 follows on-level-1 with only the bit changed, so a texture
 * cache that keys the chain without the bit reuses level 1 read through the wrong entries. */
#define COH_MCLUT_TEX  0x04046000u           /* level L at COH_MCLUT_TEX + L * 0x200, width 16 */
#define COH_MCLUT_CLUT 0x04047000u           /* 512 4444 entries (32 blocks) */

static uint32_t coherence_mclut_rgb(uint32_t k) {
    return ((k & 15u) * 17u) | ((((k >> 4) & 15u) * 17u) << 8) | ((((k >> 8) & 15u) * 17u) << 16);
}

static uint32_t coherence_mclut_list(uint32_t multi, uint32_t level) {
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_MIP_VERTS);
    /* UV from 0: half a level-0 texel per pixel, so every texel edge of every level falls on a
     * pixel edge and the texels, which change with u, give the same pixels at any render scale. */
    vtx[0] = (RawSpriteVtx){ 0.0f, 0.0f, (float)COH_MIP_X, (float)COH_MIP_Y, 0.0f };
    vtx[1] = (RawSpriteVtx){ 8.0f, 8.0f, (float)(COH_MIP_X + 16), (float)(COH_MIP_Y + 16), 0.0f };
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_LIST);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0x9E, 0x00080000u);
    DL_EMIT(0x9F, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xB0, COH_MCLUT_CLUT & 0x00FFFFFFu);                           /* CLUTADDR */
    DL_EMIT(0xB1, (COH_MCLUT_CLUT & 0xFF000000u) >> 8);                    /* CLUTADDRUPPER */
    DL_EMIT(0xC4, 0x20u);                                                  /* LOADCLUT: 512 entries */
    DL_EMIT(0xC5, 2u | (0xFFu << 8));                                      /* 4444, mask 0xFF */
    for (uint32_t l = 0; l < 4u; l++) {
        const uint32_t addr = COH_MCLUT_TEX + l * 0x200u, log2 = 3u - l;
        DL_EMIT(0xA0 + l, addr & 0x00FFFFFFu);
        DL_EMIT(0xA8 + l, 16u | ((addr & 0xFF000000u) >> 8));
        DL_EMIT(0xB8 + l, (log2 << 8) | log2);
    }
    DL_EMIT(0xC2, (3u << 16) | (multi << 8));           /* TEXMODE: levels 0-3, multi-CLUT */
    DL_EMIT(0xC8, 1u | ((level << 4) << 16));           /* TEXLEVEL: constant, bias = level */
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 5);                                   /* CLUT8 */
    DL_EMIT(0xC6, 0x004u);                              /* nearest, nearest level */
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));                      /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));  /* float UV and position, through */
    DL_EMIT(0x01, COH_MIP_VERTS & 0x00FFFFFFu);
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return COH_MIP_LIST;
}

static int coherence_run_mip_clut(void) {
    static const struct { const char *name; uint32_t multi, level; } cells[] = {
        { "on-level-1", 1u, 1u }, { "off-level-1", 0u, 1u }, { "on-level-2", 1u, 2u },
        { "on-level-3", 1u, 3u }, { "on-level-0", 1u, 0u },
    };
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u, pixels = bytes / 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *gpu = (uint32_t *)malloc(bytes);
    if (!gpu) {
        fprintf(stderr, "gpu coherence selftest [mip-clut]: no reference buffer\n");
        return 0;
    }
    for (uint32_t l = 0; l < 4u; l++) {
        uint8_t *tex = (uint8_t *)SR_HOST(COH_MCLUT_TEX + l * 0x200u);
        for (uint32_t v = 0; v < (8u >> l); v++)
            for (uint32_t u = 0; u < 16u; u++) tex[v * 16u + u] = (uint8_t)(0x10u + 4u * l + (u & 3u));
        sr_gpu_vram_dirty(COH_MCLUT_TEX + l * 0x200u, 16u * (8u >> l));
    }
    uint16_t *clut = (uint16_t *)SR_HOST(COH_MCLUT_CLUT);
    for (uint32_t k = 0; k < 512u; k++) clut[k] = (uint16_t)(0xF000u | k);
    sr_gpu_vram_dirty(COH_MCLUT_CLUT, 512u * 2u);
    int all = 1;
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        const uint32_t list = coherence_mclut_list(cells[i].multi, cells[i].level);
        coherence_reset_targets();
        memset(fb, 0, bytes);
        sr_gpu_vram_dirty(base, (uint32_t)bytes);
        ge_set_gpu_hooks(&k_hooks);
        const unsigned long long fallback0 = s_replay_stats.fallback_prims;
        ge_run_list(list, 0);
        const unsigned long long fallbacks = s_replay_stats.fallback_prims - fallback0;
        GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                                   .width = 480, .height = 272 };
        int ok = gegpu_sync_guest_fb(&desc);
        if (!ok) fprintf(stderr, "gpu coherence selftest [mip-clut %s]: GPU readback failed\n", cells[i].name);
        if (ok && fallbacks != 0u) {
            fprintf(stderr, "gpu coherence selftest [mip-clut %s]: %llu software fallback primitive(s), "
                    "want the GPU\n", cells[i].name, fallbacks);
            ok = 0;
        }
        if (ok) {
            memcpy(gpu, fb, bytes);
            memset(fb, 0, bytes);
            ge_set_gpu_hooks(NULL);
            ge_run_list(list, 0);
            ge_set_gpu_hooks(&k_hooks);
        }
        /* The first pixel reads level texel 0 (a quarter level-0 texel in, at every level). */
        const uint32_t t = 0x10u + 4u * cells[i].level;
        const uint32_t k = cells[i].multi ? (t + 256u * cells[i].level) & 511u : t;
        const uint32_t want = coherence_mclut_rgb(k);
        const size_t first = (size_t)COH_MIP_Y * stride + COH_MIP_X;
        if (ok && ((fb[first] & 0x00FFFFFFu) != want || (gpu[first] & 0x00FFFFFFu) != want)) {
            fprintf(stderr, "gpu coherence selftest [mip-clut %s]: first pixel gpu=%06x software=%06x, "
                    "want entry 0x%03x (%06x)\n", cells[i].name, gpu[first] & 0x00FFFFFFu,
                    fb[first] & 0x00FFFFFFu, k, want);
            ok = 0;
        }
        size_t diff = 0, at = 0;
        for (size_t px = 0; ok && px < pixels; px++)
            if (gpu[px] != fb[px] && diff++ == 0) at = px;
        if (diff) {
            fprintf(stderr, "gpu coherence selftest [mip-clut %s]: %zu GPU/software mismatches, first "
                    "at (%zu,%zu) gpu=%08x software=%08x\n", cells[i].name, diff, at % stride,
                    at / stride, gpu[at], fb[at]);
            ok = 0;
        }
        if (ok)
            printf("gpu coherence selftest: PASS mip-clut %-12s bit=%u level=%u entry=0x%03x\n",
                   cells[i].name, cells[i].multi, cells[i].level, k);
        if (!ok) all = 0;
    }
    free(gpu);
    coherence_reset_targets();
    return all;
}

static int coherence_run_mip_levels(void) {
    static const CohMipCase cases[] = {
        /* Constant mode: the bias is the level, clamped to the highest level; one half
         * rounds up to the next level (PSP-GE-MIP-001 c-b0f8). */
        { "const-level-1",       0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 0, 0.0f, 0 },
        { "const-level-2",       0x004u, 3u, 1u, 32u, 16, 0, 2, 0, 0, 0.0f, 0 },
        { "const-clamped",       0x004u, 1u, 1u, 48u, 16, 0, 1, 0, 0, 0.0f, 0 },
        { "const-half-rounds-up", 0x004u, 3u, 1u, 8u, 16, 0, 1, 0, 0, 0.0f, 0 },
        { "const-mode-3",        0x004u, 3u, 3u, 32u, 16, 0, 2, 0, 0, 0.0f, 0 },
        /* With mip levels on, the level of detail picks the filter within a level: a
         * magnified sprite at constant level 1 filters by the minification field
         * (nearest), and a minified one at constant level 0 by the magnification field. */
        { "const-mag-linear",    0x104u, 3u, 1u, 16u, 16, 0, 1, 0, 0, 0.0f, 0 },
        { "const-0-mag-filter",  0x005u, 3u, 1u, 0u,   4, 0, 0, 0, 0, 0.0f, 0 },
        /* GU_NEAREST_MIPMAP_LINEAR at constant level 0.5: half of level 0, half of level 1. */
        { "const-between",       0x006u, 3u, 1u, 8u,  16, 1, -1, 0, 0, 0.0f, 0 },
        /* Automatic mode: log2 of the footprint plus the bias, clamped to [0, highest]. */
        { "auto-magnified",      0x004u, 3u, 0u, 0u,  16, 0, 0, 0, 0, 0.0f, 0 },
        { "auto-level-1",        0x004u, 3u, 0u, 0u,   4, 0, 1, 0, 0, 0.0f, 0 },
        { "auto-level-2",        0x004u, 3u, 0u, 0u,   2, 0, 2, 0, 0, 0.0f, 0 },
        { "auto-biased",         0x004u, 3u, 0u, 0xF0u, 2, 0, 1, 0, 0, 0.0f, 0 },
        { "auto-clamped",        0x004u, 1u, 0u, 0u,   2, 0, 1, 0, 0, 0.0f, 0 },
        /* Bias -1.5 on a 2x minified sprite: level of detail -0.5, so level 0 through the
         * magnification field (nearest), not the minification field (linear). */
        { "auto-bias-mag-filter", 0x005u, 3u, 0u, 0xE8u, 4, 0, 0, 0, 0, 0.0f, 0 },
        /* A footprint of 1.6 texels is level of detail 9/16 (the logarithm takes the top
         * four mantissa bits): GU_NEAREST_MIPMAP_LINEAR blends 9/16 of level 1 into level 0. */
        { "auto-between",        0x006u, 3u, 0u, 0u,   5, 1, -1, 0, 0, 0.0f, 0 },
        /* A footprint of 1.33 texels is level of detail 5/16: level 0, but minified, so the
         * minification field (nearest), not the magnification field (linear). */
        { "auto-min-filter",     0x104u, 3u, 0u, 0u,   6, 0, 0, 0, 0, 0.0f, 0 },
        /* Slope mode with TEXLODSLOPE 0: its logarithm is -2032, so level 0. */
        { "slope-zero",          0x004u, 3u, 2u, 16u,  4, 0, 0, 0, 0, 0.0f, 0 },
        /* Slope mode in through mode (q = 1): 16 + log16(slope) + bias, whatever the
         * footprint; 1.5 is 8/16 above 1, which rounds up to level 2. */
        { "slope-level-1",       0x004u, 3u, 2u, 0u,  16, 0, 1, 0, 0, 1.0f, 0 },
        { "slope-level-2",       0x004u, 3u, 2u, 0u,  16, 0, 2, 0, 0, 2.0f, 0 },
        { "slope-half-rounds-up", 0x004u, 3u, 2u, 0u, 16, 0, 2, 0, 0, 1.5f, 0 },
        { "slope-biased",        0x004u, 3u, 2u, 0xF0u, 16, 0, 1, 0, 0, 2.0f, 0 },
        { "slope-clamped",       0x004u, 1u, 2u, 0u,  16, 0, 1, 0, 0, 4.0f, 0 },
        { "slope-between",       0x006u, 3u, 2u, 0u,  16, 1, -1, 0, 0, 1.5f, 0 },
        /* Level 1 declared 8x8 cannot sit in a halving chain: the software rasterizer draws,
         * in constant and per-fragment modes alike. */
        { "chain-fallback",      0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 3u, 0.0f, 1 },
        { "chain-fallback-auto", 0x004u, 3u, 0u, 0u,   4, 0, -1, 0, 3u, 0.0f, 1 },
        /* Only level 1's address changes after const-level-1: the cache must not reuse it. */
        { "level1-moved",        0x004u, 3u, 1u, 16u, 16, 0, 1, 1, 0, 0.0f, 0 },
        { "level1-back",         0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 0, 0.0f, 0 },
    };
    int ok = 1;
    coherence_mip_fill();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!coherence_run_mip_case(&cases[i])) ok = 0;
    if (!coherence_run_mip_level_dirty_same_state()) ok = 0;
    if (!coherence_run_mip_shadow()) ok = 0;
    if (!coherence_run_mip_lod()) ok = 0;
    if (!coherence_run_mip_slope_change()) ok = 0;
    if (!coherence_run_mip_clut()) ok = 0;
    coherence_mip_reset_state();
    coherence_reset_targets();
    return ok;
}

/* ---- ring wrap and blend colour parity (issue #697) -----------------------------------
 *
 * The per-slot vertex arena (VERT_ARENA_VERTS) and per-slot transfer ring are the Vulkan
 * rings a wrap could overrun while the GPU still reads them. A render that does not fit
 * the open recording moves to the next submit slot, and that slot's fence must retire
 * before its arena is written again. These fixtures drive the production hook and GE list
 * paths on synthetic guest memory and check analytic final pixels: an overwrite of
 * in-flight vertex data changes a pixel, and a stale slot cursor changes the last writer.
 *
 * The parity fixtures compare the Vulkan output with the software GE (ge.c, the arbiter)
 * for every blend factor pair, and also report the distance to the project's R6 colour
 * product (ge_raster_ref.c, SPEC_ASSUMPTION until the #343 oracle cells P8/P9 run). */
#include "../ge_raster_ref.h"

static uint32_t wrap_hash(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return v;
}

static uint32_t wrap_pixel_rgba(uint32_t pass, uint32_t idx) {
    return 0xff000000u | (wrap_hash(idx * 2654435761u ^ pass * 0x9E3779B9u) & 0xFFFFFFu);
}

static void wrap_vtx(GeVtx *v, float x, float y, uint32_t c) {
    *v = (GeVtx){ x, y, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                  (int)(c & 0xFFu), (int)((c >> 8) & 0xFFu), (int)((c >> 16) & 0xFFu), (int)(c >> 24) };
}

static void wrap_state(uint32_t base, uint32_t stride) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = 3;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535;
}

/* The GE has no buffer height, so a title can stack short targets of one stride: the second base
 * lies inside the 272 rows a target could span. A texture at that base is the second target, not
 * rows of the first, whichever slot each took. */
static int coherence_run_stacked_targets(void) {
    const char *name = "stacked-targets";
    const uint32_t stride = 256u, row = stride * 4u, rgb = 0x00FFFFFFu;
    const uint32_t base_a = 0x04100000u, base_b = base_a + 100u * row, base_dst = 0x04000000u;
    const uint32_t pixel = coherence_gbatch(3u) & rgb, dst_stride = 512u;
    uint32_t *dst = (uint32_t *)SR_HOST(base_dst);
    coherence_reset_targets();
    memset(SR_HOST(base_a), 0, 372u * row);
    memset(dst, 0, dst_stride * FB_H * 4u);
    sr_gpu_vram_dirty(base_a, 372u * row);
    sr_gpu_vram_dirty(base_dst, dst_stride * FB_H * 4u);

    /* Rows 10 to 19 of each: `a` is bound first, so it takes the lower slot. */
    int ok = coherence_queue_g1_batch(base_a, stride, 3u);
    ok = submit_pending() && ok;
    ok = ok && coherence_queue_g1_batch(base_b, stride, 3u);
    ok = submit_pending() && ok;
    const Target *a = target_find_by_fba(base_a & 0x00FFFFFFu);
    const Target *b = target_find_by_fba(base_b & 0x00FFFFFFu);
    ok = ok && a && b && a < b;

    wrap_state(base_dst, dst_stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = base_b;
    s_ge->tex_bufw = stride;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 256;
    s_ge->tex_h = 256;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);
    GeVtx p0, p1;
    wrap_vtx(&p0, 10.0f, 10.0f, 0xFFFFFFFFu); p0.u = 10.0f; p0.v = 10.0f;
    wrap_vtx(&p1, 20.0f, 20.0f, 0xFFFFFFFFu); p1.u = 20.0f; p1.v = 20.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    const uint32_t got = dst[14u * dst_stride + 14u] & rgb;
    if (!ok || got != pixel) {
        fprintf(stderr, "gpu coherence selftest [%s]: sampled 0x%06x, want 0x%06x from the second "
                        "target (ok=%d)\n", name, got, pixel, ok);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n", name, stride, s_scale);
    coherence_reset_targets();
    return 1;
}

/* Many one-pixel sprites per pass over the whole frame. Each pass rewrites every pixel,
 * so the final frame is the last pass, and every submit slot is reused many times. */
static int coherence_run_ring_wrap(void) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t pixels = (uint32_t)(FB_W * FB_H);
    const uint32_t passes = 3u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    wrap_state(base, stride);
    ge_set_gpu_hooks(&k_hooks);

    GeGpuReplayStats before, after;
    gegpu_replay_stats_get(&before);
    uint64_t spr_before = s_cnt_spr;
    for (uint32_t pass = 0; pass < passes; pass++) {
        for (uint32_t idx = 0; idx < pixels; idx++) {
            GeVtx p0, p1;
            uint32_t c = wrap_pixel_rgba(pass, idx);
            wrap_vtx(&p0, (float)(idx % FB_W), (float)(idx / FB_W), c);
            wrap_vtx(&p1, (float)(idx % FB_W) + 1.0f, (float)(idx / FB_W) + 1.0f, c);
            if (!hook_sprite(&p0, &p1, 0)) {
                fprintf(stderr, "gpu coherence selftest [ring-wrap]: hook_sprite refused pass=%u idx=%u\n",
                        pass, idx);
                coherence_reset_targets();
                return 0;
            }
        }
    }
    Target *t = s_cur;
    if (!t || !target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: target readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_replay_stats_get(&after);

    for (uint32_t idx = 0; idx < pixels; idx++) {
        uint32_t want = wrap_pixel_rgba(passes - 1u, idx);
        /* Non-clear draws write RGB only: alpha is the PSP stencil plane (build_state
         * cmask), so it must keep the initial zero from the cleared frame. */
        if ((guest[idx] >> 24) != 0 || (guest[idx] & 0xFFFFFFu) != (want & 0xFFFFFFu)) {
            fprintf(stderr, "gpu coherence selftest [ring-wrap]: idx=%u (x=%u,y=%u) rgb got=%06x want=%06x "
                            "alpha=%02x\n", idx, idx % FB_W, idx / FB_W, guest[idx] & 0xFFFFFFu,
                    want & 0xFFFFFFu, guest[idx] >> 24);
            coherence_reset_targets();
            return 0;
        }
    }
    uint64_t sprites = s_cnt_spr - spr_before;
    unsigned long long submits = after.render_submits - before.render_submits;
    if (sprites != (uint64_t)pixels * passes) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: sprite count %llu\n",
                (unsigned long long)sprites);
        coherence_reset_targets();
        return 0;
    }
    /* The vertex arena overflows once per VERT_ARENA_VERTS, so this fixture forces many
     * arena flushes; more than one full slot cycle proves a recycled, fenced slot. */
    if (submits <= SUBMIT_FRAMES) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: only %llu render submits, slot ring did not recycle\n",
                submits);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s sprites=%llu verts=%llu render_submits=%llu\n",
           "ring-wrap", (unsigned long long)sprites,
           (unsigned long long)sprites * 6ull, submits);
    coherence_reset_targets();
    return 1;
}

/* Repeated uploads of different guest content into one target. Each upload is a batched
 * transfer-ring operation; the final GPU image must equal the last content with the one
 * sprite drawn on top, proving the staged copies were not overwritten before the copy ran. */
static int coherence_run_upload_sequence(void) {
    const uint32_t fba = 0x00120000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t bytes = stride * FB_H * 4u;
    const uint32_t rounds = 12u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);

    coherence_reset_targets();
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) guest[i] = wrap_pixel_rgba(0u, i);
    sr_gpu_vram_dirty(base, bytes);
    Target *t = target_color_acquire(fba, stride, 3, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu coherence selftest [upload-sequence]: could not establish target\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuReplayStats before, after;
    gegpu_replay_stats_get(&before);
    for (uint32_t k = 1; k <= rounds; k++) {
        for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) guest[i] = wrap_pixel_rgba(k, i);
        sr_gpu_vram_dirty(base, bytes);
        if (!target_upload(t)) {
            fprintf(stderr, "gpu coherence selftest [upload-sequence]: upload %u failed\n", k);
            coherence_reset_targets();
            return 0;
        }
    }
    wrap_state(base, stride);
    ge_set_gpu_hooks(&k_hooks);
    GeVtx p0, p1;
    const uint32_t c = 0xff102030u;
    wrap_vtx(&p0, 0.0f, 0.0f, c);
    wrap_vtx(&p1, 1.0f, 1.0f, c);
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [upload-sequence]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_replay_stats_get(&after);
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) {
        uint32_t want = i == 0 ? c : wrap_pixel_rgba(rounds, i);
        if (guest[i] != want) {
            fprintf(stderr, "gpu coherence selftest [upload-sequence]: idx=%u got=%08x want=%08x\n",
                    i, guest[i], want);
            coherence_reset_targets();
            return 0;
        }
    }
    printf("gpu coherence selftest: PASS %-22s uploads=%u reservations=%llu fallbacks=%llu\n",
           "upload-sequence", rounds,
           after.upload_ring_reservations - before.upload_ring_reservations,
           after.upload_ring_fallbacks - before.upload_ring_fallbacks);
    coherence_reset_targets();
    return 1;
}

/* ---- notified texture rewrite between two same-state draws ------------------------------
 *
 * A CPU or DMAC write into a cached texture reaches hook_vram_dirty. Invalidating the
 * texture entry alone is not enough: the cached state template still carries that
 * texture's descriptor, and a following draw whose GE registers are unchanged takes the
 * template fast path in state_get without consulting tex_get, so it samples the old
 * image. This fixture draws one sprite, rewrites the whole texture and notifies the
 * write, draws a second sprite with identical registers, and requires the first sprite
 * to keep the old texels and the second to show the new ones. */
static int coherence_run_texture_dirty_same_state(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t old_texel = 0xFF0000FFu, new_texel = 0xFF00FF00u;   /* ABGR red, green */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) tex[i] = old_texel;
    sr_gpu_vram_dirty(tex_base, 64u * 4u);

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    uint64_t invalidations_before = s_cnt_tex_invalidations;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-dirty-same-state]: first draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* The notified write under test: same address, same size, new bytes, no GE
     * register change and no CPU-batch boundary before the next draw. */
    for (unsigned i = 0; i < 64u; i++) tex[i] = new_texel;
    sr_gpu_vram_dirty(tex_base, 64u * 4u);

    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-dirty-same-state]: second draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }

    uint32_t left = guest[14u * stride + 14u] & rgb;
    uint32_t right = guest[14u * stride + 34u] & rgb;
    if (left != (old_texel & rgb) || right != (new_texel & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [texture-dirty-same-state]: left=%06x (want %06x) "
                "right=%06x (want %06x)%s\n",
                left, old_texel & rgb, right, new_texel & rgb,
                right == (old_texel & rgb)
                    ? " -- the second draw sampled the stale cached texture" : "");
        coherence_reset_targets();
        return 0;
    }
    if (s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-dirty-same-state]: expected one texture "
                "invalidation, counted %llu\n",
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before));
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s invalidations=1 old=%06x new=%06x\n",
           "texture-dirty-same-state", old_texel & rgb, new_texel & rgb);
    coherence_reset_targets();
    return 1;
}

/* The clean-page index of hook_vram_dirty (#282). A notified store into a page no cached texture
 * reads marks the page, so a second store there skips the texture scan; a texture created later
 * on that page must clear the mark, or the store that next changes its texels is skipped and the
 * draw after it samples the stale cached copy. */
static int coherence_run_texture_clean_page_index(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_a = 0x04040000u, tex_b = 0x04060000u;   /* different 4 KiB pages */
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t red = 0xFF0000FFu, blue = 0xFFFF0000u, green = 0xFF00FF00u;   /* ABGR */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *a = (uint32_t *)SR_HOST(tex_a);
    uint32_t *b = (uint32_t *)SR_HOST(tex_b);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) a[i] = red;
    sr_gpu_vram_dirty(tex_a, 64u * 4u);

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_a;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: first draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* Texture B's page holds no cached texture yet: the first store scans and marks it, the
     * second skips the scan. */
    uint64_t skips_before = s_cnt_tex_clean_skips;
    for (unsigned i = 0; i < 64u; i++) b[i] = blue;
    sr_gpu_vram_dirty(tex_b, 32u * 4u);
    sr_gpu_vram_dirty(tex_b + 32u * 4u, 32u * 4u);
    if (s_cnt_tex_clean_skips != skips_before + 1u) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: expected the second store "
                "to skip the scan, skipped %llu\n",
                (unsigned long long)(s_cnt_tex_clean_skips - skips_before));
        coherence_reset_targets();
        return 0;
    }

    s_ge->tex_addr = tex_b;
    ge_state_note_external_write();   /* a TEXADDR command would bump the state serial */
    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: second draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* The store under test: B's texels change with no GE register change before the draw. */
    uint64_t invalidations_before = s_cnt_tex_invalidations;
    for (unsigned i = 0; i < 64u; i++) b[i] = green;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    /* B is now the only entry on its page and invalid: a second store there skips the scan. */
    uint64_t quiet_before = s_cnt_tex_quiet_skips;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    if (s_cnt_tex_quiet_skips != quiet_before + 1u) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: expected a quiet-page skip, "
                "skipped %llu\n", (unsigned long long)(s_cnt_tex_quiet_skips - quiet_before));
        coherence_reset_targets();
        return 0;
    }
    wrap_vtx(&p0, 50.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 58.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: third draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t left = guest[14u * stride + 14u] & rgb;
    uint32_t mid = guest[14u * stride + 34u] & rgb;
    uint32_t right = guest[14u * stride + 54u] & rgb;
    if (left != (red & rgb) || mid != (blue & rgb) || right != (green & rgb) ||
        s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-clean-page-index]: left=%06x mid=%06x right=%06x "
                "(want %06x %06x %06x), invalidations=%llu (want 1)%s\n",
                left, mid, right, red & rgb, blue & rgb, green & rgb,
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before),
                right == (blue & rgb) ? " -- the store after the texture was created was skipped" : "");
        coherence_reset_targets();
        return 0;
    }
    /* The third draw re-uploaded B, so its page is no longer quiet: the next store must
     * invalidate it again, and the draw after it must show the new texels. */
    const uint32_t yellow = 0xFF00FFFFu;
    quiet_before = s_cnt_tex_quiet_skips;
    invalidations_before = s_cnt_tex_invalidations;
    for (unsigned i = 0; i < 64u; i++) b[i] = yellow;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    wrap_vtx(&p0, 70.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 78.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: fourth draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t last = guest[14u * stride + 74u] & rgb;
    if (last != (yellow & rgb) || s_cnt_tex_quiet_skips != quiet_before ||
        s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-clean-page-index]: after re-upload last=%06x (want %06x), "
                "quiet skips=%llu (want 0), invalidations=%llu (want 1)%s\n",
                last, yellow & rgb, (unsigned long long)(s_cnt_tex_quiet_skips - quiet_before),
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before),
                last == (green & rgb) ? " -- a stale quiet mark skipped the store" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s clean=1 quiet=1 invalidations=2\n", "texture-clean-page-index");
    coherence_reset_targets();
    return 1;
}

/* A texture in main RAM changes with no notification (guest CPU stores to main RAM are not
 * tracked), so the content hash is what finds it: one texel in the upper half of a hashed
 * 8-byte word turns from red to green between two submissions, and the second draw shows
 * it. */
static int coherence_run_texture_ram_unnotified_change(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x08530000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t red = 0xFF0000FFu, green = 0xFF00FF00u;   /* ABGR */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) tex[i] = red;

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    int ok = hook_sprite(&p0, &p1, 0);
    /* A submission (a frame's present, say) separates the store from the next draw, as in a
     * title; within one submission an unchanged draw state reuses its template, texture
     * included. */
    ok = ok && submit_pending();
    tex[1] = green;          /* texel (1, 0): bytes 4-7 of the first 8-byte word; no notification */
    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [texture-ram-unnotified]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[10u * stride + 11u] & rgb;
    uint32_t second = guest[10u * stride + 31u] & rgb;
    if (first != (red & rgb) || second != (green & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [texture-ram-unnotified]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, red & rgb, second, green & rgb,
                second == (red & rgb) ? " -- the second draw reused the stale decode" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "texture-ram-unnotified", first, second);
    coherence_reset_targets();
    return 1;
}

/* One CLUT8 texture drawn twice through the same palette bytes with a different CLUT format
 * word: the palette identity in the texture cache key covers the format, so the second draw
 * decodes again (index >> 1 picks another entry) instead of reusing the first decode. The
 * case writes GeState directly, outside a list, so it notes each write as such a writer must:
 * otherwise state_get keeps the first draw's template, texture included, and never reaches
 * the key. clut-format-list sends the same change as a CLUTFORMAT command. */
static int coherence_run_clut_format_same_palette(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t index = 6u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint8_t *tex = (uint8_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    memset(tex, (int)index, 16u * 16u);
    sr_gpu_vram_dirty(tex_base, 16u * 16u);

    wrap_state(base, stride);
    for (uint32_t j = 0; j < 256u; j++) {
        uint32_t entry = 0xFF000000u | wrap_hash(0x51u + j);
        memcpy(&s_ge->clutram[j * 4u], &entry, 4);
    }
    uint32_t want_first, want_second;
    memcpy(&want_first, &s_ge->clutram[index * 4u], 4);
    memcpy(&want_second, &s_ge->clutram[(index >> 1) * 4u], 4);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 16;
    s_ge->tex_fmt = 5;       /* CLUT8 */
    s_ge->tex_w = 16;
    s_ge->tex_h = 16;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    s_ge->clut_fmt = 3u | (0xFFu << 8);                 /* 8888, shift 0, mask 0xFF */
    ge_state_note_external_write();
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 26.0f, 26.0f, white); p1.u = 16.0f; p1.v = 16.0f;
    int ok = hook_sprite(&p0, &p1, 0);
    s_ge->clut_fmt = 3u | (1u << 2) | (0xFFu << 8);     /* same bytes, shift 1 */
    ge_state_note_external_write();
    wrap_vtx(&p0, 40.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 56.0f, 26.0f, white); p1.u = 16.0f; p1.v = 16.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [clut-format-same-palette]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[18u * stride + 18u] & rgb;
    uint32_t second = guest[18u * stride + 48u] & rgb;
    if (first != (want_first & rgb) || second != (want_second & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [clut-format-same-palette]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, want_first & rgb, second, want_second & rgb,
                second == (want_first & rgb)
                    ? " -- the second draw reused the first format's decode" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "clut-format-same-palette", first, second);
    coherence_reset_targets();
    return 1;
}

/* Through mode takes z as the GE's unsigned 16-bit depth. A title that writes float positions
 * there may put any float in z: two triangles at z = -1.0 (a 2D overlay drawn last, with the
 * depth test off) must still be drawn. The Vulkan path maps z / 65535 to the clip depth, so an
 * unclamped -1.0 lies outside the depth range and every such triangle was clipped away. A
 * fraction must give the same depth on both renderers too: ge.c truncates a pixel's depth and the
 * Vulkan path rounds z / 65535 to the nearest step, so an undecoded 100.7 was 100 on one and 101
 * on the other. An infinite z saturates and is drawn, a NaN z is drawn (PSP-GE-THROUGHZ-001), and a
 * transform-mode z is left alone. */
typedef struct ThroughFloatVtx { uint32_t c; float x, y, z; } ThroughFloatVtx;

static uint32_t tfz_f24(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 8; }

/* Does a float z of `z` pass the depth test `zfunc` against the stored depth `depth`? One list
 * draws a through-mode quad at z = depth (an integer) with the depth test ALWAYS and depth writes
 * on, then a second quad over it at `z` with the test `zfunc` and depth writes off. Returns 1 when
 * the second quad's colour is there, 0 when the first one's is, -1 when neither is or the readback
 * failed. `gpu`: through the Vulkan hooks, else ge.c alone. `transform`: the second quad is drawn
 * in transform mode, with identity matrices and a viewport that maps a clip z to
 * z * 30000 + 32768, so its z is a model coordinate the decoder must leave alone. `sprite`: the
 * second quad is one through-mode sprite (two vertices) instead of two triangles. */
static int through_float_z_passes(float z, uint32_t depth, int gpu, uint32_t zfunc, int transform,
                                  int sprite) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const uint32_t vaddr = 0x00101800u, list_addr = 0x00100800u;
    const uint32_t under = 0xFF3366CCu, over = 0xFF20C040u;   /* ABGR: opaque */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    memset(s_ge, 0, sizeof(*s_ge));
    ThroughFloatVtx *vtx = (ThroughFloatVtx *)SR_HOST(vaddr);
    for (int q = 0; q < 2; q++) {
        const uint32_t c = q ? over : under;
        const float qz = q ? z : (float)depth;
        /* through mode: pixels 16..48; transform mode: clip coordinates of pixels 12..60, 13.6..54.4 */
        const int clip = q && transform;
        const float x0 = clip ? -0.95f : 16.0f, x1 = clip ? -0.75f : 48.0f;
        const float y0 = clip ? 0.9f : 16.0f, y1 = clip ? 0.6f : 48.0f;
        ThroughFloatVtx *v = vtx + 6 * q;
        v[0] = (ThroughFloatVtx){ c, x0, y0, qz };
        v[1] = (ThroughFloatVtx){ c, x1, y0, qz };
        v[2] = (ThroughFloatVtx){ c, x0, y1, qz };
        v[3] = (ThroughFloatVtx){ c, x1, y0, qz };
        v[4] = (ThroughFloatVtx){ c, x1, y1, qz };
        v[5] = (ThroughFloatVtx){ c, x0, y1, qz };
        if (q && sprite) v[1] = v[4];                   /* sprite: top-left, bottom-right */
    }
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0x9E, 0x00080000u);                         /* depth buffer */
    DL_EMIT(0x9F, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x1E, 0);                                   /* no texture */
    DL_EMIT(0x1D, 0);                                   /* no culling */
    DL_EMIT(0x21, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x23, 1);                                   /* depth test */
    DL_EMIT(0xDE, 1);                                   /* ALWAYS */
    DL_EMIT(0xE7, 0);                                   /* depth writes on */
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | 479);
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD6, 0);
    DL_EMIT(0xD7, 0xFFFF);
    DL_EMIT(0x12, (1u << 23) | (7u << 2) | (3u << 7)); /* through, 8888 colour, float pos */
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (3u << 16) | 6u);
    if (transform) {
        static const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        static const float ident44[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        DL_EMIT(0x3A, 0);                               /* world, view and projection: identity */
        for (int k = 0; k < 12; k++) DL_EMIT(0x3B, tfz_f24(ident43[k]));
        DL_EMIT(0x3C, 0);
        for (int k = 0; k < 12; k++) DL_EMIT(0x3D, tfz_f24(ident43[k]));
        DL_EMIT(0x3E, 0);
        for (int k = 0; k < 16; k++) DL_EMIT(0x3F, tfz_f24(ident44[k]));
        DL_EMIT(0x42, tfz_f24(240.0f));
        DL_EMIT(0x43, tfz_f24(-136.0f));
        DL_EMIT(0x44, tfz_f24(30000.0f));
        DL_EMIT(0x45, tfz_f24(2048.0f));
        DL_EMIT(0x46, tfz_f24(2048.0f));
        DL_EMIT(0x47, tfz_f24(32768.0f));
        DL_EMIT(0x4C, (2048u - 240u) << 4);
        DL_EMIT(0x4D, (2048u - 136u) << 4);
        DL_EMIT(0x12, (7u << 2) | (3u << 7));           /* transform mode, 8888 colour, float pos */
    }
    DL_EMIT(0xDE, zfunc);
    DL_EMIT(0xE7, 1);                                   /* depth writes off */
    if (sprite) DL_EMIT(0x04, (6u << 16) | 2u);         /* one sprite from the next two vertices */
    else DL_EMIT(0x04, (3u << 16) | 6u);                /* the next six vertices */
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    ge_set_gpu_hooks(gpu ? &k_hooks : NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (gpu) {
        GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                                   .width = 480, .height = 272 };
        if (!gegpu_sync_guest_fb(&desc)) return -1;
    }
    const uint32_t px = guest[32u * stride + 32u] & 0x00FFFFFFu;
    return px == (over & 0x00FFFFFFu) ? 1 : px == (under & 0x00FFFFFFu) ? 0 : -1;
}

static int coherence_run_through_float_z(void) {
    const char *name = "through-float-z";
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const uint32_t vaddr = 0x00101800u, list_addr = 0x00100800u;
    const uint32_t colour = 0xFF3366CCu;               /* ABGR: opaque */
    const float zs[2] = { -1.0f, 70000.0f };           /* below and above the 16-bit range */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    int ok = 1;
    for (int k = 0; ok && k < 2; k++) {
        coherence_reset_targets();
        memset(guest, 0, stride * FB_H * 4u);
        sr_gpu_vram_dirty(base, stride * FB_H * 4u);
        memset(s_ge, 0, sizeof(*s_ge));
        ThroughFloatVtx *vtx = (ThroughFloatVtx *)SR_HOST(vaddr);
        const float x0 = 16.0f, y0 = 16.0f, x1 = 48.0f, y1 = 48.0f, z = zs[k];
        vtx[0] = (ThroughFloatVtx){ colour, x0, y0, z };
        vtx[1] = (ThroughFloatVtx){ colour, x1, y0, z };
        vtx[2] = (ThroughFloatVtx){ colour, x0, y1, z };
        vtx[3] = (ThroughFloatVtx){ colour, x1, y0, z };
        vtx[4] = (ThroughFloatVtx){ colour, x1, y1, z };
        vtx[5] = (ThroughFloatVtx){ colour, x0, y1, z };
        uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
        int p = 0;
        #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
        DL_EMIT(0x10, 0);
        DL_EMIT(0x9C, base & 0x00FFFFFFu);
        DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
        DL_EMIT(0xD2, 3);
        DL_EMIT(0xD3, 0);
        DL_EMIT(0x1E, 0);                                   /* no texture */
        DL_EMIT(0x1D, 0);                                   /* no culling */
        DL_EMIT(0x21, 0);
        DL_EMIT(0x22, 0);
        DL_EMIT(0x23, 0);                                   /* no depth test */
        DL_EMIT(0xD4, 0);
        DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
        DL_EMIT(0x12, (1u << 23) | (7u << 2) | (3u << 7)); /* through, 8888 colour, float pos */
        DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
        DL_EMIT(0x04, (3u << 16) | 6u);                     /* two triangles */
        DL_EMIT(0x0F, 0);
        DL_EMIT(0x0C, 0);
        #undef DL_EMIT
        ge_set_gpu_hooks(&k_hooks);
        ge_run_list(list_addr, 0);
        GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                                   .width = 480, .height = 272 };
        if (!gegpu_sync_guest_fb(&desc)) {
            fprintf(stderr, "gpu coherence selftest [%s]: GPU readback failed\n", name);
            coherence_reset_targets();
            return 0;
        }
        const uint32_t inside = guest[32u * stride + 32u], outside = guest[8u * stride + 8u];
        /* RGB only: the alpha byte of a 32-bit target holds stencil, which this draw leaves. */
        if ((inside & 0x00FFFFFFu) != (colour & 0x00FFFFFFu) || (outside & 0x00FFFFFFu) != 0u) {
            fprintf(stderr, "gpu coherence selftest [%s]: z=%.1f: inside=0x%08x (want 0x%08x) "
                            "outside=0x%08x%s\n", name, (double)z, inside, colour, outside,
                    (inside & 0x00FFFFFFu) == 0u ? " -- the triangles were clipped away" : "");
            ok = 0;
        }
    }
    /* The depth each renderer takes for a z with a fraction, for one just outside the range and for
     * an infinite one: the depth test EQUAL must pass against `want` and fail against its
     * neighbour. An infinite z was dropped by the non-finite vertex rule before the decoder
     * saturated it; it is drawn now, at the end of the range it lies beyond. */
    static const struct { float z; uint32_t want; } k_frac[] = {
        { 100.7f, 100u }, { 100.3f, 100u }, { 0.99f, 0u }, { 65534.7f, 65534u },
        { -0.5f, 0u }, { 65535.5f, 65535u }, { INFINITY, 65535u }, { -INFINITY, 0u },
    };
    for (unsigned i = 0; i < sizeof k_frac / sizeof k_frac[0]; i++) {
        const uint32_t want = k_frac[i].want, other = want < 65535u ? want + 1u : want - 1u;
        for (int gpu = 1; gpu >= 0; gpu--) {
            const int at_want = through_float_z_passes(k_frac[i].z, want, gpu, 2u, 0, 0);
            const int at_other = through_float_z_passes(k_frac[i].z, other, gpu, 2u, 0, 0);
            if (at_want == 1 && at_other == 0) continue;
            fprintf(stderr, "gpu coherence selftest [%s]: z=%.2f on %s: equal to depth %u: %d (want 1), "
                            "to depth %u: %d (want 0)\n", name, (double)k_frac[i].z,
                    gpu ? "the Vulkan path" : "ge.c", want, at_want, other, at_other);
            ok = 0;
        }
    }
    /* A NaN z is drawn. PSP-GE-THROUGHZ-001 (docs/HARDWARE_ORACLE.md, 2026-11-10, PSP-3000): a
     * through-mode 16x16 sprite with both vertices at z = NaN, depth test ALWAYS, is drawn in full.
     * The depth the GE stores for it was not read; depth 0 is UNMEASURED (the decoder's rule for a z
     * below the range), held here only so both renderers take the same one: the depth test EQUAL
     * passes against 0 and fails against 1. Two triangles at NaN take the same decoder path. */
    for (int gpu = 1; gpu >= 0; gpu--) {
        const int sprite_drawn = through_float_z_passes(NAN, 100u, gpu, 1u, 0, 1);
        const int tris_drawn = through_float_z_passes(NAN, 100u, gpu, 1u, 0, 0);
        const int at_zero = through_float_z_passes(NAN, 0u, gpu, 2u, 0, 1);
        const int at_one = through_float_z_passes(NAN, 1u, gpu, 2u, 0, 1);
        if (sprite_drawn == 1 && tris_drawn == 1 && at_zero == 1 && at_one == 0) continue;
        fprintf(stderr, "gpu coherence selftest [%s]: on %s at z=NaN: a sprite under the depth test ALWAYS was "
                        "drawn: %d (want 1), two triangles: %d (want 1); EQUAL to depth 0: %d (want 1), to "
                        "depth 1: %d (want 0)\n", name, gpu ? "the Vulkan path" : "ge.c", sprite_drawn,
                tris_drawn, at_zero, at_one);
        ok = 0;
    }
    /* Through mode only: a transform-mode z is a model coordinate, and the decoder must leave it
     * alone. With identity matrices and a viewport of z * 30000 + 32768, a model z of -0.5 is depth
     * 17768 and one of 0.25 is depth 40268; saturated or truncated in the decoder, both would be
     * depth 32768. */
    static const struct { float z; uint32_t want; } k_scope[] = { { -0.5f, 17768u }, { 0.25f, 40268u } };
    for (unsigned i = 0; i < sizeof k_scope / sizeof k_scope[0]; i++)
        for (int gpu = 1; gpu >= 0; gpu--) {
            const int at_want = through_float_z_passes(k_scope[i].z, k_scope[i].want, gpu, 2u, 1, 0);
            const int at_zero = through_float_z_passes(k_scope[i].z, 32768u, gpu, 2u, 1, 0);
            if (at_want == 1 && at_zero == 0) continue;
            fprintf(stderr, "gpu coherence selftest [%s]: transform-mode z=%.2f on %s: equal to depth %u: %d "
                            "(want 1), to depth 32768: %d (want 0) -- the decoder changed a transform-mode z\n",
                    name, (double)k_scope[i].z, gpu ? "the Vulkan path" : "ge.c", k_scope[i].want, at_want,
                    at_zero);
            ok = 0;
        }
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS %-22s z=-1.0 and z=70000.0 drawn; a fraction and an infinity "
               "are the same depth on both renderers; NaN drawn; transform-mode z untouched\n", name);
    return ok;
}

/* The same palette change sent the way a title sends it: one list loads the palette, draws a
 * CLUT8 sprite, changes only CLUTFORMAT (shift 1), and draws again. Every command moves the
 * GE state serial, so the second sprite must rebuild its state and decode through index >> 1. */
static int coherence_run_clut_format_list(void) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u, clut_base = 0x04041000u;
    const uint32_t stride = 512u, rgb = 0x00FFFFFFu, index = 6u;
    const uint32_t vaddr = 0x00101800u, list_addr = 0x00100800u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    memset(SR_HOST(tex_base), (int)index, 16u * 16u);
    sr_gpu_vram_dirty(tex_base, 16u * 16u);
    uint32_t *clut = (uint32_t *)SR_HOST(clut_base);
    for (uint32_t j = 0; j < 256u; j++) clut[j] = 0xFF000000u | wrap_hash(0x51u + j);
    sr_gpu_vram_dirty(clut_base, 256u * 4u);
    const uint32_t want_first = clut[index] & rgb, want_second = clut[index >> 1] & rgb;

    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ 0.0f, 0.0f, 10.0f, 10.0f, 0.0f };
    vtx[1] = (RawSpriteVtx){ 16.0f, 16.0f, 26.0f, 26.0f, 0.0f };
    vtx[2] = (RawSpriteVtx){ 0.0f, 0.0f, 40.0f, 10.0f, 0.0f };
    vtx[3] = (RawSpriteVtx){ 16.0f, 16.0f, 56.0f, 26.0f, 0.0f };
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 16u | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (4u << 8) | 4u);
    DL_EMIT(0xC2, 0);
    DL_EMIT(0xC3, 5);                                   /* CLUT8 */
    DL_EMIT(0xC6, 0);                                   /* NEAREST */
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));                      /* REPLACE, RGBA */
    DL_EMIT(0xB0, clut_base & 0x00FFFFFFu);
    DL_EMIT(0xB1, (clut_base & 0xFF000000u) >> 8);
    DL_EMIT(0xC5, 3u | (0xFFu << 8));                   /* 8888, shift 0, mask 0xFF */
    DL_EMIT(0xC4, 32u);                                 /* 256 entries x 4 bytes */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0xC5, 3u | (1u << 2) | (0xFFu << 8));       /* same palette bytes, shift 1 */
    DL_EMIT(0x01, (vaddr + 2u * (uint32_t)sizeof(RawSpriteVtx)) & 0x00FFFFFFu);
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [clut-format-list]: GPU readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[18u * stride + 18u] & rgb;
    uint32_t second = guest[18u * stride + 48u] & rgb;
    coherence_reset_targets();
    if (first != want_first || second != want_second) {
        fprintf(stderr,
                "gpu coherence selftest [clut-format-list]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, want_first, second, want_second,
                second == want_first ? " -- the second draw reused the first format's decode" : "");
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "clut-format-list", first, second);
    return 1;
}

/* ---- blend parity: Vulkan vs ge.c (arbiter) vs the R6 colour product --------------------- */

#define PARITY_GRID 16u                  /* 16 x 16 sprites, 4x4 pixels each */
#define PARITY_ORIGIN 64u
#define PARITY_FF_BOUND 2                /* analytic bound for truncating vs rounding blends */

static uint32_t parity_src(uint32_t sprite) { return wrap_hash(0x51u + sprite * 2654435761u); }
static uint32_t parity_bg(uint32_t idx) { return wrap_hash(0xB6u + idx * 40503u); }

static int parity_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ge.c factor_component(): the blend factor for one channel, integer 0..255. */
static int parity_factor(int f, int src_side, int chan, const int s[3], int sa,
                         const int d[3], int da, uint32_t fixa, uint32_t fixb) {
    int sv = s[chan], dv = d[chan];
    int fixed_a = (int)((fixa >> (8 * chan)) & 0xFFu), fixed_b = (int)((fixb >> (8 * chan)) & 0xFFu);
    switch (f & 0xF) {
        case 0: return src_side ? dv : sv;
        case 1: return 255 - (src_side ? dv : sv);
        case 2: return sa;
        case 3: return 255 - sa;
        case 4: return da;
        case 5: return 255 - da;
        case 6: return sa * 2;          /* doubled alphas are not clamped (PSP-GE-RASTER-001) */
        case 7: return 255 - sa * 2;
        case 8: return da * 2;
        case 9: return 255 - da * 2;
        default: return src_side ? fixed_a : fixed_b;
    }
}

/* The same blend equation with the R6 colour product ((2x+1)(2s+1))>>10 for every
 * multiply (ge_raster_ref.c sr_ge_color_mul for factors 0..255), extended to the doubled
 * alphas' 0..510 with the factor's sign applied afterwards (PSP-GE-RASTER-001). Min, max and
 * absdiff ignore the factors and equations 6 and 7 pass the source through (as ge.c does). */
static int parity_r6_mul(int v, int f) {
    int m = ((2 * v + 1) * (2 * (f < 0 ? -f : f) + 1)) >> 10;
    return f < 0 ? -m : m;
}

static int parity_r6_chan(int s, int d, int sf, int df, int eq) {
    int sv = parity_r6_mul(s, sf);
    int dv = parity_r6_mul(d, df);
    switch (eq & 7) {
        case 1: return parity_clamp8(sv - dv);
        case 2: return parity_clamp8(dv - sv);
        case 3: return s < d ? s : d;
        case 4: return s > d ? s : d;
        case 5: return s > d ? s - d : d - s;
        case 6: case 7: return s;
        default: return parity_clamp8(sv + dv);
    }
}

/* FIX blend constants for every factor/equation cell: two distinct RGB triples whose
 * channels all differ, so a dual-FIX state (sfp and dfp both FIX, constants unequal) is
 * exercised and a channel swap or a constant mix-up changes the picture. */
#define PARITY_FIXA 0x3F7F9Fu
#define PARITY_FIXB 0x7F5F2Fu

static uint32_t parity_list(uint32_t base, uint32_t vaddr, uint32_t blend_mode, uint32_t fixa,
                            uint32_t fixb, uint32_t sprites) {
    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    uint32_t stride = 512u;
    int p = 0;
    #define PARITY_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    PARITY_EMIT(0x10, 0);
    PARITY_EMIT(0x4C, 0);
    PARITY_EMIT(0x9C, base & 0x00FFFFFFu);
    PARITY_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    PARITY_EMIT(0xD2, 3);
    PARITY_EMIT(0xD3, 0);
    PARITY_EMIT(0x1D, 0);
    PARITY_EMIT(0x1E, 0);
    PARITY_EMIT(0x1F, 0);
    PARITY_EMIT(0x20, 0);
    PARITY_EMIT(0x22, 0);
    PARITY_EMIT(0x23, 0);
    PARITY_EMIT(0xE7, 1);
    PARITY_EMIT(0xE8, 0);
    PARITY_EMIT(0xE9, 0);
    PARITY_EMIT(0x15, 0);
    PARITY_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    PARITY_EMIT(0xD4, 0);
    PARITY_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    PARITY_EMIT(0x21, 1);
    PARITY_EMIT(0xDF, blend_mode & 0xFFFu);
    PARITY_EMIT(0xE0, fixa);
    PARITY_EMIT(0xE1, fixb);
    /* 8888 colour, float position, transform bypass (through) */
    PARITY_EMIT(0x12, (7u << 2) | (3u << 7) | (1u << 23));
    PARITY_EMIT(0x01, vaddr & 0x00FFFFFFu);
    PARITY_EMIT(0x04, (6u << 16) | (sprites * 2u));
    PARITY_EMIT(0x0F, 0);
    PARITY_EMIT(0x0C, 0);
    #undef PARITY_EMIT
    return list_addr;
}

/* Guest vertex layout for VERTEXTYPE 8888 colour + float position: colour first. */
typedef struct ParityGuestVtx { uint32_t rgba; float x, y, z; } ParityGuestVtx;

static void parity_vertices(uint32_t vaddr, uint32_t sprites) {
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(vaddr);
    for (uint32_t i = 0; i < sprites; i++) {
        uint32_t gx = i % PARITY_GRID, gy = i / PARITY_GRID;
        float x0 = (float)(PARITY_ORIGIN + 4u * gx), y0 = (float)(PARITY_ORIGIN + 4u * gy);
        uint32_t c = parity_src(i);
        v[2 * i]     = (ParityGuestVtx){ c, x0, y0, 0.0f };
        v[2 * i + 1] = (ParityGuestVtx){ c, x0 + 4.0f, y0 + 4.0f, 0.0f };
    }
}

static void parity_fill_background(uint32_t base) {
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    for (uint32_t y = 0; y < FB_H; y++)
        for (uint32_t x = 0; x < FB_W; x++) guest[y * FB_W + x] = parity_bg(y * FB_W + x);
    sr_gpu_vram_dirty(base, FB_W * FB_H * 4u);
}

typedef struct ParityTally {
    unsigned combos, pixels_nonzero;
    unsigned max_gpu_vs_sw, max_gpu_vs_r6, max_sw_vs_r6;
} ParityTally;

static int coherence_run_blend_parity(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t vaddr = 0x00101000u;
    const uint32_t stride = 512u;
    const uint32_t sprites = PARITY_GRID * PARITY_GRID;
    const uint32_t bytes = FB_W * FB_H * 4u;
    uint32_t *gpu_out = (uint32_t *)malloc(bytes);
    if (!gpu_out) return 0;
    ParityTally shader = {0}, ff = {0};
    int ok = 1;

    parity_vertices(vaddr, sprites);
    for (uint32_t eq = 0; eq < 8 && ok; eq++) {
        for (uint32_t sfp = 0; sfp <= 10 && ok; sfp++) {
            for (uint32_t dfp = 0; dfp <= 10 && ok; dfp++) {
                const uint32_t fixa = PARITY_FIXA, fixb = PARITY_FIXB;
                const uint32_t mode = sfp | (dfp << 4) | (eq << 8);
                const uint32_t list = parity_list(base, vaddr, mode, fixa, fixb, sprites);
                /* the matrix draws onto an 8888 framebuffer with dithering off, so
                 * build_state's routing reduces to blend_needs_shader() */
                int shader_path = blend_needs_shader(sfp, dfp, eq, fixa, fixb);
                ParityTally *tally = shader_path ? &shader : &ff;

                /* Vulkan path */
                coherence_reset_targets();
                parity_fill_background(base);
                ge_set_gpu_hooks(&k_hooks);
                ge_run_list(list, 0);
                GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                                           .width = 480, .height = FB_H };
                if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) { ok = 0; break; }
                memcpy(gpu_out, SR_HOST(base), bytes);

                /* ge.c software path (the arbiter) */
                coherence_reset_targets();
                parity_fill_background(base);
                ge_set_gpu_hooks(NULL);
                ge_run_list(list, 0);
                ge_set_gpu_hooks(&k_hooks);
                const uint32_t *sw_out = (const uint32_t *)SR_HOST(base);

                unsigned max_gs = 0, max_gr = 0, max_sr = 0;
                for (uint32_t sprite = 0; sprite < sprites; sprite++) {
                    uint32_t sc = parity_src(sprite);
                    int s[3] = { (int)(sc & 0xFF), (int)((sc >> 8) & 0xFF), (int)((sc >> 16) & 0xFF) };
                    int sa = (int)(sc >> 24);
                    for (uint32_t py = 0; py < 4u; py++) {
                        for (uint32_t px = 0; px < 4u; px++) {
                            uint32_t idx = (PARITY_ORIGIN + 4u * (sprite / PARITY_GRID) + py) * FB_W +
                                           PARITY_ORIGIN + 4u * (sprite % PARITY_GRID) + px;
                            uint32_t dw = parity_bg(idx);
                            int d[3] = { (int)(dw & 0xFF), (int)((dw >> 8) & 0xFF), (int)((dw >> 16) & 0xFF) };
                            int da = (int)(dw >> 24);
                            uint32_t g = gpu_out[idx], w = sw_out[idx];
                            for (int ch = 0; ch < 4; ch++) {
                                int gv = (int)((g >> (8 * ch)) & 0xFF), wv = (int)((w >> (8 * ch)) & 0xFF);
                                if (abs(gv - wv) > (int)max_gs) max_gs = (unsigned)abs(gv - wv);
                                if (gv != wv) tally->pixels_nonzero++;
                            }
                            for (int ch = 0; ch < 3; ch++) {
                                int sf = parity_factor((int)sfp, 1, ch, s, sa, d, da, fixa, fixb);
                                int df = parity_factor((int)dfp, 0, ch, s, sa, d, da, fixa, fixb);
                                int ref = parity_r6_chan(s[ch], d[ch], sf, df, (int)eq);
                                int gv = (int)((g >> (8 * ch)) & 0xFF), wv = (int)((w >> (8 * ch)) & 0xFF);
                                if (abs(gv - ref) > (int)max_gr) max_gr = (unsigned)abs(gv - ref);
                                if (abs(wv - ref) > (int)max_sr) max_sr = (unsigned)abs(wv - ref);
                            }
                        }
                    }
                }
                tally->combos++;
                if (max_gs > tally->max_gpu_vs_sw) tally->max_gpu_vs_sw = max_gs;
                if (max_gr > tally->max_gpu_vs_r6) tally->max_gpu_vs_r6 = max_gr;
                if (max_sr > tally->max_sw_vs_r6) tally->max_sw_vs_r6 = max_sr;

                if ((shader_path && max_gs != 0) || (!shader_path && max_gs > PARITY_FF_BOUND)) {
                    /* first deviating pixel, with its inputs, for the failure report */
                    for (uint32_t i = 0; i < sprites * 16u; i++) {
                        uint32_t sprite = i / 16u, px = i % 4u, py = (i / 4u) % 4u;
                        uint32_t idx = (PARITY_ORIGIN + 4u * (sprite / PARITY_GRID) + py) * FB_W +
                                       PARITY_ORIGIN + 4u * (sprite % PARITY_GRID) + px;
                        if (gpu_out[idx] == sw_out[idx]) continue;
                        fprintf(stderr, "gpu coherence selftest [blend-parity]: sfp=%u dfp=%u eq=%u "
                                        "x=%u y=%u gpu=%08x ge_c=%08x src=%08x dst=%08x\n",
                                sfp, dfp, eq, idx % FB_W, idx / FB_W, gpu_out[idx], sw_out[idx],
                                parity_src(sprite), parity_bg(idx));
                        break;
                    }
                    fprintf(stderr, "gpu coherence selftest [blend-parity]: %s blend deviates from ge.c "
                                    "by %u (bound %u)\n", shader_path ? "shader" : "fixed-function",
                            max_gs, shader_path ? 0u : (unsigned)PARITY_FF_BOUND);
                    ok = 0;
                }
                coherence_reset_targets();
            }
        }
    }
    ge_set_gpu_hooks(&k_hooks);
    free(gpu_out);
    if (!ok) return 0;
    printf("gpu coherence selftest: PASS %-22s combos=%u max_delta_vs_ge_c=0 (shader blend, exact)\n",
           "blend-parity-shader", shader.combos);
    printf("gpu coherence selftest: PASS %-22s combos=%u max_delta_vs_ge_c=%u (fixed-function, bound %d)\n",
           "blend-parity-ff", ff.combos, ff.max_gpu_vs_sw, PARITY_FF_BOUND);
    printf("gpu coherence selftest: INFO r6-reference shader gpu_vs_r6=%u sw_vs_r6=%u "
           "ff gpu_vs_r6=%u sw_vs_r6=%u\n",
           shader.max_gpu_vs_r6, shader.max_sw_vs_r6, ff.max_gpu_vs_r6, ff.max_sw_vs_r6);
    return 1;
}

/* ---- submission granularity (#827, issue #323) --------------------------------------------
 *
 * SUBMIT_BATCH_MAX caps how many recorded GE-ring ops -- render batches, destination
 * snapshot copies and ring uploads -- share one vkQueueSubmit. This fixture proves the cap is
 * a submission granularity and not an ordering boundary. It builds one GE display list and
 * runs it through ge.c's interpreter and the production hooks, so nothing here reaches the
 * backend by a route the player does not take. The list runs once per cap, and every frame
 * is compared byte for byte with ge.c's software rasterizer, the reference. The list chains
 * GPU-only dependencies that would change pixels if a longer command buffer lost or
 * reordered one:
 *   - overlapping dual-FIX shader-blend sprites with a new FIX constant per draw: each draw
 *     submits the previous render op and records a destination snapshot copy, the
 *     render+snapshot pairs behind the "mixed" player-route submits in #827;
 *   - CLUT8 sprites with a palette reload per draw (a new cache entry and a ring upload into
 *     the open command buffer each), then a GE block transfer that rewrites the texels
 *     through ge.c's CPU copy and dirty notification, and draws that re-upload those cached
 *     images in place after earlier draws in the same command buffer sampled them;
 *   - render-to-texture: draws alternate between a second target and the main one, which
 *     samples the second target's newest pixels directly.
 * No CPU-visible boundary falls inside the list, so the cap is the only thing that splits
 * it: a run must cost exactly ops/cap submits and leave ops%cap ops open. That also pins
 * SR_GPU_SUBMIT_BATCH=8 to the legacy schedule, op for op. */
#define SB_FBA_A    0x00000000u            /* main target (VRAM offset) */
#define SB_FBA_B    0x00088000u            /* render-to-texture source, after A's rows */
#define SB_ZBA      0x00110000u
#define SB_LIST     0x08400000u            /* synthetic main RAM from here on */
#define SB_VTX      0x08480000u
#define SB_PAL      0x08500000u            /* 16-entry 8888 palettes, 64 bytes each */
#define SB_TEX      0x08510000u            /* 16x16 CLUT8 texels, one 256-byte copy per run */
#define SB_XFER_SRC 0x08520000u            /* the block transfer's replacement texels */
#define SB_FIXB     0x5F7F3Fu

typedef struct SubmitBatchShape {
    uint32_t blend_draws;                  /* overlapping shader-blend sprites */
    uint32_t spaced_draws;                 /* shader-blend sprites in disjoint cells */
    uint32_t tail_draws;                   /* shader-blend sprites over the CLUT sprites */
    uint32_t clut_draws;                   /* CLUT8 sprites, one palette reload each */
    uint32_t rewrite_draws;                /* CLUT8 sprites after the block-transfer rewrite */
    uint32_t rtt_rounds;                   /* draw into B, then sample B from A */
    uint32_t size;                         /* shader-blend and target-B sprite edge */
} SubmitBatchShape;

typedef struct SubmitBatchList { uint32_t *dl; uint32_t n; uint32_t voff; } SubmitBatchList;

static void sb_emit(SubmitBatchList *l, uint32_t cmd, uint32_t val) {
    l->dl[l->n++] = (cmd << 24) | (val & 0x00FFFFFFu);
}

static void sb_target(SubmitBatchList *l, uint32_t fba) {
    sb_emit(l, 0x9C, fba);
    sb_emit(l, 0x9D, 512u | (0x04000000u >> 8));
}

/* One through-mode sprite: its two vertices, then VADDR and PRIM. Untextured sprites use
 * the 8888-colour layout (ParityGuestVtx), textured ones float UV + position. */
static void sb_sprite(SubmitBatchList *l, int textured, float x, float y, float u, float v,
                      float size, uint32_t rgba) {
    uint32_t vaddr = SB_VTX + l->voff;
    if (textured) {
        RawSpriteVtx *q = (RawSpriteVtx *)SR_HOST(vaddr);
        q[0] = (RawSpriteVtx){ u, v, x, y, 0.0f };
        q[1] = (RawSpriteVtx){ u + size, v + size, x + size, y + size, 0.0f };
        l->voff += 2u * (uint32_t)sizeof(RawSpriteVtx);
    } else {
        ParityGuestVtx *q = (ParityGuestVtx *)SR_HOST(vaddr);
        q[0] = (ParityGuestVtx){ rgba, x, y, 0.0f };
        q[1] = (ParityGuestVtx){ rgba, x + size, y + size, 0.0f };
        l->voff += 2u * (uint32_t)sizeof(ParityGuestVtx);
    }
    sb_emit(l, 0x01, vaddr);
    sb_emit(l, 0x04, (6u << 16) | 2u);
}

static uint32_t sb_fixa(uint32_t i) {
    uint32_t a = wrap_hash(0x3Du + i * 2654435761u) & 0xFFFFFFu;
    return a == SB_FIXB ? a ^ 1u : a;      /* dual FIX needs two distinct constants */
}

/* Write fresh inputs (both targets, texels, palettes, transfer source) and the display
 * list; return the list address. `tex` lets each run own its texture cache entries. */
static uint32_t submit_batch_prepare(const SubmitBatchShape *sh, uint32_t tex) {
    const uint32_t bytes = FB_W * FB_H * 4u;
    uint32_t *a = (uint32_t *)SR_HOST(0x04000000u | SB_FBA_A);
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) a[i] = wrap_pixel_rgba(0xA5u, i);
    sr_gpu_vram_dirty(0x04000000u | SB_FBA_A, bytes);
    memset(SR_HOST(0x04000000u | SB_FBA_B), 0, bytes);
    sr_gpu_vram_dirty(0x04000000u | SB_FBA_B, bytes);
    uint8_t *texels = (uint8_t *)SR_HOST(tex), *repl = (uint8_t *)SR_HOST(SB_XFER_SRC);
    for (uint32_t v = 0; v < 16u; v++) {
        for (uint32_t u = 0; u < 16u; u++) {
            texels[v * 16u + u] = (uint8_t)((u * 7u + v * 3u) & 15u);
            repl[v * 16u + u] = (uint8_t)((u * 5u + v * 11u + 9u) & 15u);
        }
    }
    sr_gpu_vram_dirty(tex, 256u);
    uint32_t *pal = (uint32_t *)SR_HOST(SB_PAL);
    for (uint32_t k = 0; k < sh->clut_draws; k++)
        for (uint32_t j = 0; j < 16u; j++) pal[k * 16u + j] = wrap_pixel_rgba(0xC1u + k, j);

    const uint32_t colour = (7u << 2) | (3u << 7) | (1u << 23);   /* 8888, float pos, through */
    const uint32_t uv = (3u << 0) | (3u << 7) | (1u << 23);       /* float UV, float pos */
    const float size = (float)sh->size;
    const uint32_t span_x = (uint32_t)DRAW_W - sh->size - 32u, span_y = FB_H - sh->size - 32u;
    SubmitBatchList l = { (uint32_t *)SR_HOST(SB_LIST), 0u, 0u };
    sb_emit(&l, 0x10, (SB_VTX >> 8) & 0x000F0000u);   /* BASE: vertices live in main RAM */
    sb_emit(&l, 0x13, 0);
    sb_emit(&l, 0x4C, 0);
    sb_emit(&l, 0x4D, 0);
    sb_target(&l, SB_FBA_A);
    sb_emit(&l, 0x9E, SB_ZBA);
    sb_emit(&l, 0x9F, 512u | (0x04000000u >> 8));
    sb_emit(&l, 0xD2, 3);
    sb_emit(&l, 0xD3, 0);
    static const uint8_t k_off[] = { 0x1D, 0x1F, 0x20, 0x22, 0x23, 0x24, 0x27, 0x28, 0xE8, 0xE9 };
    for (size_t i = 0; i < sizeof(k_off); i++) sb_emit(&l, k_off[i], 0);
    sb_emit(&l, 0xE7, 1);
    sb_emit(&l, 0x15, 0);
    sb_emit(&l, 0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    sb_emit(&l, 0xD4, 0);
    sb_emit(&l, 0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    sb_emit(&l, 0xC0, 0);
    sb_emit(&l, 0xC2, 0);
    sb_emit(&l, 0xC5, 3u | (0xFFu << 8));            /* 8888 palette, mask 0xFF */
    sb_emit(&l, 0xC6, 0);
    sb_emit(&l, 0xC7, 0);
    sb_emit(&l, 0xC9, 3u | (1u << 8));               /* replace, texture alpha */

    /* Shader blend: FIXA*src + FIXB*dst with a new FIXA per draw, each sprite overlapping
     * the last, so every draw blends against the previous draw's result. */
    sb_emit(&l, 0x1E, 0);
    sb_emit(&l, 0x21, 1);
    sb_emit(&l, 0xDF, 10u | (10u << 4));
    sb_emit(&l, 0xE1, SB_FIXB);
    sb_emit(&l, 0x12, colour);
    for (uint32_t i = 0; i < sh->blend_draws; i++) {
        sb_emit(&l, 0xE0, sb_fixa(i));
        sb_sprite(&l, 0, (float)(16u + (i * 3u) % span_x), (float)(16u + (i * 2u) % span_y),
                  0.0f, 0.0f, size, wrap_pixel_rgba(0x5Au, i));
    }

    /* Shader-blend sprites in disjoint cells of a band below and to the right of the
     * overlapping ones: nothing drawn since the last snapshot copy overlaps any of them (the
     * drawn region is tracked as one bounding rectangle, so each cell also lies outside the
     * rectangle around everything before it), and each reads the existing snapshot instead
     * of copying the target again. */
    for (uint32_t k = 0; k < sh->spaced_draws; k++) {
        sb_emit(&l, 0xE0, sb_fixa(sh->blend_draws + k));
        sb_sprite(&l, 0, (float)(200u + k * 24u), 228.0f, 0.0f, 0.0f, 12.0f,
                  wrap_pixel_rgba(0x6Bu, k));
    }

    /* CLUT8 sprites, one palette per draw, overlapping one another. */
    sb_emit(&l, 0x21, 0);
    sb_emit(&l, 0x1E, 1);
    sb_emit(&l, 0xA0, tex);
    sb_emit(&l, 0xA8, 16u | ((tex & 0xFF000000u) >> 8));
    sb_emit(&l, 0xB8, 4u | (4u << 8));
    sb_emit(&l, 0xC3, 5);
    sb_emit(&l, 0xB1, (SB_PAL & 0xFF000000u) >> 8);
    sb_emit(&l, 0x12, uv);
    for (uint32_t k = 0; k < sh->clut_draws; k++) {
        sb_emit(&l, 0xB0, SB_PAL + k * 64u);
        sb_emit(&l, 0xC4, 2);
        sb_sprite(&l, 1, (float)(240u + (k * 5u) % 200u), (float)(140u + (k * 3u) % 100u),
                  0.0f, 0.0f, 16.0f, 0);
    }
    /* Rewrite the texels with a 16-bit block transfer (16 rows of 16 bytes), then draw with
     * earlier palettes: same cache keys, new contents, so the images update in place. */
    sb_emit(&l, 0xB2, SB_XFER_SRC & 0x00FFFFF0u);
    sb_emit(&l, 0xB3, 8u | ((SB_XFER_SRC & 0xFF000000u) >> 8));
    sb_emit(&l, 0xB4, tex & 0x00FFFFF0u);
    sb_emit(&l, 0xB5, 8u | ((tex & 0xFF000000u) >> 8));
    sb_emit(&l, 0xEB, 0);
    sb_emit(&l, 0xEC, 0);
    sb_emit(&l, 0xEE, 7u | (15u << 10));
    sb_emit(&l, 0xEA, 0);
    for (uint32_t j = 0; j < sh->rewrite_draws; j++) {
        sb_emit(&l, 0xB0, SB_PAL + (j % sh->clut_draws) * 64u);
        sb_emit(&l, 0xC4, 2);
        sb_sprite(&l, 1, (float)(244u + j * 6u), (float)(150u + j * 4u), 0.0f, 0.0f, 16.0f, 0);
    }

    /* Shader-blend sprites over the first CLUT sprites, which were drawn after the last
     * snapshot copy: each needs a fresh copy although no blended draw came between. */
    sb_emit(&l, 0x1E, 0);
    sb_emit(&l, 0x21, 1);
    sb_emit(&l, 0x12, colour);
    for (uint32_t t = 0; t < sh->tail_draws; t++) {
        sb_emit(&l, 0xE0, sb_fixa(sh->blend_draws + sh->spaced_draws + t));
        sb_sprite(&l, 0, (float)(236u + t * 9u), 136.0f + (float)t, 0.0f, 0.0f, 24.0f,
                  wrap_pixel_rgba(0x7Cu, t));
    }
    sb_emit(&l, 0x21, 0);

    /* Render-to-texture: draw into B, then sample B's newest sprite (and its border) from A. */
    for (uint32_t r = 0; r < sh->rtt_rounds; r++) {
        uint32_t bx = 8u + (r * 12u) % span_x, by = 8u + (r * 6u) % span_y;
        sb_target(&l, SB_FBA_B);
        sb_emit(&l, 0x1E, 0);
        sb_emit(&l, 0x12, colour);
        sb_sprite(&l, 0, (float)bx, (float)by, 0.0f, 0.0f, size, wrap_pixel_rgba(0xB0u, r));
        sb_target(&l, SB_FBA_A);
        sb_emit(&l, 0x1E, 1);
        sb_emit(&l, 0xA0, SB_FBA_B);
        sb_emit(&l, 0xA8, 512u | (0x04000000u >> 8));
        sb_emit(&l, 0xB8, 9u | (9u << 8));
        sb_emit(&l, 0xC3, 3);
        sb_emit(&l, 0x12, uv);
        sb_sprite(&l, 1, (float)(24u + (r * 20u) % span_x), (float)(160u + (r % 4u) * 8u),
                  (float)(bx - 4u), (float)(by - 4u), size + 8.0f, 0);
    }
    sb_emit(&l, 0x0F, 0);
    sb_emit(&l, 0x0C, 0);
    return SB_LIST;
}

typedef struct SubmitBatchRun {
    unsigned long long ops, submits, waits, open, snapshots, uploads, wraps, fallbacks;
    unsigned long long snapshot_px;        /* device pixels the snapshot copies moved */
    unsigned long long snapshot_barriers;  /* barrier commands the snapshot copies recorded */
    /* s_snapbar_probe over the run: image barriers, those weaker than to_layout's, copy ops, and
     * copy ops that left the target or the snapshot in another layout than its next user's */
    unsigned long long image_barriers, weak_barriers, op_copies, op_target_bad, op_snapshot_bad;
    double submit_ms, wait_ms, wall_ms;
} SubmitBatchRun;

/* Run the list at `cap` through the production hooks, then settle both targets through the
 * explicit snapshot boundary, which leaves the GPU frames in guest VRAM. */
static int submit_batch_run(const SubmitBatchShape *sh, uint32_t tex, uint32_t cap,
                            SubmitBatchRun *out) {
    memset(out, 0, sizeof(*out));
    coherence_reset_targets();
    if (!cmd_drain(GEGPU_BOUNDARY_LIFETIME)) return 0;
    uint32_t saved_cap = s_submit_batch_ops;
    if (s_async_submit) s_submit_batch_ops = cap;  /* synchronous mode is one op per submit */
    uint32_t list = submit_batch_prepare(sh, tex);
    ge_set_gpu_hooks(&k_hooks);

    GeGpuReplayStats before, after;
    unsigned long renders = s_cnt_submit;
    gegpu_replay_stats_get(&before);
    const SnapbarProbe probe0 = s_snapbar_probe;
    uint64_t started = SDL_GetTicksNS();
    ge_run_list(list, 0);
    gegpu_replay_stats_get(&after);
    out->image_barriers = s_snapbar_probe.image_barriers - probe0.image_barriers;
    out->weak_barriers = s_snapbar_probe.weak_barriers - probe0.weak_barriers;
    out->op_copies = s_snapbar_probe.op_copies - probe0.op_copies;
    out->op_target_bad = s_snapbar_probe.op_target_not_attachment - probe0.op_target_not_attachment;
    out->op_snapshot_bad = s_snapbar_probe.op_snapshot_not_sampled - probe0.op_snapshot_not_sampled;
    renders = s_cnt_submit - renders;        /* render ops recorded by the list itself */
    out->open = s_cmd_slot ? s_cmd_slot->recorded_ops : 0u;
    GeGpuFbDescriptor da = { 0x04000000u | SB_FBA_A, 3u, 512u, (uint32_t)DRAW_W, FB_H };
    GeGpuFbDescriptor db = { 0x04000000u | SB_FBA_B, 3u, 512u, (uint32_t)DRAW_W, FB_H };
    int ok = gegpu_sync_guest_fb(&da) == GEGPU_SYNC_OK && gegpu_sync_guest_fb(&db) == GEGPU_SYNC_OK;
    out->wall_ms = (double)(SDL_GetTicksNS() - started) / 1000000.0;
    s_submit_batch_ops = saved_cap;

    out->snapshots = after.snapshot_copies - before.snapshot_copies;
    out->snapshot_px = after.snapshot_copy_px - before.snapshot_copy_px;
    out->snapshot_barriers = after.snapshot_barriers - before.snapshot_barriers;
    out->uploads = after.upload_ring_reservations - before.upload_ring_reservations;
    out->wraps = after.upload_ring_wraps - before.upload_ring_wraps;
    out->fallbacks = after.upload_ring_fallbacks - before.upload_ring_fallbacks;
    out->ops = (unsigned long long)renders + out->snapshots + out->uploads + out->fallbacks;
    out->submits = after.queue_submits - before.queue_submits;
    for (int i = 0; i < GEGPU_BOUNDARY_COUNT; i++) {
        out->waits += after.boundary[i].waits - before.boundary[i].waits;
        out->submit_ms += (double)(after.boundary[i].submit_ns - before.boundary[i].submit_ns) / 1e6;
        out->wait_ms += (double)(after.boundary[i].wait_ns - before.boundary[i].wait_ns) / 1e6;
    }
    if (!ok) fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshot sync failed\n", cap);
    return ok;
}

static int submit_batch_same(const char *what, uint32_t cap, const uint32_t *gpu,
                             const uint32_t *ref) {
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) {
        if (gpu[i] != ref[i]) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u target %s x=%u y=%u "
                            "gpu=%08x ge_c=%08x\n", cap, what, i % FB_W, i / FB_W, gpu[i], ref[i]);
            return 0;
        }
    }
    return 1;
}

static int coherence_run_submit_batch_snapshot(void);

/* The case measures how snapshot copies are batched and what barriers they record, so it runs on
 * the snapshot path on every device: an interlock op (#2109) takes no snapshot copy. */
static int coherence_run_submit_batch(void) {
    const int interlock = s_dst_interlock;
    s_dst_interlock = 0;
    int ok = coherence_run_submit_batch_snapshot();
    s_dst_interlock = interlock;
    return ok;
}

static int coherence_run_submit_batch_snapshot(void) {
    const SubmitBatchShape sh = { 48u, 8u, 2u, 24u, 6u, 8u, 24u };
    const size_t bytes = (size_t)FB_W * FB_H * 4u;
    uint32_t *ref_a = (uint32_t *)malloc(bytes), *ref_b = (uint32_t *)malloc(bytes);
    if (!ref_a || !ref_b) {
        free(ref_a); free(ref_b);
        fprintf(stderr, "gpu coherence selftest [submit-batch]: out of memory\n");
        return 0;
    }
    /* ge.c's software rasterizer on the same list: the reference every cap must equal. */
    coherence_reset_targets();
    ge_set_gpu_hooks(NULL);
    ge_run_list(submit_batch_prepare(&sh, SB_TEX), 0);
    ge_set_gpu_hooks(&k_hooks);
    memcpy(ref_a, SR_HOST(0x04000000u | SB_FBA_A), bytes);
    memcpy(ref_b, SR_HOST(0x04000000u | SB_FBA_B), bytes);

    /* Legacy cap, the configured cap, and the maximum. A warm-up run first creates the
     * pipelines, depth image and targets so every measured run records the same ops. */
    const uint32_t wanted[3] = { 8u, s_submit_batch_ops, SUBMIT_BATCH_MAX };
    uint32_t caps[4] = { s_submit_batch_ops, 0u, 0u, 0u };   /* caps[0] is the warm-up */
    uint32_t ncaps = 0;
    for (uint32_t i = 0; i < (s_async_submit ? 3u : 1u); i++) {
        /* SR_GPU_SYNC_SUBMIT=1 fixes the cap at 1: one op per submit, nothing to compare */
        uint32_t cap = s_async_submit ? wanted[i] : s_submit_batch_ops;
        int dup = 0;
        for (uint32_t j = 1; j <= ncaps; j++) dup |= caps[j] == cap;
        if (!dup) caps[++ncaps] = cap;
    }
    int ok = 1;
    for (uint32_t i = 0; i <= ncaps && ok; i++) {
        SubmitBatchRun run;
        uint32_t cap = caps[i];
        ok = submit_batch_run(&sh, SB_TEX + 0x100u * (i + 1u), cap, &run) &&
             submit_batch_same("A", cap, (const uint32_t *)SR_HOST(0x04000000u | SB_FBA_A), ref_a) &&
             submit_batch_same("B", cap, (const uint32_t *)SR_HOST(0x04000000u | SB_FBA_B), ref_b);
        if (!ok || i == 0) continue;         /* the warm-up checks frames only */
        /* Non-vacuous: every overlapping and tail blend draw recorded its snapshot copy, no
         * spaced blend draw did, and every palette and rewrite draw uploaded through the GE
         * chain. */
        if (run.snapshots != sh.blend_draws + sh.tail_draws ||
            run.uploads + run.fallbacks < sh.clut_draws + sh.rewrite_draws) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshots=%llu uploads=%llu "
                            "fallbacks=%llu, want %u snapshots and >= %u uploads\n", cap,
                    run.snapshots, run.uploads, run.fallbacks, sh.blend_draws + sh.tail_draws,
                    sh.clut_draws + sh.rewrite_draws);
            ok = 0;
            break;
        }
        /* The overlapping blend draws find the snapshot holding this target with only the
         * previous sprite dirty, so each copies that box, not the frame: the copies move less
         * than one full frame apiece. Their frames still equal ge.c above. */
        const unsigned long long full_px = (unsigned long long)SCL_W * SCL_H;
        if (run.snapshot_px == 0u || run.snapshot_px >= run.snapshots * full_px) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshot copies moved "
                            "%llu px for %llu copies of %llu px: no dirty-box copy\n",
                    cap, run.snapshot_px, run.snapshots, full_px);
            ok = 0;
            break;
        }
        /* Each copy records two barrier commands, one into the transfer layouts and one out of
         * them, not one per image and direction. The second leaves the target as the colour
         * attachment of the render op recorded next, and the frames above still equal ge.c. */
        if (run.snapshot_barriers != 2u * run.snapshots) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u %llu snapshot copies "
                            "recorded %llu barrier commands, want %llu\n",
                    cap, run.snapshots, run.snapshot_barriers, 2u * run.snapshots);
            ok = 0;
            break;
        }
        /* Each copy moves both images into the transfer layouts and out again: four image
         * barriers, every one as strong as to_layout's (the source's writes, the destination's
         * reads and writes, all commands, every mip level). Every copy here is a copy op, which
         * leaves the snapshot samplable and the target as the colour attachment of the render op
         * submit_pending records next. */
        if (run.weak_barriers != 0u || run.image_barriers != 4u * run.snapshots) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u %llu snapshot copies "
                            "recorded %llu image barriers (want %llu), %llu weaker than to_layout's\n",
                    cap, run.snapshots, run.image_barriers, 4u * run.snapshots, run.weak_barriers);
            ok = 0;
            break;
        }
        if (run.op_copies != run.snapshots || run.op_target_bad != 0u || run.op_snapshot_bad != 0u) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u %llu of %llu copy ops left "
                            "the target outside COLOR_ATTACHMENT_OPTIMAL and %llu the snapshot outside "
                            "SHADER_READ_ONLY_OPTIMAL (%llu copies)\n",
                    cap, run.op_target_bad, run.op_copies, run.op_snapshot_bad, run.snapshots);
            ok = 0;
            break;
        }
        /* The cap is the only thing that splits this list. A ring that wraps or is disabled
         * (SR_GPU_XFER_RING_KB, large SR_GPU_SCALE) adds boundaries of its own, so the exact
         * count is checked only where none occurred. */
        const char *accounting = "unchecked(ring wrap/fallback)";
        if (!s_async_submit) {
            accounting = "exact";
            ok = run.submits == run.ops && run.open == 0;
        } else if (!run.wraps && !run.fallbacks) {
            accounting = "exact";
            ok = run.submits == run.ops / cap && run.open == run.ops % cap;
        }
        if (!ok) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u ops=%llu submits=%llu open=%llu, "
                            "want submits=%llu open=%llu\n", cap, run.ops, run.submits, run.open,
                    s_async_submit ? run.ops / cap : run.ops, s_async_submit ? run.ops % cap : 0ull);
            break;
        }
        printf("gpu coherence selftest: PASS %-22s cap=%u ops=%llu submits=%llu open=%llu waits=%llu "
               "submit_ms=%.3f wait_ms=%.3f wall_ms=%.3f accounting=%s frames=ge.c-exact\n",
               "submit-batch", cap, run.ops, run.submits, run.open, run.waits, run.submit_ms,
               run.wait_ms, run.wall_ms, accounting);
    }
    free(ref_a);
    free(ref_b);
    coherence_reset_targets();
    return ok;
}

/* SR_GPU_TIMESTAMPS through its environment gate: the harness's --gpu-timestamps sets or removes the
 * variable before sdl3vk_init and gegpu_init, which both read it (sr_perf_gpu_timestamps_requested).
 * Both cases below run the submit-batch list, whose recordings mix render batches, snapshot copies
 * and ring uploads, against ge.c's frames (a warm-up that makes the pipelines and targets, then the
 * measured run), then present its main target TS_PRESENTS times through gegpu_present, as
 * gui_present does, and wait for those presents so that each presentation command buffer's
 * timestamp pair (sdl3vk.c present_common) has been read back. */
#define TS_PRESENTS 4u

typedef struct {
    SubmitBatchRun run;
    unsigned long long submits;
    uint64_t buffers, total, ns[SR_PERF_GE_REASON_COUNT];
    uint64_t present_timed, present_ns;
} TsCaseRun;

static int ts_case_run(const char *name, TsCaseRun *out) {
    const SubmitBatchShape sh = { 48u, 8u, 2u, 24u, 6u, 8u, 24u };
    const size_t bytes = (size_t)FB_W * FB_H * 4u;
    uint32_t *ref_a = (uint32_t *)malloc(bytes), *ref_b = (uint32_t *)malloc(bytes);
    memset(out, 0, sizeof *out);
    if (!ref_a || !ref_b) {
        free(ref_a); free(ref_b);
        fprintf(stderr, "gpu coherence selftest [%s]: out of memory\n", name);
        return 0;
    }
    coherence_reset_targets();
    ge_set_gpu_hooks(NULL);
    ge_run_list(submit_batch_prepare(&sh, SB_TEX), 0);
    ge_set_gpu_hooks(&k_hooks);
    memcpy(ref_a, SR_HOST(0x04000000u | SB_FBA_A), bytes);
    memcpy(ref_b, SR_HOST(0x04000000u | SB_FBA_B), bytes);

    const uint32_t cap = s_submit_batch_ops;
    int ok = submit_batch_run(&sh, SB_TEX + 0x100u, cap, &out->run) && cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    uint64_t ns_before[SR_PERF_GE_REASON_COUNT];
    memcpy(ns_before, s_ts_ns, sizeof ns_before);
    const uint64_t buffers_before = s_ts_buffers;
    /* Every queue submit from here to the final drain: the list's own, the snapshot boundary's
     * readbacks, and the drain's flush of any open chain. submit_batch_run drains first. */
    GeGpuReplayStats q_before, q_after;
    gegpu_replay_stats_get(&q_before);
    ok = ok && submit_batch_run(&sh, SB_TEX + 0x200u, cap, &out->run) &&
         submit_batch_same("A", cap, (const uint32_t *)SR_HOST(0x04000000u | SB_FBA_A), ref_a) &&
         submit_batch_same("B", cap, (const uint32_t *)SR_HOST(0x04000000u | SB_FBA_B), ref_b) &&
         cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    gegpu_replay_stats_get(&q_after);
    free(ref_a);
    free(ref_b);
    out->submits = q_after.queue_submits - q_before.queue_submits;
    for (int i = 0; i < SR_PERF_GE_REASON_COUNT; i++) out->total += out->ns[i] = s_ts_ns[i] - ns_before[i];
    out->buffers = s_ts_buffers - buffers_before;
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [%s]: the submit-batch list did not give ge.c's frames\n", name);
        coherence_reset_targets();
        return 0;
    }

    uint64_t timed0 = 0, ns0 = 0, timed1 = 0, ns1 = 0;
    (void)sdl3vk_present_timestamps(&timed0, &ns0);
    for (unsigned i = 0; i < TS_PRESENTS; i++) {
        const int pr = gegpu_present(0x04000000u | SB_FBA_A, 3, 512u);
        if (pr != GEGPU_PRESENT_SHOWN) {
            fprintf(stderr, "gpu coherence selftest [%s]: present %u of the main target returned %d, not "
                            "GEGPU_PRESENT_SHOWN\n", name, i, pr);
            coherence_reset_targets();
            return 0;
        }
    }
    Target *t = target_find_by_fba(SB_FBA_A);
    /* Wait on the image the presents showed: the target's own, or its host twin (#2050). */
    if (!t || !sdl3vk_wait_image((void *)present_image(t))) {
        fprintf(stderr, "gpu coherence selftest [%s]: could not wait for the presents\n", name);
        coherence_reset_targets();
        return 0;
    }
    (void)sdl3vk_present_timestamps(&timed1, &ns1);
    out->present_timed = timed1 - timed0;
    out->present_ns = ns1 - ns0;
    coherence_reset_targets();
    return 1;
}

/* --gpu-timestamps unset and --gpu-timestamps 0 (--case gpu-timestamps-off): neither the GE nor the
 * presenter makes a query pool, and nothing is timed, neither the GE's recordings nor the presents. */
static int coherence_run_gpu_timestamps_off(void) {
    const char *v = getenv("SR_GPU_TIMESTAMPS");
    const char *shown = !v ? "unset" : v[0] ? v : "(empty)";
    if (sr_perf_gpu_timestamps_requested()) {
        fprintf(stderr, "gpu coherence selftest [gpu-timestamps-off]: SR_GPU_TIMESTAMPS is '%s'; this case "
                        "runs with it unset or 0\n", shown);
        return 0;
    }
    const int present_pool = sdl3vk_present_timestamps(NULL, NULL);
    if (s_ts_on || s_ts_pool != VK_NULL_HANDLE || present_pool) {
        fprintf(stderr, "gpu coherence selftest [gpu-timestamps-off]: SR_GPU_TIMESTAMPS %s, but a query pool "
                        "exists (GE %d, presenter %d)\n", shown, s_ts_pool != VK_NULL_HANDLE, present_pool);
        return 0;
    }
    TsCaseRun r;
    if (!ts_case_run("gpu-timestamps-off", &r)) return 0;
    if (r.buffers || r.total || r.present_timed || r.present_ns) {
        fprintf(stderr, "gpu coherence selftest [gpu-timestamps-off]: SR_GPU_TIMESTAMPS %s, but buffers=%llu "
                        "gpu_ns=%llu presents_timed=%llu present_ns=%llu were timed\n", shown,
                (unsigned long long)r.buffers, (unsigned long long)r.total,
                (unsigned long long)r.present_timed, (unsigned long long)r.present_ns);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s SR_GPU_TIMESTAMPS=%s no query pool, submits=%llu presents=%u "
           "none timed, frames=ge.c-exact\n", "gpu-timestamps-off", shown, r.submits, TS_PRESENTS);
    return 1;
}

/* --gpu-timestamps 1 (--case gpu-timestamps): gegpu_init and sdl3vk_init made their query pools;
 * every recording of the measured run is collected once its fence has signalled, and its GPU time
 * splits into render, snapshot and upload time, with only the few microseconds after a buffer's last
 * op left on the buffer's mixed submit reason; every present is read back with a GPU time. The frames
 * stay ge.c's (timestamps change no rendering). Returns 77 (SKIP) when the queue has no timestamps. */
static int coherence_run_gpu_timestamps_snapshot(void);

/* The case wants time under every reason its scene records, the snapshot copy included, so it
 * runs on the snapshot path on every device: an interlock op (#2109) takes no snapshot copy. */
static int coherence_run_gpu_timestamps(void) {
    const int interlock = s_dst_interlock;
    s_dst_interlock = 0;
    int ok = coherence_run_gpu_timestamps_snapshot();
    s_dst_interlock = interlock;
    return ok;
}

static int coherence_run_gpu_timestamps_snapshot(void) {
    if (!sr_perf_gpu_timestamps_requested()) {
        fprintf(stderr, "gpu coherence selftest [gpu-timestamps]: SR_GPU_TIMESTAMPS is not set; this case "
                        "runs with --gpu-timestamps 1\n");
        return 0;
    }
    if (!s_ts_on) {
        if (s_ts_unavailable) {
            printf("gpu coherence selftest: SKIP gpu-timestamps (the GE queue has no timestamp queries)\n");
            return 77;
        }
        fprintf(stderr, "gpu coherence selftest [gpu-timestamps]: SR_GPU_TIMESTAMPS is set, but gegpu_init "
                        "made no query pool\n");
        return 0;
    }
    if (!sdl3vk_present_timestamps(NULL, NULL)) {
        fprintf(stderr, "gpu coherence selftest [gpu-timestamps]: the GE has timestamps, but sdl3vk_init made "
                        "no presentation query pool on the same queue\n");
        return 0;
    }
    TsCaseRun r;
    if (!ts_case_run("gpu-timestamps", &r)) return 0;
    const uint64_t *ns = r.ns;
    const uint64_t upload = ns[SR_PERF_GE_TEXTURE_UPLOAD] + ns[SR_PERF_GE_TARGET_UPLOAD] +
                            ns[SR_PERF_GE_DEPTH_UPLOAD];
    if (r.buffers != r.submits || !ns[SR_PERF_GE_RENDER_BATCH] || !ns[SR_PERF_GE_SNAPSHOT_COPY] ||
        (r.run.uploads && !upload) || ns[SR_PERF_GE_MIXED_DRAIN] * 4u > r.total) {
        fprintf(stderr, "gpu coherence selftest [gpu-timestamps]: buffers=%llu submits=%llu render_ns=%llu "
                        "snapshot_ns=%llu upload_ns=%llu (uploads=%llu) mixed_ns=%llu total_ns=%llu: want every "
                        "submitted recording collected, render, snapshot and upload time, and under a quarter "
                        "left on the mixed reason\n",
                (unsigned long long)r.buffers, r.submits, (unsigned long long)ns[SR_PERF_GE_RENDER_BATCH],
                (unsigned long long)ns[SR_PERF_GE_SNAPSHOT_COPY], (unsigned long long)upload, r.run.uploads,
                (unsigned long long)ns[SR_PERF_GE_MIXED_DRAIN], (unsigned long long)r.total);
        return 0;
    }
    if (r.present_timed != TS_PRESENTS || !r.present_ns) {
        fprintf(stderr, "gpu coherence selftest [gpu-timestamps]: %llu of %u presents were read back with "
                        "%llu ns of GPU time: want every present timed, and a nonzero time\n",
                (unsigned long long)r.present_timed, TS_PRESENTS, (unsigned long long)r.present_ns);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s buffers=%llu ops=%llu render_us=%.1f snapshot_us=%.1f "
           "upload_us=%.1f mixed_us=%.1f total_us=%.1f presents_timed=%llu present_us=%.1f frames=ge.c-exact\n",
           "gpu-timestamps", (unsigned long long)r.buffers, r.run.ops,
           (double)ns[SR_PERF_GE_RENDER_BATCH] / 1e3, (double)ns[SR_PERF_GE_SNAPSHOT_COPY] / 1e3,
           (double)upload / 1e3, (double)ns[SR_PERF_GE_MIXED_DRAIN] / 1e3, (double)r.total / 1e3,
           (unsigned long long)r.present_timed, (double)r.present_ns / 1e3);
    return 1;
}

static int coherence_queue_flat_sprite(uint32_t base, uint32_t stride, uint32_t fmt,
                                       float x0, float y0, float x1, float y1) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = fmt;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    GeVtx p0 = { x0, y0, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
    GeVtx p1 = p0;
    p1.x = x1;
    p1.y = y1;
    return hook_sprite(&p0, &p1, 0) && s_nbatch;
}

/* #1170: a title's own memmove/memset replaced by a native codegen stub
 * (sr_native_memmove/sr_native_memset) must keep the scalar accessors' VRAM
 * contract.  The HST HUD copies a rendered 5551 framebuffer rect into a RAM
 * texture right after the list ends; a stub that formed its host pointer
 * without the read prepare copied whatever stale bytes guest VRAM held.  A
 * fill that skipped the write commit was later overwritten by the stale GPU
 * target on the next readback. */
static int coherence_run_native_bulk_copy(void) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u, fmt = 1u;
    const uint32_t ram = 0x08020000u;
    const uint32_t rect = 8u;
    const uint16_t want = (uint16_t)(fb_pack(0x00605040u, fmt) & 0x7fffu);
    coherence_reset_targets();
    g_sr_vram_read_pending = 0;
    memset(SR_HOST(base), 0, stride * FB_H * 2u);
    memset(SR_HOST(ram), 0xEE, rect * rect * 2u);
    if (!coherence_queue_flat_sprite(base, stride, fmt, 10.0f, 10.0f,
                                     10.0f + (float)rect, 10.0f + (float)rect)) {
        fprintf(stderr, "gpu coherence selftest: native-bulk failed to queue sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    if (!g_sr_vram_read_pending) {
        fprintf(stderr, "gpu coherence selftest: native-bulk list end missed pending ownership\n");
        coherence_reset_targets();
        return 0;
    }
    for (uint32_t row = 0; row < rect; row++)
        sr_native_memmove(ram + row * rect * 2u, base + ((10u + row) * stride + 10u) * 2u,
                          rect * 2u, 0u);
    for (uint32_t i = 0; i < rect * rect; i++) {
        uint16_t px;
        memcpy(&px, SR_HOST(ram + i * 2u), sizeof px);
        if ((px & 0x7fffu) != want) {
            fprintf(stderr,
                    "gpu coherence selftest: native-bulk memmove pixel %u copied 0x%04x, expected 0x%04x\n",
                    i, px, want);
            coherence_reset_targets();
            return 0;
        }
    }

    const uint32_t fill = base + (12u * stride + 12u) * 2u;
    sr_native_memset(fill, 0xABu, 4u, 0u);
    if (!coherence_queue_flat_sprite(base, stride, fmt, 100.0f, 100.0f, 104.0f, 104.0f)) {
        fprintf(stderr, "gpu coherence selftest: native-bulk failed to queue second sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    uint32_t filled = MEM_R32(fill);
    if (filled != 0xABABABABu) {
        fprintf(stderr,
                "gpu coherence selftest: native-bulk memset lost to the GPU target (0x%08x, expected 0xabababab)\n",
                filled);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS native-bulk-copy       5551 memmove rows=%u + memset survives readback\n",
           rect);
    coherence_reset_targets();
    return 1;
}

/* Stencil parity: the GPU stencil path against ge.c, bit for bit, in every framebuffer
 * format, except the 8888 blended cells whose state took the fixed-function path
 * (stp_classify): their alpha is exact and their colour within the fixed-function blend's
 * bound. No state of a 16-bit list may take that path. Each cell of the matrix is one state
 * (test function, reference, mask, the three
 * ops, the alpha write mask, blending) drawing two overlapping sprites onto a hashed
 * background, so the second sprite reads what the first wrote: a state that reads its own
 * writes must split there (stencil_splits). The depth list fills a striped depth buffer in
 * clear mode, with the stencil test on, which clear mode ignores, then draws the states the
 * GPU path takes under the depth test (depth-fail and tested stencil-fail ops KEEP). No
 * primitive may take the software fallback. */
#define STP_CELLS_X 24u
#define STP_PITCH 10u
#define STP_ORIGIN 16u

typedef struct StpList { uint32_t *dl; int p; } StpList;

static void stp_emit(StpList *l, uint32_t cmd, uint32_t val) {
    l->dl[l->p++] = (cmd << 24) | (val & 0x00FFFFFFu);
}

static void stp_begin(StpList *l, uint32_t base, uint32_t fmt, int ztest) {
    l->dl = (uint32_t *)SR_HOST(0x08040000u);
    l->p = 0;
    stp_emit(l, 0x10, 0x080000u);                         /* BASE: vertices in main RAM */
    stp_emit(l, 0x9C, base & 0x00FFFFFFu);
    stp_emit(l, 0x9D, 512u | ((base & 0xFF000000u) >> 8));
    stp_emit(l, 0x9E, 0);                                  /* depth at VRAM offset 0 */
    stp_emit(l, 0x9F, 512u | 0x040000u);
    stp_emit(l, 0xD2, fmt);
    stp_emit(l, 0xD3, 0);
    stp_emit(l, 0x1D, 0); stp_emit(l, 0x1E, 0); stp_emit(l, 0x1F, 0);
    stp_emit(l, 0x20, 0); stp_emit(l, 0x22, 0);
    stp_emit(l, 0x23, ztest ? 1u : 0u);
    stp_emit(l, 0x24, 1);                                  /* stencil test on throughout */
    stp_emit(l, 0xE7, 1);
    stp_emit(l, 0xE8, 0); stp_emit(l, 0xE9, 0);
    stp_emit(l, 0x15, 0);
    stp_emit(l, 0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    stp_emit(l, 0xD4, 0);
    stp_emit(l, 0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    stp_emit(l, 0xDF, 2u | (3u << 4));                     /* src alpha, 1 - src alpha */
    stp_emit(l, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
}

/* One sprite from (x0,y0) to (x1,y1) at depth z, its vertices at *vaddr. */
static void stp_sprite(StpList *l, uint32_t *vaddr, uint32_t rgba, float x0, float y0,
                       float x1, float y1, float z) {
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(*vaddr);
    v[0] = (ParityGuestVtx){ rgba, x0, y0, z };
    v[1] = (ParityGuestVtx){ rgba, x1, y1, z };
    stp_emit(l, 0x01, *vaddr & 0x00FFFFFFu);
    stp_emit(l, 0x04, (6u << 16) | 2u);
    *vaddr += 2u * (uint32_t)sizeof(ParityGuestVtx);
}

/* The two overlapping sprites of a cell, in one primitive command so they can share a batch. */
static void stp_cell_sprites(StpList *l, uint32_t *vaddr, uint32_t cell, float z) {
    float x = (float)(STP_ORIGIN + STP_PITCH * (cell % STP_CELLS_X));
    float y = (float)(STP_ORIGIN + STP_PITCH * (cell / STP_CELLS_X));
    uint32_t ca = wrap_hash(0x5713u + cell * 2u), cb = wrap_hash(0x5714u + cell * 2u);
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(*vaddr);
    v[0] = (ParityGuestVtx){ ca, x + 1.0f, y + 1.0f, z };
    v[1] = (ParityGuestVtx){ ca, x + 7.0f, y + 7.0f, z };
    v[2] = (ParityGuestVtx){ cb, x + 3.0f, y + 3.0f, z };
    v[3] = (ParityGuestVtx){ cb, x + 9.0f, y + 9.0f, z };
    stp_emit(l, 0x01, *vaddr & 0x00FFFFFFu);
    stp_emit(l, 0x04, (6u << 16) | 4u);
    *vaddr += 4u * (uint32_t)sizeof(ParityGuestVtx);
}

/* Per cell of the 8888 list being checked: 1 if its state is blended and took the
 * fixed-function stencil path (stp_classify). */
static uint8_t s_stp_ff_blend[8u * 6u * 6u];

static void stp_cell_state(StpList *l, uint32_t cell, uint32_t func, uint32_t ops) {
    static const uint32_t smask[5] = { 0xFFu, 0xF0u, 0x0Fu, 0x80u, 0x81u };
    static const uint32_t amask[5] = { 0x00u, 0x00u, 0xF0u, 0x0Fu, 0x80u };
    uint32_t h = wrap_hash(0xA11u + cell);
    stp_emit(l, 0xDC, func | ((h & 0xFFu) << 8) | (smask[cell % 5u] << 16));
    stp_emit(l, 0xDD, ops);
    stp_emit(l, 0xE9, amask[(cell / 5u) % 5u]);
    stp_emit(l, 0x21, (cell & 1u) ? 1u : 0u);
}

/* Two sprites overlap in each cell, so a pixel blends at most twice; each fixed-function blend
 * adds at most blend-parity-ff's bound, and the second carries the first's error through a
 * destination factor of at most 1. */
#define STP_FF_BLEND_BOUND (2 * PARITY_FF_BOUND)

static void stp_end(StpList *l) {
    stp_emit(l, 0x10, 0);                                  /* later lists expect BASE 0 */
    stp_emit(l, 0x0F, 0);
    stp_emit(l, 0x0C, 0);
}

/* Matrix list: every test function with every stencil-fail and depth-pass op, depth off. With
 * `only` >= 0 the list holds that cell alone. */
#define STP_MATRIX_CELLS (8u * 6u * 6u)
static uint32_t stp_matrix_list(uint32_t base, uint32_t fmt, int only) {
    StpList l; stp_begin(&l, base, fmt, 0);
    uint32_t vaddr = 0x08060000u, cell = 0;
    for (uint32_t func = 0; func < 8u; func++)
        for (uint32_t sfail = 0; sfail < 6u; sfail++)
            for (uint32_t zpass = 0; zpass < 6u; zpass++, cell++) {
                if (only >= 0 && cell != (uint32_t)only) continue;
                stp_cell_state(&l, cell, func, sfail | (((cell / 7u) % 6u) << 8) | (zpass << 16));
                stp_cell_sprites(&l, &vaddr, cell, 0.0f);
            }
    stp_end(&l);
    return 0x08040000u;
}

/* Depth list: a striped depth buffer, then depth-tested cells. With `only` >= 0 the list
 * holds that cell alone, without the depth buffer. */
#define STP_DEPTH_CELLS (8u * 6u + 5u)
static uint32_t stp_depth_list(uint32_t base, uint32_t fmt, int only) {
    static const uint32_t zfunc[4] = { 4u, 5u, 6u, 7u };   /* less, lequal, greater, gequal */
    StpList l; stp_begin(&l, base, fmt, 1);
    uint32_t vaddr = 0x08060000u;
    if (only < 0) {
        stp_emit(&l, 0xD3, 1u | 0x400u | 0x100u);          /* depth and colour, stencil kept */
        for (uint32_t x = 0; x < STP_CELLS_X * STP_PITCH; x += 2u)
            stp_sprite(&l, &vaddr, 0xFF808080u, (float)(STP_ORIGIN + x), (float)STP_ORIGIN,
                       (float)(STP_ORIGIN + x + 2u), (float)(STP_ORIGIN + 8u * STP_PITCH),
                       (x & 2u) ? 49152.0f : 16384.0f);
        stp_emit(&l, 0xD3, 0);
    }
    uint32_t cell = 0;
    for (uint32_t func = 0; func < 8u; func++)
        for (uint32_t zpass = 0; zpass < 6u; zpass++, cell++) {
            if (only >= 0 && cell != (uint32_t)only) continue;
            stp_cell_state(&l, cell, func, zpass << 16);
            stp_emit(&l, 0xDE, zfunc[cell % 4u]);
            stp_emit(&l, 0xE7, (cell / 4u) & 1u);          /* depth writes on and off */
            stp_cell_sprites(&l, &vaddr, cell, 32768.0f);
        }
    for (uint32_t sfail = 1; sfail < 6u; sfail++, cell++) {
        if (only >= 0 && cell != (uint32_t)only) continue;
        stp_cell_state(&l, cell, 1u, sfail | (2u << 16));   /* always: the fail op never runs */
        stp_emit(&l, 0xDE, zfunc[cell % 4u]);
        stp_emit(&l, 0xE7, 0);
        stp_cell_sprites(&l, &vaddr, cell, 32768.0f);
    }
    stp_end(&l);
    return 0x08040000u;
}

static void stp_fill_background(uint32_t base, uint32_t fmt) {
    if (fmt == 3u) {
        uint32_t *px = (uint32_t *)SR_HOST(base);
        for (uint32_t i = 0; i < FB_W * FB_H; i++) px[i] = wrap_hash(0x3C1u + i * 40503u);
    } else {
        uint16_t *px = (uint16_t *)SR_HOST(base);
        for (uint32_t i = 0; i < FB_W * FB_H; i++)
            px[i] = (uint16_t)wrap_hash(0x3C1u + i * 40503u);
    }
    sr_gpu_vram_dirty(base, FB_W * FB_H * (fmt == 3u ? 4u : 2u));
}

/* Which cells of an 8888 list took the fixed-function stencil path, as ge_gpu.c decided: each
 * cell runs alone and its state builds are read from stencil_fixed. Blended ones get the
 * fixed-function blend's bound; unblended ones stay exact. Returns how many are blended. */
static unsigned stp_classify(uint32_t base, int depth) {
    const unsigned cells = depth ? STP_DEPTH_CELLS : STP_MATRIX_CELLS;
    unsigned blended = 0;
    memset(s_stp_ff_blend, 0, sizeof s_stp_ff_blend);
    ge_set_gpu_hooks(&k_hooks);
    for (unsigned c = 0; c < cells; c++) {
        const unsigned long long fixed0 = s_replay_stats.stencil_fixed;
        ge_run_list(depth ? stp_depth_list(base, 3u, (int)c) : stp_matrix_list(base, 3u, (int)c), 0);
        s_stp_ff_blend[c] = (uint8_t)((c & 1u) && s_replay_stats.stencil_fixed != fixed0);
        blended += s_stp_ff_blend[c];
    }
    coherence_reset_targets();
    return blended;
}

static int coherence_run_stencil_parity(void) {
    const uint32_t base = 0x04000000u | 0x00140000u;
    const uint32_t max_bytes = FB_W * FB_H * 4u;
    uint8_t *gpu_out = (uint8_t *)malloc(max_bytes);
    if (!gpu_out) return 0;
    GeState saved = *s_ge;                 /* the lists leave the stencil test and masks on */
    int ok = 1, max_delta = 0;
    unsigned runs = 0, differing = 0, ff_cells = 0;
    const unsigned long long splits0 = s_replay_stats.stencil_splits;
    for (uint32_t fmt = 0; fmt < 4u && ok; fmt++) {
        for (int depth = 0; depth < 2 && ok; depth++) {
            const uint32_t bytes = FB_W * FB_H * (fmt == 3u ? 4u : 2u);
            if (fmt == 3u) ff_cells += stp_classify(base, depth);
            const uint32_t list = depth ? stp_depth_list(base, fmt, -1) : stp_matrix_list(base, fmt, -1);
            coherence_reset_targets();
            stp_fill_background(base, fmt);
            const unsigned long long fallback0 = s_replay_stats.fallback_prims;
            const unsigned long long fixed0 = s_replay_stats.stencil_fixed;
            ge_set_gpu_hooks(&k_hooks);
            ge_run_list(list, 0);
            GeGpuFbDescriptor desc = { .addr = base, .format = fmt, .stride = 512u,
                                       .width = 480, .height = FB_H };
            if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) { ok = 0; break; }
            memcpy(gpu_out, SR_HOST(base), bytes);
            const unsigned long long fallbacks = s_replay_stats.fallback_prims - fallback0;
            if (fmt != 3u && s_replay_stats.stencil_fixed != fixed0) {
                fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s list: %llu states "
                                "took the fixed-function stencil path, which needs an 8888 target\n",
                        fmt, depth ? "depth" : "matrix", s_replay_stats.stencil_fixed - fixed0);
                ok = 0;
            }

            coherence_reset_targets();
            stp_fill_background(base, fmt);
            ge_set_gpu_hooks(NULL);
            ge_run_list(list, 0);
            ge_set_gpu_hooks(&k_hooks);
            const uint8_t *sw_out = (const uint8_t *)SR_HOST(base);
            runs++;
            /* The list must draw: count what ge.c changed against a fresh background. */
            const uint32_t bpp = fmt == 3u ? 4u : 2u;
            unsigned drawn = 0;
            for (uint32_t i = 0; i < FB_W * FB_H; i++) {
                uint32_t bg = wrap_hash(0x3C1u + i * 40503u), w = 0;
                memcpy(&w, sw_out + i * bpp, bpp);
                if (fmt != 3u) bg &= 0xFFFFu;
                if (w != bg) drawn++;
            }
            if (drawn < 4096u) {
                fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s list changed "
                                "only %u pixels\n", fmt, depth ? "depth" : "matrix", drawn);
                ok = 0;
            }
            if (fallbacks) {
                fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s list took "
                                "the software fallback for %llu primitives\n",
                        fmt, depth ? "depth" : "matrix", fallbacks);
                ok = 0;
            }
            for (uint32_t i = 0; i < FB_W * FB_H; i++) {
                if (i % FB_W >= 480u || !memcmp(gpu_out + i * bpp, sw_out + i * bpp, bpp)) continue;
                uint32_t g = 0, w = 0;
                memcpy(&g, gpu_out + i * bpp, bpp); memcpy(&w, sw_out + i * bpp, bpp);
                uint32_t x = i % FB_W, y = i / FB_W;
                uint32_t cell = (y - STP_ORIGIN) / STP_PITCH * STP_CELLS_X + (x - STP_ORIGIN) / STP_PITCH;
                int d = 0;
                for (uint32_t c = 0; c < bpp; c++) {
                    int e = abs((int)((g >> (8 * c)) & 0xFFu) - (int)((w >> (8 * c)) & 0xFFu));
                    if (e > d) d = e;
                }
                if (d > max_delta) max_delta = d;
                if (fmt == 3u && x >= STP_ORIGIN && y >= STP_ORIGIN &&
                    x < STP_ORIGIN + STP_CELLS_X * STP_PITCH && cell < sizeof s_stp_ff_blend &&
                    s_stp_ff_blend[cell] && (g >> 24) == (w >> 24) && d <= STP_FF_BLEND_BOUND)
                    continue;
                if (!differing++) {
                    fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s cell=%u "
                                    "x=%u y=%u gpu=%08x ge_c=%08x\n",
                            fmt, depth ? "depth" : "matrix", cell, x, y, g, w);
                }
                ok = 0;
            }
            coherence_reset_targets();
        }
    }
    free(gpu_out);
    *s_ge = saved;
    const unsigned long long splits = s_replay_stats.stencil_splits - splits0;
    if (ok && !splits) {
        fprintf(stderr, "gpu coherence selftest [stencil-parity]: no state split its batch\n");
        ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [stencil-parity]: %u pixels differ from ge.c\n",
                differing);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s lists=%u splits=%llu max_delta_vs_ge_c=%d "
           "(exact but for %u blended cells on the fixed-function path: alpha exact, rgb bound %d)\n",
           "stencil-parity", runs, splits, max_delta, ff_cells, STP_FF_BLEND_BOUND);
    return 1;
}

/* Constant stencil: on an 8888 target, a test that always passes with a depth-pass op that
 * keeps the stencil or stores a constant (KEEP, or ZERO and REPLACE under a full alpha write
 * mask, or any op under a mask that keeps every bit) reads no destination (ge_gpu.c
 * stencil_fixed_alpha); blended, the constant must be 0 and the blend fixed-function. The
 * stencil-fail op, which such a test never runs, varies.
 *  - Unblended cells must match ge.c bit for bit, flat and under GEQUAL and LESS on a striped
 *    depth buffer with depth writes on and off.
 *  - Blended cells must store ge.c's alpha bit for bit, and a colour within the fixed-function
 *    blend's bound of ge.c (STP_FF_BLEND_BOUND), as a draw without the stencil test gives.
 * Their overlapping sprites must share a batch with no destination snapshot and no software
 * fallback. The refused list holds the same tests and ops in the states the fast path must
 * leave to the shader: dither with blending, each kind of blend that needs the shader, a
 * blended constant other than 0 and a partial alpha mask. None of them may count as fixed,
 * and all must match ge.c bit for bit. */
enum { STC_FLAT, STC_DEPTH, STC_BLENDED, STC_REFUSED, STC_KINDS };

static const uint32_t k_stc_blend_modes[4] = {
    2u | (3u << 4),                  /* src alpha, 1 - src alpha */
    4u | (5u << 4),                  /* dst alpha (the stencil), 1 - dst alpha */
    2u | (3u << 4) | (1u << 8),      /* subtract */
    0u | (1u << 4) | (4u << 8),      /* max */
};

/* One refused state per entry: blend mode, dither, reference, alpha mask and the depth-pass ops
 * it is drawn with (a bit per op: KEEP 1, ZERO 2, REPLACE 4). KEEP is left out where it is
 * still fixed, because the reason for refusing concerns only a stored constant. */
static const struct { uint32_t mode, dither, ref, amask, zpass_ops; } k_stc_refused[6] = {
    { 2u | (3u << 4), 1u, 0u, 0u, 7u },                /* dither with blending */
    { 2u | (3u << 4) | (5u << 8), 0u, 0u, 0u, 7u },    /* absolute difference */
    { 6u | (3u << 4), 0u, 0u, 0u, 7u },                /* doubled source alpha */
    { 10u | (10u << 4), 0u, 0u, 0u, 7u },              /* two distinct FIX colours */
    { 2u | (3u << 4), 0u, 0x5Au, 0u, 4u },             /* blended REPLACE of a constant other than 0 */
    { 0xFFu, 0u, 0x5Au, 0x0Fu, 6u },                   /* partial alpha mask, unblended */
};

static uint32_t stc_list(uint32_t base, int kind) {
    static const uint32_t zfunc[2] = { 7u, 4u };           /* gequal, less */
    const int depth = kind == STC_DEPTH, blended = kind == STC_BLENDED;
    StpList l; stp_begin(&l, base, 3u, depth);
    uint32_t vaddr = 0x08060000u, cell = 0;
    if (depth) {
        stp_emit(&l, 0xD3, 1u | 0x400u | 0x100u);          /* depth and colour, stencil kept */
        for (uint32_t x = 0; x < STP_CELLS_X * STP_PITCH; x += 2u)
            stp_sprite(&l, &vaddr, 0xFF808080u, (float)(STP_ORIGIN + x), (float)STP_ORIGIN,
                       (float)(STP_ORIGIN + x + 2u), (float)(STP_ORIGIN + 8u * STP_PITCH),
                       (x & 2u) ? 49152.0f : 16384.0f);
        stp_emit(&l, 0xD3, 0);
    }
    if (kind == STC_REFUSED) {
        stp_emit(&l, 0xE2, 0x4E2Bu); stp_emit(&l, 0xE3, 0xC1F3u);   /* a dither matrix */
        stp_emit(&l, 0xE4, 0x2A7Eu); stp_emit(&l, 0xE5, 0xD05Bu);
        stp_emit(&l, 0xE0, 0x3C6E91u); stp_emit(&l, 0xE1, 0xA05D27u);
        for (uint32_t r = 0; r < 6u; r++)
            for (uint32_t zpass = 0; zpass < 3u; zpass++)
                for (uint32_t rep = 0; rep < 2u; rep++) {
                    if (!(k_stc_refused[r].zpass_ops & (1u << zpass))) continue;
                    uint32_t h = wrap_hash(0x5C7u + cell);
                    const int blend = k_stc_refused[r].mode != 0xFFu;
                    stp_emit(&l, 0xDC, 1u | (k_stc_refused[r].ref << 8) | (((h >> 8) & 0xFFu) << 16));
                    stp_emit(&l, 0xDD, (h >> 16) % 6u | (zpass << 16));
                    stp_emit(&l, 0xE9, k_stc_refused[r].amask);
                    stp_emit(&l, 0x21, blend ? 1u : 0u);
                    stp_emit(&l, 0x20, k_stc_refused[r].dither);
                    if (blend) stp_emit(&l, 0xDF, k_stc_refused[r].mode);
                    stp_cell_sprites(&l, &vaddr, cell++, 32768.0f);
                }
        stp_emit(&l, 0x20, 0);
    }
    for (uint32_t zpass = 0; kind != STC_REFUSED && zpass < 3u; zpass++)
        for (uint32_t amask = 0; amask < 2u; amask++)
            for (uint32_t v = 0; v < (blended ? 4u : 6u); v++) {
                uint32_t h = wrap_hash(0x5C7u + cell);
                uint32_t ref = blended ? 0u : (h & 0xFFu);  /* blended: only 0 is fixed */
                stp_emit(&l, 0xDC, 1u | (ref << 8) | (((h >> 8) & 0xFFu) << 16));
                stp_emit(&l, 0xDD, (blended ? (h >> 16) % 6u : v) | (zpass << 16));
                stp_emit(&l, 0xE9, amask ? 0xFFu : 0u);
                stp_emit(&l, 0x21, blended ? 1u : 0u);
                if (blended) stp_emit(&l, 0xDF, k_stc_blend_modes[v]);
                if (depth) {
                    stp_emit(&l, 0xDE, zfunc[cell & 1u]);
                    stp_emit(&l, 0xE7, (cell >> 1) & 1u);
                }
                stp_cell_sprites(&l, &vaddr, cell++, 32768.0f);
            }
    stp_emit(&l, 0xDF, 2u | (3u << 4));
    stp_end(&l);
    return 0x08040000u;
}

/* Run `list` on the GPU path (into `out`) or through ge.c (left in VRAM) over a fresh hashed
 * background. Returns 0 if the GPU target could not be synchronized. */
static int stc_run(uint32_t base, uint32_t list, int gpu, uint8_t *out) {
    coherence_reset_targets();
    stp_fill_background(base, 3u);
    ge_set_gpu_hooks(gpu ? &k_hooks : NULL);
    ge_run_list(list, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (!gpu) return 1;
    GeGpuFbDescriptor desc = { .addr = base, .format = 3u, .stride = 512u,
                               .width = 480, .height = FB_H };
    if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) return 0;
    memcpy(out, SR_HOST(base), FB_W * FB_H * 4u);
    return 1;
}

static int coherence_run_stencil_constant(void) {
    static const char *const names[STC_KINDS] = { "flat", "depth", "blended", "refused" };
    const uint32_t base = 0x04000000u | 0x00140000u, bytes = FB_W * FB_H * 4u;
    uint8_t *gpu_out = (uint8_t *)malloc(bytes);
    if (!gpu_out) return 0;
    GeState saved = *s_ge;
    int ok = 1, max_rgb = 0;
    unsigned differing = 0;
    for (int kind = 0; kind < STC_KINDS && ok; kind++) {
        const int blended = kind == STC_BLENDED, refused = kind == STC_REFUSED;
        const char *name = names[kind];
        const GeGpuReplayStats before = s_replay_stats;
        if (!stc_run(base, stc_list(base, kind), 1, gpu_out)) { ok = 0; break; }
        const GeGpuReplayStats after = s_replay_stats;
        (void)stc_run(base, stc_list(base, kind), 0, NULL);
        const uint8_t *sw_out = (const uint8_t *)SR_HOST(base);
        if (after.fallback_prims != before.fallback_prims) {
            fprintf(stderr, "gpu coherence selftest [stencil-constant]: %s list took the software "
                            "fallback for %llu primitives\n", name,
                    after.fallback_prims - before.fallback_prims);
            ok = 0;
        }
        if (refused && after.stencil_fixed != before.stencil_fixed) {
            fprintf(stderr, "gpu coherence selftest [stencil-constant]: refused list: %llu states "
                            "took the fixed path, want 0\n",
                    after.stencil_fixed - before.stencil_fixed);
            ok = 0;
        }
        if (!refused && (after.snapshot_copies != before.snapshot_copies ||
                         after.stencil_splits != before.stencil_splits ||
                         after.stencil_fixed == before.stencil_fixed)) {
            fprintf(stderr, "gpu coherence selftest [stencil-constant]: %s list: snapshot_copies=%llu "
                            "splits=%llu fixed_states=%llu, want 0 0 and some fixed states\n", name,
                    after.snapshot_copies - before.snapshot_copies,
                    after.stencil_splits - before.stencil_splits,
                    after.stencil_fixed - before.stencil_fixed);
            ok = 0;
        }
        unsigned drawn = 0;
        for (uint32_t i = 0; i < FB_W * FB_H; i++) {
            uint32_t w = 0, g = 0;
            memcpy(&w, sw_out + i * 4u, 4u);
            memcpy(&g, gpu_out + i * 4u, 4u);
            if (w != wrap_hash(0x3C1u + i * 40503u)) drawn++;
            if (i % FB_W >= 480u) continue;
            int bad = (g >> 24) != (w >> 24);
            for (int c = 0; c < 3; c++) {
                int d = (int)((g >> (8 * c)) & 0xFFu) - (int)((w >> (8 * c)) & 0xFFu);
                if (d < 0) d = -d;
                if (d > max_rgb) max_rgb = d;
                if (blended ? d > STP_FF_BLEND_BOUND : d != 0) bad = 1;
            }
            if (!bad) continue;
            if (!differing++)
                fprintf(stderr, "gpu coherence selftest [stencil-constant]: %s x=%u y=%u gpu=%08x "
                                "ge_c=%08x\n", name, i % FB_W, i / FB_W, g, w);
            ok = 0;
        }
        if (drawn < 1024u) {
            fprintf(stderr, "gpu coherence selftest [stencil-constant]: %s list changed only %u "
                            "pixels\n", name, drawn);
            ok = 0;
        }
        coherence_reset_targets();
    }
    free(gpu_out);
    *s_ge = saved;
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [stencil-constant]: %u pixels outside the contract\n",
                differing);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s lists=%d fixed lists: snapshot_copies=0 "
           "alpha_delta_vs_ge_c=0 rgb_max_delta=%d (unblended 0, blended bound %d); refused list: "
           "fixed_states=0, exact\n", "stencil-constant", STC_KINDS, max_rgb, STP_FF_BLEND_BOUND);
    return 1;
}

/* The GE color test on the GPU path. psp.frag runs ge.c color_test() on the colour it stores,
 * so a list that uses the color test must stay on the GPU (no software fallback, no target
 * readback) and leave exactly what ge.c leaves. Each list runs three ways over a hashed
 * background: on the GPU, through ge.c, and through ge.c with the color test off, which shows
 * that the test both passed and rejected pixels (a list that rejected everything, or nothing,
 * would prove little). Kinds:
 *  - flat: two overlapping flat sprites per cell, under every function (never, always, equal,
 *    not equal), six masks, and a reference that matches the first sprite's colour under the
 *    mask (its other bits flipped) or a hashed one, on 8888, 5650, 5551 and 4444 targets;
 *  - blended (8888): the same cells with source-alpha blending, which runs after the test: the
 *    colour within the fixed-function blend's bound of ge.c (STP_FF_BLEND_BOUND), alpha exact;
 *  - textured (8888): a 16x16 texture, a quarter of its texels a key colour with hashed alpha,
 *    drawn 1:1 with REPLACE under each function, with the full mask and a 5-bit-lattice one:
 *    the colour-keyed sprite a game draws with the color test;
 *  - doubled (8888): the same sprites with REPLACE and colour doubling, which psp.frag computes
 *    exactly on whole texels (doubled and clamped), so this list stays on the GPU;
 *  - modulated (8888): the same sprites with MODULATE and a non-white vertex colour. psp.frag
 *    computes that colour in floating point, 1 away from ge.c's integer one at some texels, and
 *    the key turns that into a pixel kept or dropped, so this list must stay on ge.c (every
 *    primitive falls back) and still match it.
 * Three lists of Gouraud-shaded triangles follow (ctp_gouraud_list). A triangle whose three
 * vertices carry one colour is flat in ge.c, so it stays on the GPU; one with a vertex of another
 * colour or alpha interpolates and stays on ge.c; a list of both does each, triangle by triangle.
 * A table of states then checks color_test_colour_inexact() directly, including the Gouraud
 * triangle and transform-mode fog cases these sprite lists cannot reach, and tri_colours_vary(). */
enum { CTP_FLAT_8888, CTP_FLAT_5650, CTP_FLAT_5551, CTP_FLAT_4444, CTP_BLEND_8888,
       CTP_TEX_8888, CTP_TEXDBL_8888, CTP_TEXMOD_8888, CTP_KINDS };
#define CTP_TEX 0x08100000u                      /* 16x16 8888 texture in main RAM */
#define CTP_KEY 0x00C03080u                      /* the textured cells' key colour */

typedef struct CtpTexVtx { float u, v; uint32_t rgba; float x, y, z; } CtpTexVtx;

static uint32_t ctp_fmt(int kind) {
    return kind == CTP_FLAT_5650 ? 0u : kind == CTP_FLAT_5551 ? 1u : kind == CTP_FLAT_4444 ? 2u : 3u;
}

static uint32_t ctp_texel(uint32_t i) {
    const uint32_t h = wrap_hash(0xC7E1u + i);
    return (h & 3u) == 0u ? (CTP_KEY | (h & 0xFF000000u)) : h;
}

/* The list of `kind`, with the color test enabled (`on`) or left off. */
static uint32_t ctp_list(uint32_t base, int kind, int on) {
    static const uint32_t masks[6] = { 0xFFFFFFu, 0xF0F0F0u, 0x00FF00u, 0x000000u, 0x808080u, 0x3C5AA5u };
    StpList l; stp_begin(&l, base, ctp_fmt(kind), 0);
    stp_emit(&l, 0x24, 0);                               /* no stencil test */
    stp_emit(&l, 0x27, on ? 1u : 0u);
    uint32_t vaddr = 0x08060000u;
    if (kind != CTP_TEX_8888 && kind != CTP_TEXDBL_8888 && kind != CTP_TEXMOD_8888) {
        const int blended = kind == CTP_BLEND_8888;
        stp_emit(&l, 0x21, blended ? 1u : 0u);
        for (uint32_t cell = 0; cell < 4u * 6u * 2u; cell++) {
            const uint32_t func = cell & 3u, mask = masks[(cell >> 2) % 6u];
            const uint32_t h = wrap_hash(0xC7u + cell);
            const uint32_t ca = wrap_hash(0x5713u + cell * 2u);   /* the cell's first sprite colour */
            const uint32_t ref = (cell / 24u) ? (h & 0xFFFFFFu) : ((ca ^ (h & ~mask)) & 0xFFFFFFu);
            stp_emit(&l, 0xD8, func);
            stp_emit(&l, 0xD9, ref);
            stp_emit(&l, 0xDA, mask);
            stp_cell_sprites(&l, &vaddr, cell, 0.0f);
        }
    } else {
        uint32_t *tex = (uint32_t *)SR_HOST(CTP_TEX);
        for (uint32_t i = 0; i < 16u * 16u; i++) tex[i] = ctp_texel(i);
        stp_emit(&l, 0xA0, CTP_TEX & 0x00FFFFFFu);
        stp_emit(&l, 0xA8, 16u | ((CTP_TEX & 0xFF000000u) >> 8));
        stp_emit(&l, 0xB8, 4u | (4u << 8));
        stp_emit(&l, 0xC0, 0); stp_emit(&l, 0xC2, 0); stp_emit(&l, 0xC3, 3);
        stp_emit(&l, 0xC6, 0); stp_emit(&l, 0xC7, 0);
        const int modulate = kind == CTP_TEXMOD_8888, doubled = kind == CTP_TEXDBL_8888;
        const uint32_t vcol = modulate ? 0xFFB06A3Du : 0xFFFFFFFFu;
        /* modulate or replace, texture alpha, the colour doubled for the doubled list */
        stp_emit(&l, 0xC9, (modulate ? 0u : 3u) | (1u << 8) | (doubled ? 1u << 16 : 0u));
        stp_emit(&l, 0xCB, 0);
        stp_emit(&l, 0x1E, 1);
        stp_emit(&l, 0x21, 0);
        stp_emit(&l, 0x12, 3u | (7u << 2) | (3u << 7) | (1u << 23));
        for (uint32_t cell = 0; cell < 8u; cell++) {
            const float x = (float)(STP_ORIGIN + 20u * cell), y = (float)STP_ORIGIN;
            CtpTexVtx *v = (CtpTexVtx *)SR_HOST(vaddr);
            v[0] = (CtpTexVtx){ 0.0f, 0.0f, vcol, x, y, 0.0f };
            v[1] = (CtpTexVtx){ 16.0f, 16.0f, vcol, x + 16.0f, y + 16.0f, 0.0f };
            /* Modulated, only functions that test (ALWAYS is no test, so it would stay on the GPU). */
            static const uint32_t mod_func[8] = { 0u, 2u, 3u, 2u, 0u, 3u, 2u, 3u };
            stp_emit(&l, 0xD8, modulate ? mod_func[cell] : (cell & 3u));
            /* Modulated, the key is the key texel times the vertex colour as ge.c truncates it;
             * doubled, the key texel doubled and clamped. */
            stp_emit(&l, 0xD9, modulate ? 0x0084141Fu : doubled ? 0x00FF60FFu : CTP_KEY);
            stp_emit(&l, 0xDA, cell < 4u ? 0xFFFFFFu : 0xF8F8F8u);
            stp_emit(&l, 0x01, vaddr & 0x00FFFFFFu);
            stp_emit(&l, 0x04, (6u << 16) | 2u);
            vaddr += 2u * (uint32_t)sizeof(CtpTexVtx);
        }
        stp_emit(&l, 0x1E, 0);
        stp_emit(&l, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
    }
    stp_emit(&l, 0x27, 0);
    stp_emit(&l, 0x21, 0);
    stp_end(&l);
    return 0x08040000u;
}

/* 48 Gouraud-shaded triangles under the color test NOTEQUAL the key, every other one in the key
 * colour. `mode` 0: each triangle's three vertices carry one colour. 1: one vertex of each differs,
 * in red, in alpha or in blue by turns. 2: pairs of triangles alternate between the two. *nvary
 * counts the triangles whose vertex colours differ. */
static uint32_t ctp_gouraud_list(uint32_t base, int mode, int on, unsigned *ntris, unsigned *nvary) {
    static const uint32_t flip[3] = { 0x00000040u, 0x40000000u, 0x00400000u };
    StpList l; stp_begin(&l, base, 3u, 0);
    stp_emit(&l, 0x24, 0);                               /* no stencil test */
    stp_emit(&l, 0x21, 0);
    stp_emit(&l, 0x50, 1);                               /* Gouraud shading */
    stp_emit(&l, 0x27, on ? 1u : 0u);
    stp_emit(&l, 0xD8, 3u);
    stp_emit(&l, 0xD9, CTP_KEY);
    stp_emit(&l, 0xDA, 0xFFFFFFu);
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(0x08060000u);
    unsigned n = 0;
    *nvary = 0;
    for (uint32_t cell = 0; cell < 48u; cell++) {
        /* an odd number of sixteenths off the pixel grid, so no edge passes through a pixel centre */
        const float x = (float)(STP_ORIGIN + 18u * (cell % 24u)) + 0.0625f;
        const float y = (float)(STP_ORIGIN + 20u * (cell / 24u)) + 0.0625f;
        const uint32_t c = ((cell & 1u) ? CTP_KEY : (wrap_hash(0x60A0u + cell) & 0x00FFFFFFu)) | 0xC0000000u;
        uint32_t cv[3] = { c, c, c };
        if (mode == 1 || (mode == 2 && ((cell / 2u) & 1u))) {
            cv[cell % 3u] ^= flip[cell % 3u];
            (*nvary)++;
        }
        v[n++] = (ParityGuestVtx){ cv[0], x + 1.0f, y + 1.0f, 0.0f };
        v[n++] = (ParityGuestVtx){ cv[1], x + 15.0f, y + 2.0f, 0.0f };
        v[n++] = (ParityGuestVtx){ cv[2], x + 3.0f, y + 17.0f, 0.0f };
    }
    stp_emit(&l, 0x01, 0x08060000u & 0x00FFFFFFu);
    stp_emit(&l, 0x04, (3u << 16) | n);
    stp_emit(&l, 0x50, 0);
    stp_emit(&l, 0x27, 0);
    stp_end(&l);
    *ntris = n / 3u;
    return 0x08040000u;
}

/* Run `list` on the GPU path or through ge.c over a fresh hashed background, and copy the
 * target into `out`. Returns gegpu_sync_guest_fb's status on the GPU path (GEGPU_SYNC_OK for
 * ge.c). */
static int ctp_run(uint32_t base, uint32_t fmt, uint32_t list, int gpu, uint8_t *out) {
    const uint32_t bytes = FB_W * FB_H * (fmt == 3u ? 4u : 2u);
    coherence_reset_targets();
    stp_fill_background(base, fmt);
    ge_set_gpu_hooks(gpu ? &k_hooks : NULL);
    ge_run_list(list, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (gpu) {
        GeGpuFbDescriptor desc = { .addr = base, .format = fmt, .stride = 512u,
                                   .width = 480, .height = FB_H };
        const int sync = gegpu_sync_guest_fb(&desc);
        /* No GPU picture to sync means ge.c drew the whole list: guest VRAM holds it now, and
         * only now, before the next run repaints the background. */
        memcpy(out, SR_HOST(base), bytes);
        return sync;
    }
    memcpy(out, SR_HOST(base), bytes);
    return GEGPU_SYNC_OK;
}

static int coherence_run_color_test(void) {
    static const char *const names[CTP_KINDS] = { "flat-8888", "flat-5650", "flat-5551",
                                                  "flat-4444", "blended-8888", "textured-8888",
                                                  "doubled-8888", "modulated-8888" };
    const uint32_t base = 0x04000000u | 0x00140000u, bytes = FB_W * FB_H * 4u;
    uint8_t *gpu_out = (uint8_t *)malloc(bytes), *sw_out = (uint8_t *)malloc(bytes);
    uint8_t *off_out = (uint8_t *)malloc(bytes);
    if (!gpu_out || !sw_out || !off_out) { free(gpu_out); free(sw_out); free(off_out); return 0; }
    GeState saved = *s_ge;
    int ok = 1, max_rgb = 0;
    unsigned differing = 0, min_drawn = UINT32_MAX, min_rejected = UINT32_MAX;
    for (int kind = 0; kind < CTP_KINDS && ok; kind++) {
        const uint32_t fmt = ctp_fmt(kind), bpp = fmt == 3u ? 4u : 2u;
        const int blended = kind == CTP_BLEND_8888, stays = kind == CTP_TEXMOD_8888;
        const char *name = names[kind];
        const GeGpuReplayStats before = s_replay_stats;
        const int sync = ctp_run(base, fmt, ctp_list(base, kind, 1), 1, gpu_out);
        const GeGpuReplayStats after = s_replay_stats;
        (void)ctp_run(base, fmt, ctp_list(base, kind, 1), 0, sw_out);
        (void)ctp_run(base, fmt, ctp_list(base, kind, 0), 0, off_out);
        const unsigned long long readbacks =
            after.fallback_readbacks[GEGPU_FALLBACK_COLOR_TEST] - before.fallback_readbacks[GEGPU_FALLBACK_COLOR_TEST];
        const unsigned long long fell = after.fallback_prims - before.fallback_prims;
        if (stays ? fell != 8u : (fell != 0u || readbacks != 0u)) {
            fprintf(stderr, "gpu coherence selftest [color-test]: %s list took the software fallback "
                            "for %llu primitives (%llu color-test readbacks), want %s\n", name,
                    fell, readbacks, stays ? "all 8" : "0");
            ok = 0;
        }
        if (!stays && sync != GEGPU_SYNC_OK) {
            fprintf(stderr, "gpu coherence selftest [color-test]: %s list: the GPU target did not "
                            "sync (status %d)\n", name, sync);
            ok = 0;
            break;
        }
        unsigned drawn = 0, rejected = 0;
        for (uint32_t i = 0; i < FB_W * FB_H; i++) {
            uint32_t g = 0, w = 0, o = 0;
            memcpy(&g, gpu_out + i * bpp, bpp);
            memcpy(&w, sw_out + i * bpp, bpp);
            memcpy(&o, off_out + i * bpp, bpp);
            const uint32_t bg = fmt == 3u ? wrap_hash(0x3C1u + i * 40503u)
                                          : (uint32_t)(uint16_t)wrap_hash(0x3C1u + i * 40503u);
            if (w != bg) drawn++;
            if (w != o) rejected++;
            if (i % FB_W >= 480u) continue;
            int bad;
            if (blended) {
                bad = (g >> 24) != (w >> 24);
                for (int c = 0; c < 3; c++) {
                    int d = (int)((g >> (8 * c)) & 0xFFu) - (int)((w >> (8 * c)) & 0xFFu);
                    if (d < 0) d = -d;
                    if (d > max_rgb) max_rgb = d;
                    if (d > STP_FF_BLEND_BOUND) bad = 1;
                }
            } else {
                bad = g != w;
            }
            if (!bad) continue;
            if (!differing++)
                fprintf(stderr, "gpu coherence selftest [color-test]: %s x=%u y=%u gpu=%08x ge_c=%08x\n",
                        name, i % FB_W, i / FB_W, g, w);
            ok = 0;
        }
        if (drawn < 512u || rejected < 256u) {
            fprintf(stderr, "gpu coherence selftest [color-test]: %s list drew %u pixels and the test "
                            "rejected %u, want at least 512 and 256\n", name, drawn, rejected);
            ok = 0;
        }
        if (drawn < min_drawn) min_drawn = drawn;
        if (rejected < min_rejected) min_rejected = rejected;
        coherence_reset_targets();
    }
    /* Gouraud triangles: only those whose vertex colours differ go to ge.c. */
    unsigned gouraud_gpu = 0;
    for (int mode = 0; mode < 3 && ok; mode++) {
        static const char *const gname[3] = { "gouraud-one-colour", "gouraud-varying", "gouraud-mixed" };
        unsigned ntris = 0, nvary = 0;
        const GeGpuReplayStats before = s_replay_stats;
        const int sync = ctp_run(base, 3u, ctp_gouraud_list(base, mode, 1, &ntris, &nvary), 1, gpu_out);
        const GeGpuReplayStats after = s_replay_stats;
        (void)ctp_run(base, 3u, ctp_gouraud_list(base, mode, 1, &ntris, &nvary), 0, sw_out);
        (void)ctp_run(base, 3u, ctp_gouraud_list(base, mode, 0, &ntris, &nvary), 0, off_out);
        const unsigned long long fell = after.fallback_prims - before.fallback_prims;
        if (fell != nvary || (mode == 0 && sync != GEGPU_SYNC_OK)) {
            fprintf(stderr, "gpu coherence selftest [color-test]: %s list: %llu of %u triangles took the "
                            "software fallback, want the %u whose vertex colours differ (sync status %d)\n",
                    gname[mode], fell, ntris, nvary, sync);
            ok = 0;
        }
        gouraud_gpu += ntris - nvary;
        unsigned drawn = 0, rejected = 0, bad = 0;
        for (uint32_t i = 0; i < FB_W * FB_H; i++) {
            uint32_t g, w, o;
            memcpy(&g, gpu_out + i * 4u, 4u);
            memcpy(&w, sw_out + i * 4u, 4u);
            memcpy(&o, off_out + i * 4u, 4u);
            if (w != wrap_hash(0x3C1u + i * 40503u)) drawn++;
            if (w != o) rejected++;
            if (i % FB_W >= 480u || g == w) continue;
            if (!bad++)
                fprintf(stderr, "gpu coherence selftest [color-test]: %s x=%u y=%u gpu=%08x ge_c=%08x\n",
                        gname[mode], i % FB_W, i / FB_W, g, w);
        }
        if (bad || drawn < 512u || rejected < 256u) {
            fprintf(stderr, "gpu coherence selftest [color-test]: %s list: %u pixels differ from ge.c; it drew "
                            "%u pixels and the test rejected %u (want 0, and at least 512 and 256)\n",
                    gname[mode], bad, drawn, rejected);
            ok = 0;
        }
        coherence_reset_targets();
    }
    free(gpu_out); free(sw_out); free(off_out);
    *s_ge = saved;
    /* tri_colours_vary(): one colour, then each channel of each of the first two vertices changed
     * (the third is the one the other two are compared with, so changing it changes both). */
    {
        GeVtx tv[3];
        memset(tv, 0, sizeof tv);
        for (int k = 0; k < 3; k++) { tv[k].r = 0x80; tv[k].g = 0x30; tv[k].b = 0xC0; tv[k].a = 0xFF; }
        int vary_bad = tri_colours_vary(&tv[0], &tv[1], &tv[2]) != 0;
        for (int k = 0; k < 3; k++)
            for (int ch = 0; ch < 4; ch++) {
                GeVtx t2[3];
                memcpy(t2, tv, sizeof t2);
                int *field = ch == 0 ? &t2[k].r : ch == 1 ? &t2[k].g : ch == 2 ? &t2[k].b : &t2[k].a;
                *field ^= 1;
                if (!tri_colours_vary(&t2[0], &t2[1], &t2[2])) vary_bad++;
            }
        if (vary_bad) {
            fprintf(stderr, "gpu coherence selftest [color-test]: tri_colours_vary gave %d wrong answers of 13\n",
                    vary_bad);
            ok = 0;
        }
    }
    /* color_test_colour_inexact(interp, persp) under Gouraud shading, fog, a texture function
     * (0xFFFF: untextured; bit 8 texture alpha, bit 16 colour doubling), a filter and a
     * texture format (3 8888, 0 5650, 2 4444, 5 CLUT8). */
    static const struct { int interp, persp, gouraud, fog; uint32_t tfunc, filter; int want; uint32_t fmt; } cx[] = {
        { 0, 0, 0, 0, 0xFFFFu, 0u, 0, 3u },      /* untextured flat sprite */
        { 1, 0, 0, 0, 0xFFFFu, 0u, 0, 3u },      /* untextured flat triangle */
        { 1, 0, 1, 0, 0xFFFFu, 0u, 1, 3u },      /* Gouraud triangle whose vertex colours differ */
        { 0, 0, 1, 0, 0xFFFFu, 0u, 0, 3u },      /* Gouraud sprite, or a triangle of one colour */
        { 0, 1, 0, 1, 0xFFFFu, 0u, 1, 3u },      /* transform-mode fog */
        { 0, 0, 0, 1, 0xFFFFu, 0u, 0, 3u },      /* fog has no effect in through mode */
        { 0, 0, 0, 0, 0x103u, 0u, 0, 3u },       /* replace, nearest */
        { 0, 0, 0, 0, 0x001u, 0u, 0, 3u },       /* decal without texture alpha */
        { 0, 0, 0, 0, 0x101u, 0u, 1, 3u },       /* decal with texture alpha: a float mix */
        { 0, 0, 0, 0, 0x100u, 0u, 1, 3u },       /* modulate */
        { 0, 0, 0, 0, 0x102u, 0u, 1, 3u },       /* blend */
        { 0, 0, 0, 0, 0x104u, 0u, 1, 3u },       /* add */
        { 0, 0, 0, 0, 0x103u, 0x101u, 1, 3u },   /* replace, filtered */
        { 0, 0, 0, 0, 0x10103u, 0u, 0, 3u }, /* replace, colour doubled: whole texels doubled and clamped */
        { 0, 0, 0, 0, 0x10001u, 0u, 0, 3u }, /* decal without texture alpha, colour doubled */
        { 0, 0, 0, 0, 0x10100u, 0u, 1, 3u }, /* modulate, colour doubled */
        { 0, 0, 0, 0, 0x103u, 0u, 0, 0u },   /* replace, 5650 texels */
        { 0, 0, 0, 0, 0x103u, 0u, 0, 2u },   /* replace, 4444 texels */
        { 0, 0, 0, 0, 0x103u, 0u, 0, 5u },   /* replace, CLUT8 texels */
        { 0, 0, 0, 0, 0x100u, 0u, 1, 5u },   /* modulate, CLUT8 texels */
    };
    for (unsigned i = 0; i < sizeof(cx) / sizeof(cx[0]); i++) {
        s_ge->color_test_enable = 1; s_ge->color_test = 2u; s_ge->clear = 0;
        s_ge->shade_gouraud = cx[i].gouraud; s_ge->fog_enable = cx[i].fog;
        s_ge->tex_enable = cx[i].tfunc != 0xFFFFu; s_ge->tex_addr = s_ge->tex_enable ? CTP_TEX : 0u;
        s_ge->tex_func = cx[i].tfunc & 0x101FFu; s_ge->tex_filter = cx[i].filter;
        s_ge->tex_fmt = cx[i].fmt;
        s_ge->tex_max_level = 0u;
        const int got = color_test_colour_inexact(cx[i].interp, cx[i].persp);
        if (got != cx[i].want) {
            fprintf(stderr, "gpu coherence selftest [color-test]: colour-exactness cell %u gave %d, want %d\n",
                    i, got, cx[i].want);
            ok = 0;
        }
    }
    *s_ge = saved;
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [color-test]: %u pixels outside the contract\n", differing);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s lists=%d GPU lists: fallback_prims=0 unblended exact, "
           "blended rgb_max_delta=%d (bound %d) alpha exact; modulated list on ge.c; min drawn=%u "
           "rejected=%u; Gouraud lists=3: %u one-colour triangles on the GPU, the varying ones on ge.c, "
           "exact; exactness cells=%u\n", "color-test", CTP_KINDS, max_rgb, STP_FF_BLEND_BOUND,
           min_drawn, min_rejected, gouraud_gpu, (unsigned)(sizeof(cx) / sizeof(cx[0])));
    return 1;
}

/* Lines on the GPU path. hook_line runs ge.c draw_line_vtx's DDA and draws each pixel's own
 * values, so an untextured line list must stay on the GPU (no software fallback) and leave
 * exactly what ge.c leaves. Each list holds hashed through-mode lines over a hashed background:
 * shallow and steep, in both directions, axis-aligned and diagonal, endpoints on pixel centres
 * and edges, and lines shorter than a pixel. Kinds: flat and Gouraud colour on 8888, Gouraud on
 * 5650 and 4444, Gouraud under a GEQUAL depth test with depth writes over a striped depth
 * buffer, a line strip, and lines under the color test. A textured line samples by ge.c's line
 * footprint, so the textured list must still take the software rasterizer, and match it. */
enum { LTP_FLAT_8888, LTP_GOURAUD_8888, LTP_GOURAUD_5650, LTP_GOURAUD_4444, LTP_DEPTH_8888,
       LTP_STRIP_8888, LTP_CTEST_8888, LTP_TEX_8888, LTP_KINDS };
#define LTP_LINES 160u

static uint32_t ltp_fmt(int kind) {
    return kind == LTP_GOURAUD_5650 ? 0u : kind == LTP_GOURAUD_4444 ? 2u : 3u;
}

/* Endpoint `e` (0 or 1) of line `i`: hashed, on a 1/16 grid, with every few lines a special
 * shape. */
static void ltp_point(uint32_t i, int e, float *x, float *y, float *z) {
    const uint32_t hx = wrap_hash(0x11E0u + i * 4u), hy = wrap_hash(0x11E1u + i * 4u);
    const uint32_t hx2 = wrap_hash(0x11E2u + i * 4u), hy2 = wrap_hash(0x11E3u + i * 4u);
    float x0 = 4.0f + (float)(hx % (468u * 16u)) / 16.0f, y0 = 4.0f + (float)(hy % (260u * 16u)) / 16.0f;
    float x1 = 4.0f + (float)(hx2 % (468u * 16u)) / 16.0f, y1 = 4.0f + (float)(hy2 % (260u * 16u)) / 16.0f;
    switch (i % 8u) {
        case 1: y1 = y0; break;                                    /* horizontal */
        case 2: x1 = x0; break;                                    /* vertical */
        case 3: x1 = x0 + 40.0f; y1 = y0 + 40.0f; break;           /* diagonal */
        case 4: x1 = x0 - 0.375f; y1 = y0 + 0.25f; break;          /* under a pixel */
        case 5: x0 = floorf(x0) + 0.5f; y0 = floorf(y0) + 0.5f;    /* pixel centres */
                x1 = floorf(x1) + 0.5f; y1 = floorf(y1) + 0.5f; break;
        case 6: x0 = floorf(x0); y0 = floorf(y0); x1 = floorf(x1); y1 = floorf(y1); break;
        default: break;
    }
    if (x1 > 475.0f) x1 = 475.0f;
    if (y1 > 267.0f) y1 = 267.0f;
    *x = e ? x1 : x0; *y = e ? y1 : y0;
    *z = (float)((wrap_hash(0x11E4u + i * 2u + (uint32_t)e) >> 8) & 0xFFFFu);
}

static uint32_t ltp_list(uint32_t base, int kind) {
    const int depth = kind == LTP_DEPTH_8888, strip = kind == LTP_STRIP_8888;
    const int textured = kind == LTP_TEX_8888;
    StpList l; stp_begin(&l, base, ltp_fmt(kind), depth);
    stp_emit(&l, 0x24, 0);                               /* no stencil test */
    stp_emit(&l, 0x21, 0);
    uint32_t vaddr = 0x08060000u;
    if (depth) {
        stp_emit(&l, 0xD3, 1u | 0x400u | 0x100u);        /* fill depth and colour */
        for (uint32_t x = 0; x < 480u; x += 4u)
            stp_sprite(&l, &vaddr, 0xFF808080u, (float)x, 0.0f, (float)(x + 4u), (float)FB_H,
                       (x & 4u) ? 49152.0f : 16384.0f);
        stp_emit(&l, 0xD3, 0);
        stp_emit(&l, 0xDE, 7u);                          /* GEQUAL */
        stp_emit(&l, 0xE7, 0u);                          /* depth writes on */
    }
    stp_emit(&l, 0x50, kind == LTP_FLAT_8888 ? 0u : 1u);
    if (kind == LTP_CTEST_8888) {
        stp_emit(&l, 0x27, 1);
        stp_emit(&l, 0xD8, 3u);                          /* not equal under the mask */
        stp_emit(&l, 0xD9, 0x000080u);
        stp_emit(&l, 0xDA, 0x000080u);                   /* drops pixels with red >= 128 */
    }
    if (textured) {
        uint32_t *tex = (uint32_t *)SR_HOST(CTP_TEX);
        for (uint32_t i = 0; i < 16u * 16u; i++) tex[i] = wrap_hash(0x7E1u + i);
        stp_emit(&l, 0xA0, CTP_TEX & 0x00FFFFFFu);
        stp_emit(&l, 0xA8, 16u | ((CTP_TEX & 0xFF000000u) >> 8));
        stp_emit(&l, 0xB8, 4u | (4u << 8));
        stp_emit(&l, 0xC0, 0); stp_emit(&l, 0xC2, 0); stp_emit(&l, 0xC3, 3);
        stp_emit(&l, 0xC6, 0); stp_emit(&l, 0xC7, 0);
        stp_emit(&l, 0xC9, 3u | (1u << 8));
        stp_emit(&l, 0xCB, 0);
        stp_emit(&l, 0x1E, 1);
        stp_emit(&l, 0x12, 3u | (7u << 2) | (3u << 7) | (1u << 23));
        CtpTexVtx *v = (CtpTexVtx *)SR_HOST(vaddr);
        for (uint32_t i = 0; i < LTP_LINES; i++)
            for (int e = 0; e < 2; e++) {
                float x, y, z;
                ltp_point(i, e, &x, &y, &z);
                const uint32_t h = wrap_hash(0x7E9u + i * 2u + (uint32_t)e);
                v[i * 2u + (uint32_t)e] = (CtpTexVtx){ (float)(h & 0xFFu) / 16.0f,
                                                       (float)((h >> 8) & 0xFFu) / 16.0f,
                                                       0xFFFFFFFFu, x, y, z };
            }
        stp_emit(&l, 0x01, vaddr & 0x00FFFFFFu);
        stp_emit(&l, 0x04, (1u << 16) | (2u * LTP_LINES));
        stp_emit(&l, 0x1E, 0);
        stp_emit(&l, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
    } else {
        ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(vaddr);
        const uint32_t n = strip ? LTP_LINES + 1u : 2u * LTP_LINES;
        for (uint32_t k = 0; k < n; k++) {
            float x, y, z;
            if (strip) ltp_point(k, 0, &x, &y, &z);
            else ltp_point(k / 2u, (int)(k & 1u), &x, &y, &z);
            v[k] = (ParityGuestVtx){ wrap_hash(0x5A1u + k) | 0xFF000000u, x, y, z };
        }
        stp_emit(&l, 0x01, vaddr & 0x00FFFFFFu);
        stp_emit(&l, 0x04, ((strip ? 2u : 1u) << 16) | n);
    }
    stp_emit(&l, 0x27, 0);
    stp_emit(&l, 0x50, 1);
    stp_end(&l);
    return 0x08040000u;
}

static int coherence_run_lines(void) {
    static const char *const names[LTP_KINDS] = { "flat-8888", "gouraud-8888", "gouraud-5650",
                                                  "gouraud-4444", "depth-8888", "strip-8888",
                                                  "color-test-8888", "textured-8888" };
    const uint32_t base = 0x04000000u | 0x00140000u, bytes = FB_W * FB_H * 4u;
    uint8_t *gpu_out = (uint8_t *)malloc(bytes), *sw_out = (uint8_t *)malloc(bytes);
    uint16_t *gpu_z = (uint16_t *)malloc(FB_W * FB_H * 2u);
    if (!gpu_out || !sw_out || !gpu_z) { free(gpu_out); free(sw_out); free(gpu_z); return 0; }
    GeState saved = *s_ge;
    int ok = 1;
    unsigned differing = 0, min_drawn = UINT32_MAX;
    for (int kind = 0; kind < LTP_KINDS && ok; kind++) {
        const uint32_t fmt = ltp_fmt(kind), bpp = fmt == 3u ? 4u : 2u;
        const int textured = kind == LTP_TEX_8888;
        const char *name = names[kind];
        const GeGpuReplayStats before = s_replay_stats;
        const int sync = ctp_run(base, fmt, ltp_list(base, kind), 1, gpu_out);
        const GeGpuReplayStats after = s_replay_stats;
        /* The depth list's lines write depth: read the GPU's back into s_zbuf and keep it. */
        int zok = 1;
        if (kind == LTP_DEPTH_8888 && sync == GEGPU_SYNC_OK) {
            Target *t = target_find_by_fba(vram_off(base));
            zok = t && t->dep && depth_to_cpu(t->dep);
            if (zok) memcpy(gpu_z, s_zbuf, FB_W * FB_H * 2u);
        }
        (void)ctp_run(base, fmt, ltp_list(base, kind), 0, sw_out);
        if (kind == LTP_DEPTH_8888) {
            unsigned zdiff = 0;
            for (uint32_t i = 0; zok && i < FB_W * FB_H; i++)
                if (i % FB_W < 480u && gpu_z[i] != s_zbuf[i]) {
                    if (!zdiff++)
                        fprintf(stderr, "gpu coherence selftest [lines]: depth x=%u y=%u gpu=%04x ge_c=%04x\n",
                                i % FB_W, i / FB_W, gpu_z[i], s_zbuf[i]);
                }
            if (!zok || zdiff) {
                fprintf(stderr, "gpu coherence selftest [lines]: depth-8888: %s, %u depth values differ\n",
                        zok ? "read back" : "no GPU depth to read back", zdiff);
                ok = 0;
            }
        }
        const unsigned long long fell = after.fallback_prims - before.fallback_prims;
        if (textured ? fell == 0u : fell != 0u) {
            fprintf(stderr, "gpu coherence selftest [lines]: %s list took the software fallback for "
                            "%llu primitives, want %s\n", name, fell, textured ? "all" : "0");
            ok = 0;
        }
        if (!textured && sync != GEGPU_SYNC_OK) {
            fprintf(stderr, "gpu coherence selftest [lines]: %s list: the GPU target did not sync "
                            "(status %d)\n", name, sync);
            ok = 0;
            break;
        }
        unsigned drawn = 0;
        for (uint32_t i = 0; i < FB_W * FB_H; i++) {
            uint32_t g = 0, w = 0;
            memcpy(&w, sw_out + i * bpp, bpp);
            const uint32_t bg = fmt == 3u ? wrap_hash(0x3C1u + i * 40503u)
                                          : (uint32_t)(uint16_t)wrap_hash(0x3C1u + i * 40503u);
            if (w != bg) drawn++;
            if (textured && sync != GEGPU_SYNC_OK) continue;   /* ge.c drew it: nothing to compare */
            memcpy(&g, gpu_out + i * bpp, bpp);
            if (i % FB_W >= 480u || g == w) continue;
            if (!differing++)
                fprintf(stderr, "gpu coherence selftest [lines]: %s x=%u y=%u gpu=%08x ge_c=%08x\n",
                        name, i % FB_W, i / FB_W, g, w);
            ok = 0;
        }
        if (drawn < 2000u) {
            fprintf(stderr, "gpu coherence selftest [lines]: %s list drew only %u pixels\n", name, drawn);
            ok = 0;
        }
        if (drawn < min_drawn) min_drawn = drawn;
        coherence_reset_targets();
    }
    free(gpu_out); free(sw_out); free(gpu_z);
    *s_ge = saved;
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [lines]: %u pixels differ from ge.c\n", differing);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s lists=%d untextured: fallback_prims=0, exact; textured: "
           "ge.c; depth list's depth exact; min drawn=%u\n", "lines", LTP_KINDS, min_drawn);
    return 1;
}

/* The fallback classifier names the first unsupported state, in the order the support
 * check always tested them, and a supported state names none. */
static int coherence_run_fallback_reasons(void) {
    GeState saved = *s_ge;
    int ok = 1;
    struct { const char *what; int want; } cells[] = {
        {"stencil", GEGPU_FALLBACK_STENCIL}, {"color test (psp.frag runs it)", -1},
        {"logic op", GEGPU_FALLBACK_LOGIC_OP}, {"ram fb", GEGPU_FALLBACK_RAM_FB},
        {"wide stride", GEGPU_FALLBACK_WIDE_STRIDE},
    };
    s_ge->stencil_enable = s_ge->color_test_enable = s_ge->logic_op_enable = 0;
    s_ge->clear = 0; s_ge->mask_rgb = 0;   /* an exact write mask */
    s_ge->ztest_enable = 1;                /* a depth-tested stencil-fail INCR: unsupported */
    s_ge->stencil_test = 2u; s_ge->stencil_op = 4u;
    s_ge->fbp = 0x04000000u; s_ge->fbw = 512u;
    s_ge->tex_enable = 0;
    if (gpu_state_unsupported_reason() != -1) ok = 0;
    for (unsigned i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        s_ge->stencil_enable = i == 0;
        s_ge->color_test_enable = i == 1;
        s_ge->logic_op_enable = i == 2;
        s_ge->fbp = i == 3 ? 0x08000000u : 0x04000000u;   /* main RAM, not VRAM */
        s_ge->fbw = i == 4 ? 1024u : 512u;
        const int got = gpu_state_unsupported_reason();
        if (got != cells[i].want) {
            fprintf(stderr, "gpu coherence selftest [fallback-reasons]: %s gave %d, want %d\n",
                    cells[i].what, got, cells[i].want);
            ok = 0;
        }
    }
    s_ge->fbp = 0x04000000u; s_ge->fbw = 512u;
    s_ge->stencil_enable = 1; s_ge->color_test_enable = 1;   /* stencil is tested first */
    if (gpu_state_unsupported_reason() != GEGPU_FALLBACK_STENCIL) ok = 0;
    /* The stencil states the GPU path takes: clear mode (ge.c ignores the test there), no
     * depth test, or KEEP for the ops the depth test can leave to run. A depth test that
     * always passes (func 1) never runs its fail op. */
    struct { const char *what; int clear, ztest; uint32_t zfunc, test, op; int want; } st[] = {
        {"stencil clear", 1, 1, 4u, 2u, 0x000004u, -1},
        {"stencil no depth", 0, 0, 4u, 2u, 0x040404u, -1},
        {"stencil always", 0, 1, 4u, 1u, 0x020004u, -1},
        {"stencil keep", 0, 1, 4u, 2u, 0x050000u, -1},
        {"stencil zfail op", 0, 1, 4u, 1u, 0x000100u, GEGPU_FALLBACK_STENCIL},
        {"stencil zfail op, depth always", 0, 1, 1u, 1u, 0x010101u, -1},
    };
    s_ge->color_test_enable = 0;
    for (unsigned i = 0; i < sizeof(st) / sizeof(st[0]); i++) {
        s_ge->clear = st[i].clear; s_ge->ztest_enable = st[i].ztest; s_ge->ztest = st[i].zfunc;
        s_ge->stencil_test = st[i].test; s_ge->stencil_op = st[i].op;
        const int got = gpu_state_unsupported_reason();
        if (got != st[i].want) {
            fprintf(stderr, "gpu coherence selftest [fallback-reasons]: %s gave %d, want %d\n",
                    st[i].what, got, st[i].want);
            ok = 0;
        }
    }
    /* Write masks: a channel whose native bits are all masked or all written is the colour write
     * mask's; any other falls back. Clear mode checks RGB when it writes colour (0x100) and alpha
     * when it writes alpha (0x200); a draw's alpha is the stencil plane. */
    struct { const char *what; int clear; uint32_t mode, fmt, rgb, alpha; int want; } wm[] = {
        {"draw 8888 green masked", 0, 0u, 3u, 0x00ff00u, 0x00u, -1},
        {"draw 8888 partial green", 0, 0u, 3u, 0x00f000u, 0x00u, GEGPU_FALLBACK_WRITE_MASK},
        {"draw 8888 partial alpha", 0, 0u, 3u, 0x000000u, 0x7fu, -1},
        {"clear 8888 alpha 0xff", 1, 0x300u, 3u, 0x000000u, 0xffu, -1},
        {"clear 8888 alpha 0x7f", 1, 0x300u, 3u, 0x000000u, 0x7fu, GEGPU_FALLBACK_WRITE_MASK},
        {"clear 4444 alpha 0x0f", 1, 0x300u, 2u, 0x000000u, 0x0fu, -1},
        {"clear 4444 alpha 0x1f", 1, 0x300u, 2u, 0x000000u, 0x1fu, GEGPU_FALLBACK_WRITE_MASK},
        {"clear 5551 alpha 0x7f", 1, 0x300u, 1u, 0x000000u, 0x7fu, -1},
        {"clear 565 rgb 0x070307", 1, 0x300u, 0u, 0x070307u, 0x00u, -1},
        {"clear 565 rgb 0x070707", 1, 0x300u, 0u, 0x070707u, 0x00u, GEGPU_FALLBACK_WRITE_MASK},
        {"clear alpha only, partial rgb", 1, 0x200u, 3u, 0x0f55f0u, 0x00u, -1},
        {"clear colour only, partial alpha", 1, 0x100u, 3u, 0x000000u, 0xc3u, -1},
        {"clear colour, partial rgb", 1, 0x100u, 3u, 0x0f55f0u, 0x00u, GEGPU_FALLBACK_WRITE_MASK},
        {"clear depth only, partial masks", 1, 0x400u, 3u, 0x0f55f0u, 0xc3u, -1},
    };
    s_ge->stencil_enable = 0; s_ge->ztest_enable = 0;
    for (unsigned i = 0; i < sizeof(wm) / sizeof(wm[0]); i++) {
        s_ge->clear = wm[i].clear; s_ge->clear_mode = wm[i].mode; s_ge->fbfmt = wm[i].fmt;
        s_ge->mask_rgb = wm[i].rgb; s_ge->mask_alpha = (int)wm[i].alpha;
        const int got = gpu_state_unsupported_reason();
        if (got != wm[i].want) {
            fprintf(stderr, "gpu coherence selftest [fallback-reasons]: %s gave %d, want %d\n",
                    wm[i].what, got, wm[i].want);
            ok = 0;
        }
    }
    *s_ge = saved;
    if (ok) printf("gpu coherence selftest: PASS %-22s 12 cells, %u write-mask cells\n", "fallback-reasons",
                   (unsigned)(sizeof(wm) / sizeof(wm[0])));
    return ok;
}

/* GPU transform and lighting parity (#318). Each cell is one display list of project-written
 * command words and generated vertices (no game data) drawing transform-mode triangles into a
 * cleared 512x272 8888 target with a depth buffer. It runs three ways: through the backend with
 * GPU T&L on, through the backend with it off (ge.c's T&L, then hook_tri), and in software.
 * The first two must leave byte-identical colour and depth, and the T&L run must count every
 * transform PRIM as drawn by psp_tnl.vert, or, for a cell whose state ge.c declines (a
 * near-plane crossing, specular light, texgen, flat shading), as declined: a run that silently
 * took the other route would prove nothing. The software run is reported, not asserted: the
 * Vulkan rasterizer's own tolerances against ge.c are the blend and sample-point fixtures'. On a
 * device without fp32 subnormal preservation there is no T&L stage, and every cell must take
 * ge.c's route and match: the fallback is what that device can show. */
enum {
    TNLC_STATIC, TNLC_STRIP_CULL0, TNLC_STRIP_CULL1, TNLC_FAN_IDX16, TNLC_LIST_IDX8,
    TNLC_TEXTURED, TNLC_FOG_LINEAR, TNLC_FOG_INF, TNLC_DEPTH, TNLC_RAMP, TNLC_FIXED_FMT,
    TNLC_MORPH2, TNLC_SKIN1, TNLC_SKIN2_U8, TNLC_SKIN4_U16, TNLC_SKIN8_F, TNLC_LIGHT_DIR,
    TNLC_LIGHT_POINT, TNLC_LIGHT4_MATUPD, TNLC_LIGHT_SKIN, TNLC_OFFSET_VIEWPORT,
    TNLC_NEAR_CROSS, TNLC_SPECULAR, TNLC_FLAT, TNLC_TEXGEN, TNLC_LIGHT_SUBNORMAL,
    TNLC_SPECULAR_EXACT, TNLC_LIGHT4_POW,
};

/* The cells whose specular and powered-diffuse power is ge_powf on both routes. */
static int tnlc_pow_exact(int kind) { return kind == TNLC_SPECULAR_EXACT || kind == TNLC_LIGHT4_POW; }

typedef struct TnlCell {
    const char *name;
    int kind;
    int gpu;          /* 1: drawn by psp_tnl.vert, 0: declined to ge.c */
} TnlCell;

#define TNLP_FBA     0x00000000u                 /* VRAM-relative colour buffer */
#define TNLP_ZBA     0x00100000u                 /* VRAM-relative depth buffer */
#define TNLP_TEX     0x04180000u                 /* 8x8 8888 texture */
#define TNLP_LIST    0x00200000u                 /* the harness arena, as the other fixtures */
#define TNLP_VTX     0x00210000u
#define TNLP_IDX     0x00280000u
#define TNLP_STRIDE  512u

static uint32_t tnlp_f24(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 8; }

/* Vertex writer following decode_vtype's order and alignment: weights, texture coordinate,
 * colour (8888 only), normal, position; each field aligned to its own size, the vertex to the
 * largest. fmt values are the vertex type's field codes (0 none, 1 8-bit, 2 16-bit, 3 float). */
typedef struct { int wfmt, wn, tc, col, nrm, pos, morph; } TnlpFmt;

static uint32_t tnlp_vtype(const TnlpFmt *f, int idx) {
    uint32_t vt = (uint32_t)f->tc | ((uint32_t)(f->col ? 7 : 0) << 2) | ((uint32_t)f->nrm << 5) |
                  ((uint32_t)f->pos << 7) | ((uint32_t)idx << 11);
    if (f->wfmt) vt |= ((uint32_t)f->wfmt << 9) | ((uint32_t)(f->wn - 1) << 14);
    if (f->morph > 1) vt |= (uint32_t)(f->morph - 1) << 18;
    return vt;
}

static uint32_t tnlp_align(uint32_t off, uint32_t a) { return (off + a - 1u) & ~(a - 1u); }

static void tnlp_put(uint8_t *d, uint32_t off, int fmt, float v, int is_unsigned) {
    if (fmt == 3) { memcpy(d + off, &v, 4); return; }
    if (fmt == 2) {
        int32_t q = (int32_t)lrintf(v * 32768.0f);
        if (is_unsigned) { if (q < 0) q = 0; if (q > 65535) q = 65535; }
        else { if (q < -32768) q = -32768; if (q > 32767) q = 32767; }
        uint16_t s = (uint16_t)q; memcpy(d + off, &s, 2); return;
    }
    int32_t q = (int32_t)lrintf(v * 128.0f);
    if (is_unsigned) { if (q < 0) q = 0; if (q > 255) q = 255; }
    else { if (q < -128) q = -128; if (q > 127) q = 127; }
    d[off] = (uint8_t)q;
}

/* One morph target of one vertex at d; returns the target's stride. */
static uint32_t tnlp_vertex(uint8_t *d, const TnlpFmt *f, const float *w, float u, float v,
                            uint32_t col, const float *n, const float *p) {
    static const uint32_t sz[4] = { 0, 1, 2, 4 };
    uint32_t off = 0, maxa = 1;
    if (f->wfmt) {
        uint32_t a = sz[f->wfmt];
        off = tnlp_align(off, a);
        for (int i = 0; i < f->wn; i++) tnlp_put(d, off + a * (uint32_t)i, f->wfmt, w[i], 1);
        off += a * (uint32_t)f->wn;
        if (a > maxa) maxa = a;
    }
    if (f->tc) {
        uint32_t a = sz[f->tc];
        off = tnlp_align(off, a);
        tnlp_put(d, off, f->tc, u, 1); tnlp_put(d, off + a, f->tc, v, 1);
        off += 2u * a;
        if (a > maxa) maxa = a;
    }
    if (f->col) {
        off = tnlp_align(off, 4u);
        memcpy(d + off, &col, 4);
        off += 4u;
        if (maxa < 4u) maxa = 4u;
    }
    if (f->nrm) {
        uint32_t a = sz[f->nrm];
        off = tnlp_align(off, a);
        for (int i = 0; i < 3; i++) tnlp_put(d, off + a * (uint32_t)i, f->nrm, n[i], 0);
        off += 3u * a;
        if (a > maxa) maxa = a;
    }
    uint32_t a = sz[f->pos];
    off = tnlp_align(off, a);
    for (int i = 0; i < 3; i++) tnlp_put(d, off + a * (uint32_t)i, f->pos, p[i], 0);
    off += 3u * a;
    if (a > maxa) maxa = a;
    return tnlp_align(off, maxa);
}

typedef struct { uint32_t *dl; int n; } TnlpList;
#define TNLP_W(L, cmd, data) ((L)->dl[(L)->n++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(data) & 0x00FFFFFFu))

static void tnlp_matrix(TnlpList *L, uint32_t num_cmd, const float *m, int count) {
    TNLP_W(L, num_cmd, 0);
    for (int i = 0; i < count; i++) TNLP_W(L, num_cmd + 1u, tnlp_f24(m[i]));
}

/* A deterministic pseudo-random sequence: the same vertices on every run and host. */
static uint32_t s_tnlp_seed;
static float tnlp_rand(float lo, float hi) {
    s_tnlp_seed = s_tnlp_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(s_tnlp_seed >> 8) * (1.0f / 16777216.0f);
}

/* A rotation about the axis (ax, ay, az) by `ang` radians, translated by t, as a 4x3 matrix. */
static void tnlp_rot(float *m, float ax, float ay, float az, float ang, float tx, float ty, float tz) {
    float l = sqrtf(ax * ax + ay * ay + az * az);
    ax /= l; ay /= l; az /= l;
    float c = cosf(ang), s = sinf(ang), k = 1.0f - c;
    m[0] = c + ax * ax * k;      m[1] = ay * ax * k + az * s; m[2] = az * ax * k - ay * s;
    m[3] = ax * ay * k - az * s; m[4] = c + ay * ay * k;      m[5] = az * ay * k + ax * s;
    m[6] = ax * az * k + ay * s; m[7] = ay * az * k - ax * s; m[8] = c + az * az * k;
    m[9] = tx; m[10] = ty; m[11] = tz;
}

/* Build cell `c`'s list; returns the list address. */
static uint32_t tnlp_build(const TnlCell *c) {
    TnlpList L = { (uint32_t *)SR_HOST(TNLP_LIST), 0 };
    s_tnlp_seed = 0x1234567u + (uint32_t)c->kind * 7919u;
    const uint32_t fb = 0x04000000u | TNLP_FBA;
    TNLP_W(&L, 0x10, 0);
    TNLP_W(&L, 0x9C, fb & 0x00FFFFFFu);
    TNLP_W(&L, 0x9D, TNLP_STRIDE | ((fb & 0xFF000000u) >> 8));
    TNLP_W(&L, 0xD2, 3);
    TNLP_W(&L, 0x9E, TNLP_ZBA);
    TNLP_W(&L, 0x9F, TNLP_STRIDE);
    TNLP_W(&L, 0x15, 0);
    TNLP_W(&L, 0x16, ((FB_H - 1u) << 10) | (480u - 1u));
    TNLP_W(&L, 0xD4, 0);
    TNLP_W(&L, 0xD5, ((FB_H - 1u) << 10) | (480u - 1u));
    TNLP_W(&L, 0xD6, 0);
    TNLP_W(&L, 0xD7, 0xFFFF);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) TNLP_W(&L, cmd, 0);   /* every unit off */
    TNLP_W(&L, 0xE7, 0);                  /* depth writes on */
    TNLP_W(&L, 0xDE, 1);                  /* depth test ALWAYS when enabled */
    TNLP_W(&L, 0x50, c->kind == TNLC_FLAT ? 0 : 1);
    TNLP_W(&L, 0xE8, 0);
    TNLP_W(&L, 0xE9, 0);

    /* Clear colour and depth with a through-mode clear sprite. */
    const uint32_t clear_vtx = TNLP_VTX + 0x60000u;
    {
        uint8_t *d = (uint8_t *)SR_HOST(clear_vtx);
        const uint32_t bg = 0xff302010u;
        const float cz = c->kind == TNLC_DEPTH ? 65535.0f : 0.0f;
        float p0[3] = { 0.0f, 0.0f, cz }, p1[3] = { 480.0f, 272.0f, cz };
        memcpy(d, &bg, 4); memcpy(d + 4, p0, 12);
        memcpy(d + 16, &bg, 4); memcpy(d + 20, p1, 12);
        TNLP_W(&L, 0xD3, 0x701);
        TNLP_W(&L, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
        TNLP_W(&L, 0x10, (clear_vtx >> 8) & 0x000F0000u);
        TNLP_W(&L, 0x01, clear_vtx);
        TNLP_W(&L, 0x04, (6u << 16) | 2u);
        TNLP_W(&L, 0xD3, 0);
    }

    /* Matrices: a perspective camera looking at the origin from +z. */
    float world[12], view[12], proj[16];
    tnlp_rot(world, 0.3f, 1.0f, 0.2f, 0.6f, 0.0f, 0.0f, 0.0f);
    tnlp_rot(view, 1.0f, 0.0f, 0.0f, 0.15f, 0.05f, -0.1f, -6.0f);
    {
        const float fov = 1.0f, aspect = 480.0f / 272.0f, zn = 0.5f, zf = 100.0f;
        float f = 1.0f / tanf(fov * 0.5f);
        memset(proj, 0, sizeof(proj));
        proj[0] = f / aspect; proj[5] = f;
        proj[10] = (zf + zn) / (zn - zf); proj[11] = -1.0f;
        proj[14] = 2.0f * zf * zn / (zn - zf);
    }
    if (c->kind == TNLC_NEAR_CROSS) view[11] = -1.2f;   /* the mesh reaches through the near plane */
    if (c->kind == TNLC_LIGHT_SUBNORMAL) {
        /* World 3x3 scaled by 2^-30 and view 3x3 by 2^30: the same screen positions (powers of
         * two scale exactly), but world-space normals near 2^-66, whose squared length is
         * subnormal before light_vertex normalises them. */
        for (int i = 0; i < 9; i++) { world[i] = ldexpf(world[i], -30); view[i] = ldexpf(view[i], 30); }
    }
    tnlp_matrix(&L, 0x3A, world, 12);
    tnlp_matrix(&L, 0x3C, view, 12);
    tnlp_matrix(&L, 0x3E, proj, 16);
    const float vxs = c->kind == TNLC_OFFSET_VIEWPORT ? 230.0f : 240.0f;
    const float vys = c->kind == TNLC_OFFSET_VIEWPORT ? -130.0f : -136.0f;
    TNLP_W(&L, 0x42, tnlp_f24(vxs));
    TNLP_W(&L, 0x43, tnlp_f24(vys));
    TNLP_W(&L, 0x44, tnlp_f24(-32000.0f));
    TNLP_W(&L, 0x45, tnlp_f24(2048.0f));
    TNLP_W(&L, 0x46, tnlp_f24(2048.0f));
    TNLP_W(&L, 0x47, tnlp_f24(32767.5f));
    if (c->kind == TNLC_OFFSET_VIEWPORT) {
        TNLP_W(&L, 0x4C, ((2048u - 240u) << 4) + 5u);   /* a sub-pixel offset */
        TNLP_W(&L, 0x4D, ((2048u - 136u) << 4) + 11u);
    } else {
        TNLP_W(&L, 0x4C, (2048u - 240u) << 4);
        TNLP_W(&L, 0x4D, (2048u - 136u) << 4);
    }
    TNLP_W(&L, 0x48, tnlp_f24(1.0f));
    TNLP_W(&L, 0x49, tnlp_f24(1.0f));
    TNLP_W(&L, 0x4A, 0);
    TNLP_W(&L, 0x4B, 0);
    TNLP_W(&L, 0xC0, c->kind == TNLC_TEXGEN ? 1u : 0u);

    /* Texture for the textured cells. */
    if (c->kind == TNLC_TEXTURED || c->kind == TNLC_TEXGEN || c->kind == TNLC_FIXED_FMT) {
        uint32_t *tex = (uint32_t *)SR_HOST(TNLP_TEX);
        for (int v = 0; v < 8; v++)
            for (int u = 0; u < 8; u++)
                tex[v * 8 + u] = 0xff000000u | ((uint32_t)(v * 32) << 8) | (uint32_t)(u * 32) |
                                 (((u ^ v) & 1) ? 0x00c00000u : 0x00200000u);
        sr_gpu_vram_dirty(TNLP_TEX, 8u * 8u * 4u);
        TNLP_W(&L, 0x1E, 1);
        TNLP_W(&L, 0xA0, TNLP_TEX & 0x00FFFFFFu);
        TNLP_W(&L, 0xA8, 8u | ((TNLP_TEX & 0xFF000000u) >> 8));
        TNLP_W(&L, 0xB8, (3u << 8) | 3u);
        TNLP_W(&L, 0xC2, 0);
        TNLP_W(&L, 0xC3, 3);
        TNLP_W(&L, 0xC6, 0);
        TNLP_W(&L, 0xC7, 0);                   /* repeat */
        TNLP_W(&L, 0xC9, 0x100u | 0u);         /* MODULATE, RGBA */
        TNLP_W(&L, 0xCB, 0);
        if (c->kind == TNLC_TEXTURED) {
            TNLP_W(&L, 0x48, tnlp_f24(1.75f));
            TNLP_W(&L, 0x49, tnlp_f24(-0.5f));
            TNLP_W(&L, 0x4A, tnlp_f24(0.125f));
            TNLP_W(&L, 0x4B, tnlp_f24(0.3f));
        }
        if (c->kind == TNLC_TEXGEN) {
            float tg[12]; tnlp_rot(tg, 0.0f, 0.0f, 1.0f, 0.4f, 0.5f, 0.5f, 1.0f);
            tnlp_matrix(&L, 0x40, tg, 12);
            TNLP_W(&L, 0xC0, 1u | (0u << 8));
        }
    }

    if (c->kind == TNLC_FOG_LINEAR || c->kind == TNLC_FOG_INF) {
        TNLP_W(&L, 0x1F, 1);
        TNLP_W(&L, 0xCF, 0x00406080u);
        if (c->kind == TNLC_FOG_LINEAR) {
            TNLP_W(&L, 0xCD, tnlp_f24(7.5f));
            TNLP_W(&L, 0xCE, tnlp_f24(0.37f));
        } else {
            TNLP_W(&L, 0xCD, 0x7F8000u);           /* +infinity */
            TNLP_W(&L, 0xCE, tnlp_f24(1.0f));
        }
    }

    if (c->kind == TNLC_DEPTH || c->kind == TNLC_STATIC) {
        TNLP_W(&L, 0x23, 1);
        TNLP_W(&L, 0xDE, c->kind == TNLC_DEPTH ? 4u : 6u);   /* LESS / GREATER */
    }
    if (c->kind == TNLC_STRIP_CULL0 || c->kind == TNLC_STRIP_CULL1) {
        TNLP_W(&L, 0x1D, 1);
        TNLP_W(&L, 0x9B, c->kind == TNLC_STRIP_CULL1 ? 1u : 0u);
    }

    /* Bones: each a rotation with a translation. */
    int skin_w = c->kind == TNLC_SKIN1 ? 1 : c->kind == TNLC_SKIN2_U8 ? 2 :
                 c->kind == TNLC_SKIN4_U16 ? 4 : c->kind == TNLC_SKIN8_F ? 8 :
                 c->kind == TNLC_LIGHT_SKIN ? 3 : 0;
    if (skin_w) {
        TNLP_W(&L, 0x2A, 0);
        for (int b = 0; b < 8; b++) {
            float m[12];
            tnlp_rot(m, tnlp_rand(-1, 1), tnlp_rand(-1, 1), tnlp_rand(0.2f, 1), tnlp_rand(-0.5f, 0.5f),
                     tnlp_rand(-0.3f, 0.3f), tnlp_rand(-0.3f, 0.3f), tnlp_rand(-0.3f, 0.3f));
            for (int i = 0; i < 12; i++) TNLP_W(&L, 0x2B, tnlp_f24(m[i]));
        }
    }
    if (c->kind == TNLC_MORPH2) {
        TNLP_W(&L, 0x2C, tnlp_f24(0.625f));
        TNLP_W(&L, 0x2D, tnlp_f24(0.375f));
    }

    /* Lighting. */
    int lit = c->kind == TNLC_LIGHT_DIR || c->kind == TNLC_LIGHT_POINT ||
              c->kind == TNLC_LIGHT4_MATUPD || c->kind == TNLC_LIGHT_SKIN || c->kind == TNLC_SPECULAR ||
              c->kind == TNLC_LIGHT_SUBNORMAL || tnlc_pow_exact(c->kind);
    if (lit) {
        TNLP_W(&L, 0x17, 1);
        TNLP_W(&L, 0x53, c->kind == TNLC_LIGHT4_MATUPD ? 3u : c->kind == TNLC_LIGHT4_POW ? 7u : 0u);
        TNLP_W(&L, 0x54, 0x00080402u);
        TNLP_W(&L, 0x55, 0x00405060u);
        TNLP_W(&L, 0x56, 0x00c0a0e0u);
        TNLP_W(&L, 0x57, 0x00ffffffu);
        TNLP_W(&L, 0x58, 0xb0);
        /* A positive power where the cell means to decline (the C library's powf) or to run
           ge_powf on both routes; the powered-diffuse light of the matupdate cell then lights
           as plain diffuse. A fractional power exercises the whole of ge_powf. */
        TNLP_W(&L, 0x5B, tnlp_f24(c->kind == TNLC_SPECULAR ? 8.0f : c->kind == TNLC_SPECULAR_EXACT ? 10.5f :
                                  c->kind == TNLC_LIGHT4_POW ? 3.25f : 0.0f));
        TNLP_W(&L, 0x5C, 0x00303030u);
        TNLP_W(&L, 0x5D, 0xc0);
        int four = c->kind == TNLC_LIGHT4_MATUPD || c->kind == TNLC_LIGHT4_POW;
        int nl = four ? 4 : 1;
        for (int i = 0; i < nl; i++) {
            int point = c->kind == TNLC_LIGHT_POINT || (four && (i & 1));
            /* The ge_powf four-light cell: specular, powered diffuse, specular, diffuse. */
            static const uint32_t pow_comps[4] = { 1u, 2u, 3u, 0u };
            uint32_t comp = c->kind == TNLC_SPECULAR || c->kind == TNLC_SPECULAR_EXACT ? 1u :
                            c->kind == TNLC_LIGHT4_POW ? pow_comps[i] :
                            (c->kind == TNLC_LIGHT4_MATUPD && i == 2 ? 2u : 0u);
            TNLP_W(&L, 0x18 + (uint32_t)i, 1);
            TNLP_W(&L, 0x5F + (uint32_t)i, comp | ((uint32_t)(point ? 1 : 0) << 8));
            float pos[3] = { tnlp_rand(-3, 3), tnlp_rand(-3, 3), tnlp_rand(1, 4) };
            for (int k = 0; k < 3; k++) TNLP_W(&L, 0x63 + (uint32_t)(3 * i + k), tnlp_f24(pos[k]));
            float att[3] = { 0.6f, 0.15f, 0.02f };
            for (int k = 0; k < 3; k++) TNLP_W(&L, 0x7B + (uint32_t)(3 * i + k), tnlp_f24(att[k]));
            TNLP_W(&L, 0x8F + (uint32_t)(3 * i), 0x00201810u + (uint32_t)i * 0x00080402u);
            TNLP_W(&L, 0x90 + (uint32_t)(3 * i), 0x00c0d0e0u - (uint32_t)i * 0x00102030u);
            TNLP_W(&L, 0x91 + (uint32_t)(3 * i), 0x00ffffffu);
        }
    }

    /* The mesh: a grid of 12 x 8 quads over [-1.6, 1.6] x [-1.1, 1.1], displaced in z, with
     * colours, normals, texture coordinates and weights from the sequence. */
    TnlpFmt f = { 0, 0, 3, 1, 3, 3, 1 };
    if (skin_w) {
        f.wfmt = c->kind == TNLC_SKIN2_U8 ? 1 : c->kind == TNLC_SKIN4_U16 ? 2 : 3;
        f.wn = skin_w;
    }
    if (c->kind == TNLC_FIXED_FMT) { f.tc = 2; f.nrm = 1; f.pos = 2; }
    if (c->kind == TNLC_MORPH2) f.morph = 2;
    const int gw = 12, gh = 8;
    uint8_t *vd = (uint8_t *)SR_HOST(TNLP_VTX);
    uint32_t stride = 0, nverts = 0;
    for (int y = 0; y <= gh; y++) {
        for (int x = 0; x <= gw; x++) {
            float scale = c->kind == TNLC_FIXED_FMT ? 0.5f : 1.0f;
            float p[3] = { (-1.6f + 3.2f * (float)x / (float)gw) * scale,
                           (-1.1f + 2.2f * (float)y / (float)gh) * scale,
                           tnlp_rand(-0.3f, 0.3f) * scale };
            float n[3] = { tnlp_rand(-0.5f, 0.5f), tnlp_rand(-0.5f, 0.5f), 0.8f };
            if (c->kind == TNLC_LIGHT_SUBNORMAL)
                for (int i = 0; i < 3; i++)   /* every component at least 2^-40, as ge.c requires */
                    n[i] = ldexpf(n[i] < 0.0f ? n[i] - 0.1f : n[i] + 0.1f, -35);
            float w[8];
            float sum = 0.0f;
            for (int i = 0; i < 8; i++) { w[i] = i < skin_w ? tnlp_rand(0.0f, 1.0f) : 0.0f; sum += w[i]; }
            if (skin_w) for (int i = 0; i < skin_w; i++) w[i] = w[i] / sum;
            if (skin_w > 1) w[0] = 0.0f;   /* a zero weight: skipped */
            uint32_t col = 0xff000000u | ((uint32_t)(tnlp_rand(0, 255)) << 16) |
                           ((uint32_t)(tnlp_rand(0, 255)) << 8) | (uint32_t)(tnlp_rand(0, 255));
            if (c->kind == TNLC_RAMP) {
                uint32_t k = (uint32_t)(y * (gw + 1) + x) * 2u;
                col = 0xff000000u | ((k & 255u) << 16) | (((k * 3u) & 255u) << 8) | ((255u - k) & 255u);
            }
            float u = (float)x / (float)gw, v = (float)y / (float)gh;
            uint8_t *at = vd + (size_t)nverts * (stride ? stride : 0);
            stride = 0;
            for (int m = 0; m < f.morph; m++) {
                float pm[3] = { p[0] + 0.2f * (float)m, p[1] - 0.1f * (float)m, p[2] };
                stride += tnlp_vertex(at + stride, &f, w, u, v, col, n, pm);
            }
            nverts++;
        }
    }
    uint32_t vt_idx = 0;
    uint32_t prim = 0, count = 0;
    TNLP_W(&L, 0x10, (TNLP_VTX >> 8) & 0x000F0000u);
    if (c->kind == TNLC_FAN_IDX16 || c->kind == TNLC_LIST_IDX8) {
        /* An index buffer over the grid's vertices. */
        uint8_t *ib = (uint8_t *)SR_HOST(TNLP_IDX);
        uint32_t ni = 0;
        if (c->kind == TNLC_FAN_IDX16) {
            /* A fan around the grid's centre through a ring of vertices. */
            uint16_t centre = (uint16_t)((gh / 2) * (gw + 1) + gw / 2);
            memcpy(ib + 2u * ni++, &centre, 2);
            for (int k = 0; k <= 16; k++) {
                float a = 6.2831853f * (float)k / 16.0f;
                int gx = gw / 2 + (int)lrintf(5.0f * cosf(a)), gy = gh / 2 + (int)lrintf(3.0f * sinf(a));
                uint16_t ix = (uint16_t)(gy * (gw + 1) + gx);
                memcpy(ib + 2u * ni++, &ix, 2);
            }
            vt_idx = 2; prim = 5;
        } else {
            for (int y = 0; y < gh && ni + 6u <= 255u * 3u; y += 2) {
                for (int x = 0; x < gw; x += 2) {
                    uint8_t a = (uint8_t)(y * (gw + 1) + x), b = (uint8_t)(a + 1);
                    uint8_t c2 = (uint8_t)(a + gw + 1), d2 = (uint8_t)(c2 + 1);
                    ib[ni++] = a; ib[ni++] = b; ib[ni++] = c2;
                    ib[ni++] = b; ib[ni++] = d2; ib[ni++] = c2;
                }
            }
            vt_idx = 1; prim = 3;
        }
        count = ni;
        TNLP_W(&L, 0x12, tnlp_vtype(&f, (int)vt_idx));
        TNLP_W(&L, 0x02, TNLP_IDX & 0x00FFFFFFu);
        TNLP_W(&L, 0x01, TNLP_VTX & 0x00FFFFFFu);
        TNLP_W(&L, 0x04, (prim << 16) | count);
    } else if (c->kind == TNLC_STRIP_CULL0 || c->kind == TNLC_STRIP_CULL1 || c->kind == TNLC_FLAT ||
               c->kind == TNLC_MORPH2 || c->kind == TNLC_FIXED_FMT) {
        /* One strip per grid row pair, through indices: vertices (y, x), (y + 1, x), ... */
        uint8_t *ib = (uint8_t *)SR_HOST(TNLP_IDX);
        TNLP_W(&L, 0x12, tnlp_vtype(&f, 2));
        TNLP_W(&L, 0x01, TNLP_VTX & 0x00FFFFFFu);
        for (int y = 0; y < gh; y++) {
            uint32_t at = TNLP_IDX + (uint32_t)y * 0x100u;
            uint16_t *row = (uint16_t *)(ib + (size_t)y * 0x100u);
            for (int x = 0; x <= gw; x++) {
                row[2 * x] = (uint16_t)(y * (gw + 1) + x);
                row[2 * x + 1] = (uint16_t)((y + 1) * (gw + 1) + x);
            }
            TNLP_W(&L, 0x02, at & 0x00FFFFFFu);
            TNLP_W(&L, 0x04, (4u << 16) | (uint32_t)(2 * (gw + 1)));
        }
    } else {
        /* A triangle list over the grid, two triangles per quad, written out vertex by vertex. */
        uint8_t *ld = (uint8_t *)SR_HOST(TNLP_VTX + 0x20000u);
        uint32_t nl = 0;
        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                int q[6] = { y * (gw + 1) + x, y * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x,
                             y * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x };
                for (int k = 0; k < 6; k++)
                    memcpy(ld + (size_t)nl++ * stride, vd + (size_t)q[k] * stride, stride);
            }
        }
        TNLP_W(&L, 0x12, tnlp_vtype(&f, 0));
        TNLP_W(&L, 0x10, ((TNLP_VTX + 0x20000u) >> 8) & 0x000F0000u);
        TNLP_W(&L, 0x01, (TNLP_VTX + 0x20000u) & 0x00FFFFFFu);
        TNLP_W(&L, 0x04, (3u << 16) | nl);
    }
    TNLP_W(&L, 0x0F, 0);
    TNLP_W(&L, 0x0C, 0);
    return TNLP_LIST;
}

typedef struct { uint32_t *color; uint16_t *depth; } TnlpOut;

/* Run cell c's list one way (mode 0: software, 1: backend with ge.c's T&L, 2: backend with
 * GPU T&L) and copy the colour and depth buffers out. */
static int tnlp_run(const TnlCell *c, int mode, TnlpOut *out, unsigned long long *taken) {
    const uint32_t fb = 0x04000000u | TNLP_FBA, zb = 0x04000000u | TNLP_ZBA;
    coherence_reset_targets();
    s_twin_case = c->name;   /* the targets this run makes are checked under its name */
    memset(SR_HOST(fb), 0, TNLP_STRIDE * FB_H * 4u);
    memset(SR_HOST(zb), 0, TNLP_STRIDE * FB_H * 2u);
    sr_gpu_vram_dirty(fb, TNLP_STRIDE * FB_H * 4u);
    sr_gpu_vram_dirty(zb, TNLP_STRIDE * FB_H * 2u);
    uint32_t list = tnlp_build(c);
    ge_pow_set_exact(tnlc_pow_exact(c->kind));
    ge_tnl_set_enabled(mode == 2);
    ge_tnl_counts(NULL, NULL, 1);
    ge_set_gpu_hooks(mode ? &k_hooks : NULL);
    ge_run_list(list, 0);
    ge_set_gpu_hooks(&k_hooks);
    ge_tnl_counts(taken, NULL, 0);
    ge_tnl_set_enabled(-1);
    ge_pow_set_exact(0);
    if (mode) {
        GeGpuFbDescriptor desc = { .addr = fb, .format = 3, .stride = TNLP_STRIDE, .width = 480, .height = FB_H };
        if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) return 0;
        if (s_nbatch && !submit_pending()) return 0;
        /* Only this fixture's depth surface: ge.c has one depth mirror, and reading an earlier
         * fixture's surface back into it would overwrite this one's. */
        for (int i = 0; i < MAX_DEP; i++)
            if (s_deps[i].used && s_deps[i].zba == TNLP_ZBA && !depth_to_cpu(&s_deps[i])) return 0;
    }
    memcpy(out->color, SR_HOST(fb), TNLP_STRIDE * FB_H * 4u);
    /* ge.c's depth surface: the software GE's own, or the backend's depth image read back. */
    const uint16_t *z = ge_zbuf_ptr();
    for (uint32_t y = 0; y < FB_H; y++)
        memcpy(out->depth + (size_t)y * TNLP_STRIDE, z + (size_t)y * ge_zbuf_stride(), TNLP_STRIDE * 2u);
    return 1;
}

static int coherence_run_tnl_parity(void) {
    static const TnlCell cells[] = {
        { "static-list-ztest", TNLC_STATIC, 1 },
        { "strip-cull0", TNLC_STRIP_CULL0, 1 },
        { "strip-cull1", TNLC_STRIP_CULL1, 1 },
        { "fan-index16", TNLC_FAN_IDX16, 1 },
        { "list-index8", TNLC_LIST_IDX8, 1 },
        { "textured-scale-offset", TNLC_TEXTURED, 1 },
        { "fog-linear", TNLC_FOG_LINEAR, 1 },
        { "fog-infinite-end", TNLC_FOG_INF, 1 },
        { "depth-less", TNLC_DEPTH, 1 },
        { "colour-ramp", TNLC_RAMP, 1 },
        { "fixed-formats", TNLC_FIXED_FMT, 1 },
        { "morph-2", TNLC_MORPH2, 1 },
        { "skin-1-float", TNLC_SKIN1, 1 },
        { "skin-2-u8", TNLC_SKIN2_U8, 1 },
        { "skin-4-u16", TNLC_SKIN4_U16, 1 },
        { "skin-8-float", TNLC_SKIN8_F, 1 },
        { "light-directional", TNLC_LIGHT_DIR, 1 },
        { "light-point", TNLC_LIGHT_POINT, 1 },
        { "light-4-matupdate", TNLC_LIGHT4_MATUPD, 1 },
        { "light-skinned", TNLC_LIGHT_SKIN, 1 },
        { "offset-viewport", TNLC_OFFSET_VIEWPORT, 1 },
        { "near-crossing", TNLC_NEAR_CROSS, 0 },
        { "specular", TNLC_SPECULAR, 0 },
        { "flat", TNLC_FLAT, 0 },
        { "texgen", TNLC_TEXGEN, 0 },
        { "light-subnormal-length", TNLC_LIGHT_SUBNORMAL, 1 },
        { "specular-ge-powf", TNLC_SPECULAR_EXACT, 1 },
        { "light-4-ge-powf", TNLC_LIGHT4_POW, 1 },
    };
    const size_t px = (size_t)TNLP_STRIDE * FB_H;
    TnlpOut sw = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    TnlpOut cpu = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    TnlpOut gpu = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    int ok = sw.color && sw.depth && cpu.color && cpu.depth && gpu.color && gpu.depth;
    unsigned taken_cells = 0;
    const int buffers_ok = ok;
    for (size_t k = 0; buffers_ok && k < sizeof(cells) / sizeof(cells[0]); k++) {
        const TnlCell *c = &cells[k];
        unsigned long long t_sw = 0, t_cpu = 0, t_gpu = 0;
        if (!tnlp_run(c, 0, &sw, &t_sw) || !tnlp_run(c, 1, &cpu, &t_cpu) ||
            !tnlp_run(c, 2, &gpu, &t_gpu)) {
            fprintf(stderr, "gpu coherence selftest [tnl-parity/%s]: a run failed\n", c->name);
            ok = 0;
            break;
        }
        size_t cdiff = 0, zdiff = 0, first = px, zfirst = px;
        for (size_t i = 0; i < px; i++) {
            if (cpu.color[i] != gpu.color[i]) { cdiff++; if (first == px) first = i; }
            if (cpu.depth[i] != gpu.depth[i]) { zdiff++; if (zfirst == px) zfirst = i; }
        }
        size_t swdiff = 0, drawn = 0;
        int swmax = 0;
        for (size_t i = 0; i < px; i++) {
            if (cpu.color[i] != 0xff302010u) drawn++;
            if (sw.color[i] == cpu.color[i]) continue;
            swdiff++;
            for (int ch = 0; ch < 32; ch += 8) {
                int d = abs((int)((sw.color[i] >> ch) & 255u) - (int)((cpu.color[i] >> ch) & 255u));
                if (d > swmax) swmax = d;
            }
        }
        const int want_gpu = c->gpu && s_vs_tnl != VK_NULL_HANDLE;
        const int route_ok = want_gpu ? t_gpu > 0 : t_gpu == 0;
        if (cdiff || zdiff || !route_ok || !drawn) {
            fprintf(stderr, "gpu coherence selftest [tnl-parity/%s]: FAIL colour_diff=%zu depth_diff=%zu "
                    "gpu_tnl_prims=%llu (want %s) drawn=%zu",
                    c->name, cdiff, zdiff, t_gpu, want_gpu ? ">0" : "0", drawn);
            if (first < px)
                fprintf(stderr, " first=(%zu,%zu) cpu=%08x gpu=%08x", first % TNLP_STRIDE,
                        first / TNLP_STRIDE, (unsigned)cpu.color[first], (unsigned)gpu.color[first]);
            if (zfirst < px)
                fprintf(stderr, " zfirst=(%zu,%zu) cpu=%04x gpu=%04x sw=%04x", zfirst % TNLP_STRIDE,
                        zfirst / TNLP_STRIDE, cpu.depth[zfirst], gpu.depth[zfirst], sw.depth[zfirst]);
            fprintf(stderr, "\n");
            ok = 0;
            continue;
        }
        if (want_gpu) taken_cells++;
        printf("gpu coherence selftest: PASS tnl-parity %-22s gpu_tnl_prims=%llu drawn=%zu "
               "software_diff=%zu max_delta=%d\n", c->name, t_gpu, drawn, swdiff, swmax);
    }
    free(sw.color); free(sw.depth); free(cpu.color); free(cpu.depth); free(gpu.color); free(gpu.depth);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s cells=%zu gpu_tnl_cells=%u%s\n", "tnl-parity",
               sizeof(cells) / sizeof(cells[0]), taken_cells,
               s_vs_tnl ? "" : " (no fp32 subnormal preservation: every cell checked on ge.c's route)");
    return ok;
}

/* The whole device-size image, not only the top-left device pixel of each scale x scale cell
 * that a readback to guest VRAM keeps (write_guest_fb). Colour is left in TRANSFER_SRC, as
 * target_readback leaves it; `after` is the layout the image returns to. */
static int coherence_read_device(VkImage img, VkImageAspectFlags aspect, VkImageLayout *layout,
                                 VkImageLayout after, uint32_t bpp, void *out) {
    if (s_nbatch && !submit_pending()) return 0;
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, img, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = aspect;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_xfer, 1, &c);
    to_layout(s_cmd, img, aspect, layout, after);
    if (!cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION)) return 0;
    memcpy(out, s_xfer_map, (size_t)SCL_W * SCL_H * bpp);
    return 1;
}

static int coherence_check_device_color(const char *step, const uint32_t *dev, const uint8_t *guest,
                                        uint32_t stride, uint32_t fmt) {
    const uint32_t bpp = fmt == 3u ? 4u : 2u;
    for (uint32_t y = 0; y < SCL_H; y++) {
        for (uint32_t x = 0; x < SCL_W; x++) {
            const uint32_t gx = x / (uint32_t)s_scale, gy = y / (uint32_t)s_scale;
            uint32_t raw = 0, want = 0;
            if (gx < stride) {                     /* columns past the stride upload as 0 */
                memcpy(&raw, guest + (gy * stride + gx) * bpp, bpp);
                want = fb_unpack(raw, fmt);
            }
            if (dev[y * SCL_W + x] != want) {
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: %s fmt=%u stride=%u scale=%d: "
                        "device pixel (%u,%u) = 0x%08x, want 0x%08x\n", step, fmt, stride, s_scale,
                        x, y, dev[y * SCL_W + x], want);
                return 0;
            }
        }
    }
    return 1;
}

/* Every device pixel of each scale x scale cell holds its guest pixel after a whole-target
 * upload, after a CPU-write patch across a row boundary, and after a depth upload (#1296):
 * a readback to guest VRAM keeps only one device pixel per cell, so a row or column of a
 * cell left unwritten would not show there. Every format runs at a full and at a short
 * stride, whose columns past the stride upload as 0. At scale 1 the cells are single pixels;
 * gpu-coherence-selftest also runs this case alone at render scales 2 and 4. */
static int coherence_run_scaled_upload(void) {
    static const struct { uint32_t fmt, stride; } cells[] = {
        { 3u, 512u }, { 0u, 512u }, { 1u, 512u }, { 2u, 512u },
        { 3u, 480u }, { 0u, 480u }, { 1u, 448u }, { 2u, 496u },
    };
    const uint32_t fba = 0x00100000u, base = 0x04000000u | fba;
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev) return 0;
    int ok = 1;
    for (uint32_t c = 0; ok && c < sizeof(cells) / sizeof(cells[0]); c++) {
        const uint32_t fmt = cells[c].fmt, stride = cells[c].stride, bpp = fmt == 3u ? 4u : 2u;
        coherence_reset_targets();
        uint8_t *guest = (uint8_t *)SR_HOST(base);
        for (uint32_t i = 0; i < stride * FB_H; i++) {
            const uint32_t raw = (i + 1u) * 0x9e3779b1u ^ (fmt << 24);
            memcpy(guest + i * bpp, &raw, bpp);
        }
        const char *fail = NULL;           /* "" once the check has printed its own line */
        Target *t = target_color_acquire(fba, stride, fmt, 1);
        if (!t || !t->gpu_valid)
            fail = "the target was not established";
        else if (!coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev))
            fail = "device read after the upload failed";
        else if (!coherence_check_device_color("whole-target upload", dev, guest, stride, fmt))
            fail = "";

        /* A CPU write from mid row 60 into row 61, held and applied as a patch. */
        if (!fail) {
            const uint32_t off = (60u * stride + 37u) * bpp, len = (stride + 50u) * bpp;
            for (uint32_t i = 0; i < len; i++) guest[off + i] = (uint8_t)(0x5au + i * 13u);
            const unsigned long long syncs0 = s_replay_stats.cpu_write_syncs;
            const unsigned long uploads0 = s_cnt_upload;
            sr_gpu_vram_dirty(base + off, len);
            const int held = t->cpu_n;
            t = target_color_acquire(fba, stride, fmt, 1);
            if (!t || !t->gpu_valid)
                fail = "the target was lost after the CPU write";
            else if (!held || s_replay_stats.cpu_write_syncs != syncs0 + 1u ||
                     s_cnt_upload != uploads0 + 1u) {   /* one held span, one patch */
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: held=%d syncs=%llu uploads=%lu\n",
                        held, s_replay_stats.cpu_write_syncs - syncs0, s_cnt_upload - uploads0);
                fail = "the CPU write was not applied as a patch";
            }
            else if (!coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev))
                fail = "device read after the patch failed";
            else if (!coherence_check_device_color("CPU-write patch", dev, guest, stride, fmt))
                fail = "";
        }
        if (fail) {
            if (*fail)
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: fmt=%u stride=%u scale=%d: %s\n",
                        fmt, stride, s_scale, fail);
            ok = 0;
        }
    }

    /* Depth: ge.c's CPU surface, uploaded at device size. */
    if (ok) {
        coherence_reset_targets();
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = base; s_ge->fbw = 512u;
        s_ge->zbp = 0x04080000u; s_ge->zbw = 512u;
        DepthEnt *d = NULL;
        uint16_t *dev16 = (uint16_t *)dev;
        ok = ge_zbuf_prepare_current() && (s_zbuf = ge_zbuf_ptr()) != NULL;
        if (ok) {
            for (uint32_t y = 0; y < FB_H; y++)
                for (uint32_t x = 0; x < FB_W; x++)
                    s_zbuf[y * 512u + x] = (uint16_t)(0x1234u + y * 977u + x * 31u);
            ge_zbuf_note_external_write();
            d = depth_acquire(0x00080000u, 512u);
        }
        if (d) d->cpu_dirty = 1;
        ok = d && depth_from_cpu(d) &&
             coherence_read_device(d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, DEPTH_BPP, dev16);
        /* depth=32 (#2051): each D32 texel as its 16-bit step, in place (texel i's bytes are read
         * before step i overwrites them) */
        for (size_t i = 0; ok && s_depth32 && i < (size_t)SCL_W * SCL_H; i++) dev16[i] = depth_texel(dev16, i);
        for (uint32_t y = 0; ok && y < SCL_H; y++) {
            for (uint32_t x = 0; x < SCL_W; x++) {
                const uint16_t want = s_zbuf[(y / (uint32_t)s_scale) * 512u + x / (uint32_t)s_scale];
                if (dev16[y * SCL_W + x] != want) {
                    fprintf(stderr, "gpu coherence selftest [scaled-upload]: depth upload scale=%d: "
                            "device pixel (%u,%u) = 0x%04x, want 0x%04x\n", s_scale, x, y,
                            dev16[y * SCL_W + x], want);
                    ok = 0;
                    break;
                }
            }
        }
        if (!ok && !d) fprintf(stderr, "gpu coherence selftest [scaled-upload]: no depth surface\n");
    }
    free(dev);
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS %-22s cells=%u scale=%d\n", "scaled-upload",
               (unsigned)(sizeof(cells) / sizeof(cells[0])), s_scale);
    return ok;
}

/* Fill a whole device-size image from `src`; every device pixel can differ, which an upload from
 * guest memory cannot do. The image is left in `after`. */
static int coherence_write_device(VkImage img, VkImageAspectFlags aspect, VkImageLayout *layout,
                                  VkImageLayout after, uint32_t bpp, const void *src) {
    if (s_nbatch && !submit_pending()) return 0;
    memcpy(s_xfer_map, src, (size_t)SCL_W * SCL_H * bpp);
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, img, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = aspect;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_xfer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    to_layout(s_cmd, img, aspect, layout, after);
    return cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION);
}

/* A white-box write of a whole authentic image (coherence_write_device) is a write the host
 * twin (#2050) does not mirror: count it and seed the twin from the image, as an upload from
 * VRAM does, so the twin stays in step. Nothing to do with the twin off. */
static int coherence_wrote_device(Target *t, DepthEnt *d) {
    if (t) t->wgen++;
    if (d) d->wgen++;
    if (!s_twin || !cmd_begin()) return !s_twin;
    if (t) (void)twin_seed_target(t, 1);
    if (d) (void)twin_seed_depth(d, 1);
    return cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION);
}

/* The staging bytes past the rows a readback keeps still hold `poison`. */
static int coherence_xfer_tail_is(uint8_t poison, uint32_t bpp) {
    const uint8_t *map = (const uint8_t *)s_xfer_map;
    for (size_t i = (size_t)SCL_W * FB_H * bpp; i < (size_t)SCL_W * SCL_H * bpp; i++)
        if (map[i] != poison) return 0;
    return 1;
}

static int coherence_check_guest_fb(const char *step, const uint32_t *dev, uint32_t base,
                                    uint32_t stride, uint32_t fmt) {
    const uint32_t bpp = fmt == 3u ? 4u : 2u, width = stride < FB_W ? stride : FB_W;
    const uint8_t *guest = (const uint8_t *)SR_HOST(base);
    for (uint32_t y = 0; y < FB_H; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t px = dev[y * (uint32_t)s_scale * SCL_W + x * (uint32_t)s_scale];
            const uint32_t want = fb_pack(px, fmt);
            uint32_t got = 0;
            memcpy(&got, guest + (y * stride + x) * bpp, bpp);
            if (got != want) {
                fprintf(stderr, "gpu coherence selftest [readback-rows]: %s fmt=%u stride=%u scale=%d: "
                        "guest pixel (%u,%u) = 0x%08x, want 0x%08x\n", step, fmt, stride, s_scale,
                        x, y, got, want);
                return 0;
            }
        }
    }
    return 1;
}

/* Every device pixel differs, and guest memory gets the top-left one of each scale x scale cell
 * through all four readbacks: target_readback, the deferred presentation copy, and both depth
 * paths. Past the kept rows the staging buffer stays as it was, so the rows a readback does not
 * need are not copied (#323), unless SR_GPU_READBACK_FULL selects the reference path. */
static int coherence_run_readback_rows(void) {
    static const struct { uint32_t fmt, stride; } cells[] = {
        { 3u, 512u }, { 0u, 512u }, { 1u, 480u }, { 2u, 448u },
    };
    const uint32_t fba = 0x00100000u, base = 0x04000000u | fba;
    const char *full_env = getenv("SR_GPU_READBACK_FULL");
    const int rows_only = s_scale > 1 && !(full_env && full_env[0] && strcmp(full_env, "0") != 0);
    const uint8_t poison = 0xa5u;
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev) return 0;
    int ok = 1;
    for (uint32_t c = 0; ok && c < sizeof(cells) / sizeof(cells[0]); c++) {
        const uint32_t fmt = cells[c].fmt, stride = cells[c].stride, bpp = fmt == 3u ? 4u : 2u;
        coherence_reset_targets();
        memset(SR_HOST(base), 0, (size_t)stride * FB_H * bpp);
        Target *t = target_color_acquire(fba, stride, fmt, 1);
        ok = t != NULL;
        for (uint32_t pass = 0; ok && pass < 2; pass++) {
            for (uint32_t i = 0; i < SCL_W * SCL_H; i++)
                dev[i] = ((i + 1u + pass) * 0x9e3779b1u) | 0xff000000u;
            ok = coherence_write_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev) &&
                 coherence_wrote_device(t, NULL);
            if (!ok) break;
            t->gpu_valid = 1;
            t->render_gen++;
            coherence_image_newer(t);
            memset(SR_HOST(base), 0, (size_t)stride * FB_H * bpp);
            if (pass == 0) {
                memset(s_xfer_map, poison, (size_t)SCL_W * SCL_H * 4u);
                ok = target_readback(t);
                if (ok && rows_only && !coherence_xfer_tail_is(poison, 4u)) {
                    fprintf(stderr, "gpu coherence selftest [readback-rows]: fmt=%u scale=%d: "
                            "target_readback copied rows it does not keep\n", fmt, s_scale);
                    ok = 0;
                }
            } else {
                ok = target_prepare_present(t) && readback_wait_target(t, 1);
            }
            ok = ok && t->clean_gen == t->render_gen &&
                 coherence_check_guest_fb(pass ? "deferred copy" : "target_readback", dev, base,
                                          stride, fmt);
        }
    }
    if (ok) {
        coherence_reset_targets();
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = base; s_ge->fbw = 512u;
        s_ge->zbp = 0x04080000u; s_ge->zbw = 512u;
        uint16_t *dev16 = (uint16_t *)dev;
        ok = ge_zbuf_prepare_current() && (s_zbuf = ge_zbuf_ptr()) != NULL;
        DepthEnt *d = ok ? depth_acquire(0x00080000u, 512u) : NULL;
        ok = d != NULL;
        for (uint32_t i = 0; ok && i < SCL_W * SCL_H; i++) dev16[i] = (uint16_t)((i + 7u) * 40503u);
        /* depth=32 (#2051): the same steps as D32 values */
        float *devf = s_depth32 ? (float *)malloc((size_t)SCL_W * SCL_H * 4u) : NULL;
        if (s_depth32 && !devf) ok = 0;
        for (uint32_t i = 0; ok && devf && i < SCL_W * SCL_H; i++) devf[i] = depth_unorm(dev16[i]);
        ok = ok && coherence_write_device(d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, DEPTH_BPP,
                                          devf ? (const void *)devf : (const void *)dev16) &&
             coherence_wrote_device(NULL, d);
        free(devf);
        if (ok) {
            memset(s_xfer_map, poison, (size_t)SCL_W * SCL_H * DEPTH_BPP);
            d->guest_dirty = 1;
            ok = depth_readback_to_guest(d);
            if (ok && rows_only && !coherence_xfer_tail_is(poison, DEPTH_BPP)) {
                fprintf(stderr, "gpu coherence selftest [readback-rows]: depth readback copied rows "
                        "it does not keep at scale %d\n", s_scale);
                ok = 0;
            }
        }
        for (uint32_t y = 0; ok && y < FB_H; y++) {
            for (uint32_t x = 0; x < FB_W; x++) {
                const uint32_t addr = 0x04000000u + ((0x00080000u + (y * 512u + x) * 2u) ^ 0x2040u);
                const uint16_t want = dev16[y * (uint32_t)s_scale * SCL_W + x * (uint32_t)s_scale];
                uint16_t got;
                memcpy(&got, SR_HOST_LINEAR(addr), sizeof got);
                if (got != want) {
                    fprintf(stderr, "gpu coherence selftest [readback-rows]: depth readback (%u,%u) "
                            "= 0x%04x, want 0x%04x\n", x, y, got, want);
                    ok = 0;
                    break;
                }
            }
        }
        if (ok) {
            d->cpu_dirty = 0;
            memset(s_xfer_map, poison, (size_t)SCL_W * SCL_H * DEPTH_BPP);
            ok = depth_to_cpu(d) && (!rows_only || coherence_xfer_tail_is(poison, DEPTH_BPP));
            if (!ok) fprintf(stderr, "gpu coherence selftest [readback-rows]: depth_to_cpu copy\n");
        }
        for (uint32_t y = 0; ok && y < FB_H; y++) {
            for (uint32_t x = 0; x < FB_W; x++) {
                const uint16_t want = dev16[y * (uint32_t)s_scale * SCL_W + x * (uint32_t)s_scale];
                if (s_zbuf[y * 512u + x] != want) {
                    fprintf(stderr, "gpu coherence selftest [readback-rows]: depth_to_cpu (%u,%u) "
                            "= 0x%04x, want 0x%04x\n", x, y, s_zbuf[y * 512u + x], want);
                    ok = 0;
                    break;
                }
            }
        }
    }
    free(dev);
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS %-22s cells=%u scale=%d rows_only=%d\n", "readback-rows",
               (unsigned)(sizeof(cells) / sizeof(cells[0])), s_scale, rows_only);
    return ok;
}

/* ---- widescreen Fill (#1881): the wing canvases on both renderers ----------------------------
 * The list of ge-cpu-profile-validate-selftest's test_ge_wide_fill: sceGuClear-style clear
 * strips, a transform-mode quad from guest x -240 to 720 (rows 68..203), band shapes in and
 * across each screen edge (rows 4..63 left, 208..267 right: flat and gouraud triangle pairs and
 * transform-mode sprites at non-integer corners, each drawn again WIDE_K pixels toward the
 * centre so the authentic frame shows it whole), a through-mode bar (rows 100..139), and in the
 * fade variant an untextured whole-screen sprite. At 16:9, 21:9 and 32:9, on the software
 * rasterizer and on this backend: guest VRAM must be byte-identical with Fill off and on; each
 * renderer's canvas must hold the quad, the clear, the fade, and the shapes at the columns its
 * own frame puts their copies (left-wing column c is frame column c - D + WIDE_K, right-wing
 * column c is frame column c - D + 480 - WIDE_K); the two canvases must agree byte for byte; a
 * narrower scissor must reach no wing; a shown canvas must be cleared by the next wing draw;
 * and a frame whose every wing primitive this backend drops (a logic op: the software
 * rasterizer's state) must leave the canvas undrawn and black. */
#define WIDE_LIST      0x002C0000u
#define WIDE_VTX       0x002C8000u
#define WIDE_FB        0x04000000u
#define WIDE_VRAM      0x04000000u
#define WIDE_VRAM_SIZE 0x00200000u
#define WIDE_D         76
#define WIDE_K         270
#define WIDE_BG        0xff302010u
#define WIDE_QUAD      0xff20c040u
#define WIDE_BAR       0xff0000ffu
#define WIDE_FADE      0xff808080u
enum { WF_FADE = 1, WF_NARROW = 2, WF_NOCLEAR = 4, WF_LOGICOP = 8 };

typedef struct { float x0, x1, y0, y1; uint32_t c0, c1; int sprite, right; } WideShape;
static const WideShape k_wide_shapes[] = {
    { -50.625f,  -30.0f,     4.25f,  17.0f,   0xff4060a0u, 0xff4060a0u, 0, 0 },
    { -20.625f,   19.6875f, 21.25f,  34.0f,   0xff1080f0u, 0xfff08010u, 0, 0 },
    { -11.25f,    -0.9375f, 38.25f,  51.0f,   0xffa0a020u, 0xffa0a020u, 1, 0 },
    {  -0.9375f,  18.75f,   55.25f,  63.75f,  0xff20a0a0u, 0xff20a0a0u, 1, 0 },
    { 510.0f,    530.625f,  208.25f, 221.0f,  0xff6040a0u, 0xff6040a0u, 0, 1 },
    { 460.3125f, 500.625f,  225.25f, 238.0f,  0xfff08010u, 0xff1080f0u, 0, 1 },
    { 491.25f,   501.5625f, 242.25f, 255.0f,  0xff20a0a0u, 0xff20a0a0u, 1, 1 },
    { 460.3125f, 481.875f,  259.25f, 267.75f, 0xffa0a020u, 0xffa0a020u, 1, 1 },
};
#define WIDE_NSHAPES (sizeof k_wide_shapes / sizeof k_wide_shapes[0])

static uint32_t s_wide_vc;

static void wide_vtx(uint32_t color, float x, float y) {
    uint8_t *d = (uint8_t *)SR_HOST(s_wide_vc);
    const float pos[3] = { x, y, 0.0f };
    memcpy(d, &color, 4);
    memcpy(d + 4, pos, 12);
    s_wide_vc += 16u;
}

static void wide_shape(TnlpList *L, const WideShape *sh, float shift) {
    const float x0 = (sh->x0 + shift - 240.0f) / 240.0f, x1 = (sh->x1 + shift - 240.0f) / 240.0f;
    const float y0 = (136.0f - sh->y0) / 136.0f, y1 = (136.0f - sh->y1) / 136.0f;
    const uint32_t at = s_wide_vc;
    TNLP_W(L, 0x50, sh->c0 != sh->c1 ? 1u : 0u);
    if (sh->sprite) {
        wide_vtx(sh->c1, x0, y0); wide_vtx(sh->c1, x1, y1);
    } else {
        wide_vtx(sh->c0, x0, y0); wide_vtx(sh->c1, x1, y0); wide_vtx(sh->c0, x0, y1);
        wide_vtx(sh->c1, x1, y0); wide_vtx(sh->c1, x1, y1); wide_vtx(sh->c0, x0, y1);
    }
    TNLP_W(L, 0x01, at);
    TNLP_W(L, 0x04, sh->sprite ? (6u << 16) | 2u : (3u << 16) | 6u);
}

static uint32_t wide_build(int flags) {
    TnlpList L = { (uint32_t *)SR_HOST(WIDE_LIST), 0 };
    const uint32_t through = (7u << 2) | (3u << 7) | (1u << 23), transform = (7u << 2) | (3u << 7);
    s_wide_vc = WIDE_VTX;
    TNLP_W(&L, 0x10, 0);
    TNLP_W(&L, 0x9C, WIDE_FB & 0x00FFFFFFu);
    TNLP_W(&L, 0x9D, 512u | ((WIDE_FB & 0xFF000000u) >> 8));
    TNLP_W(&L, 0xD2, 3);
    TNLP_W(&L, 0x15, 0);
    TNLP_W(&L, 0x16, ((FB_H - 1u) << 10) | 479u);
    TNLP_W(&L, 0xD4, 0);
    TNLP_W(&L, 0xD5, ((FB_H - 1u) << 10) | ((flags & WF_NARROW) ? 400u : 479u));
    TNLP_W(&L, 0xD6, 0);
    TNLP_W(&L, 0xD7, 0xFFFF);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) TNLP_W(&L, cmd, 0);   /* every unit off */
    TNLP_W(&L, 0x50, 0);                                                /* flat shading */
    TNLP_W(&L, 0xE8, 0);
    TNLP_W(&L, 0xE9, 0);

    uint32_t at = s_wide_vc;                    /* sceGuClear's strips: 8 x 64 pixels */
    if (!(flags & WF_NOCLEAR)) {
        for (int i = 0; i < 8; i++) { wide_vtx(WIDE_BG, (float)(i * 64), 0.0f); wide_vtx(WIDE_BG, (float)(i * 64 + 64), 272.0f); }
        TNLP_W(&L, 0xD3, 0x301);
        TNLP_W(&L, 0x12, through);
        TNLP_W(&L, 0x01, at);
        TNLP_W(&L, 0x04, (6u << 16) | 16u);
        TNLP_W(&L, 0xD3, 0);
    }

    static const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    static const float ident44[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    tnlp_matrix(&L, 0x3A, ident43, 12);
    tnlp_matrix(&L, 0x3C, ident43, 12);
    tnlp_matrix(&L, 0x3E, ident44, 16);
    TNLP_W(&L, 0x42, tnlp_f24(240.0f));
    TNLP_W(&L, 0x43, tnlp_f24(-136.0f));
    TNLP_W(&L, 0x44, tnlp_f24(-32000.0f));
    TNLP_W(&L, 0x45, tnlp_f24(2048.0f));
    TNLP_W(&L, 0x46, tnlp_f24(2048.0f));
    TNLP_W(&L, 0x47, tnlp_f24(32767.5f));
    TNLP_W(&L, 0x4C, (2048u - 240u) << 4);
    TNLP_W(&L, 0x4D, (2048u - 136u) << 4);
    TNLP_W(&L, 0x12, transform);
    if (flags & WF_LOGICOP) {                   /* COPY: the same pixels, drawn in software */
        TNLP_W(&L, 0xE6, 3);
        TNLP_W(&L, 0x28, 1);
    }
    if (!(flags & WF_NOCLEAR)) {
        at = s_wide_vc;                         /* clip x -2..2 is guest x -240..720 */
        wide_vtx(WIDE_QUAD, -2.0f, 0.5f); wide_vtx(WIDE_QUAD, 2.0f, 0.5f); wide_vtx(WIDE_QUAD, -2.0f, -0.5f);
        wide_vtx(WIDE_QUAD, 2.0f, 0.5f); wide_vtx(WIDE_QUAD, 2.0f, -0.5f); wide_vtx(WIDE_QUAD, -2.0f, -0.5f);
        TNLP_W(&L, 0x01, at);
        TNLP_W(&L, 0x04, (3u << 16) | 6u);
    }
    for (size_t i = 0; i < WIDE_NSHAPES; i++) {
        wide_shape(&L, &k_wide_shapes[i], 0.0f);
        wide_shape(&L, &k_wide_shapes[i], k_wide_shapes[i].right ? -(float)WIDE_K : (float)WIDE_K);
    }
    TNLP_W(&L, 0x50, 0);
    TNLP_W(&L, 0x28, 0);

    at = s_wide_vc;
    wide_vtx(WIDE_BAR, 0.0f, 100.0f); wide_vtx(WIDE_BAR, 480.0f, 140.0f);
    if (flags & WF_FADE) { wide_vtx(WIDE_FADE, 0.0f, 0.0f); wide_vtx(WIDE_FADE, 480.0f, 272.0f); }
    TNLP_W(&L, 0x12, through);
    TNLP_W(&L, 0x01, at);
    TNLP_W(&L, 0x04, (6u << 16) | ((flags & WF_FADE) ? 4u : 2u));
    TNLP_W(&L, 0x0F, 0);
    TNLP_W(&L, 0x0C, 0);
    return WIDE_LIST;
}

/* One frame of the list on the current canvases: zeroed VRAM, then the list; copy all of
 * guest VRAM (after the Vulkan frame is synced back) and, if a canvas was drawn, the canvas
 * (512 x 272 RGBA8888 words). */
static int wide_frame(int gpu, int flags, uint32_t *vram, uint32_t *canvas) {
    memset(SR_HOST(WIDE_VRAM), 0, WIDE_VRAM_SIZE);
    sr_gpu_vram_dirty(WIDE_VRAM, WIDE_VRAM_SIZE);
    ge_wide_note_display(WIDE_FB);
    ge_set_gpu_hooks(gpu ? &k_hooks : NULL);
    ge_run_list(wide_build(flags), 0);
    ge_set_gpu_hooks(&k_hooks);
    int ok = 1;
    if (gpu) {
        GeGpuFbDescriptor desc = { .addr = WIDE_FB, .format = 3, .stride = 512, .width = 480, .height = FB_H };
        if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) ok = 0;
    }
    memcpy(vram, SR_HOST(WIDE_VRAM), WIDE_VRAM_SIZE);
    if (ok && canvas && ge_wide_wing()) {
        if (gpu) ok = gegpu_wide_read(WIDE_FB, canvas);
        else {
            int fmt = -1;
            const uint32_t *c = ge_wide_canvas(WIDE_FB, &fmt);
            ok = c && fmt == 3;
            if (ok) memcpy(canvas, c, 512u * FB_H * 4u);
        }
    }
    return ok;
}

/* clip_x_widen (docs/ENHANCEMENT_CONTRACT.md section 11.3.6): with Fill on the runtime scales the x row (elements
 * 0, 4, 8, 12) of each column-major clip matrix by 480 / (480 + 2D) and touches nothing else; with Fill off it
 * leaves every float alone. Two matrices at different offsets from one base, in guest RAM away from the frames. */
static int wide_clip_x_check(int wing, int cull, uint32_t off0, uint32_t off1) {
    const uint32_t base = SR_RAM_BASE + 0x00f00000u;
    const uint32_t offsets[2] = {off0, off1};
    /* A stock matrix pair: what the title's call leaves each time it rebuilds them. */
    const int rebuilds = 2;   /* call, call again with no rebuild, then rebuild and call */
    int ok = 1;
    for (int pass = 0; pass < rebuilds + 1; pass++) {
        if (pass != 1) {   /* pass 1 is the call that finds the matrices as the last call left them */
            for (uint32_t m = 0; m < 2; m++)
                for (uint32_t e = 0; e < 16u; e++) {
                    const float v = 1.0f + (float)e + 100.0f * (float)m;
                    memcpy(SR_HOST(base + offsets[m] + 4u * e), &v, sizeof v);
                }
        }
        ge_wide_set_cull(cull);
        sr_enhancement_widen_clip_x(base, offsets, 2u);
        ge_wide_set_cull(0);
        /* Scaled once with a wing and the key on, whatever the pass; any other pair leaves them alone. */
        const float scale = (wing && cull) ? 480.0f / (480.0f + 2.0f * (float)wing) : 1.0f;
        for (uint32_t m = 0; m < 2; m++)
            for (uint32_t e = 0; e < 16u; e++) {
                float got;
                memcpy(&got, SR_HOST(base + offsets[m] + 4u * e), sizeof got);
                const float v = 1.0f + (float)e + 100.0f * (float)m;
                const float want = (e % 4u == 0u) ? v * scale : v;
                if (got != want) {
                    fprintf(stderr, "gpu coherence selftest [wide-fill]: clip_x_widen wing %d cull %d pass %d "
                                    "matrix %u element %u: %g, want %g\n", wing, cull, pass, m, e, (double)got,
                            (double)want);
                    ok = 0;
                }
            }
    }
    return ok;
}

/* The table that stops a matrix being scaled twice holds 4096 matrices and never evicts. One call listing more
 * than that must scale each tracked matrix once, leave every other matrix untouched (never scaled untracked),
 * and count those in ge_wide_cull_untracked, which the run report names; a repeat call changes nothing and
 * counts them again. Run after the other clip_x_widen cells: the table is never cleared. */
#define FILL_N 4200u
static int wide_clip_x_fill_check(int wing) {
    static uint32_t offsets[FILL_N];
    const uint32_t base = SR_RAM_BASE + 0x00f00000u, first = 0x20000u;   /* clear of the other cells' matrices */
    const float scale = 480.0f / (480.0f + 2.0f * (float)wing);
    int ok = 1;
    for (uint32_t i = 0; i < FILL_N; i++) {
        offsets[i] = first + 64u * i;
        for (uint32_t e = 0; e < 16u; e++) {
            const float v = 1.0f + (float)e + (float)(i % 7u);
            memcpy(SR_HOST(base + offsets[i] + 4u * e), &v, sizeof v);
        }
    }
    unsigned long left_alone[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++) {
        const unsigned long u0 = ge_wide_cull_untracked();
        ge_wide_set_cull(1);
        sr_enhancement_widen_clip_x(base, offsets, FILL_N);
        ge_wide_set_cull(0);
        unsigned long scaled = 0, untouched = 0;
        for (uint32_t i = 0; i < FILL_N; i++) {
            float got;
            memcpy(&got, SR_HOST(base + offsets[i]), sizeof got);
            const float v = 1.0f + (float)(i % 7u);
            if (got == v * scale) scaled++;
            else if (got == v) untouched++;
            else ok = 0;   /* scaled twice, or partly */
        }
        left_alone[pass] = ge_wide_cull_untracked() - u0;
        if (scaled + untouched != FILL_N || untouched != left_alone[pass] || untouched == 0 ||
            untouched >= FILL_N) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: clip_x_widen table full pass %d: scaled %lu, "
                            "untouched %lu, counted untracked %lu of %u\n", pass, scaled, untouched,
                    left_alone[pass], FILL_N);
            ok = 0;
        }
    }
    if (left_alone[0] != left_alone[1]) ok = 0;
    return ok;
}

/* A fresh run with wing width `wing`: Fill is turned on for it and off after. */
static int wide_run(int gpu, int wing, int flags, uint32_t *vram, uint32_t *canvas) {
    coherence_reset_targets();
    if (ge_wide_set_wing(wing) != wing) return 0;
    int ok = wide_frame(gpu, flags, vram, canvas);
    if (ok && wing && !(flags & WF_NARROW) && !ge_wide_drawn(WIDE_FB)) ok = 0;
    if (!wing && (ge_wide_canvas(WIDE_FB, NULL) || ge_wide_drawn(WIDE_FB))) ok = 0;
    ge_wide_set_wing(0);
    return ok;
}

static uint32_t wide_want(const uint32_t *frame, int wing, int flags, uint32_t x, uint32_t y) {
    const uint32_t d = (uint32_t)wing;
    if (x >= 2u * d) return 0u;
    if (flags & WF_FADE) return WIDE_FADE;
    if (y >= 68u && y <= 203u) return (flags & WF_NOCLEAR) ? 0u : WIDE_QUAD;
    if (y < 68u && x < d) return frame[y * 512u + x - d + WIDE_K];
    if (y > 203u && x >= d) return frame[y * 512u + x - d + 480u - WIDE_K];
    return (flags & WF_NOCLEAR) ? 0u : WIDE_BG;
}

/* Whether row y crosses a gouraud shape, whose colours the two renderers interpolate within
 * one step of each other (each matches its own frame exactly). */
static int wide_gouraud_row(uint32_t y) {
    for (size_t i = 0; i < WIDE_NSHAPES; i++)
        if (k_wide_shapes[i].c0 != k_wide_shapes[i].c1 && (float)y + 1.0f > k_wide_shapes[i].y0 &&
            (float)y < k_wide_shapes[i].y1)
            return 1;
    return 0;
}

static int wide_near(uint32_t a, uint32_t b) {
    for (int sh = 0; sh < 32; sh += 8) {
        const int d = (int)((a >> sh) & 0xFFu) - (int)((b >> sh) & 0xFFu);
        if (d < -1 || d > 1) return 0;
    }
    return 1;
}

/* Each renderer's canvas against its own frame, exactly, and at render scale 1 the two canvases
 * against each other: byte for byte, except within one step per channel on a gouraud shape's
 * rows. Above scale 1 a Vulkan readback is each pixel's top-left sample, which at a sub-pixel
 * shape edge need not be the 1x software pixel, so there each renderer answers to its own
 * frame, whose copies of the shapes are sampled the same way. */
static int wide_compare(const char *name, int wing, int flags, const uint32_t *sw_frame, const uint32_t *sw_wing,
                        const uint32_t *gpu_frame, const uint32_t *gpu_wing) {
    for (uint32_t y = 0; y < FB_H; y++)
        for (uint32_t x = 0; x < 512u; x++) {
            const size_t i = (size_t)y * 512u + x;
            const uint32_t sw_want = wide_want(sw_frame, wing, flags, x, y);
            const uint32_t gpu_want = wide_want(gpu_frame, wing, flags, x, y);
            const int agree = sw_wing[i] == gpu_wing[i] || (wide_gouraud_row(y) && wide_near(sw_wing[i], gpu_wing[i]));
            if (sw_wing[i] != sw_want || gpu_wing[i] != gpu_want || (s_scale == 1 && !agree)) {
                fprintf(stderr, "gpu coherence selftest [wide-fill]: %s canvas (%u,%u) software=%08x (want %08x) "
                                "vulkan=%08x (want %08x) flags=%d\n",
                        name, x, y, (unsigned)sw_wing[i], (unsigned)sw_want, (unsigned)gpu_wing[i],
                        (unsigned)gpu_want, flags);
                return 0;
            }
        }
    /* The shapes are really in the wings: the mapping alone could pass on two empty bands. */
    if (!(flags & WF_FADE)) {
        const uint32_t d = (uint32_t)wing;
        const uint32_t xl = 40u <= d ? d - 40u : 0u, xr = d + 40u;   /* guest -40 and 520 */
        if ((40u <= d && sw_wing[10u * 512u + xl] != k_wide_shapes[0].c1) ||
            (xr < 2u * d && sw_wing[215u * 512u + xr] != k_wide_shapes[4].c1)) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: %s a band shape is missing from the wings\n", name);
            return 0;
        }
    }
    return 1;
}

/* Buffers: VRAM images for sw off/on and gpu off/on, then two canvases. */
typedef struct { uint32_t *sw_off, *sw_on, *gpu_off, *gpu_on, *sw_wing, *gpu_wing; } WideBufs;

static int wide_aspect(const char *name, int win_w, int win_h, int want_wing, const WideBufs *B) {
    const int wing = ge_wide_wing_for(win_w, win_h);
    if (wing != want_wing) {
        fprintf(stderr, "gpu coherence selftest [wide-fill]: %s (%dx%d) gives wing %d, want %d\n",
                name, win_w, win_h, wing, want_wing);
        return 0;
    }
    int ok = 1;
    for (int fade = 0; ok && fade < 2; fade++) {
        const int flags = fade ? WF_FADE : 0;
        unsigned long drawn0, dropped0, drawn1, dropped1, drawn2, dropped2;
        gegpu_wide_counts(&drawn0, &dropped0);
        if (!wide_run(0, 0, flags, B->sw_off, NULL) || !wide_run(0, wing, flags, B->sw_on, B->sw_wing)) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: a software run failed (%s fade=%d)\n", name, fade);
            ok = 0; break;
        }
        if (!wide_run(1, 0, flags, B->gpu_off, NULL)) { ok = 0; break; }
        gegpu_wide_counts(&drawn1, &dropped1);
        if (!wide_run(1, wing, flags, B->gpu_on, B->gpu_wing)) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: a Vulkan run failed (%s fade=%d)\n", name, fade);
            ok = 0; break;
        }
        gegpu_wide_counts(&drawn2, &dropped2);
        if (drawn1 != drawn0 || dropped1 != dropped0 || (wing ? drawn2 == drawn1 : drawn2 != drawn1) ||
            dropped2 != dropped1) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: %s wing primitives off=%lu on=%lu dropped=%lu\n",
                    name, drawn1 - drawn0, drawn2 - drawn1, dropped2 - dropped0);
            ok = 0;
        }
        if (memcmp(B->sw_off, B->sw_on, WIDE_VRAM_SIZE) || memcmp(B->gpu_off, B->gpu_on, WIDE_VRAM_SIZE)) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: %s Fill changed guest VRAM (fade=%d)\n", name, fade);
            ok = 0;
        }
        if (ok && wing) ok = wide_compare(name, wing, flags, B->sw_off, B->sw_wing, B->gpu_off, B->gpu_wing);
        /* The bar is in the authentic frame, so the comparison above saw a list that drew it. */
        if (ok && !fade && B->sw_on[120u * 512u + 240u] != WIDE_BAR) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: the bar is missing from the frame\n");
            ok = 0;
        }
        if (ok)
            printf("gpu coherence selftest: PASS %-22s %s fade=%d wing=%d scale=%d\n",
                   "wide-fill", name, fade, wing, s_scale);
    }
    return ok;
}

/* Frames after the first, at the 21:9 wing width, on the same canvases: a narrower scissor
 * reaches no wing, a shown canvas is cleared by the next wing draw (a frame with no clear
 * holds its own shapes on black), and on this backend a frame whose every wing primitive is
 * dropped leaves the canvas undrawn and black. The Vulkan leg runs first and each leg runs
 * whatever the other found, so either renderer's failure is reported on its own. */
static int wide_frames(const WideBufs *B) {
    int ok_all = 1;
    unsigned long drawn0, dropped0, drawn1, dropped1;
    for (int leg = 0; leg < 2; leg++) {
        const int gpu = leg == 0;
        int ok = 1;
        const char *who = gpu ? "vulkan" : "software";
        uint32_t *vram = gpu ? B->gpu_on : B->sw_on, *canvas = gpu ? B->gpu_wing : B->sw_wing;
        coherence_reset_targets();
        ge_wide_set_wing(WIDE_D);
        gegpu_wide_counts(&drawn0, &dropped0);
        if (!wide_frame(gpu, WF_NARROW, vram, NULL) || ge_wide_drawn(WIDE_FB)) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: %s: a narrower scissor reached a wing\n", who);
            ok = 0;
        }
        gegpu_wide_counts(&drawn1, &dropped1);
        if (ok && drawn1 != drawn0) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: %s: a narrower scissor drew wing primitives\n", who);
            ok = 0;
        }
        if (ok && (!wide_frame(gpu, 0, vram, canvas) || !ge_wide_drawn(WIDE_FB))) ok = 0;
        ge_wide_shown(WIDE_FB);
        if (ok && (!wide_frame(gpu, WF_NOCLEAR, vram, canvas) || !ge_wide_drawn(WIDE_FB))) {
            fprintf(stderr, "gpu coherence selftest [wide-fill]: %s: the frame after a shown one drew no wing\n", who);
            ok = 0;
        }
        if (ok) {
            for (uint32_t y = 0; ok && y < FB_H; y++)
                for (uint32_t x = 0; x < 512u; x++) {
                    const uint32_t want = wide_want(vram, WIDE_D, WF_NOCLEAR, x, y);
                    if (canvas[(size_t)y * 512u + x] != want) {
                        fprintf(stderr, "gpu coherence selftest [wide-fill]: %s: shown canvas (%u,%u) = %08x, want %08x\n",
                                who, x, y, (unsigned)canvas[(size_t)y * 512u + x], (unsigned)want);
                        ok = 0; break;
                    }
                }
        }
        if (ok && gpu) {
            ge_wide_shown(WIDE_FB);
            gegpu_wide_counts(&drawn0, &dropped0);
            if (!wide_frame(gpu, WF_NOCLEAR | WF_LOGICOP, vram, canvas)) ok = 0;
            gegpu_wide_counts(&drawn1, &dropped1);
            int black = 1;
            for (size_t i = 0; ok && i < (size_t)512u * FB_H; i++) if (canvas[i]) { black = 0; break; }
            if (!ok || ge_wide_drawn(WIDE_FB) || dropped1 == dropped0 || drawn1 != drawn0 || !black) {
                fprintf(stderr, "gpu coherence selftest [wide-fill]: every wing primitive dropped: drawn=%d "
                                "dropped=%lu recorded=%lu black=%d\n",
                        ge_wide_drawn(WIDE_FB), dropped1 - dropped0, drawn1 - drawn0, black);
                ok = 0;
            }
        }
        /* The second pair lies 41344 bytes apart, which shared a slot in a direct-mapped table: each
         * matrix is scaled once however the pair falls (a repeat call must not scale it again). */
        if (ok && ge_wide_set_wing(WIDE_D) == WIDE_D)
            ok = wide_clip_x_check(WIDE_D, 1, 640u, 448u) && wide_clip_x_check(WIDE_D, 0, 640u, 448u) &&
                 wide_clip_x_check(WIDE_D, 1, 0u, 41344u) && wide_clip_x_fill_check(WIDE_D);
        ge_wide_set_wing(0);
        if (ok) ok = wide_clip_x_check(0, 1, 640u, 448u);
        if (ok) printf("gpu coherence selftest: PASS %-22s %s frames scale=%d\n", "wide-fill", who, s_scale);
        else fprintf(stderr, "gpu coherence selftest [wide-fill]: FAIL %s frames\n", who);
        ok_all &= ok;
    }
    return ok_all;
}

/* ---- finer Bezier and spline patches (#2053): both renderers -------------------------------
 * draw_patch (ge.c) tessellates for both renderers, so the Vulkan backend receives the same
 * finer triangles as the software rasterizer. Through-mode patches with a colour per control
 * point (a curved Bezier patch, a four-patch open spline, and two Bezier patches that share a
 * curved edge) at factors 1 and 4: at render scale 1 the two renderers must cover the same
 * pixels and agree within one step per channel (the gouraud bound of the wide-fill case), and
 * on each renderer factor 4 must draw a different picture from factor 1. Above scale 1 a
 * readback is each pixel's top-left device sample, so only the second check runs there. */
#define PTESS_LIST 0x002E0000u
#define PTESS_VTX  0x002E8000u
#define PTESS_FB   0x04000000u

static uint32_t s_ptess_vc;

static void ptess_vtx(uint32_t color, float x, float y) {
    uint8_t *d = (uint8_t *)SR_HOST(s_ptess_vc);
    const float pos[3] = { x, y, 0.0f };
    memcpy(d, &color, 4);
    memcpy(d + 4, pos, 12);
    s_ptess_vc += 16u;
}

static uint32_t ptess_build(void) {
    TnlpList L = { (uint32_t *)SR_HOST(PTESS_LIST), 0 };
    const uint32_t through = (7u << 2) | (3u << 7) | (1u << 23);
    s_ptess_vc = PTESS_VTX;
    TNLP_W(&L, 0x10, 0);
    TNLP_W(&L, 0x9C, PTESS_FB & 0x00FFFFFFu);
    TNLP_W(&L, 0x9D, 512u | ((PTESS_FB & 0xFF000000u) >> 8));
    TNLP_W(&L, 0xD2, 3);
    TNLP_W(&L, 0x15, 0);
    TNLP_W(&L, 0x16, ((FB_H - 1u) << 10) | 479u);
    TNLP_W(&L, 0xD4, 0);
    TNLP_W(&L, 0xD5, ((FB_H - 1u) << 10) | 479u);
    TNLP_W(&L, 0xD6, 0);
    TNLP_W(&L, 0xD7, 0xFFFF);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) TNLP_W(&L, cmd, 0);   /* every unit off */
    TNLP_W(&L, 0x50, 1);                                                /* gouraud */
    TNLP_W(&L, 0xE8, 0);
    TNLP_W(&L, 0xE9, 0);
    TNLP_W(&L, 0x12, through);
    TNLP_W(&L, 0x37, 0);                                                /* triangle patches */

    static const float bulge[4] = { 0.0f, 50.0f, -35.0f, 10.0f };
    uint32_t at = s_ptess_vc;                                           /* curved Bezier patch */
    for (int v = 0; v < 4; v++)
        for (int u = 0; u < 4; u++)
            ptess_vtx(0xFF000000u | (uint32_t)(40 + 60 * u) << 16 | (uint32_t)(30 + 70 * v) << 8 | (uint32_t)(200 - 20 * (u + v)),
                      20.0f + 45.0f * (float)u + (u == 1 || u == 2 ? bulge[v] : 0.0f),
                      20.0f + 40.0f * (float)v + (v == 1 || v == 2 ? bulge[u] * 0.5f : 0.0f));
    TNLP_W(&L, 0x36, (1u << 8) | 2u);
    TNLP_W(&L, 0x01, at);
    TNLP_W(&L, 0x05, 0x0404u);

    at = s_ptess_vc;                                                    /* four-patch open spline */
    for (int v = 0; v < 4; v++)
        for (int u = 0; u < 7; u++)
            ptess_vtx(0xFF000000u | (uint32_t)(30 * u) << 16 | (uint32_t)(60 * v) << 8 | 0x80u,
                      220.0f + 38.0f * (float)u, 20.0f + 40.0f * (float)v + ((u & 1) ? 25.0f : -15.0f));
    TNLP_W(&L, 0x36, (1u << 8) | 2u);
    TNLP_W(&L, 0x01, at);
    TNLP_W(&L, 0x06, 0x0407u | (3u << 16) | (3u << 18));

    static const float edge[4] = { 200.0f, 245.0f, 155.0f, 200.0f };   /* two patches, one curved edge */
    static const float lx[3] = { 100.0f, 115.0f, 130.0f }, rx[3] = { 265.0f, 285.0f, 300.0f };
    TNLP_W(&L, 0x36, (3u << 8) | 1u);
    for (int side = 0; side < 2; side++) {
        at = s_ptess_vc;
        for (int v = 0; v < 4; v++) {
            const float y = 180.0f + 28.0f * (float)v;
            const uint32_t c = 0xFF000000u | (uint32_t)(60 * v) << 8 | (side ? 0xC0u : 0x40u);
            if (side) ptess_vtx(c, edge[v], y);
            for (int u = 0; u < 3; u++) ptess_vtx(c, side ? rx[u] : lx[u], y);
            if (!side) ptess_vtx(c, edge[v], y);
        }
        TNLP_W(&L, 0x01, at);
        TNLP_W(&L, 0x05, 0x0404u);
    }
    TNLP_W(&L, 0x0F, 0);
    TNLP_W(&L, 0x0C, 0);
    return PTESS_LIST;
}

static int ptess_frame(int gpu, uint32_t *out) {
    const size_t bytes = (size_t)512 * FB_H * 4u;
    memset(SR_HOST(PTESS_FB), 0, bytes);
    sr_gpu_vram_dirty(PTESS_FB, (uint32_t)bytes);
    ge_set_gpu_hooks(gpu ? &k_hooks : NULL);
    ge_run_list(ptess_build(), 0);
    ge_set_gpu_hooks(&k_hooks);
    int ok = 1;
    if (gpu) {
        GeGpuFbDescriptor desc = { .addr = PTESS_FB, .format = 3, .stride = 512, .width = 480, .height = FB_H };
        ok = gegpu_sync_guest_fb(&desc) == GEGPU_SYNC_OK;
    }
    memcpy(out, SR_HOST(PTESS_FB), bytes);
    return ok;
}

static int coherence_run_patch_tess(void) {
    const size_t px = (size_t)512 * FB_H;
    uint32_t *buf = (uint32_t *)malloc(px * 4u * 4u);
    if (!buf) return 0;
    uint32_t *sw[2] = { buf, buf + px }, *gpu[2] = { buf + 2 * px, buf + 3 * px };
    static const int factors[2] = { 1, 4 };
    int ok = 1;
    size_t near_px[2] = { 0, 0 }, drawn[2] = { 0, 0 };
    for (int i = 0; i < 2 && ok; i++) {
        ge_patch_tess_set(factors[i]);
        coherence_reset_targets();
        ok = ptess_frame(0, sw[i]) && ptess_frame(1, gpu[i]);
        if (!ok) {
            fprintf(stderr, "gpu coherence selftest [patch-tess]: factor %d: GPU readback failed\n", factors[i]);
            break;
        }
        for (size_t p = 0; s_scale == 1 && p < px; p++) {
            if ((p % 512u) >= 480u) continue;
            const uint32_t a = sw[i][p], b = gpu[i][p];
            if (a) drawn[i]++;
            if (a == b) continue;
            if ((a == 0u) != (b == 0u) || !wide_near(a, b)) {
                fprintf(stderr, "gpu coherence selftest [patch-tess]: factor %d: pixel (%zu,%zu) gpu=%08x "
                                "software=%08x\n", factors[i], p % 512u, p / 512u, b, a);
                ok = 0;
                break;
            }
            near_px[i]++;
        }
    }
    ge_patch_tess_set(1);
    if (ok && (memcmp(sw[0], sw[1], px * 4u) == 0 || memcmp(gpu[0], gpu[1], px * 4u) == 0)) {
        fprintf(stderr, "gpu coherence selftest [patch-tess]: factor 4 drew the factor 1 picture (software %s, "
                        "gpu %s)\n", memcmp(sw[0], sw[1], px * 4u) ? "differs" : "same",
                memcmp(gpu[0], gpu[1], px * 4u) ? "differs" : "same");
        ok = 0;
    }
    coherence_reset_targets();
    free(buf);
    if (ok && s_scale == 1)
        printf("gpu coherence selftest: PASS %-22s factors 1 and 4: same coverage (%zu and %zu pixels), "
               "%zu and %zu pixels one step apart; factor 4 differs from 1 on both\n", "patch-tess",
               drawn[0], drawn[1], near_px[0], near_px[1]);
    else if (ok)
        printf("gpu coherence selftest: PASS %-22s scale %d: factor 4 differs from 1 on both renderers (the "
               "software comparison needs scale 1)\n", "patch-tess", s_scale);
    return ok;
}

/* The picture gegpu_present shows with Fill on (Vulkan only; the software presenter composes
 * the same picture in gui.c): the left wing, the frame, then the right wing, (480 + 2D) x 272.
 * `want_wings` 1: the wings are the canvas's; 0: they are black. RGB only: the canvas holds the
 * GE's alpha, and the presenter shows no alpha. */
static int wide_picture_check(const char *what, const uint32_t *pic, int cols, const uint32_t *frame,
                              const uint32_t *canvas, int want_wings) {
    const uint32_t d = (uint32_t)WIDE_D;
    if (cols != (int)(480u + 2u * d)) {
        fprintf(stderr, "gpu coherence selftest [wide-fill]: present %s: %d columns, want %u\n", what, cols,
                480u + 2u * d);
        return 0;
    }
    for (uint32_t y = 0; y < FB_H; y++)
        for (uint32_t x = 0; x < (uint32_t)cols; x++) {
            uint32_t want;
            if (x < d) want = want_wings ? canvas[y * 512u + x] : 0u;
            else if (x < d + 480u) want = frame[y * 512u + x - d];
            else want = want_wings ? canvas[y * 512u + x - 480u] : 0u;
            const uint32_t got = pic[(size_t)y * (uint32_t)cols + x];
            /* With supersampling (contract 10.21) the frame is the average of its samples: a pixel by
             * an edge mixes the colours on either side, so each channel lies within those of the
             * guest pixels around it. The wings come from the canvas and stay exact. */
            if (s_ssaa > 1 && x >= d && x < d + 480u && (got & 0x00FFFFFFu) != (want & 0x00FFFFFFu)) {
                const uint32_t fx = x - d;
                int inside = 1;
                for (int sh = 0; sh < 24; sh += 8) {
                    uint32_t lo = 255u, hi = 0u;
                    for (uint32_t ny = y ? y - 1u : 0u; ny <= y + 1u && ny < FB_H; ny++)
                        for (uint32_t nx = fx ? fx - 1u : 0u; nx <= fx + 1u && nx < 480u; nx++) {
                            const uint32_t c = (frame[ny * 512u + nx] >> sh) & 0xFFu;
                            if (c < lo) lo = c;
                            if (c > hi) hi = c;
                        }
                    const uint32_t g = (got >> sh) & 0xFFu;
                    if (g < lo || g > hi) inside = 0;
                }
                if (inside) continue;
            }
            if ((got & 0x00FFFFFFu) != (want & 0x00FFFFFFu)) {
                fprintf(stderr, "gpu coherence selftest [wide-fill]: present %s: picture (%u,%u) = %08x, want "
                                "%08x\n", what, x, y, (unsigned)got, (unsigned)want);
                return 0;
            }
        }
    return 1;
}

static int wide_present(const WideBufs *B) {
    const size_t cap = (size_t)(480 + 2 * GE_WIDE_MAX_WING) * FB_H;
    uint32_t *pic = (uint32_t *)malloc(cap * 4u);
    if (!pic) return 0;
    int ok = 1, cols = 0;
    coherence_reset_targets();
    ge_wide_set_wing(WIDE_D);
    /* A frame whose wings are drawn: wings from its canvas, the frame between them. */
    if (!wide_frame(1, 0, B->gpu_on, B->gpu_wing) || !ge_wide_drawn(WIDE_FB) ||
        !gegpu_wide_present_read(WIDE_FB, 3, 512, pic, &cols) ||
        !wide_picture_check("drawn", pic, cols, B->gpu_on, B->gpu_wing, 1))
        ok = 0;
    /* Shown, then presented again with no draw in between (a 30 Hz frame on a 60 Hz VBLANK): it
     * keeps its wings. */
    ge_wide_shown(WIDE_FB);
    if (ok && (!gegpu_wide_present_read(WIDE_FB, 3, 512, pic, &cols) ||
               !wide_picture_check("again", pic, cols, B->gpu_on, B->gpu_wing, 1))) {
        fprintf(stderr, "gpu coherence selftest [wide-fill]: a frame presented twice lost its wings\n");
        ok = 0;
    }
    /* The next frame draws, none of it in a wing: its wings are black, not the last frame's. */
    ge_wide_shown(WIDE_FB);
    if (ok && (!wide_frame(1, WF_NARROW, B->gpu_on, NULL) || ge_wide_drawn(WIDE_FB) ||
               !gegpu_wide_present_read(WIDE_FB, 3, 512, pic, &cols) ||
               !wide_picture_check("undrawn", pic, cols, B->gpu_on, B->gpu_wing, 0))) {
        fprintf(stderr, "gpu coherence selftest [wide-fill]: a new frame with no wing draw showed old wings\n");
        ok = 0;
    }
    /* Fill off: no picture is made, and the frame is presented alone, as before. */
    ge_wide_set_wing(0);
    if (ok && gegpu_wide_present_read(WIDE_FB, 3, 512, pic, &cols)) {
        fprintf(stderr, "gpu coherence selftest [wide-fill]: Fill off still made a wide picture\n");
        ok = 0;
    }
    free(pic);
    if (ok) printf("gpu coherence selftest: PASS %-22s vulkan present scale=%d\n", "wide-fill", s_scale);
    else fprintf(stderr, "gpu coherence selftest [wide-fill]: FAIL vulkan present\n");
    return ok;
}

static int coherence_run_wide_fill(void) {
    const size_t px = (size_t)512 * FB_H, vw = WIDE_VRAM_SIZE / 4u;
    uint32_t *buf = (uint32_t *)malloc((vw * 4u + px * 2u) * 4u);
    if (!buf) return 0;
    const WideBufs B = { buf, buf + vw, buf + 2 * vw, buf + 3 * vw, buf + 4 * vw, buf + 4 * vw + px };
    /* 480x272 is 30:17: a 16:9 window widens it by under 4 pixels, so 16:9 has no wings. */
    /* Every part runs, so one failure does not hide another's. */
    int ok = wide_aspect("16:9", 1920, 1080, 0, &B);
    ok &= wide_aspect("21:9", 2520, 1080, WIDE_D, &B);
    ok &= wide_aspect("32:9", 3840, 1080, 240, &B);
    ok &= wide_frames(&B);
    ok &= wide_present(&B);
    coherence_reset_targets();
    free(buf);
    return ok;
}

/* Free every depth surface (none of them newer than guest VRAM), so a case starts from empty slots. */
static void coherence_reset_depths(void) {
    coherence_reset_targets();
    (void)cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *d = &s_deps[i];
        if (!d->used) continue;
        if (d->view) vkDestroyImageView(s_dev, d->view, NULL);
        if (d->img) vkDestroyImage(s_dev, d->img, NULL);
        if (d->mem) vkFreeMemory(s_dev, d->mem, NULL);
        twin_depth_free(d);
        memset(d, 0, sizeof(*d));
    }
}

/* Depth newer than guest VRAM survives eviction. A draw that writes depth marks its surface
 * newer than VRAM (#1967); when a fifth depth surface arrives, depth_acquire evicts slot 0. The
 * evicted surface's values must reach guest VRAM first: a CPU read of that depth, or the surface's
 * next use (seeded from VRAM), must see what the GPU drew, not what VRAM held before. A surface no
 * draw wrote depth to is not written back when it goes (checked with a sixth surface). */
static int coherence_run_depth_evict_newer(void) {
    const char *name = "depth-evict-newer";
    const uint32_t base = 0x04100000u, stride = 512u;
    /* Five depth surfaces, none overlapping the colour target (0x100000..0x188000) or the first. */
    static const uint32_t zb[5] = { 0x04000000u, 0x04050000u, 0x040A0000u, 0x04190000u, 0x04048000u };
    const uint32_t depth_offset = (10u * stride + 10u) * 2u;
    const uint32_t depth_swizzled = (zb[0] + depth_offset) ^ 0x2040u;   /* the GE's absolute form */
    coherence_reset_depths();
    g_sr_vram_read_pending = 0;
    memset(SR_HOST(base), 0, stride * FB_H * 4u);
    memset(SR_HOST(zb[0]), 0, 0x44000u + 0x2040u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    sr_gpu_vram_dirty(zb[0], 0x44000u + 0x2040u);

    int ok = 1;
    for (int i = 0; ok && i < 5; i++) {
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = base;
        s_ge->fbw = stride;
        s_ge->fbfmt = 3u;
        s_ge->zbp = zb[i];
        s_ge->zbw = stride;
        s_ge->scis_x2 = (int)FB_W - 1;
        s_ge->scis_y2 = (int)FB_H - 1;
        s_ge->maxz = 65535u;
        if (i == 0) {                          /* the first draw writes depth 0x1111 */
            s_ge->ztest_enable = 1;
            s_ge->ztest = 1u;                  /* ALWAYS */
            s_ge->zwrite_disable = 0;
        }
        GeVtx p0 = { 10.0f, 10.0f, (float)0x1111u, 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
        GeVtx p1 = p0;
        p1.x = 12.0f;
        p1.y = 12.0f;
        ok = hook_sprite(&p0, &p1, 0) && s_nbatch && submit_pending();
        if (ok && i == 0) {
            Target *t = target_find_by_fba(base & 0x00FFFFFFu);
            ok = t && t->dep == &s_deps[0] && s_deps[0].guest_dirty;
        }
    }
    /* The fifth surface took slot 0: the first one is gone from the GPU. */
    ok = ok && s_deps[0].used && s_deps[0].zba == (zb[4] & 0x00FFFFFFu);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [%s]: setup failed (slot 0 zba=0x%06x dirty=%d)\n",
                name, s_deps[0].zba, s_deps[0].guest_dirty);
        coherence_reset_depths();
        return 0;
    }
    uint16_t kept = 0u;
    memcpy(&kept, SR_HOST_LINEAR(depth_swizzled), sizeof kept);
    if (kept != 0x1111u) {
        fprintf(stderr, "gpu coherence selftest [%s]: evicting a depth surface newer than guest VRAM "
                        "lost it: VRAM holds 0x%04x, the GPU drew 0x1111\n", name, kept);
        coherence_reset_depths();
        return 0;
    }
    /* A clean surface is not written back when it goes. Slot 0 now holds the fifth surface, which
     * no draw wrote depth to. Its VRAM gets a marker behind the surface's back (as a colour target
     * aliasing it would leave there, the #1967 hazard), and a sixth surface evicts it: the marker
     * must survive. A writeback of every evicted surface would put the stale seed over it. */
    const uint32_t clean_swizzled = (zb[4] + depth_offset) ^ 0x2040u;
    uint16_t marker = 0u;
    memcpy(&marker, SR_HOST_LINEAR(clean_swizzled), sizeof marker);
    marker = (uint16_t)~marker;
    memcpy(SR_HOST_LINEAR(clean_swizzled), &marker, sizeof marker);
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = 3u;
    s_ge->zbp = zb[0];                         /* free again since the first eviction */
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    {
        GeVtx p0 = { 10.0f, 10.0f, (float)0x1111u, 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
        GeVtx p1 = p0;
        p1.x = 12.0f;
        p1.y = 12.0f;
        ok = hook_sprite(&p0, &p1, 0) && s_nbatch && submit_pending() &&
             s_deps[0].used && s_deps[0].zba == (zb[0] & 0x00FFFFFFu);
    }
    uint16_t survived = 0u;
    memcpy(&survived, SR_HOST_LINEAR(clean_swizzled), sizeof survived);
    if (!ok || survived != marker) {
        fprintf(stderr, "gpu coherence selftest [%s]: evicting a clean depth surface %s: VRAM holds 0x%04x, "
                        "the marker was 0x%04x\n", name, ok ? "wrote it back" : "(setup failed)", survived, marker);
        coherence_reset_depths();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n", name, stride, s_scale);
    coherence_reset_depths();
    return 1;
}

/* A zbp change alone moves the depth buffer. A title may keep its colour target and point zbp at
 * another depth buffer; the next draws must test and write that buffer, not the one the colour
 * target was last drawn with. */
static int coherence_run_depth_zbp_switch(void) {
    const char *name = "depth-zbp-switch";
    const uint32_t base = 0x04100000u, stride = 512u;
    static const uint32_t zb[2] = { 0x04000000u, 0x04050000u };
    static const uint16_t z[2] = { 0x1111u, 0x2222u };
    const uint32_t depth_offset = (10u * stride + 10u) * 2u;
    coherence_reset_depths();
    g_sr_vram_read_pending = 0;
    memset(SR_HOST(base), 0, stride * FB_H * 4u);
    memset(SR_HOST(zb[0]), 0, 0x94000u + 0x2040u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    sr_gpu_vram_dirty(zb[0], 0x94000u + 0x2040u);

    int ok = 1;
    for (int i = 0; ok && i < 2; i++) {
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = base;
        s_ge->fbw = stride;
        s_ge->fbfmt = 3u;
        s_ge->zbp = zb[i];
        s_ge->zbw = stride;
        s_ge->scis_x2 = (int)FB_W - 1;
        s_ge->scis_y2 = (int)FB_H - 1;
        s_ge->maxz = 65535u;
        s_ge->ztest_enable = 1;
        s_ge->ztest = 1u;                      /* ALWAYS */
        s_ge->zwrite_disable = 0;
        GeVtx p0 = { 10.0f, 10.0f, (float)z[i], 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
        GeVtx p1 = p0;
        p1.x = 12.0f;
        p1.y = 12.0f;
        /* No submit between the draws: the second one arrives with the first still current. */
        ok = hook_sprite(&p0, &p1, 0) && s_nbatch;
    }
    ok = ok && submit_pending();
    for (int i = 0; ok && i < MAX_DEP; i++)
        if (s_deps[i].used) ok = depth_readback_to_guest(&s_deps[i]);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [%s]: setup failed\n", name);
        coherence_reset_depths();
        return 0;
    }
    for (int i = 0; i < 2; i++) {
        uint16_t got = 0u;
        memcpy(&got, SR_HOST_LINEAR((zb[i] + depth_offset) ^ 0x2040u), sizeof got);
        if (got != z[i]) {
            fprintf(stderr, "gpu coherence selftest [%s]: depth buffer %d (zbp 0x%08x) holds 0x%04x, "
                            "its draw wrote 0x%04x\n", name, i, zb[i], got, z[i]);
            ok = 0;
        }
    }
    if (ok)
        printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n", name, stride, s_scale);
    coherence_reset_depths();
    return ok;
}

/* A zbw change alone moves the depth buffer too: the same zbp read with another stride is another
 * surface. The first draw writes 0x1111 at (10,10) with zbw 512, the second 0x2222 at (10,30) with
 * zbw 256, colour target and zbp unchanged. Each surface's readback must hold its own draw: before
 * the fast path keyed on zbw, the second draw landed in the 512-wide surface (row 30 there), and no
 * 256-wide surface existed. */
static int coherence_run_depth_zbw_switch(void) {
    const char *name = "depth-zbw-switch";
    const uint32_t base = 0x04100000u, stride = 512u, zb = 0x04000000u;
    static const uint32_t zbw[2] = { 512u, 256u };
    static const float row[2] = { 10.0f, 30.0f };
    static const uint16_t z[2] = { 0x1111u, 0x2222u };
    coherence_reset_depths();
    g_sr_vram_read_pending = 0;
    memset(SR_HOST(base), 0, stride * FB_H * 4u);
    memset(SR_HOST(zb), 0, 0x44000u + 0x2040u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    sr_gpu_vram_dirty(zb, 0x44000u + 0x2040u);

    int ok = 1;
    for (int i = 0; ok && i < 2; i++) {
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = base;
        s_ge->fbw = stride;
        s_ge->fbfmt = 3u;
        s_ge->zbp = zb;
        s_ge->zbw = zbw[i];
        s_ge->scis_x2 = (int)FB_W - 1;
        s_ge->scis_y2 = (int)FB_H - 1;
        s_ge->maxz = 65535u;
        s_ge->ztest_enable = 1;
        s_ge->ztest = 1u;                      /* ALWAYS */
        s_ge->zwrite_disable = 0;
        GeVtx p0 = { 10.0f, row[i], (float)z[i], 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
        GeVtx p1 = p0;
        p1.x = 12.0f;
        p1.y = row[i] + 2.0f;
        /* No submit between the draws: the second one arrives with the first still current. */
        ok = hook_sprite(&p0, &p1, 0) && s_nbatch;
    }
    ok = ok && submit_pending();
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [%s]: setup failed\n", name);
        coherence_reset_depths();
        return 0;
    }
    /* The two surfaces share VRAM, so each is read back and checked on its own. */
    for (int i = 0; ok && i < 2; i++) {
        DepthEnt *d = NULL;
        for (int k = 0; k < MAX_DEP; k++)
            if (s_deps[k].used && s_deps[k].zba == vram_off(zb) && s_deps[k].zstride == zbw[i]) d = &s_deps[k];
        if (!d) {
            fprintf(stderr, "gpu coherence selftest [%s]: no depth surface for zbw %u: the draw with "
                            "that zbw went to another surface\n", name, zbw[i]);
            ok = 0;
            break;
        }
        if (!depth_readback_to_guest(d)) {
            fprintf(stderr, "gpu coherence selftest [%s]: readback of the zbw %u surface failed\n", name, zbw[i]);
            ok = 0;
            break;
        }
        const uint32_t off = ((uint32_t)row[i] * zbw[i] + 10u) * 2u;
        uint16_t got = 0u;
        memcpy(&got, SR_HOST_LINEAR((zb + off) ^ 0x2040u), sizeof got);
        if (got != z[i]) {
            fprintf(stderr, "gpu coherence selftest [%s]: zbw %u surface holds 0x%04x at (10,%d), its draw "
                            "wrote 0x%04x\n", name, zbw[i], got, (int)row[i], z[i]);
            ok = 0;
        }
    }
    if (ok)
        printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n", name, stride, s_scale);
    coherence_reset_depths();
    return ok;
}

/* An eviction writeback that overlaps the new surface seeds it. Slot 0 holds zbp A at zbw 512, newer
 * than VRAM (0x1111 at (10,10)); three more surfaces fill the slots; then a draw with zbp A at zbw 256
 * evicts slot 0. The writeback puts 0x1111 in VRAM at the address that the 256-wide surface reads as
 * (10,20), so the new surface must be seeded with it, not with the CPU depth view target_acquire
 * loaded for zbw 256 before the writeback. depth_acquire reloads that view because a surface newer
 * than VRAM keeps g_sr_vram_read_pending set (hook_vram_pending) until it is written back. That draw
 * tests depth GREATER at z=0x0800: at (10,20) it must fail (the seed is 0x1111), and at (100,100),
 * where nothing was drawn, it must pass. */
static int coherence_run_depth_evict_overlap(void) {
    const char *name = "depth-evict-overlap";
    const uint32_t base = 0x04100000u, stride = 512u;
    static const uint32_t zb[4] = { 0x04000000u, 0x04050000u, 0x040A0000u, 0x04190000u };
    coherence_reset_depths();
    g_sr_vram_read_pending = 0;
    memset(SR_HOST(base), 0, stride * FB_H * 4u);
    memset(SR_HOST(zb[0]), 0, 0x44000u + 0x2040u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    sr_gpu_vram_dirty(zb[0], 0x44000u + 0x2040u);

    int ok = 1;
    for (int i = 0; ok && i < 5; i++) {
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = base;
        s_ge->fbw = stride;
        s_ge->fbfmt = 3u;
        s_ge->zbp = zb[i < 4 ? i : 0];
        s_ge->zbw = i < 4 ? stride : 256u;
        s_ge->scis_x2 = (int)FB_W - 1;
        s_ge->scis_y2 = (int)FB_H - 1;
        s_ge->maxz = 65535u;
        if (i == 0) {                          /* the first draw writes depth 0x1111 */
            s_ge->ztest_enable = 1;
            s_ge->ztest = 1u;                  /* ALWAYS */
            s_ge->zwrite_disable = 0;
        } else if (i == 4) {                   /* the last one tests GREATER, without writing depth */
            s_ge->ztest_enable = 1;
            s_ge->ztest = 6u;
            s_ge->zwrite_disable = 1;
        }
        if (i < 4) {
            GeVtx p0 = { 10.0f, 10.0f, (float)0x1111u, 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
            GeVtx p1 = p0;
            p1.x = 12.0f;
            p1.y = 12.0f;
            ok = hook_sprite(&p0, &p1, 0) && s_nbatch && submit_pending();
            if (ok && i == 0) ok = s_deps[0].used && s_deps[0].guest_dirty;
            continue;
        }
        static const float at[2][2] = { { 10.0f, 20.0f }, { 100.0f, 100.0f } };
        for (int k = 0; ok && k < 2; k++) {
            GeVtx p0 = { at[k][0], at[k][1], (float)0x0800u, 1.0f, 0.0f, 0.0f, 1.0f, 0xa0, 0xb0, 0xc0, 0xff };
            GeVtx p1 = p0;
            p1.x = at[k][0] + 2.0f;
            p1.y = at[k][1] + 2.0f;
            ok = hook_sprite(&p0, &p1, 0) && s_nbatch;
        }
        ok = ok && submit_pending();
    }
    /* The zbw 256 surface took slot 0. */
    ok = ok && s_deps[0].used && s_deps[0].zba == vram_off(zb[0]) && s_deps[0].zstride == 256u;
    Target *t = ok ? target_find_by_fba(base & 0x00FFFFFFu) : NULL;
    ok = ok && t && target_readback(t);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [%s]: setup failed (slot 0 zba=0x%06x zstride=%u)\n",
                name, s_deps[0].zba, s_deps[0].zstride);
        coherence_reset_depths();
        return 0;
    }
    uint32_t seeded = 0u, open = 0u;
    memcpy(&seeded, SR_HOST(base + (20u * stride + 10u) * 4u), sizeof seeded);
    memcpy(&open, SR_HOST(base + (100u * stride + 100u) * 4u), sizeof open);
    if ((open & 0x00FFFFFFu) != 0x00c0b0a0u) {
        fprintf(stderr, "gpu coherence selftest [%s]: the control draw at (100,100) did not land: 0x%08x\n",
                name, open);
        ok = 0;
    }
    if ((seeded & 0x00FFFFFFu) != 0u) {
        fprintf(stderr, "gpu coherence selftest [%s]: the draw at (10,20) passed GREATER against the new "
                        "surface (colour 0x%08x): it was seeded from the CPU depth view loaded before the "
                        "eviction wrote 0x1111 there\n", name, seeded);
        ok = 0;
    }
    if (ok)
        printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n", name, stride, s_scale);
    coherence_reset_depths();
    return ok;
}

/* A target still bound to a depth surface that is evicted drops every framebuffer made over that
 * surface: the colour and depth pass's, the interlock path's depth-only one (#2109), and the host
 * twin's two. Target A draws with the first depth surface; target B then draws with four more, and
 * the fifth evicts slot 0 while A holds it. A framebuffer left on A would stand over destroyed
 * views, and A's next draw would make a new one over it without destroying it. A then draws again
 * with the surface in slot 0 and must have its framebuffers back. */
static int coherence_run_depth_evict_holder(void) {
    const char *name = "depth-evict-holder";
    static const uint32_t zb[5] = { 0x04000000u, 0x04044000u, 0x04088000u, 0x040CC000u, 0x04110000u };
    const uint32_t col_a = 0x04154000u, col_b = 0x04198000u, stride = 512u;
    coherence_reset_depths();
    g_sr_vram_read_pending = 0;
    memset(SR_HOST(col_a), 0, stride * FB_H * 2u);
    memset(SR_HOST(col_b), 0, stride * FB_H * 2u);
    sr_gpu_vram_dirty(col_a, stride * FB_H * 2u);
    sr_gpu_vram_dirty(col_b, stride * FB_H * 2u);

    int ok = 1;
    Target *ta = NULL;
    int kept[4] = { 0, 0, 0, 0 };
    /* step 0: A with surface 0; steps 1-4: B with surfaces 1-4; step 5: A with surface 4 */
    for (int i = 0; ok && i < 6; i++) {
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = i == 0 || i == 5 ? col_a : col_b;
        s_ge->fbw = stride;
        s_ge->fbfmt = 0u;
        s_ge->zbp = zb[i < 5 ? i : 4];
        s_ge->zbw = stride;
        s_ge->scis_x2 = (int)FB_W - 1;
        s_ge->scis_y2 = (int)FB_H - 1;
        s_ge->maxz = 65535u;
        GeVtx p0 = { 10.0f, 10.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
        GeVtx p1 = p0;
        p1.x = 12.0f;
        p1.y = 12.0f;
        ok = hook_sprite(&p0, &p1, 0) && s_nbatch && submit_pending();
        if (ok && i == 0) {
            ta = target_find_by_fba(col_a & 0x00FFFFFFu);
            ok = ta && ta->dep == &s_deps[0] && ta->fb;
        }
        if (ok && i == 4) {
            /* the fifth surface took slot 0, which A held */
            ok = s_deps[0].used && s_deps[0].zba == (zb[4] & 0x00FFFFFFu) && ta->used && !ta->dep;
            kept[0] = ta->fb != VK_NULL_HANDLE; kept[1] = ta->fb_rmw != VK_NULL_HANDLE;
            kept[2] = ta->tfb != VK_NULL_HANDLE; kept[3] = ta->tfb_rmw != VK_NULL_HANDLE;
        }
    }
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [%s]: setup failed (slot 0 zba=0x%06x, target A %s)\n", name,
                s_deps[0].zba, !ta ? "missing" : ta->dep ? "still bound to a depth surface" : "unbound");
        coherence_reset_depths();
        return 0;
    }
    if (kept[0] || kept[1] || kept[2] || kept[3]) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the target that held the evicted depth surface kept "
                        "a framebuffer over it (colour and depth %d, interlock depth-only %d, twin %d, twin "
                        "interlock depth-only %d; want none)\n", name, kept[0], kept[1], kept[2], kept[3]);
        ok = 0;
    }
    /* A drew again: bound to slot 0, with every framebuffer its paths use */
    const int want_rmw = s_dst_interlock && ta->rmw_set != VK_NULL_HANDLE;
    const int want_twin = s_twin && ta->timg != VK_NULL_HANDLE;
    if (ta->dep != &s_deps[0] || !ta->fb || (want_rmw && !ta->fb_rmw) || (want_twin && !ta->tfb) ||
        (want_twin && want_rmw && !ta->tfb_rmw)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the target's next draw did not make its "
                        "framebuffers again (colour and depth %d, interlock depth-only %d of %d, twin %d of "
                        "%d, twin interlock depth-only %d)\n", name, ta->fb != VK_NULL_HANDLE,
                ta->fb_rmw != VK_NULL_HANDLE, want_rmw, ta->tfb != VK_NULL_HANDLE, want_twin,
                ta->tfb_rmw != VK_NULL_HANDLE);
        ok = 0;
    }
    if (ok)
        printf("gpu coherence selftest: PASS %-22s the holder dropped %s and made them again\n", name,
               want_rmw ? (want_twin ? "its four framebuffers" : "both its framebuffers")
                        : (want_twin ? "its framebuffer and its twin's" : "its framebuffer"));
    coherence_reset_depths();
    return ok;
}

/* ---- the host twin: Enhanced 32-bit colour (#2050) -------------------------------------------
 *
 * The twin is decided at init (`--twin 1|native`, which also turns Ace Mode on: the twin is an
 * Ace feature), and the authentic chain and the twin both run at the requested scale. The twin
 * cases compare runs: a twin-off run writes a reference file (`--twin-ref`) and a twin-on run at
 * the same scale checks against it.
 *
 * twin-scene, one scene through ge.c's list interpreter and the production hooks: a dithered
 * horizontal gradient on a 5551 target (T1), a gradient under a shader-blended translucent
 * overlay (16-bit blends run in the shader, so this reads the destination snapshot), an 8888
 * target (T2) that samples T1 as a texture (a direct render-target bind), and a third 5551
 * target (T3) that takes a GPU block transfer of T1's gradient.
 *   - Twin off: the shown image is the authentic one, and T1's gradient is dithered on the 5551
 *     lattice. The run writes its scale and the hashes of guest VRAM and the shown images.
 *   - Twin on, any colour: guest VRAM equals the twin-off run at the same scale (the twin is not
 *     guest-visible), no twin was ever behind its image (no re-seed), and twin passes ran.
 *   - Twin on, native colour: the shown images equal the twin-off run's at the same scale too.
 *   - Twin on, full colour: the shown gradients are smooth (every row rises and takes far more
 *     than the lattice's 32 levels), T2's twin holds T1's twin (the twin's pass samples twins),
 *     and T3's twin holds T1's twin rows (the transfer reached the twins).
 * Built with -DSR_TWIN_SELFTEST_NATIVE_MUTANT, the full-colour twin keeps the 16-bit colour snap
 * (twin_pc): the full-colour checks must then fail.
 *
 * Identity after every case (`--twin native` at scale 1, where the twin's scale equals the
 * authentic one): coherence_reset_targets first compares every in-step twin with its authentic
 * image, colour and depth. A twin found behind, or any re-seed in the suite, fails it. Built with
 * one -DSR_TWIN_SELFTEST_SKIP_SITE_<site> (the write happens, its mirror and count do not), the
 * suite must fail for that site.
 *
 * twin-sync (`--twin native`, scale 1): a write the twin does not take (wgen bumped by hand) makes
 * the next present show the authentic image and re-seed the twin once; the present after it
 * shows the twin again, equal to the image. Two sub-cells then run a GPU block transfer from T1 to
 * T3 with T3's twin behind (a mirrored write must not bring it in step: twin_note_write's `in_step`)
 * and with T1's twin behind (the twins must not repeat the copy: hook_xfer's `src->tgen == src->wgen`).
 *
 * twin-drain-fail: as twin-alloc-fail with the drain of twin_off_for_run failing too
 * (s_twin_inject_drain_fail): the twin goes off and nothing is freed. */

#define TWIN_T1 0x00100000u                /* 5551, 512 x 272 x 2 bytes */
#define TWIN_T2 0x00150000u                /* 8888 */
#define TWIN_T3 0x00060000u                /* 5551, the block transfer's destination */
#define TWIN_VTX 0x00102000u               /* the vertices, after the list */
#define TWIN_LIST 0x00101000u              /* the display list */

typedef struct { uint32_t rgba; float x, y, z; } TwinCVtx;          /* colour, position */
typedef struct { float u, v; uint32_t rgba; float x, y, z; } TwinTVtx;   /* uv, colour, position */

static uint32_t twin_list(void) {
    uint32_t *dl = (uint32_t *)SR_HOST(TWIN_LIST);
    TwinCVtx *cv = (TwinCVtx *)SR_HOST(TWIN_VTX);
    int p = 0;
    #define TWIN_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    /* rows 0..135: the gradient; rows 136..271: the gradient, then a white overlay at alpha 64 */
    const uint32_t lo = 0xFF302010u, hi = 0xFFC0D0E0u, white = 0x40FFFFFFu;
    const float y_mid = 136.0f, y_end = (float)FB_H;
    int n = 0;
    #define TWIN_RECT(c0, c1, y0, y1) do { \
        cv[n++] = (TwinCVtx){ c0, 0.0f, y0, 0.0f }; cv[n++] = (TwinCVtx){ c1, 480.0f, y0, 0.0f }; \
        cv[n++] = (TwinCVtx){ c1, 480.0f, y1, 0.0f }; cv[n++] = (TwinCVtx){ c0, 0.0f, y0, 0.0f }; \
        cv[n++] = (TwinCVtx){ c1, 480.0f, y1, 0.0f }; cv[n++] = (TwinCVtx){ c0, 0.0f, y1, 0.0f }; } while (0)
    TWIN_RECT(lo, hi, 0.0f, y_mid);
    TWIN_RECT(lo, hi, y_mid, y_end);
    TWIN_RECT(white, white, y_mid, y_end);
    #undef TWIN_RECT
    TwinTVtx *tv = (TwinTVtx *)(cv + n);
    tv[0] = (TwinTVtx){ 0.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f, 0.0f };
    tv[1] = (TwinTVtx){ 480.0f, (float)FB_H, 0xFFFFFFFFu, 480.0f, (float)FB_H, 0.0f };
    const uint32_t tv_addr = TWIN_VTX + (uint32_t)n * (uint32_t)sizeof(TwinCVtx);
    /* T3's first draw: a sprite over rows 200..271 (the transfer then fills rows 0..135) */
    TwinCVtx *sv = (TwinCVtx *)(tv + 2);
    sv[0] = (TwinCVtx){ 0xFF808080u, 0.0f, 200.0f, 0.0f };
    sv[1] = (TwinCVtx){ 0xFF808080u, 480.0f, (float)FB_H, 0.0f };
    const uint32_t sv_addr = tv_addr + 2u * (uint32_t)sizeof(TwinTVtx);

    TWIN_EMIT(0x10, 0);                                    /* BASE */
    TWIN_EMIT(0x9C, TWIN_T1);                              /* FBP */
    TWIN_EMIT(0x9D, 512u | (0x04u << 16));
    TWIN_EMIT(0xD2, 1);                                    /* 5551 */
    TWIN_EMIT(0x9E, 0x000000u);                            /* ZBP: VRAM 0, below the targets */
    TWIN_EMIT(0x9F, 512u | (0x04u << 16));
    TWIN_EMIT(0xD3, 0);                                    /* clear mode off */
    TWIN_EMIT(0x1D, 0); TWIN_EMIT(0x1E, 0); TWIN_EMIT(0x1F, 0);
    TWIN_EMIT(0x22, 0); TWIN_EMIT(0x23, 0); TWIN_EMIT(0x24, 0);   /* no alpha, depth, stencil test */
    TWIN_EMIT(0x25, 0); TWIN_EMIT(0x27, 0); TWIN_EMIT(0x28, 0);   /* no AA, colour test, logic op */
    TWIN_EMIT(0xE7, 1); TWIN_EMIT(0xE8, 0); TWIN_EMIT(0xE9, 0);   /* no depth write, no masks */
    TWIN_EMIT(0x4C, 0); TWIN_EMIT(0x4D, 0); TWIN_EMIT(0xD6, 0); TWIN_EMIT(0xD7, 0xFFFFu);
    TWIN_EMIT(0x15, 0); TWIN_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    TWIN_EMIT(0xD4, 0); TWIN_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    TWIN_EMIT(0x50, 1);                                    /* gouraud */
    /* the PSP's usual 4x4 matrix: -4 0 -3 1 / 2 -2 3 -1 / -3 1 -4 0 / 3 -1 2 -2 (4-bit fields) */
    TWIN_EMIT(0xE2, 0x1D0Cu); TWIN_EMIT(0xE3, 0xF3E2u); TWIN_EMIT(0xE4, 0x0C1Du); TWIN_EMIT(0xE5, 0xE2F3u);
    TWIN_EMIT(0x20, 1);                                    /* dither on */
    TWIN_EMIT(0x21, 0);                                    /* blend off */
    TWIN_EMIT(0x12, (7u << 2) | (3u << 7) | (1u << 23));   /* 8888 colour, float pos, through */
    TWIN_EMIT(0x01, TWIN_VTX);
    TWIN_EMIT(0x04, (3u << 16) | 12u);                     /* both gradients */
    TWIN_EMIT(0x21, 1);
    TWIN_EMIT(0xDF, 2u | (3u << 4));                       /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA, add */
    TWIN_EMIT(0x04, (3u << 16) | 6u);                      /* the overlay (the vertex pointer advanced) */
    /* the second target samples the first through a sprite, 1:1, nearest */
    TWIN_EMIT(0x9C, TWIN_T2);
    TWIN_EMIT(0x9D, 512u | (0x04u << 16));
    TWIN_EMIT(0xD2, 3);
    TWIN_EMIT(0x20, 0); TWIN_EMIT(0x21, 0);
    TWIN_EMIT(0x1E, 1);                                    /* texture on */
    TWIN_EMIT(0xA0, TWIN_T1);
    TWIN_EMIT(0xA8, 512u | (0x04u << 16));
    TWIN_EMIT(0xB8, 9u | (9u << 8));                       /* 512 x 512 */
    TWIN_EMIT(0xC0, 0); TWIN_EMIT(0xC8, 0);                /* UV mapping, level 0 */
    TWIN_EMIT(0xC2, 0); TWIN_EMIT(0xC3, 1);                /* linear, one level, 5551 */
    TWIN_EMIT(0xC6, 0); TWIN_EMIT(0xC7, 0x101u);           /* nearest, clamp */
    TWIN_EMIT(0xC9, 3u | 0x100u);                          /* replace, RGBA */
    TWIN_EMIT(0xCB, 0);
    TWIN_EMIT(0x12, 3u | (7u << 2) | (3u << 7) | (1u << 23));
    TWIN_EMIT(0x01, tv_addr);
    TWIN_EMIT(0x04, (6u << 16) | 2u);
    /* the third target: one untextured sprite, then a GPU block transfer of T1's rows 0..135 */
    TWIN_EMIT(0x9C, TWIN_T3);
    TWIN_EMIT(0x9D, 512u | (0x04u << 16));
    TWIN_EMIT(0xD2, 1);
    TWIN_EMIT(0x1E, 0);                                    /* texture off */
    TWIN_EMIT(0x12, (7u << 2) | (3u << 7) | (1u << 23));
    TWIN_EMIT(0x01, sv_addr);
    TWIN_EMIT(0x04, (6u << 16) | 2u);
    TWIN_EMIT(0xB2, TWIN_T1);                              /* source: T1, stride 512 */
    TWIN_EMIT(0xB3, 512u | (0x04u << 16));
    TWIN_EMIT(0xB4, TWIN_T3);                              /* destination: T3, stride 512 */
    TWIN_EMIT(0xB5, 512u | (0x04u << 16));
    TWIN_EMIT(0xEB, 0); TWIN_EMIT(0xEC, 0);
    TWIN_EMIT(0xEE, 479u | (135u << 10));                  /* 480 x 136 */
    TWIN_EMIT(0xEA, 0);                                    /* 16-bit */
    TWIN_EMIT(0x0F, 0);
    TWIN_EMIT(0x0C, 0);
    #undef TWIN_EMIT
    return TWIN_LIST;
}

/* Read `img` (w x h device pixels of `bpp` bytes) into `out`, through a buffer of its own: the
 * twin is larger than s_xfer when the authentic chain runs at 1x. */
static int twin_read_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout *layout,
                           VkImageLayout after, uint32_t bpp, uint32_t w, uint32_t h, void *out) {
    VkBuffer buf; VkDeviceMemory mem; void *map;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * bpp;
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &map)) return 0;
    int ok = 0;
    if ((!s_nbatch || submit_pending()) && cmd_begin()) {
        to_layout(s_cmd, img, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c = {0};
        c.imageSubresource.aspectMask = aspect;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = w; c.imageExtent.height = h; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(s_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        to_layout(s_cmd, img, aspect, layout, after);
        if (cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION)) {
            memcpy(out, map, (size_t)bytes);
            ok = 1;
        }
    }
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
    return ok;
}

static uint64_t twin_hash(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

/* The hash of the visible part (DRAW_W guest columns) of a w-wide device image. */
static uint64_t twin_hash_visible(const uint32_t *img, int scale) {
    const uint32_t w = (uint32_t)FB_W * (uint32_t)scale, h = (uint32_t)FB_H * (uint32_t)scale;
    uint64_t x = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y++) x = twin_hash(img + (size_t)y * w, (size_t)DRAW_W * scale * 4u, x);
    return x;
}

/* Identity of every in-step twin with its authentic image (colour and depth), when the two
 * chains have the same scale; a twin behind its image counts as a failure. Run before the
 * targets of a case are freed (coherence_reset_targets) and at the end of the suite. */
static unsigned s_twin_identity_failures = 0;
static int s_twin_identity_checking = 0;

static void twin_identity_check(const char *when) {
    if (!s_twin || s_twin_identity_checking) return;
    const int compare = s_tscale == s_scale && !s_twin_color;   /* else only the in-step check */
    s_twin_identity_checking = 1;
    const size_t px = (size_t)SCL_W * SCL_H;
    uint32_t *a = (uint32_t *)malloc(px * 4u), *b = (uint32_t *)malloc(px * 4u);
    if (!a || !b) { free(a); free(b); s_twin_identity_failures++; s_twin_identity_checking = 0; return; }
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || !t->gpu_valid || !t->timg) continue;
        if (t->tgen != t->wgen) {
            if (s_twin_identity_failures++ < 8) {
                fflush(stdout);
                fprintf(stderr, "gpu coherence selftest [twin identity]: %s (%s): twin of target 0x%06x is "
                        "behind (wgen %llu tgen %llu; last lag: %s)\n", when, s_twin_case, t->fba,
                        (unsigned long long)t->wgen, (unsigned long long)t->tgen,
                        s_twin_lag_cause ? s_twin_lag_cause : "none");
            }
            continue;
        }
        if (!compare) continue;
        if (!coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 4u, a) ||
            !twin_read_image(t->timg, VK_IMAGE_ASPECT_COLOR_BIT, &t->tlayout,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 4u, TSCL_W, TSCL_H, b)) {
            s_twin_identity_failures++;
            continue;
        }
        if (memcmp(a, b, px * 4u) != 0) {
            size_t k = 0, nd = 0;
            while (k < px && a[k] == b[k]) k++;
            for (size_t j = k; j < px; j++) nd += a[j] != b[j];
            if (s_twin_identity_failures++ < 8) {
                fflush(stdout);
                fprintf(stderr, "gpu coherence selftest [twin identity]: %s (%s): twin of target 0x%06x "
                        "differs in %zu pixels, first (%u,%u): image 0x%08x, twin 0x%08x (wgen %llu, tfb %d, "
                        "depth twin %d)\n", when, s_twin_case, t->fba, nd,
                        (unsigned)(k % SCL_W), (unsigned)(k / SCL_W), a[k], b[k],
                        (unsigned long long)t->wgen, t->tfb != VK_NULL_HANDLE,
                        t->dep && t->dep->timg != VK_NULL_HANDLE);
            }
        }
    }
    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *d = &s_deps[i];
        if (!d->used || !d->timg || d->cpu_dirty) continue;
        if (d->tgen != d->wgen) {
            if (s_twin_identity_failures++ < 8)
                fprintf(stderr, "gpu coherence selftest [twin identity]: %s: twin of depth 0x%06x is "
                        "behind (last lag: %s)\n", when, d->zba,
                        s_twin_lag_cause ? s_twin_lag_cause : "none");
            continue;
        }
        if (!compare) continue;
        uint16_t *da = (uint16_t *)a, *db = (uint16_t *)b;
        if (!coherence_read_device(d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, DEPTH_BPP, da) ||
            !twin_read_image(d->timg, VK_IMAGE_ASPECT_DEPTH_BIT, &d->tlayout,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, DEPTH_BPP, TSCL_W, TSCL_H, db)) {
            s_twin_identity_failures++;
            continue;
        }
        if (memcmp(da, db, px * DEPTH_BPP) != 0 && s_twin_identity_failures++ < 8)
            fprintf(stderr, "gpu coherence selftest [twin identity]: %s (%s): twin of depth 0x%06x differs\n",
                    when, s_twin_case, d->zba);
    }
    free(a); free(b);
    s_twin_identity_checking = 0;
}

typedef struct {
    int scale;                             /* the run's shown scale */
    uint64_t vram[3], shown[3];
} TwinRef;

typedef struct {
    uint8_t *vram[3];                      /* guest VRAM of T1, T2, T3 after the scene */
    uint32_t *shown[3];                    /* what the presenter would show for each */
    int shown_scale[3];
    int shown_twin[3];                     /* the presenter would show the twin */
    TwinRef ref;
} TwinRun;

static const uint32_t k_twin_fba[3] = { TWIN_T1, TWIN_T2, TWIN_T3 };
static const uint32_t k_twin_fmt[3] = { 1u, 3u, 1u };

static void twin_run_free(TwinRun *r) {
    for (int i = 0; i < 3; i++) { free(r->vram[i]); free(r->shown[i]); }
    memset(r, 0, sizeof(*r));
}

static int twin_run_scene(TwinRun *r) {
    coherence_reset_targets();
    coherence_reset_depths();
    for (int i = 0; i < 3; i++) {
        const uint32_t bytes = 512u * FB_H * (k_twin_fmt[i] == 3u ? 4u : 2u);
        memset(SR_HOST(0x04000000u | k_twin_fba[i]), 0, bytes);
        sr_gpu_vram_dirty(0x04000000u | k_twin_fba[i], bytes);
    }
    ge_set_gpu_hooks(&k_hooks);
    if (ge_run_list(twin_list(), 0) != 0) return 0;
    if (s_nbatch && !submit_pending()) return 0;
    for (int i = 0; i < 3; i++) {
        Target *t = target_find_by_fba(k_twin_fba[i]);
        if (!t || !t->gpu_valid) {
            fprintf(stderr, "gpu coherence selftest [twin-scene]: target %d is not on the GPU\n", i + 1);
            return 0;
        }
        const uint32_t bytes = 512u * FB_H * (k_twin_fmt[i] == 3u ? 4u : 2u);
        GeGpuFbDescriptor d = { .addr = 0x04000000u | k_twin_fba[i], .format = k_twin_fmt[i],
                                .stride = 512, .width = 480, .height = FB_H };
        if (gegpu_sync_guest_fb(&d) != GEGPU_SYNC_OK) return 0;
        r->vram[i] = (uint8_t *)malloc(bytes);
        if (!r->vram[i]) return 0;
        memcpy(r->vram[i], SR_HOST(0x04000000u | k_twin_fba[i]), bytes);
        r->ref.vram[i] = twin_hash(r->vram[i], bytes, 0xcbf29ce484222325ull);
        /* what gegpu_present hands the presenter (target_prepare_present's choice, and with
         * supersampling its resolve) */
        t->tshow = s_twin && t->timg && !s_twin_authentic_shown && t->tgen == t->wgen;
        if (t->tshow && s_ssaa > 1) {
            if (!cmd_begin()) return 0;
            ssaa_resolve(t);
            if (!cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION)) return 0;
        }
        r->shown_twin[i] = t->tshow;
        const int sc = present_scale(t);
        const uint32_t w = (uint32_t)FB_W * (uint32_t)sc, h = (uint32_t)FB_H * (uint32_t)sc;
        r->shown[i] = (uint32_t *)malloc((size_t)w * h * 4u);
        r->shown_scale[i] = sc;
        if (!r->shown[i]) return 0;
        if (!twin_read_image(present_image(t), VK_IMAGE_ASPECT_COLOR_BIT, present_layout(t),
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 4u, w, h, r->shown[i]))
            return 0;
        t->tshow = 0;
        r->ref.shown[i] = twin_hash_visible(r->shown[i], sc);
        const char *dump = getenv("SR_GPU_TWIN_DUMP");
        if (dump && dump[0]) {
            char path[512];
            snprintf(path, sizeof(path), "%s_t%d_s%d.raw", dump, i + 1, sc);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(r->shown[i], 4u, (size_t)w * h, f); fclose(f); }
        }
    }
    r->ref.scale = r->shown_scale[0];
    return 1;
}

/* Over device rows [y0, y1) and the visible columns: are all rows non-decreasing in every colour
 * channel, does every channel sit on the 5551 lattice, and how many red values are there? */
static void twin_gradient_stats(const uint32_t *img, int scale, uint32_t y0, uint32_t y1, int *monotonic,
                                int *distinct_red, int *on_5551) {
    uint8_t seen[256] = {0};
    *monotonic = 1; *on_5551 = 1; *distinct_red = 0;
    const uint32_t w = DRAW_W * (uint32_t)scale, stride = FB_W * (uint32_t)scale;
    for (uint32_t y = y0 * (uint32_t)scale; y < y1 * (uint32_t)scale; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = img[y * stride + x];
            for (int ch = 0; ch < 3; ch++) {
                uint32_t v = (c >> (8 * ch)) & 0xFFu;
                if (x && v < ((img[y * stride + x - 1] >> (8 * ch)) & 0xFFu)) *monotonic = 0;
                if (((v >> 3) << 3 | (v >> 5)) != v) *on_5551 = 0;
            }
            seen[c & 0xFFu] = 1;
        }
    }
    for (int i = 0; i < 256; i++) *distinct_red += seen[i];
}

/* Device rows [y0, y1) of two images of the same scale, visible columns. */
static int twin_rows_equal(const uint32_t *a, const uint32_t *b, int scale, uint32_t y0, uint32_t y1) {
    const uint32_t w = DRAW_W * (uint32_t)scale, stride = FB_W * (uint32_t)scale;
    for (uint32_t y = y0 * (uint32_t)scale; y < y1 * (uint32_t)scale; y++)
        if (memcmp(a + (size_t)y * stride, b + (size_t)y * stride, w * 4u) != 0) return 0;
    return 1;
}

/* T2's sprite samples T1 with nearest filtering, 1:1 in guest texels: the PSP samples at the
 * pixel centre, and nearest takes the centre of guest texel floor(u), which is device texel
 * s*floor(X/s) + s/2 of a twin at scale s. So T2's twin equals T1's twin read at those texels. */
static int twin_sampled_equal(const uint32_t *t2, const uint32_t *t1, int scale) {
    const uint32_t s = (uint32_t)scale, w = DRAW_W * s, stride = FB_W * s, h = FB_H * s;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t sy = s * (y / s) + s / 2u;
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t sx = s * (x / s) + s / 2u;
            if (t2[(size_t)y * stride + x] != t1[(size_t)sy * stride + sx]) {
                fprintf(stderr, "gpu coherence selftest [twin-scene]: T2 (%u,%u) = 0x%08x, T1 (%u,%u) = 0x%08x\n",
                        x, y, t2[(size_t)y * stride + x], sx, sy, t1[(size_t)sy * stride + sx]);
                return 0;
            }
        }
    }
    return 1;
}

static int twin_ref_write(const char *path, const TwinRef *r) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "%d", r->scale);
    for (int i = 0; i < 3; i++) fprintf(f, " %016llx %016llx", (unsigned long long)r->vram[i],
                                        (unsigned long long)r->shown[i]);
    fprintf(f, "\n");
    return fclose(f) == 0;
}

static int twin_ref_read(const char *path, TwinRef *r) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long v[6];
    int n = fscanf(f, "%d %llx %llx %llx %llx %llx %llx", &r->scale, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    fclose(f);
    if (n != 7) return 0;
    for (int i = 0; i < 3; i++) { r->vram[i] = v[2 * i]; r->shown[i] = v[2 * i + 1]; }
    return 1;
}

/* ---- the pipeline cache on disk (#2149) ------------------------------------------------
 *
 * Run by the recipe as three processes over one empty folder (`--pipeline-cache DIR`):
 *   pipeline-cache-cold    the cache starts empty; the blend matrix below creates its pipelines,
 *                          and the frame hash is written to DIR/cold-frames.txt. Shutdown saves.
 *   pipeline-cache-warm    the cache must start from that file; the same matrix must give the
 *                          same frames, byte for byte. Creation times of both runs are printed
 *                          (not asserted: a driver keeps a disk cache of its own).
 *   pipeline-cache-reject  re-opens the cache over a truncated, a foreign, a corrupt and a
 *                          changed-key file, and with the switch off or refused. Each start must
 *                          run without the file's data, and the run must still draw. */

static int pcache_set_env(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value ? value : "") == 0;
#else
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
#endif
}

/* Every blend state of the parity matrix on the Vulkan path, one guest frame each (about 970
 * draws over the pipelines they need). *hash: FNV-1a 64 of every frame the guest reads back. */
static int pcache_scene(uint32_t eq_end, uint64_t *hash) {
    const uint32_t base = 0x04000000u | 0x00140000u, vaddr = 0x00101000u;
    const uint32_t sprites = PARITY_GRID * PARITY_GRID;
    parity_vertices(vaddr, sprites);
    ge_set_gpu_hooks(&k_hooks);
    *hash = 0;
    for (uint32_t eq = 0; eq < eq_end; eq++)
        for (uint32_t sfp = 0; sfp <= 10; sfp++)
            for (uint32_t dfp = 0; dfp <= 10; dfp++) {
                const uint32_t list = parity_list(base, vaddr, sfp | (dfp << 4) | (eq << 8), PARITY_FIXA,
                                                  PARITY_FIXB, sprites);
                coherence_reset_targets();
                parity_fill_background(base);
                ge_run_list(list, 0);
                GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = 512u, .width = 480, .height = FB_H };
                if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) return 0;
                *hash = nk_pcache_fnv1a64(*hash, SR_HOST(base), FB_W * FB_H * 4u);
            }
    coherence_reset_targets();
    return 1;
}

/* DIR/cache/gegpu/cold-frames.txt: next to the cache file, in the folder the harness made the
 * data root. */
static int pcache_ref_path(char *out, size_t n) {
    if (!s_pcache_path[0] || snprintf(out, n, "%s", s_pcache_path) >= (int)n) return 0;
    char *cut = NULL;
    for (char *p = out; *p; p++)
        if (*p == '/' || *p == '\\') cut = p;
    if (!cut) return 0;
    const size_t at = (size_t)(cut - out) + 1u;
    return snprintf(out + at, n - at, "cold-frames.txt") < (int)(n - at);
}

static int coherence_run_pipeline_cache(int warm) {
    const char *name = warm ? "pipeline-cache-warm" : "pipeline-cache-cold";
    char ref[4200];
    if (!s_pcache || !pcache_ref_path(ref, sizeof ref)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the pipeline cache is not on (run with "
                        "--pipeline-cache DIR)\n", name);
        return 0;
    }
    if (s_pcache_loaded != warm) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the cache %s\n", name,
                warm ? "did not start from the cold run's file" : "started from a file: the folder was not empty");
        return 0;
    }
    const unsigned long creates0 = s_pcache_creates, through0 = s_pcache_through;
    const uint64_t ns0 = s_pcache_create_ns;
    uint64_t hash = 0;
    if (!pcache_scene(8u, &hash)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the scene did not run\n", name);
        return 0;
    }
    const unsigned long creates = s_pcache_creates - creates0;
    const double ms = (double)(s_pcache_create_ns - ns0) / 1e6;
    if (!creates) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the scene created no pipeline\n", name);
        return 0;
    }
    /* The pipelines went through the cache, on any driver: every create took the open cache's
     * handle (pipe_create counts the handle it passed). */
    const unsigned long through = s_pcache_through - through0;
    if (through != creates) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL %lu of %lu pipelines were created with the cache's "
                        "handle: the pipelines did not go through it\n", name, through, creates);
        return 0;
    }
    /* What the driver kept, where it keeps anything. A cold cache must grow past the header it
     * opened with. A driver whose cache did not grow at all, though every create above took its
     * handle, keeps no pipeline data (lavapipe returns only its header): that is the driver's
     * property, measured here, and the growth check is reported as not run, naming the driver. */
    const size_t held = pcache_data_size();
    char drv[300] = "an unnamed device";
    {
        Sdl3VkInfo vi;
        if (sdl3vk_get_vk(&vi) && vi.physical) {
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties((VkPhysicalDevice)vi.physical, &pp);
            snprintf(drv, sizeof drv, "%s (vendor 0x%04x, driver 0x%08x)", pp.deviceName, pp.vendorID,
                     pp.driverVersion);
        }
    }
    if (!warm && held == s_pcache_held_n) {
        printf("gpu coherence selftest: SKIP %s growth check (NOT_RUN): %s returned the same %zu bytes before "
               "and after %lu pipelines created through its cache: this driver keeps no pipeline data\n",
               name, drv, held, creates);
    } else if (!warm && held < s_pcache_held_n) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the cache holds %zu bytes after %lu pipelines, fewer "
                        "than the %zu it opened with\n", name, held, creates, s_pcache_held_n);
        return 0;
    }
    if (warm && (s_pcache_held_n < s_pcache_file_n || held < s_pcache_file_n)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the cache opened with %zu bytes and holds %zu after the "
                        "scene, against the file's %zu: the driver did not take the cold run's pipelines\n",
                name, s_pcache_held_n, held, s_pcache_file_n);
        return 0;
    }
    if (!warm) {
        FILE *f = nk_fopen_utf8(ref, "w");
        int ok = f && fprintf(f, "%016llx %lu %.3f\n", (unsigned long long)hash, creates, ms) > 0;
        if (f) ok = fclose(f) == 0 && ok;
        if (!ok) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL could not write cold-frames.txt\n", name);
            return 0;
        }
        printf("gpu coherence selftest: PASS %-22s %lu pipelines created in %.1f ms from an empty cache, all "
               "with its handle (%zu bytes, then %zu), frames %016llx\n", name, creates, ms, s_pcache_held_n,
               held, (unsigned long long)hash);
        return 1;
    }
    unsigned long long cold_hash = 0;
    unsigned long cold_creates = 0;
    double cold_ms = 0.0;
    FILE *f = nk_fopen_utf8(ref, "r");
    int got = f ? fscanf(f, "%llx %lu %lf", &cold_hash, &cold_creates, &cold_ms) : 0;
    if (f) fclose(f);
    if (got != 3) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL cold-frames.txt is missing or unreadable\n", name);
        return 0;
    }
    if (cold_hash != hash || cold_creates != creates) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL frames %016llx (%lu pipelines) against the cold "
                        "run's %016llx (%lu)\n", name, (unsigned long long)hash, creates, cold_hash, cold_creates);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s %lu pipelines created in %.1f ms from %zu cached bytes "
           "(cold: %.1f ms), frames identical to the cold run\n", name, creates, ms, s_pcache_loaded_n, cold_ms);
    return 1;
}

/* Destroys every pipeline the renderer holds, once the device is idle, so the next draws create
 * theirs again through whichever cache is open (or none). */
static void pcache_drop_pipelines(void) {
    if (s_nbatch) (void)submit_pending();
    (void)cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    (void)vkDeviceWaitIdle(s_dev);
    for (int i = 0; i < s_pipe_n; i++)
        if (s_pipes[i].pipe) vkDestroyPipeline(s_dev, s_pipes[i].pipe, NULL);
    s_pipe_n = 0;
}

/* One reopen of the cache over `file` (NULL: no file) with SR_GPU_PIPELINE_CACHE=`mode`. */
static int pcache_reopen_cell(const char *cell, VkPhysicalDevice pdev, const char *path, const uint8_t *file,
                              size_t n, const char *mode, int want_cache, int want_discard) {
    pcache_close(0);
    if (file) {
        const char *why = NULL;
        if (!nk_pcache_write_file(path, file, n, &why)) {
            fprintf(stderr, "gpu coherence selftest [pipeline-cache-reject]: FAIL %s: %s\n", cell, why);
            return 0;
        }
    }
    const unsigned long discards0 = s_pcache_discards;
    if (!pcache_set_env("SR_GPU_PIPELINE_CACHE", mode)) return 0;
    pcache_open(pdev);
    pcache_drop_pipelines();
    const unsigned long creates0 = s_pcache_creates;
    uint64_t hash = 0;
    const int drew = pcache_scene(1u, &hash) &&   /* the run goes on, with or without the cache, */
                     s_pcache_creates > creates0;      /* and creates its pipelines again */
    const int ok = drew && !s_pcache_loaded && (s_pcache != VK_NULL_HANDLE) == want_cache &&
                   s_pcache_discards - discards0 == (unsigned long)want_discard;
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [pipeline-cache-reject]: FAIL %s: drew %d, cache %s, loaded %d, "
                        "discarded %lu (want cache %s, %d discarded)\n", cell, drew,
                s_pcache ? "on" : "off", s_pcache_loaded, s_pcache_discards - discards0,
                want_cache ? "on" : "off", want_discard);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s %-24s the run went on %s, %lu pipelines created\n",
           "pipeline-cache-reject", cell, want_cache ? "with an empty cache" : "without a cache",
           s_pcache_creates - creates0);
    return 1;
}

static int coherence_run_pipeline_cache_reject(void) {
    const char *name = "pipeline-cache-reject";
    Sdl3VkInfo vi;
    if (!s_pcache || !sdl3vk_get_vk(&vi)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the pipeline cache is not on (run with "
                        "--pipeline-cache DIR)\n", name);
        return 0;
    }
    const VkPhysicalDevice pdev = (VkPhysicalDevice)vi.physical;
    char path[sizeof s_pcache_path];   /* pcache_open clears s_pcache_path when the switch is off */
    memcpy(path, s_pcache_path, sizeof path);
    /* A good file of this run's own: the driver's data after a small scene. */
    uint64_t hash = 0;
    size_t sz = 0;
    if (!pcache_scene(1u, &hash) || vkGetPipelineCacheData(s_dev, s_pcache, &sz, NULL) != VK_SUCCESS || sz < 32) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the driver gave no cache data\n", name);
        return 0;
    }
    uint8_t *data = (uint8_t *)malloc(sz);
    if (!data || vkGetPipelineCacheData(s_dev, s_pcache, &sz, data) != VK_SUCCESS) { free(data); return 0; }
    size_t n = 0;
    uint8_t *good = nk_pcache_pack(&s_pcache_key, data, sz, &n);
    NkPcacheKey other = s_pcache_key;
    other.driver_version += 1u;
    size_t on = 0;
    uint8_t *changed = nk_pcache_pack(&other, data, sz, &on);
    uint8_t *corrupt = good ? (uint8_t *)malloc(n) : NULL;
    static const uint8_t foreign[96] = { 'P', 'K', 3, 4 };   /* a ZIP header, then zeros */
    int ok = good && changed && corrupt;
    if (ok) {
        memcpy(corrupt, good, n);
        corrupt[n - 1] ^= 0x01u;
        const char *saved = getenv("SR_GPU_PIPELINE_CACHE");
        char keep[64];
        snprintf(keep, sizeof keep, "%s", saved ? saved : "on");
        ok = pcache_reopen_cell("truncated file", pdev, path, good, n - 1, "on", 1, 1) &&
             pcache_reopen_cell("foreign file", pdev, path, foreign, sizeof foreign, "on", 1, 1) &&
             pcache_reopen_cell("corrupt data", pdev, path, corrupt, n, "on", 1, 1) &&
             pcache_reopen_cell("another driver", pdev, path, changed, on, "on", 1, 1) &&
             pcache_reopen_cell("switch off", pdev, path, NULL, 0, "off", 0, 0) &&
             pcache_reopen_cell("switch refused", pdev, path, NULL, 0, "maybe", 0, 0);
        /* Back on over the good file: it loads again, so the cells above were each refused for
         * their own reason and not for something the folder or the device does every time. */
        if (ok) {
            pcache_close(0);
            const char *why = NULL;
            ok = nk_pcache_write_file(path, good, n, &why) && pcache_set_env("SR_GPU_PIPELINE_CACHE", keep);
            if (ok) pcache_open(pdev);
            if (!ok || !s_pcache_loaded || s_pcache_loaded_n != sz) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL the good file did not load again\n", name);
                ok = 0;
            } else {
                printf("gpu coherence selftest: PASS %-22s %-24s loaded again, %zu bytes\n", name, "good file", sz);
            }
        }
    }
    free(corrupt); free(changed); free(good); free(data);
    return ok;
}

static int coherence_run_twin_scene(void) {
    const char *name = "twin-scene";
    const char *ref_path = getenv("SR_GPU_TWIN_REF");
    /* A device that refuses the twin (gegpu_init says why) runs twin-off: report that as a SKIP,
     * and never let such a run write or check a reference meant for the twin. */
    if (s_twin_unavailable) {
        printf("gpu coherence selftest: SKIP %s: the host twin was requested (SR_GPU_TWIN_COLOR=%s) but this "
               "device refused it\n", name, getenv("SR_GPU_TWIN_COLOR"));
        return 1;
    }
    const unsigned long ops0 = s_cnt_twin_ops, reseed0 = s_cnt_twin_reseed;
    TwinRun run = {0};
    int ok = twin_run_scene(&run);
    if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: the scene did not run\n", name);
    if (ok && !s_twin) {
        int mono, distinct, lattice;
        twin_gradient_stats(run.shown[0], run.shown_scale[0], 0, 136, &mono, &distinct, &lattice);
        if (mono || !lattice || distinct > 32) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL the authentic gradient is not dithered "
                    "5551 (monotonic=%d lattice=%d red levels=%d)\n", name, mono, lattice, distinct);
            ok = 0;
        }
        if (ok && ref_path && !twin_ref_write(ref_path, &run.ref)) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL cannot write %s\n", name, ref_path);
            ok = 0;
        }
        if (ok)
            printf("gpu coherence selftest: PASS %s twin off scale=%d (reference %s)\n", name, s_scale,
                   ref_path ? ref_path : "not written");
    } else if (ok) {
        if (s_cnt_twin_ops == ops0 || s_cnt_twin_reseed != reseed0) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL twin ops=%lu re-seeds=%lu (want ops, no "
                    "re-seed)\n", name, s_cnt_twin_ops - ops0, s_cnt_twin_reseed - reseed0);
            ok = 0;
        }
        for (int i = 0; i < 3; i++) if (!run.shown_twin[i]) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL target %d shows its authentic image\n", name, i + 1);
            ok = 0;
        }
        TwinRef ref;
        int have_ref = ref_path && twin_ref_read(ref_path, &ref);
        if (ref_path && !have_ref) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL no reference in %s\n", name, ref_path);
            ok = 0;
        }
        const char *checked = "no reference";
        if (have_ref && ref.scale != s_scale) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL the reference was made at scale %d, this run "
                    "is at %d: the twin-on run is checked against the twin-off run at its own scale\n",
                    name, ref.scale, s_scale);
            ok = 0;
        } else if (have_ref) {
            checked = "guest VRAM equals the twin-off run at this scale";
            for (int i = 0; i < 3; i++) if (run.ref.vram[i] != ref.vram[i]) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL guest VRAM of target %d differs from "
                        "the twin-off run at scale %d\n", name, i + 1, s_scale);
                ok = 0;
            }
            /* With supersampling the shown image is an average of samples the twin-off run never
             * took (a gradient's, for one), so only guest VRAM is compared (ssaa-edge checks it). */
            if (!s_twin_color && s_ssaa == 1) {
                checked = "guest VRAM and the shown images equal the twin-off run at this scale";
                for (int i = 0; i < 3; i++) if (run.ref.shown[i] != ref.shown[i]) {
                    fprintf(stderr, "gpu coherence selftest [%s]: FAIL the native twin of target %d differs "
                            "from the twin-off image at scale %d\n", name, i + 1, s_scale);
                    ok = 0;
                }
            }
        }
        /* T2's twin samples T1's twin (the direct bind), nearest, at each guest texel's centre.
         * Exact at scale 1 only: above it, main's hook_sprite shifts sprite UVs by 0.5 - 0.5 /
         * scale of a texel (centre_shift), in both passes, so a device pixel can take the next
         * texel. #2104 removes that shift; whichever of the two lands second runs this at every
         * scale. Above 1x the full-colour check below shows that the pass sampled the twin. */
        if (s_scale == 1 && s_ssaa == 1 && !twin_sampled_equal(run.shown[1], run.shown[0], run.shown_scale[0])) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL T2's twin is not T1's twin sampled at its "
                    "texel centres (the twin's pass did not sample the twin)\n", name);
            ok = 0;
        }
        int mono = 0, distinct = 0, lattice = 0, omono = 0, odistinct = 0, olattice = 0;
        if (s_twin_color) {
            const int sc = run.shown_scale[0];
            twin_gradient_stats(run.shown[0], sc, 0, 136, &mono, &distinct, &lattice);
            if (!mono || lattice || distinct < 160) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL the shown gradient is not full colour "
                        "(monotonic=%d lattice=%d red levels=%d)\n", name, mono, lattice, distinct);
                ok = 0;
            }
            twin_gradient_stats(run.shown[0], sc, 136, FB_H, &omono, &odistinct, &olattice);
            if (!omono || olattice || odistinct < 120) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL the blended gradient is not full colour "
                        "(monotonic=%d lattice=%d red levels=%d)\n", name, omono, olattice, odistinct);
                ok = 0;
            }
            if (!twin_rows_equal(run.shown[2], run.shown[0], sc, 0, 136)) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL T3's twin rows are not T1's twin rows (the "
                        "block transfer did not reach the twins)\n", name);
                ok = 0;
            }
            /* T2's twin took T1's full-colour twin, not T1's dithered 5551 image. */
            int smono = 0, sdistinct = 0, slattice = 0;
            twin_gradient_stats(run.shown[1], sc, 0, 136, &smono, &sdistinct, &slattice);
            if (slattice || sdistinct <= 32) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL T2's twin is on the 5551 lattice (red levels=%d): "
                        "the twin's pass sampled the authentic T1\n", name, sdistinct);
                ok = 0;
            }
        }
        if (ok)
            printf("gpu coherence selftest: PASS %s twin %dx colour %s: %s; twin ops=%lu, no re-seed%s\n",
                   name, s_tscale, s_twin_color ? "full" : "native", checked, s_cnt_twin_ops - ops0,
                   s_twin_color ? "; shown gradients smooth" : "");
        if (ok && s_twin_color)
            printf("gpu coherence selftest: twin-scene levels: shown gradient %d red levels, blended %d\n",
                   distinct, odistinct);
    }
    twin_run_free(&run);
    coherence_reset_targets();
    coherence_reset_depths();
    return ok;
}

/* ---- supersampling (Ace ssaa=2|4, #2073, ENHANCEMENT_CONTRACT.md 10.21) -----------------------
 *
 * ssaa-edge, run with supersampling off and at 2x and 4x (`--ssaa 2|4`), at the render scales the
 * recipe names: a white triangle with a long diagonal edge and a thin sliver on a black 8888 target,
 * drawn through ge.c's list interpreter and the production hooks.
 *   - Guest VRAM after the scene: with SR_GPU_SSAA_REF the run without supersampling writes its
 *     scale and the hash of the target's guest bytes, and a supersampled run at the same scale must
 *     match them: supersampling changes nothing the guest reads.
 *   - The shown image (what gegpu_present hands the presenter, at the render scale): without
 *     supersampling every visible pixel is black or white; with it, pixels on the edges take values
 *     in between (the resolve's average), only within a device pixel and a half of an edge, and
 *     every pixel further away is exactly black or white. 4x gives more distinct grey levels than
 *     2x can (a 2x2 box has three between black and white).
 *
 * ssaa-limit: the size check (ssaa_choose) with limits forced small: an image side over the
 * device's largest and a memory budget each lower the factor or turn it off, with a message that
 * names the reason. With `--ssaa N --ssaa-budget-mb M --ssaa-expect F` the run's own decision at
 * init is checked too: the factor in effect, the Ace line's effective value and the twin's scale. */

#define SS_BASE (0x04000000u | 0x00160000u)
#define SS_VTX 0x00102000u
#define SS_LIST 0x00100800u
#define SS_WHITE 0xFFFFFFFFu
#define SS_BLACK 0xFF000000u

static unsigned long long s_ssaa_expect = 0;   /* --ssaa-expect: the factor init must choose (0 none) */

void gegpu_selftest_ssaa_limits(unsigned long long budget_bytes, unsigned max_dim, int expect) {
    s_ssaa_test_budget = budget_bytes;
    s_ssaa_test_dim = max_dim;
    s_ssaa_expect = expect > 0 ? (unsigned long long)expect : 0u;
}

/* Signed distance, in guest pixels, from (x, y) to the triangle's diagonal edge x / 480 + y / 272 = 1,
 * and to the sliver's long edges (two lines from (40, 200) to (440, 204) and (440, 212)). */
static double ss_edge_dist(double x, double y) {
    const double a = 1.0 / 480.0, b = 1.0 / 272.0;
    double d = fabs(a * x + b * y - 1.0) / sqrt(a * a + b * b);
    /* the sliver: a triangle (40,200) (440,204) (440,212) */
    static const double sv[3][2] = { { 40, 200 }, { 440, 204 }, { 440, 212 } };
    for (int e = 0; e < 3; e++) {
        const double x0 = sv[e][0], y0 = sv[e][1], x1 = sv[(e + 1) % 3][0], y1 = sv[(e + 1) % 3][1];
        const double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
        double t = ((x - x0) * dx + (y - y0) * dy) / len2;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        const double px = x0 + t * dx - x, py = y0 + t * dy - y, ds = sqrt(px * px + py * py);
        if (ds < d) d = ds;
    }
    return d;
}

static int coherence_run_ssaa_edge(void) {
    const char *name = "ssaa-edge";
    const char *ref_path = getenv("SR_GPU_SSAA_REF");
    if (s_twin_unavailable && g_ace_on && ace_effective()->ssaa > 1) {
        printf("gpu coherence selftest: SKIP %s: supersampling was asked for but this device refused the "
               "host twin\n", name);
        return 1;
    }
    coherence_reset_targets();
    for (uint32_t i = 0; i < 512u * FB_H; i++) ((uint32_t *)SR_HOST(SS_BASE))[i] = SS_BLACK;
    sr_gpu_vram_dirty(SS_BASE, 512u * FB_H * 4u);

    ThroughFloatVtx *v = (ThroughFloatVtx *)SR_HOST(SS_VTX);
    v[0] = (ThroughFloatVtx){ SS_WHITE, 0.0f, 0.0f, 0.0f };
    v[1] = (ThroughFloatVtx){ SS_WHITE, 480.0f, 0.0f, 0.0f };
    v[2] = (ThroughFloatVtx){ SS_WHITE, 0.0f, 272.0f, 0.0f };
    v[3] = (ThroughFloatVtx){ SS_WHITE, 40.0f, 200.0f, 0.0f };
    v[4] = (ThroughFloatVtx){ SS_WHITE, 440.0f, 204.0f, 0.0f };
    v[5] = (ThroughFloatVtx){ SS_WHITE, 440.0f, 212.0f, 0.0f };
    uint32_t *dl = (uint32_t *)SR_HOST(SS_LIST);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, SS_BASE & 0x00FFFFFFu);
    DL_EMIT(0x9D, 512u | ((SS_BASE & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);                                   /* 8888 */
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x1E, 0);                                   /* no texture */
    DL_EMIT(0x1D, 0);                                   /* no culling */
    DL_EMIT(0x21, 0);                                   /* no blend */
    DL_EMIT(0x22, 0);                                   /* no alpha test */
    DL_EMIT(0x23, 0);                                   /* no depth test */
    DL_EMIT(0xE7, 1);                                   /* no depth writes */
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | 479);
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0x12, (1u << 23) | (7u << 2) | (3u << 7)); /* through, 8888 colour, float pos */
    DL_EMIT(0x01, SS_VTX & 0x00FFFFFFu);
    DL_EMIT(0x04, (3u << 16) | 6u);                     /* two triangles */
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    ge_set_gpu_hooks(&k_hooks);
    int ok = ge_run_list(SS_LIST, 0) == 0;
    GeGpuFbDescriptor desc = { .addr = SS_BASE, .format = 3, .stride = 512u, .width = 480, .height = FB_H };
    if (ok && (s_nbatch && !submit_pending())) ok = 0;
    if (ok && gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) ok = 0;
    Target *t = ok ? target_find_by_fba(SS_BASE & 0x001FFFFFu) : NULL;
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the scene did not run on the GPU\n", name);
        coherence_reset_targets();
        return 0;
    }
    const uint64_t vram = twin_hash(SR_HOST(SS_BASE), (size_t)512u * FB_H * 4u, 0xcbf29ce484222325ull);
    /* guest VRAM: the triangle is there at all */
    if (((uint32_t *)SR_HOST(SS_BASE))[10 * 512 + 10] != SS_WHITE ||
        ((uint32_t *)SR_HOST(SS_BASE))[260 * 512 + 470] != SS_BLACK) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL guest VRAM does not hold the scene\n", name);
        ok = 0;
    }

    /* what gegpu_present hands the presenter: target_prepare_present, the production step, chooses the
     * image and resolves it */
    if (ok && !target_prepare_present(t)) ok = 0;
    const int sc = present_scale(t);
    const uint32_t w = (uint32_t)FB_W * (uint32_t)sc, h = (uint32_t)FB_H * (uint32_t)sc;
    uint32_t *img = (uint32_t *)malloc((size_t)w * h * 4u);
    if (!img || !ok || !twin_read_image(present_image(t), VK_IMAGE_ASPECT_COLOR_BIT, present_layout(t),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 4u, w, h, img)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the shown image could not be read\n", name);
        t->tshow = 0;
        free(img);
        coherence_reset_targets();
        return 0;
    }
    const int shown_twin = t->tshow;
    t->tshow = 0;
    if (s_ssaa > 1 && (!shown_twin || sc != s_scale || s_tscale != s_scale * s_ssaa)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL supersampling %dx shows %s at %dx (twin %dx, render "
                "scale %dx)\n", name, s_ssaa, shown_twin ? "the resolve" : "the authentic image", sc, s_tscale,
                s_scale);
        ok = 0;
    }
    unsigned long mid = 0, far_mid = 0, near_far_off = 0;
    uint8_t levels[256] = {0};
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < (uint32_t)DRAW_W * (uint32_t)sc; x++) {
            const uint32_t px = img[y * w + x];
            const int is_bw = px == SS_WHITE || px == SS_BLACK;
            const double d = ss_edge_dist(((double)x + 0.5) / sc, ((double)y + 0.5) / sc);
            const int near = d <= 1.5 / sc;
            if (!is_bw) {
                const uint8_t r = (uint8_t)(px & 0xFFu), g = (uint8_t)((px >> 8) & 0xFFu),
                              bl = (uint8_t)((px >> 16) & 0xFFu);
                mid++;
                if (r != g || g != bl || (px >> 24) != 0xFFu) near_far_off++;   /* grey, opaque */
                levels[r] = 1;
                if (!near) far_mid++;
            }
        }
    }
    unsigned distinct = 0;
    for (int i = 0; i < 256; i++) distinct += levels[i];
    if (s_ssaa == 1 && mid) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL without supersampling %lu shown pixels are neither "
                "black nor white\n", name, mid);
        ok = 0;
    }
    if (s_ssaa > 1) {
        const unsigned min_levels = s_ssaa == 4 ? 4u : 1u;
        if (mid < (unsigned long)DRAW_W * (unsigned long)sc / 2u || far_mid || near_far_off || distinct < min_levels) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL supersampling %dx: %lu filtered pixels (want at "
                    "least %u), %lu away from every edge (want 0), %lu not opaque grey (want 0), %u grey levels "
                    "(want at least %u)\n", name, s_ssaa, mid, DRAW_W * (unsigned)sc / 2u, far_mid, near_far_off,
                    distinct, min_levels);
            ok = 0;
        }
    }
    free(img);
    const char *checked = "no reference";
    if (ok && ref_path) {
        if (s_ssaa == 1) {
            FILE *f = fopen(ref_path, "w");
            if (!f || fprintf(f, "scale=%d vram=%016llx\n", s_scale, (unsigned long long)vram) < 0) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL cannot write %s\n", name, ref_path);
                ok = 0;
            }
            if (f) fclose(f);
            checked = "reference written";
        } else {
            FILE *f = fopen(ref_path, "r");
            int rs = 0;
            unsigned long long rv = 0;
            if (!f || fscanf(f, "scale=%d vram=%llx", &rs, &rv) != 2) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL no reference in %s\n", name, ref_path);
                ok = 0;
            } else if (rs != s_scale) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL the reference was made at scale %d, this run "
                        "is at %d\n", name, rs, s_scale);
                ok = 0;
            } else if (rv != vram) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL guest VRAM differs from the run without "
                        "supersampling at scale %d\n", name, s_scale);
                ok = 0;
            } else {
                checked = "guest VRAM equals the run without supersampling";
            }
            if (f) fclose(f);
        }
    }
    if (ok)
        printf("gpu coherence selftest: PASS %s ssaa=%d scale=%d: %lu filtered pixels, %u grey levels; %s\n",
               name, s_ssaa, s_scale, mid, distinct, checked);
    coherence_reset_targets();
    return ok;
}

/* ssaa-sprite: sprite-scaled's nearest one-texel-per-pixel sprites (on a texel edge, half and a quarter
 * of a texel off, flipped), drawn and presented through the production path. The PSP shows one texel
 * across each guest pixel, and above 1x every device pixel of it does too (sprite_nearest_axis), so
 * each guest pixel's samples are equal and their average is that texel: the shown image equals the
 * authentic device image at every pixel of the sprites, at 2x and 4x as without supersampling. At
 * render scale 1 the twin draws above 1x from its own vertices (s_tvcpu); drawing it from the
 * authentic ones, made for 1x, splits a quarter-off sprite's texels inside each guest pixel. */
static int coherence_run_ssaa_sprite(void) {
    const char *name = "ssaa-sprite";
    if (s_twin_unavailable && g_ace_on && ace_effective()->ssaa > 1) {
        printf("gpu coherence selftest: SKIP %s: supersampling was asked for but this device refused the "
               "host twin\n", name);
        return 1;
    }
    const uint32_t fba = 0x00160000u, base = 0x04000000u | fba, stride = 512u;
    const int y0 = 10;
    coherence_sprite_list(k_spr_cells, SPR_NEAREST_1TO1, y0, fba, stride);
    coherence_reset_targets();
    memset(SR_HOST(base), 0, (size_t)stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(COH_SAMPLE_LIST, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride, .width = 480, .height = FB_H };
    int ok = gegpu_sync_guest_fb(&desc) == GEGPU_SYNC_OK;
    Target *t = ok ? target_find_by_fba(fba) : NULL;
    const uint32_t s = (uint32_t)s_scale;
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    uint32_t *img = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    ok = ok && t && dev && img &&
         coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               4u, dev) &&
         target_prepare_present(t) && present_scale(t) == s_scale &&
         twin_read_image(present_image(t), VK_IMAGE_ASPECT_COLOR_BIT, present_layout(t),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 4u, SCL_W, SCL_H, img);
    const int shown_twin = t ? t->tshow : 0;
    if (t) t->tshow = 0;
    if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: FAIL the sprites could not be drawn or read\n", name);
    if (ok && s_ssaa > 1 && !shown_twin) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL supersampling %dx shows the authentic image\n", name,
                s_ssaa);
        ok = 0;
    }
    unsigned long differ = 0, px = 0;
    uint8_t levels[256] = {0};
    for (int c = 0; ok && c < SPR_NEAREST_1TO1; c++) {
        const SprCell *cell = &k_spr_cells[c];
        for (uint32_t dy = 0; dy < (uint32_t)cell->n * s; dy++)
            for (uint32_t dx = 0; dx < (uint32_t)cell->n * s; dx++) {
                const size_t at = ((size_t)y0 * s + dy) * SCL_W + (size_t)cell->x * s + dx;
                const uint32_t a = dev[at] & 0x00FFFFFFu, b = img[at] & 0x00FFFFFFu;
                px++;
                levels[a & 0xFFu] = 1;
                if (a != b && differ++ < 8)
                    fprintf(stderr, "gpu coherence selftest [%s/%s]: ssaa=%d scale=%u device pixel (%u,%u) shown "
                            "%06x, authentic %06x\n", name, cell->name, s_ssaa, s, dx, dy, b, a);
            }
    }
    unsigned distinct = 0;
    for (int i = 0; i < 256; i++) distinct += levels[i];
    if (ok && (differ || distinct < 8u)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL ssaa=%d scale=%u: %lu of %lu sprite pixels differ from "
                "the authentic image (want 0), %u texel values (want at least 8)\n", name, s_ssaa, s, differ, px,
                distinct);
        ok = 0;
    }
    free(dev);
    free(img);
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS %s ssaa=%d scale=%u: %lu sprite pixels shown as the authentic image, "
               "%u texel values\n", name, s_ssaa, s, px, distinct);
    return ok;
}

static int coherence_run_ssaa_limit(void) {
    const char *name = "ssaa-limit";
    int ok = 1;
    char why[320];
    struct { int want, scale; uint32_t dim; uint64_t budget; int expect; const char *says; } k_cells[] = {
        { 4, 1, 16384u, 1ull << 40, 4, "" },
        { 4, 4, 8192u, 1ull << 40, 4, "" },                     /* 8192 x 4352 fits a side of 8192 */
        { 4, 4, 4096u, 1ull << 40, 2, "and this GPU's largest is 4096" },
        { 2, 4, 2048u, 1ull << 40, 1, "2x at render scale 4x needs 4096x2176 images" },
        { 4, 1, 0, 0, 2, "MB of GPU memory, over the" },        /* budget: exactly what 2x needs */
        { 2, 4, 16384u, 1ull << 20, 1, "2x at render scale 4x needs about" },
        { 2, 2, 16384u, 0, 2, "" },                             /* budget: exactly what 2x needs */
        /* nothing fits: the reason names 4x's and then 2x's */
        { 4, 4, 2048u, 1ull << 40, 1, "largest is 2048; 2x at render scale 4x needs 4096x2176 images" },
    };
    for (unsigned i = 0; i < sizeof(k_cells) / sizeof(k_cells[0]); i++) {
        const uint32_t dim = k_cells[i].dim ? k_cells[i].dim : 16384u;
        const uint64_t budget = k_cells[i].budget ? k_cells[i].budget
                                                  : ssaa_bytes(k_cells[i].scale, 2, 2u);
        const int got = ssaa_choose(k_cells[i].want, k_cells[i].scale, dim, budget, 2u, why, sizeof(why));
        const int says = k_cells[i].says[0] ? strstr(why, k_cells[i].says) != NULL : why[0] == '\0';
        if (got != k_cells[i].expect || !says) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL cell %u: want %d at scale %d (side %u, budget %llu MB) "
                    "chose %d, want %d; message \"%s\", want \"%s\"\n", name, i, k_cells[i].want,
                    k_cells[i].scale, dim, (unsigned long long)(budget >> 20), got, k_cells[i].expect, why,
                    k_cells[i].says);
            ok = 0;
        }
    }
    /* The estimate grows with the factor's square (the resolve images at the render scale do not):
     * 4x holds more than three times what 2x does, and a 32-bit depth format more than 16-bit. */
    if (ssaa_bytes(4, 4, 2u) <= 3u * ssaa_bytes(4, 2, 2u) || ssaa_bytes(1, 2, 4u) <= ssaa_bytes(1, 2, 2u)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the size estimate does not grow with the factor and the "
                "depth format\n", name);
        ok = 0;
    }
    if (s_ssaa_expect) {
        const int eff = g_ace_on ? ace_effective()->ssaa : 0;
        const int want_eff = s_ssaa_expect > 1 ? (int)s_ssaa_expect : 0;
        if ((unsigned long long)s_ssaa != s_ssaa_expect || eff != want_eff || s_tscale != s_scale * s_ssaa ||
            (s_ssaa > 1) != (s_twin != 0)) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL init chose %d (Ace line's effective %d, twin %s at "
                    "%dx), want %llu\n", name, s_ssaa, eff, s_twin ? "on" : "off", s_tscale, s_ssaa_expect);
            ok = 0;
        }
        /* The Ace report says what the run got: the factor in effect (1 when unavailable, with the size
         * check's reason), and the request beside a lowered one. */
        static char json[ACE_REPORT_MAX];
        char want[64];
        const int req = g_ace_on ? ace_requested()->ssaa : 0;
        snprintf(want, sizeof(want), "\"ssaa\": %llu", s_ssaa_expect);
        int rep = g_ace_on && ace_report_json(json, sizeof(json)) > 0 && strstr(json, want) != NULL;
        if (rep && s_ssaa_expect > 1 && (int)s_ssaa_expect < req) {
            snprintf(want, sizeof(want), "\"ssaa_requested\": %d", req);
            rep = strstr(json, want) != NULL;
        }
        if (rep && s_ssaa_expect == 1)
            rep = strstr(json, "over the") != NULL || strstr(json, "largest is") != NULL;
        if (!rep) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL the Ace report does not say %llu of %d with its "
                    "reason:\n%s\n", name, s_ssaa_expect, req, g_ace_on ? json : "(Ace off)");
            ok = 0;
        }
    }
    if (ok)
        printf("gpu coherence selftest: PASS %s: every forced limit lowers the factor with its reason%s\n", name,
               s_ssaa_expect ? "; the run's own choice is the expected one" : "");
    return ok;
}

/* twin-gate: the switch's values and the Ace gate (ENHANCEMENT_CONTRACT.md 10.11). Only "1" and
 * "native" ask for the twin; unset, empty, "0" and "off" are off; anything else is refused and off,
 * never more permissive. With Ace Mode off (`--authentic`) the twin stays off whatever the value.
 * The run's own value must have decided s_twin exactly so, unless the device refused the twin. */
static int coherence_run_twin_gate(void) {
    const char *name = "twin-gate";
    static const struct { const char *v; int want; } k_vals[] = {
        { NULL, 0 }, { "", 0 }, { "0", 0 }, { "off", 0 }, { "1", 1 }, { "native", 2 },
        { "on", -1 }, { "yes", -1 }, { "true", -1 }, { "2", -1 }, { "Native", -1 }, { "1 ", -1 },
        { "OFF", -1 }, { "full", -1 },
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(k_vals) / sizeof(k_vals[0]); i++) {
        const int got = twin_mode_parse(k_vals[i].v);
        if (got != k_vals[i].want) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL SR_GPU_TWIN_COLOR=%s%s%s reads as %d, want %d\n",
                    name, k_vals[i].v ? "\"" : "", k_vals[i].v ? k_vals[i].v : "(unset)",
                    k_vals[i].v ? "\"" : "", got, k_vals[i].want);
            ok = 0;
        }
    }
    const char *v = getenv("SR_GPU_TWIN_COLOR");
    const int mode = twin_mode_parse(v);
    /* Supersampling (s_ssaa > 1) runs the twin too, in native colour unless the switch says 1. */
    const int want_on = g_ace_on && (mode > 0 || s_ssaa > 1);
    if (s_twin_unavailable) {
        printf("gpu coherence selftest: SKIP %s run check: this device refused the twin\n", name);
    } else if (s_twin != want_on || (want_on && s_twin_color != (mode == 1))) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL SR_GPU_TWIN_COLOR=%s with Ace Mode %s: twin %s, "
                "want %s\n", name, v ? v : "(unset)", g_ace_on ? "on" : "off", s_twin ? "on" : "off",
                want_on ? "on" : "off");
        ok = 0;
    }
    if (ok)
        printf("gpu coherence selftest: PASS %s value=%s ace=%s twin=%s\n", name, v ? v : "(unset)",
               g_ace_on ? "on" : "off", s_twin ? "on" : "off");
    return ok;
}

/* twin-alloc-fail (`--twin 1|native`, a twin-off reference at the same scale): the Nth twin
 * allocation of the scene fails (N = 1: T1's twin; 2: its depth twin; 3: their framebuffer; 4 and
 * 5: T2's twin and framebuffer, after T1's twin has drawn). Each time the twin must go off for the
 * run, once, and the scene must then equal the twin-off run exactly: guest VRAM and the shown
 * images, at the user's scale, never a lower one. The twin is made again between steps. With
 * supersampling asked for (`--ssaa`), the Ace report must then give `"ssaa": 1` (section 10.21). */
static int coherence_run_twin_alloc_fail(void) {
    const char *name = "twin-alloc-fail";
    if (s_twin_unavailable) {
        printf("gpu coherence selftest: SKIP %s: this device refused the twin\n", name);
        return 1;
    }
    TwinRef ref;
    const char *ref_path = getenv("SR_GPU_TWIN_REF");
    if (!s_twin || !ref_path || !twin_ref_read(ref_path, &ref) || ref.scale != s_scale) {
        fprintf(stderr, "gpu coherence selftest [%s]: needs --twin 1|native and a twin-off reference at "
                "scale %d\n", name, s_scale);
        return 0;
    }
    const int color = s_twin_color;
    int ok = 1;
    for (int n = 1; ok && n <= 5; n++) {
        coherence_reset_targets();
        coherence_reset_depths();
        if (!s_twin && !twin_init(color)) {
            fprintf(stderr, "gpu coherence selftest [%s]: the twin could not be made again\n", name);
            return 0;
        }
        const unsigned long fail0 = s_cnt_twin_alloc_fail;
        s_twin_inject_fail = n;
        TwinRun run = {0};
        int ran = twin_run_scene(&run);
        const int reached = s_twin_inject_fail == 0;
        s_twin_inject_fail = 0;
        if (!ran || !reached) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: %s\n", name, n,
                    !ran ? "the scene did not run" : "the scene never made that many twin allocations");
            ok = 0;
        } else if (s_twin || s_cnt_twin_alloc_fail != fail0 + 1) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: twin %s, failures counted %lu (want "
                    "off, 1)\n", name, n, s_twin ? "still on" : "off", s_cnt_twin_alloc_fail - fail0);
            ok = 0;
        } else {
            for (int i = 0; i < 3; i++) {
                if (run.shown_scale[i] != s_scale || run.ref.vram[i] != ref.vram[i] ||
                    run.ref.shown[i] != ref.shown[i]) {
                    fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: target %d (shown at %dx) differs "
                            "from the twin-off run at %dx (%s)\n", name, n, i + 1, run.shown_scale[i], s_scale,
                            run.ref.vram[i] != ref.vram[i] ? "guest VRAM" : "the shown image");
                    ok = 0;
                }
            }
            static char json[ACE_REPORT_MAX];
            if (ok && g_ace_on && ace_requested()->ssaa > 1 &&
                (ace_report_json(json, sizeof(json)) <= 0 || !strstr(json, "\"ssaa\": 1,"))) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: supersampling %dx was asked for and "
                        "the twin is off, but the Ace report does not give \"ssaa\": 1\n", name, n,
                        ace_requested()->ssaa);
                ok = 0;
            }
        }
        twin_run_free(&run);
    }
    coherence_reset_targets();
    coherence_reset_depths();
    if (ok)
        printf("gpu coherence selftest: PASS %s 5 steps colour %s scale=%d: the twin went off once each time and "
               "the scene equals the twin-off run\n", name, color ? "full" : "native", s_scale);
    return ok;
}

/* twin-drain-fail (`--twin 1|native`, a twin-off reference at the same scale): as twin-alloc-fail,
 * but twin_off_for_run's drain also fails (s_twin_inject_drain_fail: the device may be lost, work
 * in flight). Then the twin must go off for the run and be shown nowhere, record no further twin
 * pass, and free NOTHING at that point (the twin pool, the twin snapshot and every twin already made
 * stay for gegpu_shutdown, which waits for the device first), and the scene must still equal the
 * twin-off run exactly. The cell frees the leftovers itself between steps, as shutdown would. */
static int coherence_run_twin_drain_fail(void) {
    const char *name = "twin-drain-fail";
    if (s_twin_unavailable) {
        printf("gpu coherence selftest: SKIP %s: this device refused the twin\n", name);
        return 1;
    }
    TwinRef ref;
    const char *ref_path = getenv("SR_GPU_TWIN_REF");
    if (!s_twin || !ref_path || !twin_ref_read(ref_path, &ref) || ref.scale != s_scale) {
        fprintf(stderr, "gpu coherence selftest [%s]: needs --twin 1|native and a twin-off reference at "
                "scale %d\n", name, s_scale);
        return 0;
    }
    const int color = s_twin_color;
    int ok = 1;
    for (int n = 1; ok && n <= 5; n++) {
        coherence_reset_targets();
        coherence_reset_depths();
        if (!s_twin) {
            twin_release();                     /* what shutdown does for the previous step's leftovers */
            if (!twin_init(color)) {
                fprintf(stderr, "gpu coherence selftest [%s]: the twin could not be made again\n", name);
                return 0;
            }
        }
        const unsigned long fail0 = s_cnt_twin_alloc_fail;
        s_twin_inject_fail = n;
        s_twin_inject_drain_fail = 1;
        TwinRun run = {0};
        int ran = twin_run_scene(&run);
        const int reached = s_twin_inject_fail == 0;
        s_twin_inject_fail = 0;
        s_twin_inject_drain_fail = 0;
        if (!ran || !reached) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: %s\n", name, n,
                    !ran ? "the scene did not run" : "the scene never made that many twin allocations");
            ok = 0;
        } else if (s_twin || s_twin_color || s_cnt_twin_alloc_fail != fail0 + 1) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: twin %s, failures counted %lu (want "
                    "off, 1)\n", name, n, s_twin ? "still on" : "off", s_cnt_twin_alloc_fail - fail0);
            ok = 0;
        } else {
            /* nothing freed: the pool and the snapshot, and (after the first allocation) T1's twin */
            Target *t1 = target_find_by_fba(TWIN_T1);
            if (s_dpool_twin == VK_NULL_HANDLE || s_tsnapimg == VK_NULL_HANDLE ||
                (n >= 2 && (!t1 || t1->timg == VK_NULL_HANDLE))) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: the twins were freed after a "
                        "failed drain (pool %s, snapshot %s, T1's twin %s): the GPU may still be using "
                        "them\n", name, n, s_dpool_twin ? "kept" : "freed", s_tsnapimg ? "kept" : "freed",
                        t1 && t1->timg ? "kept" : "freed");
                ok = 0;
            }
            for (int i = 0; i < 3; i++) {
                Target *t = target_find_by_fba(k_twin_fba[i]);
                if (t && t->tshow) {
                    fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: target %d still shows a twin\n",
                            name, n, i + 1);
                    ok = 0;
                }
                if (run.shown_scale[i] != s_scale || run.ref.vram[i] != ref.vram[i] ||
                    run.ref.shown[i] != ref.shown[i]) {
                    fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: target %d (shown at %dx) differs "
                            "from the twin-off run at %dx (%s)\n", name, n, i + 1, run.shown_scale[i], s_scale,
                            run.ref.vram[i] != ref.vram[i] ? "guest VRAM" : "the shown image");
                    ok = 0;
                }
            }
            /* the scene again, over the targets that kept their twins: no twin pass, no twin copy */
            const unsigned long ops0 = s_cnt_twin_ops, copies0 = s_cnt_twin_copies;
            if (ok && (ge_run_list(twin_list(), 0) != 0 || (s_nbatch && !submit_pending()))) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: the scene did not run again\n",
                        name, n);
                ok = 0;
            } else if (ok && (s_cnt_twin_ops != ops0 || s_cnt_twin_copies != copies0)) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL step %d: twin passes +%lu, twin copies +%lu "
                        "after the twin went off (want 0, 0)\n", name, n, s_cnt_twin_ops - ops0,
                        s_cnt_twin_copies - copies0);
                ok = 0;
            }
        }
        twin_run_free(&run);
    }
    coherence_reset_targets();
    coherence_reset_depths();
    twin_release();                             /* the leftovers of the last step */
    if (ok)
        printf("gpu coherence selftest: PASS %s 5 steps colour %s scale=%d: the twin went off, nothing was "
               "freed, no twin pass ran after it and the scene equals the twin-off run\n", name,
               color ? "full" : "native", s_scale);
    return ok;
}

/* twin-sync's block-transfer sub-cells: a transfer from T1 to T3 (a one-command list through ge.c
 * and hook_xfer) while one of the two twins is behind its image (wgen bumped by hand). Both leave
 * the destination's twin behind, so the next present re-seeds it:
 *   dst_behind = 1: the source is in step, so the twins repeat the copy (twin_copy), but a copy
 *     over a twin that was already behind does not bring it in step (the `in_step` guard of
 *     twin_note_write): rows the copy does not cover are still missing from it.
 *   dst_behind = 0: the source's twin is behind, so the copy must NOT be repeated on the twins
 *     (hook_xfer's `src->tgen == src->wgen`): it would copy stale rows, and the destination's twin,
 *     which was in step, would then be marked in step. */
static int coherence_twin_sync_xfer(const char *name, int dst_behind) {
    const char *what = dst_behind ? "a transfer over a twin behind its image"
                                  : "a transfer from a twin behind its image";
    TwinRun run = {0};
    int ok = twin_run_scene(&run);
    twin_run_free(&run);
    Target *t1 = ok ? target_find_by_fba(TWIN_T1) : NULL, *t3 = ok ? target_find_by_fba(TWIN_T3) : NULL;
    if (!t1 || !t3 || !t1->timg || !t3->timg || t1->tgen != t1->wgen || t3->tgen != t3->wgen) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL %s: no twins in step to start from\n", name, what);
        return 0;
    }
    Target *lag = dst_behind ? t3 : t1;
    lag->wgen++;                                /* a write the twin did not take */
    s_twin_lag_cause = "twin-sync's simulated write";
    uint32_t *dl = (uint32_t *)SR_HOST(TWIN_LIST);
    int p = 0;
    #define TWIN_XF(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    TWIN_XF(0xB2, TWIN_T1); TWIN_XF(0xB3, 512u | (0x04u << 16));
    TWIN_XF(0xB4, TWIN_T3); TWIN_XF(0xB5, 512u | (0x04u << 16));
    TWIN_XF(0xEB, 0); TWIN_XF(0xEC, 0);
    TWIN_XF(0xEE, 479u | (135u << 10));        /* 480 x 136 */
    TWIN_XF(0xEA, 0);                           /* 16-bit */
    TWIN_XF(0x0F, 0); TWIN_XF(0x0C, 0);
    #undef TWIN_XF
    const unsigned long xfer0 = s_cnt_xferblit, copies0 = s_cnt_twin_copies;
    ge_set_gpu_hooks(&k_hooks);
    if (ge_run_list(TWIN_LIST, 0) != 0 || (s_nbatch && !submit_pending())) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL %s: the transfer list did not run\n", name, what);
        ok = 0;
    } else if (s_cnt_xferblit != xfer0 + 1) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL %s: hook_xfer did not take the transfer\n", name, what);
        ok = 0;
    } else if (s_cnt_twin_copies != copies0 + (dst_behind ? 1u : 0u)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL %s: the twins repeated the copy %lu times (want %u)\n",
                name, what, s_cnt_twin_copies - copies0, dst_behind ? 1u : 0u);
        ok = 0;
    } else if (t3->tgen == t3->wgen) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL %s: the destination's twin is marked in step "
                "(%s)\n", name, what, dst_behind ? "the in_step guard of twin_note_write: a write that did "
                "not cover a twin behind its image brought it in step" :
                "hook_xfer's src->tgen == src->wgen: the copy was repeated from a twin behind its image");
        ok = 0;
    }
    /* a present re-seeds each twin that is behind and shows the authentic image; then all is in step */
    for (int i = 0; ok && i < 2; i++) {
        Target *t = i ? t1 : t3;
        if (t->tgen == t->wgen) continue;
        if (!target_prepare_present(t) || t->tshow || t->tgen != t->wgen) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL %s: the present did not re-seed the twin of "
                    "target 0x%06x\n", name, what, t->fba);
            ok = 0;
        }
        t->tshow = 0;
    }
    if (!ok) {                                  /* keep the identity check of the next reset quiet */
        t1->tgen = t1->wgen; t3->tgen = t3->wgen;
    }
    return ok;
}

/* Fail-closed sync: see the section comment. */
static int coherence_run_twin_sync(void) {
    const char *name = "twin-sync";
    if (!s_twin || s_tscale != s_scale) {
        fprintf(stderr, "gpu coherence selftest [%s]: needs --twin native at scale 1\n", name);
        return 0;
    }
    TwinRun run = {0};
    int ok = twin_run_scene(&run);
    twin_run_free(&run);
    Target *t = ok ? target_find_by_fba(TWIN_T1) : NULL;
    if (!t || !t->timg) { fprintf(stderr, "gpu coherence selftest [%s]: no twin\n", name); ok = 0; }
    const unsigned long reseed0 = s_cnt_twin_reseed, auth0 = s_cnt_twin_present_auth;
    if (ok) {
        t->wgen++;                              /* a write the twin did not take */
        s_twin_lag_cause = "twin-sync's simulated write";
        ok = target_prepare_present(t);
        if (ok && (t->tshow || s_cnt_twin_present_auth != auth0 + 1 || s_cnt_twin_reseed != reseed0 + 1)) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL a twin behind its image: showed twin=%d, "
                    "authentic presents +%lu, re-seeds +%lu (want 0, 1, 1)\n", name, t->tshow,
                    s_cnt_twin_present_auth - auth0, s_cnt_twin_reseed - reseed0);
            ok = 0;
        }
        if (ok) ok = target_prepare_present(t);
        if (ok && (!t->tshow || s_cnt_twin_reseed != reseed0 + 1)) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL the present after the re-seed did not show "
                    "the twin\n", name);
            ok = 0;
        }
        t->tshow = 0;
        const unsigned failures0 = s_twin_identity_failures;
        twin_identity_check(name);
        if (s_twin_identity_failures != failures0) ok = 0;
    }
    /* the block-transfer sub-cells: the `in_step` guard of twin_note_write and hook_xfer's
     * `src->tgen == src->wgen` (each alone is pinned here; see coherence_twin_sync_xfer) */
    if (ok && !coherence_twin_sync_xfer(name, 1)) ok = 0;
    if (ok && !coherence_twin_sync_xfer(name, 0)) ok = 0;
    if (s_nbatch) (void)submit_pending();
    (void)cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    readback_poll();
    for (int i = 0; i < READBACK_FRAMES; i++)
        if (s_readback[i].pending) (void)readback_finish(&s_readback[i], 1, 0);
    /* this case's re-seed is expected: take it out of the suite's no-re-seed count */
    s_cnt_twin_reseed = reseed0;
    if (ok) printf("gpu coherence selftest: PASS %s a twin behind its image showed the authentic image, "
                   "was re-seeded once, then shown again; a block transfer over or from a twin behind "
                   "its image left the destination's twin behind\n", name);
    coherence_reset_targets();
    coherence_reset_depths();
    return ok;
}

/* ---- ordered destination reads (#2109) ----------------------------------------------------
 *
 * Shader-blend and stencil draws read the destination. On the snapshot path every primitive of a
 * batch reads one copy of the target taken before the batch, so a primitive drawn over an earlier
 * one of the same batch blends against the pre-batch pixels; ge.c blends it over what the earlier
 * one stored. The interlock path (op_rmw in ge_gpu.c) loads and stores the target inside a
 * pixel-ordered interlock instead. Each cell compares the backend's picture with ge.c's, from the
 * same hashed background. Axis-aligned sprites cover the same pixels under both raster rules, so
 * the comparison is exact.
 *
 * - On the interlock path (s_dst_interlock) the picture equals ge.c, the op took the interlock
 *   (s_cnt_rmw_ops) and made no snapshot copy.
 * - On the snapshot path the known hazard stays and is pinned: every differing pixel lies in the
 *   cell's overlap, at least one does, and no op took the interlock.
 * - With the host twin on (#2050, `--twin native|1`) the picture and the path are those of the
 *   twin-off run, and the twin's pass takes the path the op took. Every cell's native twin must
 *   equal its image (twin_identity_check). The pair cells (order, mask, alpha) also read the twin
 *   device pixel by device pixel: a native twin equals the image, and a full-colour twin holds the
 *   image's alpha and the blends at 8 bits per channel without dither, each sprite over what the
 *   one before it stored on the interlock path and over the background on the snapshot path
 *   (dro_twin_pair_check).
 *
 * dst-read-order (the failing-before cell): two overlapping translucent sprites in one primitive
 * command, so they share a batch, on a 5650 target with dither (any blend on a 16-bit target runs
 * in the shader) and on an 8888 target with two distinct FIX constants (a state fixed-function
 * Vulkan cannot express). The colours make one and two blends differ after the 5650 quantization.
 * dst-read-mask: the same with a colour write mask that keeps one channel (the per-channel
 * approximation: the RMW store merges that channel from the loaded destination).
 * dst-read-alpha: 5551 and 4444 targets, whose alpha (the stencil) a non-stencil draw keeps.
 * dst-read-mixed: an opaque sprite, the overlapping blended pair, then an opaque sprite over part of
 * the pair, in one op: fixed-function, RMW and fixed-function runs with a break between each.
 * dst-read-depth, -ctest, -zrange, -sfail (the failing-before cells of the rmwzfb rule's four
 * kills: the alpha test, the color test, a narrow depth range and a stencil-fail KEEP): a
 * depth-writing, depth-tested blend whose overlapping pair must blend in order, with a primitive
 * behind the first in the same batch and one the kill rejects, then an opaque sprite behind them
 * all. On the interlock path the op takes the interlock, the run owns depth in the shader, and
 * colour and depth equal ge.c; on the snapshot path depth equals ge.c and the hazard is pinned to
 * the overlap.
 * dst-read-feedback: a stencil state that reads its own writes, texturing from its own target. Its
 * second sprite copies what the first drew, so its batch must start from a destination copy that
 * holds the first sprite, and that copy is the texture: the op keeps the snapshot path.
 * dst-read-ztest: the blended pair under the depth test GEQUAL over a depth wall, with depth writes
 * off and on, and with the second sprite hidden by the first's depth write in the same batch: the
 * depth-tested translucent draw. On the interlock path colour and depth equal ge.c and the op took
 * the interlock; on the snapshot path depth equals ge.c and the hazard is pinned where it applies.
 * dst-read-pins: the snapshot path made no interlock object (device extension, features chain,
 * STORAGE_BIT, depth-only pass, variant module); the interlock path made all of them. */
#define DRO_LIST 0x08040000u
#define DRO_VERTS 0x08060000u
#define DRO_BASE (0x04000000u | 0x00140000u)

typedef struct DroState {
    const char *name;
    uint32_t fmt, dither, blend_mode, fixa, fixb, pmskc;
} DroState;

typedef struct DroBox { uint32_t x0, y0, x1, y1; } DroBox;

static int dro_in(const DroBox *b, uint32_t x, uint32_t y) {
    return x >= b->x0 && x < b->x1 && y >= b->y0 && y < b->y1;
}

/* The two blended sprites of every cell, and where they overlap. */
static const float k_dro_pair[2][5] = {
    { 40.0f, 40.0f, 120.0f, 100.0f, 0.0f },
    { 80.0f, 70.0f, 160.0f, 130.0f, 0.0f },
};
static const uint32_t k_dro_pair_rgba[2] = { 0x803050F0u, 0x80E0C020u };
static const DroBox k_dro_overlap = { 80u, 70u, 120u, 100u };

/* The common head of a dst-read list: framebuffer, depth buffer, scissor, every test off,
 * blending on with the state's mode and constants, through-mode 8888 colour + float position. */
static void dro_begin(StpList *l, const DroState *st) {
    l->dl = (uint32_t *)SR_HOST(DRO_LIST);
    l->p = 0;
    stp_emit(l, 0x10, 0x080000u);                         /* BASE: vertices in main RAM */
    stp_emit(l, 0x9C, DRO_BASE & 0x00FFFFFFu);
    stp_emit(l, 0x9D, 512u | ((DRO_BASE & 0xFF000000u) >> 8));
    stp_emit(l, 0x9E, 0);                                  /* depth at VRAM offset 0 */
    stp_emit(l, 0x9F, 512u | 0x040000u);
    stp_emit(l, 0xD2, st->fmt);
    stp_emit(l, 0xD3, 0);
    stp_emit(l, 0x1D, 0); stp_emit(l, 0x1E, 0); stp_emit(l, 0x1F, 0);
    stp_emit(l, 0x20, st->dither ? 1u : 0u);
    stp_emit(l, 0x22, 0); stp_emit(l, 0x23, 0); stp_emit(l, 0x24, 0);
    stp_emit(l, 0xE7, 1);                                  /* depth writes masked */
    stp_emit(l, 0xE8, st->pmskc); stp_emit(l, 0xE9, 0);
    stp_emit(l, 0x15, 0);
    stp_emit(l, 0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    stp_emit(l, 0xD4, 0);
    stp_emit(l, 0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    /* A dither matrix with every offset from -4 to 3 somewhere. */
    stp_emit(l, 0xE2, 0x1D0Cu); stp_emit(l, 0xE3, 0xF3E2u);
    stp_emit(l, 0xE4, 0x0C1Du); stp_emit(l, 0xE5, 0xE2F3u);
    stp_emit(l, 0x21, 1);
    stp_emit(l, 0xDF, st->blend_mode & 0xFFFu);
    stp_emit(l, 0xE0, st->fixa);
    stp_emit(l, 0xE1, st->fixb);
    stp_emit(l, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
}

/* `n` sprites in one primitive command, from *vaddr on. */
static void dro_sprites(StpList *l, uint32_t *vaddr, const float (*box)[5], const uint32_t *rgba,
                        uint32_t n) {
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(*vaddr);
    for (uint32_t i = 0; i < n; i++) {
        v[2 * i]     = (ParityGuestVtx){ rgba[i], box[i][0], box[i][1], box[i][4] };
        v[2 * i + 1] = (ParityGuestVtx){ rgba[i], box[i][2], box[i][3], box[i][4] };
    }
    stp_emit(l, 0x01, *vaddr & 0x00FFFFFFu);
    stp_emit(l, 0x04, (6u << 16) | (2u * n));
    *vaddr += 2u * n * (uint32_t)sizeof(ParityGuestVtx);
}

/* dst-read-order, -mask, -alpha: the blended pair alone. */
static uint32_t dro_pair_list(const DroState *st) {
    StpList l;
    uint32_t vaddr = DRO_VERTS;
    dro_begin(&l, st);
    dro_sprites(&l, &vaddr, k_dro_pair, k_dro_pair_rgba, 2u);
    stp_end(&l);
    return DRO_LIST;
}

/* dst-read-mixed: an opaque sprite under the pair, the pair, an opaque sprite over the pair's
 * right part (not over the overlap). */
static uint32_t dro_mixed_list(const DroState *st) {
    static const float under[1][5] = { { 20.0f, 60.0f, 100.0f, 140.0f, 0.0f } };
    static const float over[1][5] = { { 130.0f, 50.0f, 200.0f, 120.0f, 0.0f } };
    static const uint32_t under_rgba[1] = { 0xFF20A060u }, over_rgba[1] = { 0xFF6040C0u };
    StpList l;
    uint32_t vaddr = DRO_VERTS;
    dro_begin(&l, st);
    stp_emit(&l, 0x21, 0);
    dro_sprites(&l, &vaddr, under, under_rgba, 1u);
    stp_emit(&l, 0x21, 1);
    dro_sprites(&l, &vaddr, k_dro_pair, k_dro_pair_rgba, 2u);
    stp_emit(&l, 0x21, 0);
    dro_sprites(&l, &vaddr, over, over_rgba, 1u);
    stp_end(&l);
    return DRO_LIST;
}

/* dst-read-depth, -ctest, -zrange and -sfail, `kind` 0 to 3. Depth is cleared to 0 in clear mode
 * (for -sfail the stencil too, then the stencil of K's box to 1). Then one primitive command under
 * the depth test GEQUAL with depth writes on: the blended pair P1 and P2 at z 32768, which overlap
 * (k_drk_overlap, the ordered-blend hazard); P3 at z 25268, which lies behind P1 where they meet
 * (a depth test against depth the batch wrote itself); and K at z 47768, which the kind's test
 * rejects: 0 the alpha test GREATER 0x40 (K's alpha is 0x20), 1 the color test NOTEQUAL K's colour,
 * 2 the transform-mode depth range 0-40000, 3 the stencil test EQUAL 0 with a stencil-fail KEEP.
 * Then Q at z 25268 under the depth test LESS with depth writes off, in the same shader-depth run:
 * it passes over P1 and P2 and fails over P3 and the cleared depth, and writes none.
 * Last an opaque sprite at z 20000 under GEQUAL with depth writes off over all of them: where K
 * wrote depth anyway it would be hidden.
 * -zrange draws the command in transform mode, with identity matrices and a viewport that maps clip
 * x, y and z to 240 + 240 x, 136 - 136 y and 32768 + 30000 z: the boxes lie on 30 by 17 pixel
 * steps and the depths on 7500 steps, which those maps reach exactly. */
static const float k_drk_box[5][5] = {
    { 60.0f, 34.0f, 150.0f, 102.0f, 32768.0f },    /* P1 */
    { 120.0f, 68.0f, 210.0f, 136.0f, 32768.0f },   /* P2 */
    { 30.0f, 17.0f, 90.0f, 68.0f, 25268.0f },      /* P3 */
    { 240.0f, 34.0f, 300.0f, 102.0f, 47768.0f },   /* K */
    { 30.0f, 51.0f, 150.0f, 119.0f, 25268.0f },    /* Q */
};
static const uint32_t k_drk_rgba[5] = { 0x803050F0u, 0x80E0C020u, 0x8040C0E0u, 0x80608020u, 0x80A040C0u };
static const DroBox k_drk_overlap = { 120u, 68u, 150u, 102u };

static uint32_t dro_kill_list(const DroState *st, int kind) {
    static const float clear[1][5] = { { 0.0f, 0.0f, 480.0f, 272.0f, 0.0f } };
    static const float behind[1][5] = { { 30.0f, 17.0f, 330.0f, 153.0f, 20000.0f } };
    static const uint32_t clear_rgba[1] = { 0 }, stencil_rgba[1] = { 0xFF000000u };
    static const uint32_t behind_rgba[1] = { 0xFF20A060u };
    float box[5][5];
    uint32_t rgba[5];
    memcpy(box, k_drk_box, sizeof box);
    memcpy(rgba, k_drk_rgba, sizeof rgba);
    if (kind == 0) rgba[3] = (rgba[3] & 0x00FFFFFFu) | 0x20000000u;
    if (kind == 2) {
        for (int i = 0; i < 5; i++) {
            box[i][0] = (box[i][0] - 240.0f) / 240.0f; box[i][2] = (box[i][2] - 240.0f) / 240.0f;
            box[i][1] = (136.0f - box[i][1]) / 136.0f; box[i][3] = (136.0f - box[i][3]) / 136.0f;
            box[i][4] = (box[i][4] - 32768.0f) / 30000.0f;
        }
    }
    StpList l;
    uint32_t vaddr = DRO_VERTS;
    dro_begin(&l, st);
    stp_emit(&l, 0x17, 0);                                /* no lighting */
    stp_emit(&l, 0xD3, 1u | 0x400u | (kind == 3 ? 0x200u : 0u));   /* clear mode: depth (stencil) */
    dro_sprites(&l, &vaddr, clear, clear_rgba, 1u);
    if (kind == 3) {
        stp_emit(&l, 0xD3, 1u | 0x200u);                  /* K's box: stencil 1 */
        dro_sprites(&l, &vaddr, &k_drk_box[3], stencil_rgba, 1u);
    }
    stp_emit(&l, 0xD3, 0);
    stp_emit(&l, 0x23, 1);
    stp_emit(&l, 0xE7, 0);                                /* depth writes on */
    stp_emit(&l, 0xDE, 7);                                /* GEQUAL */
    if (kind == 0) {
        stp_emit(&l, 0x22, 1);
        stp_emit(&l, 0xDB, 6u | (0x40u << 8) | (0xFFu << 16));   /* alpha GREATER 0x40 */
    } else if (kind == 1) {
        stp_emit(&l, 0x27, 1);
        stp_emit(&l, 0xD8, 3u);                           /* color NOTEQUAL K's */
        stp_emit(&l, 0xD9, rgba[3] & 0x00FFFFFFu);
        stp_emit(&l, 0xDA, 0xFFFFFFu);
    } else if (kind == 2) {
        static const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        static const float ident44[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        stp_emit(&l, 0x3A, 0);                            /* world, view, projection: identity */
        for (int k = 0; k < 12; k++) stp_emit(&l, 0x3B, tfz_f24(ident43[k]));
        stp_emit(&l, 0x3C, 0);
        for (int k = 0; k < 12; k++) stp_emit(&l, 0x3D, tfz_f24(ident43[k]));
        stp_emit(&l, 0x3E, 0);
        for (int k = 0; k < 16; k++) stp_emit(&l, 0x3F, tfz_f24(ident44[k]));
        stp_emit(&l, 0x42, tfz_f24(240.0f));
        stp_emit(&l, 0x43, tfz_f24(-136.0f));
        stp_emit(&l, 0x44, tfz_f24(30000.0f));
        stp_emit(&l, 0x45, tfz_f24(2048.0f));
        stp_emit(&l, 0x46, tfz_f24(2048.0f));
        stp_emit(&l, 0x47, tfz_f24(32768.0f));
        stp_emit(&l, 0x4C, (2048u - 240u) << 4);
        stp_emit(&l, 0x4D, (2048u - 136u) << 4);
        stp_emit(&l, 0xD6, 0);
        stp_emit(&l, 0xD7, 40000u);                       /* depth range 0-40000 */
        stp_emit(&l, 0x12, (7u << 2) | (3u << 7));        /* transform mode */
    } else {
        stp_emit(&l, 0x24, 1);
        stp_emit(&l, 0xDC, 2u | (0u << 8) | (0xFFu << 16));      /* stencil EQUAL 0 */
        stp_emit(&l, 0xDD, 0);                            /* KEEP, KEEP, KEEP */
    }
    dro_sprites(&l, &vaddr, (const float (*)[5])box, rgba, 4u);
    stp_emit(&l, 0xE7, 1);                                /* Q: depth writes off, LESS */
    stp_emit(&l, 0xDE, 4);
    dro_sprites(&l, &vaddr, (const float (*)[5])&box[4], &rgba[4], 1u);
    stp_emit(&l, 0xDE, 7);                                /* GEQUAL again for the sprite behind */
    stp_emit(&l, 0x12, (7u << 2) | (3u << 7) | (1u << 23));   /* through mode */
    stp_emit(&l, 0x22, 0);
    stp_emit(&l, 0x24, 0);
    stp_emit(&l, 0x27, 0);
    stp_emit(&l, 0x21, 0);
    stp_emit(&l, 0xE7, 1);                                /* depth writes off */
    dro_sprites(&l, &vaddr, behind, behind_rgba, 1u);
    stp_end(&l);
    return DRO_LIST;
}

/* Pixels of the visible 480 columns where the backend's picture differs from ge.c's, how many of
 * them lie outside `box`, and how many of them still hold the background (stp_fill_background).
 * The first difference is printed. */
static unsigned dro_compare(const char *cell, const DroState *st, const uint8_t *gpu, const uint8_t *sw,
                            const DroBox *box, unsigned *outside, unsigned *kept) {
    const uint32_t bpp = st->fmt == 3u ? 4u : 2u;
    unsigned differing = 0;
    *outside = *kept = 0;
    for (uint32_t i = 0; i < FB_W * FB_H; i++) {
        uint32_t x = i % FB_W, y = i / FB_W;
        if (x >= 480u || !memcmp(gpu + i * bpp, sw + i * bpp, bpp)) continue;
        uint32_t g = 0, w = 0, bg = wrap_hash(0x3C1u + i * 40503u);
        memcpy(&g, gpu + i * bpp, bpp); memcpy(&w, sw + i * bpp, bpp);
        if (bpp == 2u) bg &= 0xFFFFu;
        if (!differing++)
            fprintf(stderr, "gpu coherence selftest [%s]: %s x=%u y=%u gpu=%0*x ge_c=%0*x\n",
                    cell, st->name, x, y, (int)(2 * bpp), g, (int)(2 * bpp), w);
        if (x < box->x0 || x >= box->x1 || y < box->y0 || y >= box->y1) (*outside)++;
        if (g == bg) (*kept)++;
    }
    return differing;
}

typedef struct DroRun {
    unsigned long long batches, copies, rmw_ops, rmw_batches, rmw_breaks, fallbacks, twin_rmw_ops, zruns;
} DroRun;

/* Set by a pair cell around dro_run_both in a twin run: the image and its twin, whole and at
 * device size, read after the backend's half. s_dro_twin_read says both were read and in step. */
static uint32_t *s_dro_img_px = NULL, *s_dro_twin_px = NULL;
static int s_dro_twin_read = 0;

static void dro_twin_capture(const char *state) {
    s_dro_twin_read = 0;
    if (!s_twin || !s_dro_img_px || !s_dro_twin_px) return;
    if (s_nbatch && !submit_pending()) return;
    Target *t = target_find_by_fba(DRO_BASE & 0x00FFFFFFu);
    if (!t || !t->timg || t->tgen != t->wgen) {
        fprintf(stderr, "gpu coherence selftest [dst-read]: %s: the target has no twin in step with it "
                        "(twin %d, wgen %llu, tgen %llu)\n", state, t && t->timg,
                t ? (unsigned long long)t->wgen : 0ull, t ? (unsigned long long)t->tgen : 0ull);
        return;
    }
    s_dro_twin_read = coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 4u, s_dro_img_px) &&
                      twin_read_image(t->timg, VK_IMAGE_ASPECT_COLOR_BIT, &t->tlayout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 4u, TSCL_W, TSCL_H,
                                      s_dro_twin_px);
}

/* Depth at VRAM offset 0, as the GE sees it (ge.c's mirror, or the backend's image read back). */
static void dro_depth(uint16_t *out) {
    const uint16_t *z = ge_zbuf_ptr();
    for (uint32_t y = 0; y < FB_H; y++)
        memcpy(out + (size_t)y * 480u, z + (size_t)y * ge_zbuf_stride(), 480u * 2u);
}

/* Run `list` through the production hooks and then through ge.c alone, from the same hashed
 * background, into gpu_out (and gpu_z) and, in guest VRAM, the software result (and sw_z). */
static int dro_run_both(const DroState *st, uint32_t list, uint8_t *gpu_out, uint16_t *gpu_z,
                        uint16_t *sw_z, DroRun *run) {
    const uint32_t bytes = FB_W * FB_H * (st->fmt == 3u ? 4u : 2u);
    coherence_reset_targets();
    stp_fill_background(DRO_BASE, st->fmt);
    const unsigned long long b0 = s_replay_stats.batches, c0 = s_replay_stats.snapshot_copies;
    const unsigned long long o0 = s_cnt_rmw_ops, r0 = s_cnt_rmw_batches, k0 = s_cnt_rmw_breaks;
    const unsigned long long f0 = s_replay_stats.fallback_prims, t0 = s_cnt_twin_rmw_ops;
    const unsigned long long z0 = s_cnt_rmw_zruns;
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list, 0);
    GeGpuFbDescriptor desc = { .addr = DRO_BASE, .format = st->fmt, .stride = 512u,
                               .width = 480, .height = FB_H };
    if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) return 0;
    memcpy(gpu_out, SR_HOST(DRO_BASE), bytes);
    if (gpu_z) {
        if (s_nbatch && !submit_pending()) return 0;
        for (int i = 0; i < MAX_DEP; i++)
            if (s_deps[i].used && s_deps[i].zba == 0u && !depth_to_cpu(&s_deps[i])) return 0;
        dro_depth(gpu_z);
    }
    dro_twin_capture(st->name);
    s_twin_case = st->name;   /* the reset below checks this run's twin against its image */
    run->twin_rmw_ops = s_cnt_twin_rmw_ops - t0;
    run->batches = s_replay_stats.batches - b0;
    run->copies = s_replay_stats.snapshot_copies - c0;
    run->rmw_ops = s_cnt_rmw_ops - o0;
    run->rmw_batches = s_cnt_rmw_batches - r0;
    run->rmw_breaks = s_cnt_rmw_breaks - k0;
    run->fallbacks = s_replay_stats.fallback_prims - f0;
    run->zruns = s_cnt_rmw_zruns - z0;
    coherence_reset_targets();
    stp_fill_background(DRO_BASE, st->fmt);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (sw_z) dro_depth(sw_z);
    return 1;
}

/* The path the cells expect: the interlock where the device was made for it, host twin or not. */
static int dro_ordered(void) { return s_dst_interlock; }

/* The end of a failure message in a twin run (the twin mutants' kill lines name it). */
static const char *dro_twin_note(void) { return s_twin ? " (host twin on)" : ""; }

/* The colour (0x00BBGGRR) the image holds for background pixel `i` of a `fmt` target: the hashed
 * guest pixel (stp_fill_background) expanded by bit replication. */
static uint32_t dro_bg_rgb(uint32_t fmt, uint32_t i) {
    uint32_t v = wrap_hash(0x3C1u + i * 40503u), r, g, b;
    if (fmt == 3u) return v & 0x00FFFFFFu;
    v &= 0xFFFFu;
    if (fmt == 0u) {
        r = v & 31u; g = (v >> 5) & 63u; b = (v >> 11) & 31u;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    } else if (fmt == 1u) {
        r = v & 31u; g = (v >> 5) & 31u; b = (v >> 10) & 31u;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    } else {
        r = (v & 15u) * 17u; g = ((v >> 4) & 15u) * 17u; b = ((v >> 8) & 15u) * 17u;
    }
    return r | (g << 8) | (b << 16);
}

/* One sprite of the pair (`src`, 0xAABBGGRR) blended over `dst` and stored over `old` under the
 * state's write mask, at 8 bits per channel: ge.c blend_chan's arithmetic, floor((2v + 1)(2f + 1)
 * / 1024) for each product, for the equation and factors the pair cells use (ADD; SRC_ALPHA,
 * ONE_MINUS_SRC_ALPHA, FIX) and whole-channel masks. Returns 0 for a state outside that set. */
static int dro_blend_rgb(const DroState *st, uint32_t src, uint32_t dst, uint32_t old, uint32_t *out) {
    const uint32_t sf = st->blend_mode & 15u, df = (st->blend_mode >> 4) & 15u, sa = src >> 24;
    if ((st->blend_mode >> 8) & 7u) return 0;
    uint32_t res = 0;
    for (uint32_t ch = 0; ch < 3u; ch++) {
        const uint32_t sh = 8u * ch, sv = (src >> sh) & 0xFFu, dv = (dst >> sh) & 0xFFu;
        const uint32_t mask = (st->pmskc >> sh) & 0xFFu;
        uint32_t fs, fd;
        if (sf == 2u) fs = sa; else if (sf == 3u) fs = 255u - sa;
        else if (sf == 10u) fs = (st->fixa >> sh) & 0xFFu; else return 0;
        if (df == 2u) fd = sa; else if (df == 3u) fd = 255u - sa;
        else if (df == 10u) fd = (st->fixb >> sh) & 0xFFu; else return 0;
        uint32_t c = (((2u * sv + 1u) * (2u * fs + 1u)) >> 10) + (((2u * dv + 1u) * (2u * fd + 1u)) >> 10);
        if (c > 255u) c = 255u;
        if (mask == 0xFFu) c = (old >> sh) & 0xFFu;
        else if (mask) return 0;
        res |= c << sh;
    }
    *out = res;
    return 1;
}

/* The pair cells' twin (see the section comment), after a dro_run_both that captured it. Returns 0
 * and prints the first difference when the twin is not the one the path gives; *in_order takes the
 * overlap's device pixels where the two paths' pictures differ. */
static int dro_twin_pair_check(const char *cell, const DroState *st, int ordered, unsigned *in_order) {
    *in_order = 0;
    if (!s_dro_twin_read) {
        fprintf(stderr, "gpu coherence selftest [%s]: %s: the host twin was not read\n", cell, st->name);
        return 0;
    }
    const uint32_t sc = (uint32_t)s_tscale;
    unsigned bad = 0, outside = 0;
    for (uint32_t y = 0; y < FB_H * sc; y++) {
        for (uint32_t x = 0; x < 480u * sc; x++) {
            const uint32_t gx = x / sc, gy = y / sc, i = gy * FB_W + gx;
            const uint32_t twin = s_dro_twin_px[(size_t)y * TSCL_W + x];
            /* the authentic device pixel this twin pixel lies in (the twin runs at a whole multiple of
             * the render scale with supersampling, #2073) */
            const uint32_t k = (uint32_t)(s_tscale / s_scale);
            const uint32_t img = s_dro_img_px[(size_t)(y / k) * SCL_W + x / k];
            uint32_t want = img;
            if (s_twin_color) {
                const uint32_t bg = dro_bg_rgb(st->fmt, i);
                uint32_t chain = bg, flat = bg;
                for (uint32_t k = 0; k < 2u; k++) {
                    if ((float)gx < k_dro_pair[k][0] || (float)gx >= k_dro_pair[k][2] ||
                        (float)gy < k_dro_pair[k][1] || (float)gy >= k_dro_pair[k][3]) continue;
                    if (!dro_blend_rgb(st, k_dro_pair_rgba[k], chain, chain, &chain) ||
                        !dro_blend_rgb(st, k_dro_pair_rgba[k], bg, flat, &flat)) {
                        fprintf(stderr, "gpu coherence selftest [%s]: %s: the full-colour twin model does "
                                        "not cover this state\n", cell, st->name);
                        return 0;
                    }
                }
                want = (ordered ? chain : flat) | (img & 0xFF000000u);
                if (chain != flat) (*in_order)++;
            }
            if (twin == want) continue;
            if (!bad++)
                fprintf(stderr, "gpu coherence selftest [%s]: %s host twin x=%u y=%u (device) twin=%08x "
                                "want=%08x image=%08x\n", cell, st->name, x, y, twin, want, img);
            if (!dro_in(&k_dro_overlap, gx, gy)) outside++;
        }
    }
    if (bad)
        fprintf(stderr, "gpu coherence selftest [%s]: %s host twin (%s colour) differs from %s in %u "
                        "device pixels (%u outside the overlap) on the %s path\n", cell, st->name,
                s_twin_color ? "full" : "native",
                !s_twin_color ? "its image" : ordered ? "the ordered 8-bit blend" : "the pre-batch 8-bit blend",
                bad, outside, ordered ? "interlock" : "snapshot");
    return !bad;
}

/* One overlap cell over its states: `list` builds each state's list, `rmw_batches` is how many
 * destination-reading batches its op holds, `min_breaks` the passes an interlock op records. */
static int dro_overlap_cell(const char *cell, const DroState *states, uint32_t nstates,
                            uint32_t (*list)(const DroState *), unsigned rmw_batches,
                            unsigned min_breaks) {
    uint8_t *gpu_out = (uint8_t *)malloc(FB_W * FB_H * 4u);
    if (!gpu_out) return 0;
    GeState saved = *s_ge;
    const int ordered = dro_ordered();
    /* the pair alone: the twin is read and checked too */
    const int twin_pair = s_twin && list == dro_pair_list;
    int ok = 1;
    unsigned pinned = 0, twin_in_order = 0;
    if (twin_pair) {
        s_dro_img_px = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
        s_dro_twin_px = (uint32_t *)malloc((size_t)TSCL_W * TSCL_H * 4u);
        if (!s_dro_img_px || !s_dro_twin_px) ok = 0;
    }
    for (uint32_t s = 0; s < nstates && ok; s++) {
        const DroState *st = &states[s];
        DroRun run;
        if (!dro_run_both(st, list(st), gpu_out, NULL, NULL, &run)) { ok = 0; break; }
        if (twin_pair) {
            unsigned in_order = 0;
            if (!dro_twin_pair_check(cell, st, ordered, &in_order)) ok = 0;
            twin_in_order += in_order;
        }
        if (s_twin && run.twin_rmw_ops != run.rmw_ops) {
            fprintf(stderr, "gpu coherence selftest [%s]: %s: %llu interlock ops, %llu of them recorded "
                            "into the host twin (want all)\n", cell, st->name, run.rmw_ops, run.twin_rmw_ops);
            ok = 0;
        }
        unsigned outside = 0, kept = 0;
        unsigned diff = dro_compare(cell, st, gpu_out, (const uint8_t *)SR_HOST(DRO_BASE),
                                    &k_dro_overlap, &outside, &kept);
        if (run.fallbacks) {
            fprintf(stderr, "gpu coherence selftest [%s]: %s took the software fallback for %llu "
                            "primitives\n", cell, st->name, run.fallbacks);
            ok = 0;
        }
        if (ordered) {
            if (diff && kept == diff) {
                fprintf(stderr, "gpu coherence selftest [%s]: %s on the interlock path: the RMW store had "
                                "no effect (%u pixels kept the background)%s\n", cell, st->name, diff,
                        dro_twin_note());
                ok = 0;
            } else if (diff) {
                fprintf(stderr, "gpu coherence selftest [%s]: %s differs from ge.c in %u pixels (%u "
                                "outside the overlap) on the interlock path%s\n", cell, st->name, diff, outside,
                        dro_twin_note());
                ok = 0;
            }
            if (run.rmw_ops != 1u || run.rmw_batches != rmw_batches || run.copies != 0u ||
                run.rmw_breaks < min_breaks) {
                fprintf(stderr, "gpu coherence selftest [%s]: %s interlock ops %llu (want 1), RMW batches "
                                "%llu (want %u), snapshot copies %llu (want 0), breaks %llu (want >= %u)\n",
                        cell, st->name, run.rmw_ops, run.rmw_batches, rmw_batches, run.copies,
                        run.rmw_breaks, min_breaks);
                ok = 0;
            }
        } else {
            /* The known hazard (#2109): confined to the overlap, and present. */
            if (!diff || outside) {
                fprintf(stderr, "gpu coherence selftest [%s]: %s on the snapshot path differs from ge.c in "
                                "%u pixels, %u outside the overlap (want 1 or more, none outside)\n",
                        cell, st->name, diff, outside);
                ok = 0;
            }
            if (run.rmw_ops || !run.copies) {
                fprintf(stderr, "gpu coherence selftest [%s]: %s on the snapshot path took %llu interlock "
                                "ops and %llu snapshot copies (want 0 and 1 or more)\n",
                        cell, st->name, run.rmw_ops, run.copies);
                ok = 0;
            }
            pinned += diff;
        }
    }
    *s_ge = saved;
    coherence_reset_targets();
    free(gpu_out);
    free(s_dro_img_px); free(s_dro_twin_px);
    s_dro_img_px = s_dro_twin_px = NULL;
    s_dro_twin_read = 0;
    if (twin_pair && s_twin_color && twin_in_order == 0u) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the ordered and pre-batch 8-bit blends "
                        "never differ, so the full-colour twin check cannot tell the paths apart\n", cell);
        ok = 0;
    }
    char twin_note[160] = "";
    if (twin_pair && s_twin_color)
        snprintf(twin_note, sizeof twin_note, "; the full-colour twin holds the %s 8-bit blend (the two "
                 "differ in %u device pixels)", ordered ? "ordered" : "pre-batch", twin_in_order);
    else if (twin_pair)
        snprintf(twin_note, sizeof twin_note, "; the native twin equals the image");
    if (ok && ordered)
        printf("gpu coherence selftest: PASS %-22s interlock: equal to ge.c, no snapshot copy%s\n", cell,
               twin_note);
    else if (ok)
        printf("gpu coherence selftest: PASS %-22s snapshot path: the known #2109 hazard, %u pixels, "
               "all in the overlap%s\n", cell, pinned, twin_note);
    return ok;
}

static const DroState k_dro_order[2] = {
    { "5650+dither", 0u, 1u, 2u | (3u << 4), 0u, 0u, 0u },                 /* src alpha, 1 - src alpha */
    { "8888 dual-FIX", 3u, 0u, 10u | (10u << 4), 0x5F7F9Fu, 0x9F7F5Fu, 0u },
};
static const DroState k_dro_mask[2] = {
    { "5650 blue kept", 0u, 0u, 2u | (3u << 4), 0u, 0u, 0xFF0000u },
    { "8888 dual-FIX green kept", 3u, 0u, 10u | (10u << 4), 0x5F7F9Fu, 0x9F7F5Fu, 0x00FF00u },
};
static const DroState k_dro_alpha[2] = {
    { "5551", 1u, 0u, 2u | (3u << 4), 0u, 0u, 0u },
    { "4444", 2u, 0u, 2u | (3u << 4), 0u, 0u, 0u },
};
static const DroState k_dro_mixed[1] = {
    { "8888 dual-FIX", 3u, 0u, 10u | (10u << 4), 0x5F7F9Fu, 0x9F7F5Fu, 0u },
};

static int coherence_run_dst_read_order(void) {
    return dro_overlap_cell("dst-read-order", k_dro_order, 2u, dro_pair_list, 1u, 1u);
}
static int coherence_run_dst_read_mask(void) {
    return dro_overlap_cell("dst-read-mask", k_dro_mask, 2u, dro_pair_list, 1u, 1u);
}
static int coherence_run_dst_read_alpha(void) {
    return dro_overlap_cell("dst-read-alpha", k_dro_alpha, 2u, dro_pair_list, 1u, 1u);
}
static int coherence_run_dst_read_mixed(void) {
    return dro_overlap_cell("dst-read-mixed", k_dro_mixed, 1u, dro_mixed_list, 1u, 3u);
}

/* dst-read-depth, -ctest, -zrange, -sfail (dro_kill_list): `what` names the kill in the messages. */
static int dro_kill_cell(const char *cell, const DroState *stp, int kind, const char *what) {
    const DroState st = *stp;
    uint8_t *gpu_out = (uint8_t *)malloc(FB_W * FB_H * 4u);
    uint16_t *gz = (uint16_t *)malloc(480u * FB_H * 2u), *wz = (uint16_t *)malloc(480u * FB_H * 2u);
    int ok = gpu_out && gz && wz;
    const int ordered = dro_ordered();
    GeState saved = *s_ge;
    DroRun run = {0};
    unsigned diff = 0;
    if (ok && !dro_run_both(&st, dro_kill_list(&st, kind), gpu_out, gz, wz, &run)) ok = 0;
    if (ok) {
        unsigned outside = 0, kept = 0, zdiff = 0;
        diff = dro_compare(cell, &st, gpu_out, (const uint8_t *)SR_HOST(DRO_BASE), &k_drk_overlap,
                           &outside, &kept);
        for (uint32_t i = 0; i < 480u * FB_H; i++) {
            if (gz[i] == wz[i]) continue;
            if (!zdiff++)
                fprintf(stderr, "gpu coherence selftest [%s]: depth x=%u y=%u gpu=%u ge_c=%u\n", cell,
                        i % 480u, i / 480u, gz[i], wz[i]);
        }
        /* The interlock path equals ge.c; the snapshot path keeps the known hazard, in the
         * overlap only, and its depth equals ge.c. A stencil batch that blends never merges
         * overlapping primitives (stencil_reads_own_writes), so for -sfail the snapshot path
         * equals ge.c too, at the price of a copy per split batch. With depth=32 (s_depth32) the
         * interlock path keeps the depth-writing op on the snapshot path (rmw_refuses), as there. */
        const int split = kind == 3, zshd = ordered && !s_depth32;
        if (zdiff || (zshd || split ? diff != 0u : (!diff || outside))) {
            fprintf(stderr, "gpu coherence selftest [%s]: colour differs from ge.c in %u pixels, depth "
                            "differs from ge.c in %u pixels (%u colour pixels outside the overlap) on the %s "
                            "path%s\n", cell, diff, zdiff, outside,
                    ordered ? (s_depth32 ? "interlock (depth=32)" : "interlock") : "snapshot", dro_twin_note());
            ok = 0;
        }
        if (ordered && s_depth32 && (run.copies < (split ? 2u : 1u) || run.zruns)) {
            fprintf(stderr, "gpu coherence selftest [%s]: with depth=32 the %s depth-writing op took %llu "
                            "snapshot copies (want %s) and %llu shader-depth runs (want 0)\n", cell, what,
                    run.copies, split ? "2 or more" : "1 or more", run.zruns);
            ok = 0;
        }
        if (zshd && (run.rmw_ops < 1u || run.copies || run.zruns < 1u)) {
            fprintf(stderr, "gpu coherence selftest [%s]: the %s depth-writing op took %llu interlock ops "
                            "(want 1 or more), %llu snapshot copies (want 0), %llu shader-depth runs (want 1 "
                            "or more)\n", cell, what, run.rmw_ops, run.copies, run.zruns);
            ok = 0;
        }
        if (!ordered && (run.rmw_ops || run.copies < (split ? 2u : 1u))) {
            fprintf(stderr, "gpu coherence selftest [%s]: on the snapshot path the %s op took %llu interlock "
                            "ops and %llu snapshot copies (want 0 and %s)\n", cell, what, run.rmw_ops,
                    run.copies, split ? "2 or more" : "1 or more");
            ok = 0;
        }
        if (s_twin && run.twin_rmw_ops != run.rmw_ops) {
            fprintf(stderr, "gpu coherence selftest [%s]: %llu interlock ops, %llu of them recorded into the "
                            "host twin (want all)\n", cell, run.rmw_ops, run.twin_rmw_ops);
            ok = 0;
        }
        if (run.fallbacks) {
            fprintf(stderr, "gpu coherence selftest [%s]: %llu primitives took the software "
                            "fallback\n", cell, run.fallbacks);
            ok = 0;
        }
    }
    *s_ge = saved;
    coherence_reset_targets();
    free(gpu_out); free(gz); free(wz);
    if (ok && ordered && s_depth32)
        printf("gpu coherence selftest: PASS %-22s interlock with depth=32: the depth-writing op kept the "
               "snapshot path (%llu copies, %u pixels of the known hazard, all in the overlap)\n", cell,
               run.copies, diff);
    else if (ok && ordered)
        printf("gpu coherence selftest: PASS %-22s interlock, depth in the shader (%llu runs): colour and "
               "depth equal ge.c\n", cell, run.zruns);
    else if (ok && kind == 3)
        printf("gpu coherence selftest: PASS %-22s snapshot path: colour and depth equal ge.c; the stencil "
               "batch split at the overlaps (%llu snapshot copies)\n", cell, run.copies);
    else if (ok)
        printf("gpu coherence selftest: PASS %-22s snapshot path: depth equals ge.c; the known #2109 hazard, "
               "%u pixels, all in the overlap\n", cell, diff);
    return ok;
}

static int coherence_run_dst_read_depth(void) {
    static const DroState st = { "5650 alpha-tested", 0u, 0u, 2u | (3u << 4), 0u, 0u, 0u };
    return dro_kill_cell("dst-read-depth", &st, 0, "alpha-tested");
}

static int coherence_run_dst_read_ctest(void) {
    static const DroState st = { "5650 colour-tested", 0u, 0u, 2u | (3u << 4), 0u, 0u, 0u };
    return dro_kill_cell("dst-read-ctest", &st, 1, "colour-tested");
}

static int coherence_run_dst_read_zrange(void) {
    static const DroState st = { "5650 depth range", 0u, 0u, 2u | (3u << 4), 0u, 0u, 0u };
    return dro_kill_cell("dst-read-zrange", &st, 2, "depth-ranged");
}

static int coherence_run_dst_read_sfail(void) {
    static const DroState st = { "5551 stencil-fail KEEP", 1u, 0u, 2u | (3u << 4), 0u, 0u, 0u };
    return dro_kill_cell("dst-read-sfail", &st, 3, "stencil-tested");
}

typedef struct DroTexVtx { float u, v; uint32_t rgba; float x, y, z; } DroTexVtx;

/* dst-read-feedback: the draw-once stencil mask (NOTEQUAL against bit 7, REPLACE on depth pass, an
 * 8888 target) texturing from its own target, texel for texel (REPLACE, nearest). The first sprite
 * copies columns 0-15 to columns 16-31; the second, whose box meets the first's, copies columns
 * 16-31, the first sprite's output, to columns 32-47. */
static uint32_t dro_feedback_list(void) {
    StpList l;
    stp_begin(&l, DRO_BASE, 3u, 0);
    stp_emit(&l, 0xDC, 3u | (0x80u << 8) | (0x80u << 16));
    stp_emit(&l, 0xDD, 2u << 16);
    stp_emit(&l, 0xE9, 0);
    stp_emit(&l, 0x21, 0);
    stp_emit(&l, 0xA0, DRO_BASE & 0x00FFFFFFu);
    stp_emit(&l, 0xA8, 512u | ((DRO_BASE & 0xFF000000u) >> 8));
    stp_emit(&l, 0xB8, 9u | (9u << 8));
    stp_emit(&l, 0xC0, 0); stp_emit(&l, 0xC2, 0); stp_emit(&l, 0xC3, 3);
    stp_emit(&l, 0xC6, 0); stp_emit(&l, 0xC7, 0);
    stp_emit(&l, 0xC9, 3u);                                /* REPLACE, colour only */
    stp_emit(&l, 0xCB, 0);
    stp_emit(&l, 0x1E, 1);
    stp_emit(&l, 0x12, 3u | (7u << 2) | (3u << 7) | (1u << 23));
    DroTexVtx *v = (DroTexVtx *)SR_HOST(DRO_VERTS);
    v[0] = (DroTexVtx){ 0.0f, 40.0f, 0xFFFFFFFFu, 16.0f, 40.0f, 0.0f };
    v[1] = (DroTexVtx){ 16.0f, 72.0f, 0xFFFFFFFFu, 32.0f, 72.0f, 0.0f };
    v[2] = (DroTexVtx){ 16.0f, 40.0f, 0xFFFFFFFFu, 32.0f, 40.0f, 0.0f };
    v[3] = (DroTexVtx){ 32.0f, 72.0f, 0xFFFFFFFFu, 48.0f, 72.0f, 0.0f };
    stp_emit(&l, 0x01, DRO_VERTS & 0x00FFFFFFu);
    stp_emit(&l, 0x04, (6u << 16) | 4u);
    stp_emit(&l, 0x1E, 0);
    stp_emit(&l, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
    stp_end(&l);
    return DRO_LIST;
}

static int coherence_run_dst_read_feedback(void) {
    static const DroState st = { "8888 stencil, own target as texture", 3u, 0u, 0u, 0u, 0u, 0u };
    static const DroBox none = { 0u, 0u, 0u, 0u };
    uint8_t *gpu_out = (uint8_t *)malloc(FB_W * FB_H * 4u);
    int ok = gpu_out != NULL;
    GeState saved = *s_ge;
    DroRun run = {0};
    if (ok && !dro_run_both(&st, dro_feedback_list(), gpu_out, NULL, NULL, &run)) ok = 0;
    if (ok) {
        unsigned outside = 0, kept = 0, copied = 0;
        const unsigned diff = dro_compare("dst-read-feedback", &st, gpu_out, (const uint8_t *)SR_HOST(DRO_BASE),
                                          &none, &outside, &kept);
        const uint32_t *sw = (const uint32_t *)SR_HOST(DRO_BASE);
        for (uint32_t y = 40u; y < 72u; y++)
            for (uint32_t x = 32u; x < 48u; x++)
                if (sw[y * FB_W + x] != wrap_hash(0x3C1u + (y * FB_W + x) * 40503u)) copied++;
        if (diff || copied < 64u) {
            fprintf(stderr, "gpu coherence selftest [dst-read-feedback]: %u pixels differ from ge.c; ge.c's "
                            "second sprite changed %u pixels (want 0, and at least 64) -- the second sprite "
                            "must read what the first one drew\n", diff, copied);
            ok = 0;
        }
        if (run.rmw_ops) {
            fprintf(stderr, "gpu coherence selftest [dst-read-feedback]: the op took the interlock (%llu ops); "
                            "an op whose destination-reading batch samples its own target keeps the snapshot "
                            "path\n", run.rmw_ops);
            ok = 0;
        }
        if (run.fallbacks) {
            fprintf(stderr, "gpu coherence selftest [dst-read-feedback]: %llu primitives took the software "
                            "fallback\n", run.fallbacks);
            ok = 0;
        }
    }
    *s_ge = saved;
    coherence_reset_targets();
    free(gpu_out);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s equal to ge.c; the op kept the snapshot\n", "dst-read-feedback");
    return ok;
}

/* dst-read-ztest: depth cleared to 0, then a depth-only clear-mode wall at z 25000 across part of
 * the overlap; then the blended pair under the depth test GEQUAL, the first sprite at z 20000 (hidden
 * behind the wall). Every kill in that op is the depth test itself, so the rmwzfb rule lets it take
 * the interlock. Three legs:
 * - the second sprite at z 30000, depth writes off: it is in front of the first and of the wall,
 *   so it blends over the first in the overlap;
 * - the same with depth writes on;
 * - the second sprite at z 15000, depth writes on: the first sprite's depth write, in the same
 *   batch, hides it in the overlap, so the snapshot path has no hazard there either. */
static const DroBox k_dro_wall = { 100u, 50u, 140u, 120u };

typedef struct DroZLeg {
    const char *name;
    float z1;          /* the second sprite's depth; the first is at 20000 */
    int zwrite, hazard;
} DroZLeg;

static const DroZLeg k_dro_zlegs[3] = {
    { "in front, depth writes off", 30000.0f, 0, 1 },
    { "in front, depth writes on", 30000.0f, 1, 1 },
    { "hidden by the first, depth writes on", 15000.0f, 1, 0 },
};

static uint32_t dro_ztest_list(const DroState *st, const DroZLeg *leg) {
    static const float clear[1][5] = { { 0.0f, 0.0f, 480.0f, 272.0f, 0.0f } };
    const float wall[1][5] = { { (float)k_dro_wall.x0, (float)k_dro_wall.y0, (float)k_dro_wall.x1,
                                 (float)k_dro_wall.y1, 25000.0f } };
    float pair[2][5];
    memcpy(pair, k_dro_pair, sizeof pair);
    pair[0][4] = 20000.0f;
    pair[1][4] = leg->z1;
    static const uint32_t clear_rgba[1] = { 0 };
    StpList l;
    uint32_t vaddr = DRO_VERTS;
    dro_begin(&l, st);
    stp_emit(&l, 0xD3, 1u | 0x400u);                     /* clear mode: depth only */
    dro_sprites(&l, &vaddr, clear, clear_rgba, 1u);
    dro_sprites(&l, &vaddr, wall, clear_rgba, 1u);
    stp_emit(&l, 0xD3, 0);
    stp_emit(&l, 0x23, 1);
    stp_emit(&l, 0xDE, 7);                                /* GEQUAL */
    stp_emit(&l, 0xE7, leg->zwrite ? 0u : 1u);
    dro_sprites(&l, &vaddr, (const float (*)[5])pair, k_dro_pair_rgba, 2u);
    stp_end(&l);
    return DRO_LIST;
}

static int coherence_run_dst_read_ztest(void) {
    static const DroState states[2] = {
        { "5650+dither", 0u, 1u, 2u | (3u << 4), 0u, 0u, 0u },
        { "8888 dual-FIX", 3u, 0u, 10u | (10u << 4), 0x5F7F9Fu, 0x9F7F5Fu, 0u },
    };
    const DroBox first = { (uint32_t)k_dro_pair[0][0], (uint32_t)k_dro_pair[0][1],
                           (uint32_t)k_dro_pair[0][2], (uint32_t)k_dro_pair[0][3] };
    const DroBox second = { (uint32_t)k_dro_pair[1][0], (uint32_t)k_dro_pair[1][1],
                            (uint32_t)k_dro_pair[1][2], (uint32_t)k_dro_pair[1][3] };
    uint8_t *gpu_out = (uint8_t *)malloc(FB_W * FB_H * 4u);
    uint16_t *gz = (uint16_t *)malloc(480u * FB_H * 2u), *wz = (uint16_t *)malloc(480u * FB_H * 2u);
    int ok = gpu_out && gz && wz;
    GeState saved = *s_ge;
    const int ordered = dro_ordered();
    unsigned pinned = 0;
    const uint32_t nlegs = (uint32_t)(sizeof k_dro_zlegs / sizeof k_dro_zlegs[0]);
    const uint32_t nstates = (uint32_t)(sizeof states / sizeof states[0]);
    for (uint32_t r = 0; r < nstates * nlegs && ok; r++) {
        const DroState *st = &states[r / nlegs];
        const DroZLeg *leg = &k_dro_zlegs[r % nlegs];
        DroRun run;
        if (!dro_run_both(st, dro_ztest_list(st, leg), gpu_out, gz, wz, &run)) { ok = 0; break; }
        unsigned outside = 0, kept = 0, zdiff = 0, walled = 0, occluded = 0;
        unsigned diff = dro_compare("dst-read-ztest", st, gpu_out, (const uint8_t *)SR_HOST(DRO_BASE),
                                    &k_dro_overlap, &outside, &kept);
        for (uint32_t i = 0; i < 480u * FB_H; i++) {
            const uint32_t x = i % 480u, y = i / 480u;
            /* In ge.c's depth: the first sprite hidden behind the wall (outside the second), and in
             * the third leg the second sprite hidden by the first (in the overlap, off the wall). */
            if (dro_in(&k_dro_wall, x, y) && dro_in(&first, x, y) && !dro_in(&second, x, y) && wz[i] == 25000u)
                walled++;
            if (dro_in(&k_dro_overlap, x, y) && !dro_in(&k_dro_wall, x, y) && wz[i] == 20000u) occluded++;
            if (gz[i] == wz[i]) continue;
            if (!zdiff++)
                fprintf(stderr, "gpu coherence selftest [dst-read-ztest]: %s, %s: depth x=%u y=%u gpu=%u "
                                "ge_c=%u\n", st->name, leg->name, x, y, gz[i], wz[i]);
        }
        if (zdiff) {
            fprintf(stderr, "gpu coherence selftest [dst-read-ztest]: %s, %s: depth differs from ge.c in %u "
                            "pixels\n", st->name, leg->name, zdiff);
            ok = 0;
        }
        if (!walled || (!leg->hazard && !occluded)) {
            fprintf(stderr, "gpu coherence selftest [dst-read-ztest]: %s, %s: the scene is not the cell's: "
                            "%u pixels of the first sprite behind the wall (want 1 or more), %u of the second "
                            "behind the first (want %s)\n", st->name, leg->name, walled, occluded,
                    leg->hazard ? "any" : "1 or more");
            ok = 0;
        }
        if (run.fallbacks) {
            fprintf(stderr, "gpu coherence selftest [dst-read-ztest]: %s, %s: %llu primitives took the "
                            "software fallback\n", st->name, leg->name, run.fallbacks);
            ok = 0;
        }
        if (s_twin && run.twin_rmw_ops != run.rmw_ops) {
            fprintf(stderr, "gpu coherence selftest [dst-read-ztest]: %s, %s: %llu interlock ops, %llu of "
                            "them recorded into the host twin (want all)\n", st->name, leg->name,
                    run.rmw_ops, run.twin_rmw_ops);
            ok = 0;
        }
        if (ordered) {
            if (diff) {
                fprintf(stderr, "gpu coherence selftest [dst-read-ztest]: %s, %s: colour differs from ge.c in "
                                "%u pixels (%u outside the overlap) on the interlock path\n",
                        st->name, leg->name, diff, outside);
                ok = 0;
            }
            if (run.rmw_ops != 1u || run.rmw_batches != 1u || run.copies != 0u || run.zruns != 0u) {
                fprintf(stderr, "gpu coherence selftest [dst-read-ztest]: %s, %s: interlock ops %llu (want 1), "
                                "RMW batches %llu (want 1), snapshot copies %llu (want 0), shader depth runs %llu "
                                "(want 0: no kill, so the early depth test owns depth)\n",
                        st->name, leg->name, run.rmw_ops, run.rmw_batches, run.copies, run.zruns);
                ok = 0;
            }
        } else {
            /* The known hazard (#2109): confined to the overlap, and present where the second
             * sprite blends over the first; none where the first hides it. */
            const int colour_ok = leg->hazard ? diff && !outside : !diff;
            if (!colour_ok || run.rmw_ops || !run.copies) {
                fprintf(stderr, "gpu coherence selftest [dst-read-ztest]: %s, %s on the snapshot path: colour "
                                "differs from ge.c in %u pixels, %u outside the overlap (want %s); %llu "
                                "interlock ops, %llu snapshot copies (want 0 and 1 or more)\n",
                        st->name, leg->name, diff, outside,
                        leg->hazard ? "1 or more, none outside" : "none", run.rmw_ops, run.copies);
                ok = 0;
            }
            pinned += diff;
        }
    }
    *s_ge = saved;
    coherence_reset_targets();
    free(gpu_out); free(gz); free(wz);
    if (ok && ordered)
        printf("gpu coherence selftest: PASS %-22s interlock: colour and depth equal ge.c in %u depth legs, "
               "no snapshot copy\n", "dst-read-ztest", (unsigned)(sizeof k_dro_zlegs / sizeof k_dro_zlegs[0]));
    else if (ok)
        printf("gpu coherence selftest: PASS %-22s snapshot path: depth equals ge.c; the known #2109 hazard, "
               "%u pixels, all in the overlap, none where the first sprite hides the second\n",
               "dst-read-ztest", pinned);
    return ok;
}

static int coherence_run_dst_read_pins(void) {
    Sdl3VkInfo vi;
    if (!sdl3vk_get_vk(&vi)) return 0;
    if (!s_target_usage) {
        fprintf(stderr, "gpu coherence selftest [dst-read-pins]: no colour target was made before the pins\n");
        return 0;
    }
    const uint32_t base_ext = vi.denorm_preserve_f32 ? 2u : 1u;
    const int storage = (s_target_usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    const int objects = s_rp_rmw != VK_NULL_HANDLE || s_fs_rmw != VK_NULL_HANDLE ||
                        s_playout_rmw != VK_NULL_HANDLE || s_dpool_rmw != VK_NULL_HANDLE ||
                        s_rmw_dlayout != VK_NULL_HANDLE || s_rmw_zbuf != VK_NULL_HANDLE;
    const int rmw_pipes = s_rmw_pipe_n[0] + s_rmw_pipe_n[1];
    int ok = 1;
    if (vi.dst_read != (s_dst_interlock ? SDL3VK_DST_READ_INTERLOCK : SDL3VK_DST_READ_SNAPSHOT)) {
        fprintf(stderr, "gpu coherence selftest [dst-read-pins]: the device was made for path %d, the GE "
                        "runs %s\n", vi.dst_read, s_dst_interlock ? "interlock" : "snapshot");
        ok = 0;
    }
    if (!s_dst_interlock) {
        if (vi.device_extensions != base_ext || vi.device_features2 || storage || objects || rmw_pipes) {
            fprintf(stderr, "gpu coherence selftest [dst-read-pins]: the snapshot path made interlock state: "
                            "extensions %u (want %u), features chain %d, STORAGE_BIT %d, objects %d, RMW "
                            "pipelines %d\n", vi.device_extensions, base_ext, vi.device_features2, storage,
                    objects, rmw_pipes);
            ok = 0;
        }
    } else if (vi.device_extensions != base_ext + 1u || !vi.device_features2 || vi.device_features ||
               !storage || !s_rp_rmw || !s_fs_rmw || !s_playout_rmw || !s_dpool_rmw || !s_rmw_dlayout ||
               !s_rmw_zbuf) {
        fprintf(stderr, "gpu coherence selftest [dst-read-pins]: the interlock path lacks state: extensions "
                        "%u (want %u), features chain %d, pEnabledFeatures %d, STORAGE_BIT %d\n",
                vi.device_extensions, base_ext + 1u, vi.device_features2, vi.device_features, storage);
        ok = 0;
    }
    if (ok)
        printf("gpu coherence selftest: PASS %-22s %s path: %s\n", "dst-read-pins",
               s_dst_interlock ? "interlock" : "snapshot",
               s_dst_interlock ? "extension, features chain, STORAGE_BIT and RMW objects present"
                               : "no extension, features chain, STORAGE_BIT or RMW object");
    return ok;
}

/* SR_GPU_DST_READ=interlock that the device refused (sdl3vk printed the reason): the interlock
 * expectations cannot run, and the snapshot ones must not pass in their place. */
static int dro_interlock_refused(void) {
    const char *forced = getenv("SR_GPU_DST_READ");
    return forced && strcmp(forced, "interlock") == 0 && !s_dst_interlock;
}

/* Every dst-read cell (the whole suite and `--case dst-read`). A refused forced interlock is a
 * SKIP line, never exit 77: the hosted lavapipe leg treats 77 as a failure. */
static int coherence_run_dst_read(void) {
    if (dro_interlock_refused()) {
        printf("gpu coherence selftest: SKIP dst-read (SR_GPU_DST_READ=interlock was refused; the reason "
               "is printed above)\n");
        return 1;
    }
    int ok = 1;
    if (!coherence_run_dst_read_order()) ok = 0;
    if (!coherence_run_dst_read_mask()) ok = 0;
    if (!coherence_run_dst_read_alpha()) ok = 0;
    if (!coherence_run_dst_read_mixed()) ok = 0;
    if (!coherence_run_dst_read_depth()) ok = 0;
    if (!coherence_run_dst_read_ctest()) ok = 0;
    if (!coherence_run_dst_read_zrange()) ok = 0;
    if (!coherence_run_dst_read_sfail()) ok = 0;
    if (!coherence_run_dst_read_feedback()) ok = 0;
    if (!coherence_run_dst_read_ztest()) ok = 0;
    if (!coherence_run_dst_read_pins()) ok = 0;
    return ok;
}

/* One dst-read cell alone (`--case dst-read-NAME`, the mutant targets): a refused forced interlock
 * exits 77, a SKIP for those targets. dst-read-pins needs a colour target, so the order cell runs
 * first. */
static int coherence_run_dst_read_one(const char *name) {
    if (dro_interlock_refused()) {
        printf("gpu coherence selftest: SKIP %s (SR_GPU_DST_READ=interlock was refused)\n", name);
        return 77;
    }
    if (strcmp(name, "dst-read-order") == 0) return coherence_run_dst_read_order();
    if (strcmp(name, "dst-read-mask") == 0) return coherence_run_dst_read_mask();
    if (strcmp(name, "dst-read-alpha") == 0) return coherence_run_dst_read_alpha();
    if (strcmp(name, "dst-read-mixed") == 0) return coherence_run_dst_read_mixed();
    if (strcmp(name, "dst-read-depth") == 0) return coherence_run_dst_read_depth();
    if (strcmp(name, "dst-read-ctest") == 0) return coherence_run_dst_read_ctest();
    if (strcmp(name, "dst-read-zrange") == 0) return coherence_run_dst_read_zrange();
    if (strcmp(name, "dst-read-sfail") == 0) return coherence_run_dst_read_sfail();
    if (strcmp(name, "dst-read-feedback") == 0) return coherence_run_dst_read_feedback();
    if (strcmp(name, "dst-read-ztest") == 0) return coherence_run_dst_read_ztest();
    if (strcmp(name, "dst-read-pins") == 0)
        return coherence_run_dst_read_order() && coherence_run_dst_read_pins();
    fprintf(stderr, "gpu coherence selftest: unknown SR_GPU_COHERENCE_CASE '%s'\n", name);
    return 0;
}

/* stencil-once: write-once stencil states (ge_gpu.c stencil_write_once). At a pixel, the first
 * fragment of such a state that passes its tests stores a stencil that rejects every later one, so
 * the pixel is that fragment over the destination from before the run, however the primitives
 * overlap. They share one batch (no stencil_splits), and the snapshot path draws the batch last
 * triangle first; the interlock path keeps the staged order. Four parts, each against ge.c bit for
 * bit with no software fallback:
 *  - mesh (1x only: its edges cross pixels at fractions a higher scale samples elsewhere): the
 *    draw-once mask a game draws translucent shapes with (NOTEQUAL against bit 7, REPLACE on depth
 *    pass, alpha-blended, flat-shaded, 8888) over a hashed background, in one TRIANGLES command: a
 *    16x6 grid of jittered triangles that share their edges, a fan of seven, the same grid again
 *    moved by a fraction of a cell (every pixel of it lies under the first grid or beside it), and
 *    two triangles that meet at a pixel centre. No split at all;
 *  - matrix: every test function with every depth-pass op, six test masks, five alpha write masks
 *    and five references (7200 states) in each framebuffer format, the stencil-fail op KEEP, as two
 *    overlapping sprites over a background whose stencil takes every value the format holds inside
 *    the overlap. A state called write-once that is not one leaves the second sprite out where
 *    ge.c draws it, and the other way round;
 *  - anchors: states whose answer is known from the definition must merge (no split) or split once;
 *  - depth legs: the draw-once state under the depth test, over a depth wall that hides the first
 *    sprite in part of the overlap. With depth writes off, or the depth test ALWAYS and depth writes
 *    on, it merges, and colour and depth equal ge.c. Under GEQUAL with depth writes on it must
 *    split: drawn last to first, the second sprite's depth would hide the first;
 *  - kill legs: the alpha test rejects the first sprite of the pair, then the second: the pair
 *    still merges, and the sprite that is left shows in the overlap;
 *  - feedback leg (dro_feedback_list): the state textures from its own target, and the second
 *    sprite copies what the first drew. That pair is not write-once: it must split, and read the
 *    first sprite's pixels.
 * The mesh is drawn with back-face culling on, so a triangle whose vertices were reordered would
 * vanish. */
#define SON_BASE (0x04000000u | 0x00140000u)
#define SON_LIST 0x08040000u
#define SON_VERTS 0x08060000u
#define SON_PITCH 28u
#define SON_COLS 17u
#define SON_ROWS 9u
#define SON_PER_LIST (SON_COLS * SON_ROWS)
#define SON_STATES (8u * 6u * 6u * 5u * 5u)
#define SON_MESH_FRONT 1u                    /* GE_FRONTFACE for the mesh's winding */

static const uint32_t k_son_smask[6] = { 0xFFu, 0xF0u, 0x0Fu, 0x80u, 0x81u, 0x00u };
static const uint32_t k_son_amask[5] = { 0x00u, 0xF0u, 0x0Fu, 0x80u, 0xFFu };
static const uint32_t k_son_ref[5] = { 0x00u, 0x80u, 0xFFu, 0x5Au, 0x0Fu };

typedef struct SonRun {
    unsigned long long splits, once_batches, once_tris, fallbacks, rmw_ops;
} SonRun;

/* The stencil state of a cell: the stencil-fail and depth-fail ops are KEEP. */
static void son_state(StpList *l, uint32_t func, uint32_t ref, uint32_t smask, uint32_t zpass,
                      uint32_t amask, int blend) {
    stp_emit(l, 0xDC, func | (ref << 8) | (smask << 16));
    stp_emit(l, 0xDD, zpass << 16);
    stp_emit(l, 0xE9, amask);
    stp_emit(l, 0x21, blend ? 1u : 0u);
}

/* A hashed background whose stencil (the alpha) takes every value the format holds inside any
 * 16 x 16 pixels: all 256 on 8888, the 16 of 4444, both of 5551. */
static void son_fill_background(uint32_t fmt) {
    for (uint32_t i = 0; i < FB_W * FB_H; i++) {
        const uint32_t x = i % FB_W, y = i / FB_W, h = wrap_hash(0x50Eu + i * 40503u);
        if (fmt == 3u) {
            ((uint32_t *)SR_HOST(SON_BASE))[i] = (h & 0x00FFFFFFu) | (((x & 15u) | ((y & 15u) << 4)) << 24);
        } else {
            uint16_t px = (uint16_t)h;
            if (fmt == 2u) px = (uint16_t)((h & 0x0FFFu) | ((x & 15u) << 12));
            else if (fmt == 1u) px = (uint16_t)((h & 0x7FFFu) | ((x & 1u) << 15));
            ((uint16_t *)SR_HOST(SON_BASE))[i] = px;
        }
    }
    sr_gpu_vram_dirty(SON_BASE, FB_W * FB_H * (fmt == 3u ? 4u : 2u));
}

/* Run `list` through the production hooks and then through ge.c alone, from the same background
 * (`structured`: son_fill_background, else stp_fill_background). gpu_out takes the backend's
 * picture and guest VRAM is left holding ge.c's; gpu_z and sw_z take the depth when given. */
static int son_run_both(uint32_t fmt, uint32_t list, int structured, uint8_t *gpu_out,
                        uint16_t *gpu_z, uint16_t *sw_z, SonRun *run) {
    const uint32_t bytes = FB_W * FB_H * (fmt == 3u ? 4u : 2u);
    coherence_reset_targets();
    if (structured) son_fill_background(fmt); else stp_fill_background(SON_BASE, fmt);
    const unsigned long long s0 = s_replay_stats.stencil_splits, b0 = s_cnt_once_batches;
    const unsigned long long t0 = s_cnt_once_tris, f0 = s_replay_stats.fallback_prims, o0 = s_cnt_rmw_ops;
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list, 0);
    GeGpuFbDescriptor desc = { .addr = SON_BASE, .format = fmt, .stride = 512u,
                               .width = 480, .height = FB_H };
    if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) return 0;
    memcpy(gpu_out, SR_HOST(SON_BASE), bytes);
    if (gpu_z) {
        if (s_nbatch && !submit_pending()) return 0;
        for (int i = 0; i < MAX_DEP; i++)
            if (s_deps[i].used && s_deps[i].zba == 0u && !depth_to_cpu(&s_deps[i])) return 0;
        dro_depth(gpu_z);
    }
    run->splits = s_replay_stats.stencil_splits - s0;
    run->once_batches = s_cnt_once_batches - b0;
    run->once_tris = s_cnt_once_tris - t0;
    run->fallbacks = s_replay_stats.fallback_prims - f0;
    run->rmw_ops = s_cnt_rmw_ops - o0;
    coherence_reset_targets();
    if (structured) son_fill_background(fmt); else stp_fill_background(SON_BASE, fmt);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (sw_z) dro_depth(sw_z);
    return 1;
}

/* Pixels of the visible 480 columns where `gpu` differs from guest VRAM (ge.c's picture); the
 * first one goes to *at (x | y << 16) with both values. */
static unsigned son_differing(uint32_t fmt, const uint8_t *gpu, uint32_t *at, uint32_t *g, uint32_t *w) {
    const uint32_t bpp = fmt == 3u ? 4u : 2u;
    const uint8_t *sw = (const uint8_t *)SR_HOST(SON_BASE);
    unsigned n = 0;
    for (uint32_t i = 0; i < FB_W * FB_H; i++) {
        if (i % FB_W >= 480u || !memcmp(gpu + i * bpp, sw + i * bpp, bpp)) continue;
        if (!n++) {
            *at = (i % FB_W) | ((i / FB_W) << 16);
            *g = *w = 0;
            memcpy(g, gpu + i * bpp, bpp); memcpy(w, sw + i * bpp, bpp);
        }
    }
    return n;
}

static float son_jitter(uint32_t h) {
    return (float)((int)(h % 13u) - 6) * 0.125f + 0.0625f;   /* eighths of a pixel, plus 1/16 */
}

static void son_grid(ParityGuestVtx *v, int *n, float ox, float oy, uint32_t seed) {
    enum { COLS = 16, ROWS = 6 };
    const float cell = 12.0f;
    float gx[ROWS + 1][COLS + 1], gy[ROWS + 1][COLS + 1];
    for (int j = 0; j <= ROWS; j++)
        for (int i = 0; i <= COLS; i++) {
            const uint32_t h = wrap_hash(seed + (uint32_t)(j * 97 + i));
            gx[j][i] = ox + cell * (float)i + son_jitter(h);
            gy[j][i] = oy + cell * (float)j + son_jitter(h >> 8);
        }
    for (int j = 0; j < ROWS; j++)
        for (int i = 0; i < COLS; i++) {
            const uint32_t c0 = (wrap_hash(seed ^ (uint32_t)(j * 131 + i * 7)) & 0x00FFFFFFu) | 0x80000000u;
            const uint32_t c1 = (wrap_hash(seed ^ (uint32_t)(j * 131 + i * 7 + 3)) & 0x00FFFFFFu) | 0x60000000u;
            const float ax = gx[j][i], ay = gy[j][i], bx = gx[j][i + 1], by = gy[j][i + 1];
            const float cx = gx[j + 1][i + 1], cy = gy[j + 1][i + 1], ex = gx[j + 1][i], ey = gy[j + 1][i];
            const int diag = (i + j) & 1;   /* which diagonal splits the cell */
            v[(*n)++] = (ParityGuestVtx){ c0, ax, ay, 0.0f };
            v[(*n)++] = (ParityGuestVtx){ c0, bx, by, 0.0f };
            v[(*n)++] = diag ? (ParityGuestVtx){ c0, cx, cy, 0.0f } : (ParityGuestVtx){ c0, ex, ey, 0.0f };
            v[(*n)++] = diag ? (ParityGuestVtx){ c1, ax, ay, 0.0f } : (ParityGuestVtx){ c1, bx, by, 0.0f };
            v[(*n)++] = (ParityGuestVtx){ c1, cx, cy, 0.0f };
            v[(*n)++] = (ParityGuestVtx){ c1, ex, ey, 0.0f };
        }
}

static uint32_t son_mesh_list(int *ntris) {
    StpList l;
    stp_begin(&l, SON_BASE, 3u, 0);
    stp_emit(&l, 0x50, 0);                                 /* flat shading */
    stp_emit(&l, 0x9B, SON_MESH_FRONT);                    /* every triangle of the mesh faces front */
    stp_emit(&l, 0x1D, 1);                                 /* back-face culling */
    son_state(&l, 3u, 0x80u, 0x80u, 2u, 0x00u, 1);         /* NOTEQUAL bit 7, REPLACE, blended */
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(SON_VERTS);
    int n = 0;
    son_grid(v, &n, 24.0f, 40.0f, 0x51u);
    /* the fan: seven rim points at hashed radii and angles around an off-sample centre */
    const float fx = 300.0625f, fy = 120.1875f;
    float rx[7], ry[7];
    for (int k = 0; k < 7; k++) {
        const uint32_t h = wrap_hash(0x7A11u + (uint32_t)k);
        const float a = 6.2831853f * ((float)k + 0.1f + 0.3f * (float)(h & 7u) / 8.0f) / 7.0f;
        const float r = 18.0f + (float)((h >> 4) & 7u);
        rx[k] = floorf((fx + r * cosf(a)) * 8.0f) / 8.0f + 0.0625f;
        ry[k] = floorf((fy + r * sinf(a)) * 8.0f) / 8.0f + 0.0625f;
    }
    for (int k = 0; k < 7; k++) {
        const uint32_t c = (wrap_hash(0x7B00u + (uint32_t)k) & 0x00FFFFFFu) | 0x90000000u;
        v[n++] = (ParityGuestVtx){ c, fx, fy, 0.0f };
        v[n++] = (ParityGuestVtx){ c, rx[k], ry[k], 0.0f };
        v[n++] = (ParityGuestVtx){ c, rx[(k + 1) % 7], ry[(k + 1) % 7], 0.0f };
    }
    son_grid(v, &n, 24.0f + 5.25f, 40.0f + 4.125f, 0x52u);
    /* two triangles that meet only at the pixel centre (400.5, 180.5) */
    v[n++] = (ParityGuestVtx){ 0xA0336699u, 390.0625f, 172.0625f, 0.0f };
    v[n++] = (ParityGuestVtx){ 0xA0336699u, 400.5f, 180.5f, 0.0f };
    v[n++] = (ParityGuestVtx){ 0xA0336699u, 390.0625f, 190.0625f, 0.0f };
    v[n++] = (ParityGuestVtx){ 0xB0996633u, 400.5f, 180.5f, 0.0f };
    v[n++] = (ParityGuestVtx){ 0xB0996633u, 410.0625f, 172.0625f, 0.0f };
    v[n++] = (ParityGuestVtx){ 0xB0996633u, 410.0625f, 190.0625f, 0.0f };
    stp_emit(&l, 0x01, SON_VERTS & 0x00FFFFFFu);
    stp_emit(&l, 0x04, (3u << 16) | (uint32_t)n);
    stp_emit(&l, 0x1D, 0);
    stp_end(&l);
    *ntris = n / 3;
    return SON_LIST;
}

/* Two overlapping sprites in one primitive command: the first over [1, 21) of the cell at (x, y),
 * the second over [5, 25), so they overlap in 16 x 16 pixels. */
static void son_pair(StpList *l, uint32_t *vaddr, float x, float y, uint32_t ca, uint32_t cb,
                     float za, float zb) {
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(*vaddr);
    v[0] = (ParityGuestVtx){ ca, x + 1.0f, y + 1.0f, za };
    v[1] = (ParityGuestVtx){ ca, x + 21.0f, y + 21.0f, za };
    v[2] = (ParityGuestVtx){ cb, x + 5.0f, y + 5.0f, zb };
    v[3] = (ParityGuestVtx){ cb, x + 25.0f, y + 25.0f, zb };
    stp_emit(l, 0x01, *vaddr & 0x00FFFFFFu);
    stp_emit(l, 0x04, (6u << 16) | 4u);
    *vaddr += 4u * (uint32_t)sizeof(ParityGuestVtx);
}

/* Matrix state `s`: function, depth-pass op, test mask, alpha write mask, reference. A test that
 * always passes stays unblended: blended, its 8888 state may take the fixed-function stencil path,
 * whose colour is only within the fixed-function blend's bound of ge.c (stencil-parity). */
static void son_matrix_state(StpList *l, uint32_t s) {
    const uint32_t func = s % 8u;
    son_state(l, func, k_son_ref[(s / 1440u) % 5u], k_son_smask[(s / 48u) % 6u], (s / 8u) % 6u,
              k_son_amask[(s / 288u) % 5u], func != 1u && ((s / 7u) & 1u));
}

static uint32_t son_matrix_list(uint32_t fmt, uint32_t first) {
    StpList l;
    stp_begin(&l, SON_BASE, fmt, 0);
    uint32_t vaddr = SON_VERTS;
    for (uint32_t c = 0; c < SON_PER_LIST && first + c < SON_STATES; c++) {
        const uint32_t s = first + c;
        son_matrix_state(&l, s);
        son_pair(&l, &vaddr, (float)(2u + SON_PITCH * (c % SON_COLS)), (float)(2u + SON_PITCH * (c / SON_COLS)),
                 wrap_hash(0x50E1u + s * 2u), wrap_hash(0x50E2u + s * 2u), 0.0f, 0.0f);
    }
    stp_end(&l);
    return SON_LIST;
}

typedef struct SonAnchor {
    const char *name;
    uint32_t fmt, func, ref, smask, zpass, amask;
    int blend, once;
} SonAnchor;

static const SonAnchor k_son_anchors[] = {
    { "8888 NOTEQUAL bit 7, REPLACE, blended", 3u, 3u, 0x80u, 0x80u, 2u, 0x00u, 1, 1 },
    { "5551 NOTEQUAL bit 7, REPLACE, blended", 1u, 3u, 0x80u, 0x80u, 2u, 0x00u, 1, 1 },
    { "4444 NOTEQUAL 0xF0, REPLACE", 2u, 3u, 0xF0u, 0xF0u, 2u, 0x00u, 0, 1 },
    { "8888 EQUAL 0, INCR, blended", 3u, 2u, 0x00u, 0xFFu, 4u, 0x00u, 1, 1 },
    { "8888 NOTEQUAL 0, ZERO", 3u, 3u, 0x00u, 0xFFu, 1u, 0x00u, 0, 1 },
    /* 0x80 >= stencil passes again after the increment */
    { "8888 GEQUAL 0x80, INCR, blended", 3u, 7u, 0x80u, 0xFFu, 4u, 0x00u, 1, 0 },
    /* the alpha write mask keeps bit 7, so the stored stencil passes again */
    { "8888 NOTEQUAL bit 7, REPLACE, bit 7 write-protected", 3u, 3u, 0x80u, 0x80u, 2u, 0x80u, 1, 0 },
    /* a 565 pixel's stencil reads 0 whatever was stored */
    { "5650 NOTEQUAL bit 7, REPLACE, blended", 0u, 3u, 0x80u, 0x80u, 2u, 0x00u, 1, 0 },
};

static uint32_t son_anchor_list(const SonAnchor *a) {
    StpList l;
    stp_begin(&l, SON_BASE, a->fmt, 0);
    uint32_t vaddr = SON_VERTS;
    son_state(&l, a->func, a->ref, a->smask, a->zpass, a->amask, a->blend);
    son_pair(&l, &vaddr, 64.0f, 64.0f, 0x803050F0u, 0x60E0C020u, 0.0f, 0.0f);
    stp_end(&l);
    return SON_LIST;
}

typedef struct SonZLeg {
    const char *name;
    uint32_t zfunc;
    int zwrite, once;
} SonZLeg;

static const SonZLeg k_son_zlegs[3] = {
    { "GEQUAL, depth writes off", 7u, 0, 1 },
    { "ALWAYS, depth writes on", 1u, 1, 1 },
    { "GEQUAL, depth writes on", 7u, 1, 0 },
};

/* Depth cleared to 0 and a depth-only wall at z 25000 across the left part of the pair's overlap,
 * both in clear mode; then the draw-once pair, the first sprite at z 20000 (behind the wall) and
 * the second at z 30000 (in front of it). */
static uint32_t son_depth_list(uint32_t fmt, const SonZLeg *leg) {
    StpList l;
    stp_begin(&l, SON_BASE, fmt, 1);
    uint32_t vaddr = SON_VERTS;
    stp_emit(&l, 0xD3, 1u | 0x400u);                       /* clear mode: depth only */
    stp_sprite(&l, &vaddr, 0u, 0.0f, 0.0f, 480.0f, 272.0f, 0.0f);
    stp_sprite(&l, &vaddr, 0u, 66.0f, 60.0f, 78.0f, 100.0f, 25000.0f);
    stp_emit(&l, 0xD3, 0);
    son_state(&l, 3u, 0x80u, 0x80u, 2u, 0x00u, 1);
    stp_emit(&l, 0xDE, leg->zfunc);
    stp_emit(&l, 0xE7, leg->zwrite ? 0u : 1u);
    son_pair(&l, &vaddr, 64.0f, 64.0f, 0x803050F0u, 0x60E0C020u, 20000.0f, 30000.0f);
    stp_end(&l);
    return SON_LIST;
}

/* The draw-once pair under the alpha test GREATER 0x40: `killed` (0 or 1) is the sprite whose
 * alpha 0x20 the test rejects, so the other one, alpha 0x80, must show in the overlap. */
static uint32_t son_kill_list(int killed) {
    StpList l;
    stp_begin(&l, SON_BASE, 3u, 0);
    uint32_t vaddr = SON_VERTS;
    son_state(&l, 3u, 0x80u, 0x80u, 2u, 0x00u, 1);
    stp_emit(&l, 0x22, 1);
    stp_emit(&l, 0xDB, 6u | (0x40u << 8) | (0xFFu << 16));
    son_pair(&l, &vaddr, 64.0f, 64.0f, killed == 0 ? 0x203050F0u : 0x803050F0u,
             killed == 1 ? 0x20E0C020u : 0x80E0C020u, 0.0f, 0.0f);
    stp_emit(&l, 0x22, 0);
    stp_end(&l);
    return SON_LIST;
}

static int coherence_run_stencil_once(void) {
    if (dro_interlock_refused()) {
        printf("gpu coherence selftest: SKIP stencil-once (SR_GPU_DST_READ=interlock was refused; the "
               "reason is printed above)\n");
        return 1;
    }
    uint8_t *gpu_out = (uint8_t *)malloc(FB_W * FB_H * 4u);
    uint16_t *gz = (uint16_t *)malloc(480u * FB_H * 2u), *wz = (uint16_t *)malloc(480u * FB_H * 2u);
    if (!gpu_out || !gz || !wz) { free(gpu_out); free(gz); free(wz); return 0; }
    GeState saved = *s_ge;
    const int ordered = dro_ordered();
    int ok = 1, ntris = 0;
    uint32_t at = 0, g = 0, w = 0;
    SonRun run = {0};

    /* mesh */
    unsigned mesh_drawn = 0;
    if (s_scale == 1) {
        const uint32_t list = son_mesh_list(&ntris);
        if (!son_run_both(3u, list, 0, gpu_out, NULL, NULL, &run)) {
            fprintf(stderr, "gpu coherence selftest [stencil-once]: mesh: the GPU run failed\n");
            ok = 0;
        } else {
            const unsigned differing = son_differing(3u, gpu_out, &at, &g, &w);
            const uint32_t *sw = (const uint32_t *)SR_HOST(SON_BASE);
            unsigned replaced = 0;
            for (uint32_t i = 0; i < FB_W * FB_H; i++) {
                const uint32_t bg = wrap_hash(0x3C1u + i * 40503u);
                if (i % FB_W >= 480u) continue;
                if (sw[i] != bg) mesh_drawn++;
                if ((bg & 0x80000000u) == 0u && (sw[i] >> 24) == 0x80u) replaced++;
            }
            if (differing) {
                fprintf(stderr, "gpu coherence selftest [stencil-once]: mesh: %u pixels differ from ge.c, the "
                                "first at x=%u y=%u gpu=%08x ge_c=%08x\n", differing, at & 0xFFFFu, at >> 16, g, w);
                ok = 0;
            }
            if (mesh_drawn < 4096u || replaced == 0u) {
                fprintf(stderr, "gpu coherence selftest [stencil-once]: mesh: the list changed %u pixels, %u "
                                "of them by REPLACE (want at least 4096, and some)\n", mesh_drawn, replaced);
                ok = 0;
            }
            if (run.splits || run.fallbacks) {
                fprintf(stderr, "gpu coherence selftest [stencil-once]: mesh: %llu stencil splits and %llu "
                                "software fallback primitives over %d triangles (want 0 and 0)\n",
                        run.splits, run.fallbacks, ntris);
                ok = 0;
            }
            if (ordered ? run.rmw_ops == 0 || run.once_tris != 0
                        : run.once_tris != (unsigned long long)ntris) {
                fprintf(stderr, "gpu coherence selftest [stencil-once]: mesh: %llu triangles drawn last to "
                                "first in %llu batches, %llu interlock ops (want %s)\n", run.once_tris,
                        run.once_batches, run.rmw_ops,
                        ordered ? "none reversed, in an interlock op" : "every triangle reversed");
                ok = 0;
            }
        }
    }

    /* matrix */
    unsigned long long matrix_splits = 0, matrix_once = 0;
    unsigned matrix_bad = 0;
    for (uint32_t fmt = 0; fmt < 4u; fmt++)
        for (uint32_t first = 0; first < SON_STATES; first += SON_PER_LIST) {
            if (!son_run_both(fmt, son_matrix_list(fmt, first), 1, gpu_out, NULL, NULL, &run)) {
                fprintf(stderr, "gpu coherence selftest [stencil-once]: matrix fmt=%u states from %u: the "
                                "GPU run failed\n", fmt, first);
                ok = 0;
                continue;
            }
            matrix_splits += run.splits;
            matrix_once += run.once_batches;
            const unsigned differing = son_differing(fmt, gpu_out, &at, &g, &w);
            if (differing || run.fallbacks) {
                const uint32_t x = at & 0xFFFFu, y = at >> 16;
                const uint32_t s = first + (y - 2u) / SON_PITCH * SON_COLS + (x - 2u) / SON_PITCH;
                if (matrix_bad++ < 4u)
                    fprintf(stderr, "gpu coherence selftest [stencil-once]: matrix fmt=%u: %u pixels differ "
                                    "from ge.c, %llu fallback primitives; the first at x=%u y=%u gpu=%0*x ge_c=%0*x, "
                                    "state %u (func %u, depth-pass op %u, test mask %02x, alpha mask %02x, "
                                    "ref %02x)\n", fmt, differing, run.fallbacks, x, y, fmt == 3u ? 8 : 4, g,
                            fmt == 3u ? 8 : 4, w, s, s % 8u, (s / 8u) % 6u, k_son_smask[(s / 48u) % 6u],
                            k_son_amask[(s / 288u) % 5u], k_son_ref[(s / 1440u) % 5u]);
                ok = 0;
            }
        }
    if (matrix_splits == 0u || (!ordered && matrix_once == 0u)) {
        fprintf(stderr, "gpu coherence selftest [stencil-once]: matrix: %llu splits and %llu batches drawn "
                        "last to first (want some of each: it holds states of both kinds)\n",
                matrix_splits, matrix_once);
        ok = 0;
    }

    /* anchors */
    for (unsigned i = 0; i < sizeof k_son_anchors / sizeof k_son_anchors[0]; i++) {
        const SonAnchor *a = &k_son_anchors[i];
        if (!son_run_both(a->fmt, son_anchor_list(a), 1, gpu_out, NULL, NULL, &run)) { ok = 0; continue; }
        const unsigned differing = son_differing(a->fmt, gpu_out, &at, &g, &w);
        if (differing || run.fallbacks || run.splits != (a->once ? 0u : 1u)) {
            fprintf(stderr, "gpu coherence selftest [stencil-once]: anchor '%s': %llu splits (want %u), %u "
                            "pixels differ from ge.c, %llu fallback primitives\n", a->name, run.splits,
                    a->once ? 0u : 1u, differing, run.fallbacks);
            ok = 0;
        }
    }

    /* depth legs */
    for (uint32_t r = 0; r < 6u; r++) {
        const SonZLeg *leg = &k_son_zlegs[r % 3u];
        const uint32_t fmt = r < 3u ? 3u : 1u;
        if (!son_run_both(fmt, son_depth_list(fmt, leg), 0, gpu_out, gz, wz, &run)) { ok = 0; continue; }
        const unsigned differing = son_differing(fmt, gpu_out, &at, &g, &w);
        unsigned zdiff = 0;
        for (uint32_t i = 0; i < 480u * FB_H; i++) zdiff += gz[i] != wz[i];
        if (differing || zdiff || run.fallbacks || run.splits != (leg->once ? 0u : 1u)) {
            fprintf(stderr, "gpu coherence selftest [stencil-once]: depth leg '%s' fmt=%u: colour differs "
                            "from ge.c in %u pixels, depth in %u; %llu splits (want %u), %llu fallback "
                            "primitives\n", leg->name, fmt, differing, zdiff, run.splits,
                    leg->once ? 0u : 1u, run.fallbacks);
            if (differing)
                fprintf(stderr, "gpu coherence selftest [stencil-once]: the first at x=%u y=%u gpu=%0*x "
                                "ge_c=%0*x\n", at & 0xFFFFu, at >> 16, fmt == 3u ? 8 : 4, g, fmt == 3u ? 8 : 4, w);
            ok = 0;
        }
    }

    /* kill legs */
    for (int killed = 0; killed < 2; killed++) {
        if (!son_run_both(3u, son_kill_list(killed), 0, gpu_out, NULL, NULL, &run)) { ok = 0; continue; }
        const unsigned differing = son_differing(3u, gpu_out, &at, &g, &w);
        if (differing || run.fallbacks || run.splits) {
            fprintf(stderr, "gpu coherence selftest [stencil-once]: kill leg (the alpha test rejects sprite "
                            "%d): %u pixels differ from ge.c, %llu splits (want 0), %llu fallback primitives\n",
                    killed + 1, differing, run.splits, run.fallbacks);
            ok = 0;
        }
    }

    /* feedback leg */
    if (!son_run_both(3u, dro_feedback_list(), 0, gpu_out, NULL, NULL, &run)) {
        ok = 0;
    } else {
        const unsigned differing = son_differing(3u, gpu_out, &at, &g, &w);
        if (differing || run.fallbacks || run.splits != 1u) {
            fprintf(stderr, "gpu coherence selftest [stencil-once]: feedback leg: %u pixels differ from ge.c "
                            "(the first at x=%u y=%u gpu=%08x ge_c=%08x), %llu splits (want 1: the second "
                            "sprite reads the first one's pixels), %llu fallback primitives\n", differing,
                    at & 0xFFFFu, at >> 16, g, w, run.splits, run.fallbacks);
            ok = 0;
        }
    }

    *s_ge = saved;
    coherence_reset_targets();
    free(gpu_out); free(gz); free(wz);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s %s; matrix states=%u x 4 formats exact, %llu batches "
               "reversed, %llu splits; anchors=%u; depth legs=6 colour and depth exact; kill legs=2; "
               "feedback leg splits (%s path)\n",
               "stencil-once",
               s_scale == 1 ? "mesh exact with no split" : "mesh not run above 1x", SON_STATES, matrix_once,
               matrix_splits, (unsigned)(sizeof k_son_anchors / sizeof k_son_anchors[0]),
               ordered ? "interlock" : "snapshot");
    (void)mesh_drawn;
    return ok;
}

/* twin-soft (`--twin 1` or `--twin native`, scale 1): the software renderer's host twin (ge.c,
 * ge_twin_*, #2050) on twin-scene's list with no GPU hooks, run with the twin off, then on in the
 * run's colour (ge_twin_set_mode), then off again. Checks:
 *   - guest VRAM of T1, T2 and T3 with the twin on equals the twin-off runs byte for byte, and with
 *     the twin off ge_twin_present_row shows nothing (the CPU presenter converts VRAM as before);
 *   - full colour: T1's shown rows are smooth (every row rises and takes far more than the 5551
 *     lattice's 32 red levels; the blended half too), T3's shown rows 0..135 are T1's (the block
 *     transfer copied the slots), and T1 and T3 agree with the Vulkan twin's shown images
 *     (twin-scene's run) within one step per colour channel (wide_near). The presenters show red,
 *     green and blue, so alpha is not compared. T2 shows T1's 16-bit texels, on the 5551 lattice:
 *     software texture fetches read VRAM (the render-to-texture difference of contract 10.11);
 *   - native colour: every shown row equals the CPU presenter's conversion of guest VRAM;
 *   - witness: a CPU store into a twinned pixel of T1 makes that pixel show its authentic value,
 *     while the pixel beside it keeps the twin's colour. */
typedef struct { uint8_t *vram[3]; uint32_t *shown[3]; int presented; } TwinSoftRun;

static void twin_soft_free(TwinSoftRun *r) {
    for (int i = 0; i < 3; i++) { free(r->vram[i]); free(r->shown[i]); }
    memset(r, 0, sizeof(*r));
}

/* The presenter's view of a raw pixel (gui.c convert_fb_pixels), red in the low byte, no alpha. */
static uint32_t twin_soft_cpu_shown(uint32_t p, uint32_t fmt) {
    uint32_t r, g, b;
    if (fmt == 3u) { r = p & 0xFFu; g = (p >> 8) & 0xFFu; b = (p >> 16) & 0xFFu; }
    else if (fmt == 1u) { r = (p & 0x1Fu) << 3; g = ((p >> 5) & 0x1Fu) << 3; b = ((p >> 10) & 0x1Fu) << 3; }
    else if (fmt == 2u) { r = (p & 0xFu) << 4; g = ((p >> 4) & 0xFu) << 4; b = ((p >> 8) & 0xFu) << 4; }
    else { r = (p & 0x1Fu) << 3; g = ((p >> 5) & 0x3Fu) << 2; b = ((p >> 11) & 0x1Fu) << 3; }
    return r | (g << 8) | (b << 16);
}

static int twin_soft_run(TwinSoftRun *r) {
    coherence_reset_targets();
    coherence_reset_depths();
    for (int i = 0; i < 3; i++)
        memset(SR_HOST(0x04000000u | k_twin_fba[i]), 0, 512u * FB_H * (k_twin_fmt[i] == 3u ? 4u : 2u));
    ge_set_gpu_hooks(NULL);
    int ok = ge_run_list(twin_list(), 0) == 0;
    r->presented = 0;
    for (int i = 0; ok && i < 3; i++) {
        const uint32_t bytes = 512u * FB_H * (k_twin_fmt[i] == 3u ? 4u : 2u);
        r->vram[i] = (uint8_t *)malloc(bytes);
        r->shown[i] = (uint32_t *)calloc((size_t)FB_W * FB_H, 4u);
        if (!r->vram[i] || !r->shown[i]) { ok = 0; break; }
        memcpy(r->vram[i], SR_HOST(0x04000000u | k_twin_fba[i]), bytes);
        for (uint32_t y = 0; y < FB_H; y++)
            r->presented += ge_twin_present_row(0x04000000u | k_twin_fba[i], (int)k_twin_fmt[i], 512u, (int)y,
                                                r->shown[i] + (size_t)y * FB_W, DRAW_W);
    }
    ge_set_gpu_hooks(&k_hooks);
    return ok;
}

static int coherence_run_twin_soft(void) {
    const char *name = "twin-soft";
    if (s_twin_unavailable) {
        printf("gpu coherence selftest: SKIP %s: the host twin was requested (SR_GPU_TWIN_COLOR=%s) but this "
               "device refused it, and the case compares against the Vulkan twin\n", name, getenv("SR_GPU_TWIN_COLOR"));
        return 1;
    }
    if (!s_twin || s_scale != 1) {
        fprintf(stderr, "gpu coherence selftest [%s]: needs --twin 1 or --twin native at scale 1\n", name);
        return 0;
    }
    const int mode = s_twin_color ? 1 : 2;
    int ok = 1;
    /* the Vulkan twin's shown images of the same list (full colour only) */
    TwinRun vk = {0};
    if (mode == 1 && !twin_run_scene(&vk)) {
        fprintf(stderr, "gpu coherence selftest [%s]: the Vulkan twin-scene run did not run\n", name);
        ok = 0;
    }
    TwinSoftRun off = {0}, on = {0}, off2 = {0};
    if (ok && ge_twin_set_mode(0) != 0) ok = 0;
    if (ok && !twin_soft_run(&off)) ok = 0;
    if (ok && ge_twin_set_mode(mode) != mode) {
        fprintf(stderr, "gpu coherence selftest [%s]: the software twin could not be turned on\n", name);
        ok = 0;
    }
    if (ok && !twin_soft_run(&on)) ok = 0;
    if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: a software run did not run\n", name);
    if (ok && (off.presented != 0 || on.presented != 3 * FB_H)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL rows shown from the twin: %d with it off (want 0), "
                "%d with it on (want %d)\n", name, off.presented, on.presented, 3 * FB_H);
        ok = 0;
    }
    for (int i = 0; ok && i < 3; i++) {
        if (memcmp(off.vram[i], on.vram[i], 512u * FB_H * (k_twin_fmt[i] == 3u ? 4u : 2u)) != 0) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL guest VRAM of target %d differs from the "
                    "twin-off software run\n", name, i + 1);
            ok = 0;
        }
    }
    int distinct = 0, odistinct = 0, worst = 0;
    if (ok && mode == 1) {
        int mono, lattice, omono, olattice;
        twin_gradient_stats(on.shown[0], 1, 0, 136, &mono, &distinct, &lattice);
        if (!mono || lattice || distinct < 160) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL the software twin's gradient is not full colour "
                    "(monotonic=%d lattice=%d red levels=%d)\n", name, mono, lattice, distinct);
            ok = 0;
        }
        twin_gradient_stats(on.shown[0], 1, 136, FB_H, &omono, &odistinct, &olattice);
        if (!omono || olattice || odistinct < 120) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL the software twin's blended gradient is not full "
                    "colour (monotonic=%d lattice=%d red levels=%d)\n", name, omono, olattice, odistinct);
            ok = 0;
        }
        if (!twin_rows_equal(on.shown[2], on.shown[0], 1, 0, 136)) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL T3's shown rows are not T1's (the block transfer "
                    "did not copy the twin)\n", name);
            ok = 0;
        }
        int smono, sdistinct, slattice;
        twin_gradient_stats(on.shown[1], 1, 0, 136, &smono, &sdistinct, &slattice);
        if (!slattice) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL T2 is off the 5551 lattice (red levels=%d): the "
                    "software texture fetch no longer reads T1's 16-bit texels; update contract 10.11\n",
                    name, sdistinct);
            ok = 0;
        }
        for (int k = 0; k < 2; k++) {
            const int i = k ? 2 : 0;
            size_t far = 0, fx = 0, fy = 0;
            for (uint32_t y = 0; y < FB_H; y++)
                for (uint32_t x = 0; x < DRAW_W; x++) {
                    const uint32_t a = on.shown[i][(size_t)y * FB_W + x] & 0x00FFFFFFu;
                    const uint32_t b = vk.shown[i][(size_t)y * FB_W + x] & 0x00FFFFFFu;
                    for (int sh = 0; sh < 24; sh += 8) {
                        const int d = abs((int)((a >> sh) & 0xFFu) - (int)((b >> sh) & 0xFFu));
                        if (d > worst) worst = d;
                    }
                    if (!wide_near(a, b) && far++ == 0) { fx = x; fy = y; }
                }
            if (far) {
                fprintf(stderr, "gpu coherence selftest [%s]: FAIL T%d's software twin differs from the Vulkan "
                        "twin by more than one step in %zu pixels, first (%zu,%zu): software 0x%06x, Vulkan "
                        "0x%06x\n", name, i + 1, far, fx, fy,
                        on.shown[i][fy * FB_W + fx] & 0x00FFFFFFu, vk.shown[i][fy * FB_W + fx] & 0x00FFFFFFu);
                ok = 0;
            }
        }
    }
    if (ok && mode == 2) {
        for (int i = 0; ok && i < 3; i++) {
            for (uint32_t y = 0; ok && y < FB_H; y++)
                for (uint32_t x = 0; x < DRAW_W; x++) {
                    uint32_t p;
                    if (k_twin_fmt[i] == 3u) memcpy(&p, on.vram[i] + ((size_t)y * 512u + x) * 4u, 4u);
                    else { uint16_t h; memcpy(&h, on.vram[i] + ((size_t)y * 512u + x) * 2u, 2u); p = h; }
                    if ((on.shown[i][(size_t)y * FB_W + x] & 0x00FFFFFFu) != twin_soft_cpu_shown(p, k_twin_fmt[i])) {
                        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the native software twin of target %d "
                                "differs from the CPU presenter's image at (%u,%u)\n", name, i + 1, x, y);
                        ok = 0;
                        break;
                    }
                }
        }
    }
    /* witness: a CPU store the twin does not take, into a pixel it holds */
    if (ok) {
        const uint32_t px = 100u, py = 40u, addr = 0x04000000u | (TWIN_T1 + (py * 512u + px) * 2u);
        uint32_t before[DRAW_W], after[DRAW_W];
        ge_set_gpu_hooks(NULL);
        const int b_ok = ge_twin_present_row(0x04000000u | TWIN_T1, 1, 512u, (int)py, before, DRAW_W);
        const uint16_t raw = sr_r16(addr), stored = (uint16_t)(raw ^ 0x0015u);
        sr_w16(addr, stored);
        const int a_ok = ge_twin_present_row(0x04000000u | TWIN_T1, 1, 512u, (int)py, after, DRAW_W);
        ge_set_gpu_hooks(&k_hooks);
        const uint32_t want = twin_soft_cpu_shown(stored, 1u);
        if (!b_ok || !a_ok || (after[px] & 0x00FFFFFFu) != want ||
            (after[px + 1] & 0x00FFFFFFu) != (before[px + 1] & 0x00FFFFFFu)) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL witness: after a CPU store of 0x%04x at (%u,%u) the "
                    "pixel shows 0x%06x (want its authentic 0x%06x; before the store 0x%06x), its neighbour "
                    "0x%06x (before 0x%06x)\n", name, stored, px, py, after[px] & 0x00FFFFFFu, want,
                    before[px] & 0x00FFFFFFu, after[px + 1] & 0x00FFFFFFu, before[px + 1] & 0x00FFFFFFu);
            ok = 0;
        }
    }
    /* off again: nothing is shown from the twin, and VRAM is the twin-off run's */
    if (ok && (ge_twin_set_mode(0) != 0 || !twin_soft_run(&off2) || off2.presented != 0)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL with the twin turned off again, %d rows were shown "
                "from it\n", name, off2.presented);
        ok = 0;
    }
    for (int i = 0; ok && i < 3; i++)
        if (memcmp(off.vram[i], off2.vram[i], 512u * FB_H * (k_twin_fmt[i] == 3u ? 4u : 2u)) != 0) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL guest VRAM of target %d changed between the "
                    "twin-off runs\n", name, i + 1);
            ok = 0;
        }
    if (ok && mode == 1)
        printf("gpu coherence selftest: PASS %s software twin colour full: guest VRAM equals the twin-off run; "
               "shown gradient %d red levels, blended %d; T3 holds T1's twin rows; T1 and T3 within %d of the "
               "Vulkan twin per channel (bound 1); T2 shows T1's 16-bit texels; a CPU store shows the "
               "authentic pixel; off shows nothing from the twin\n", name, distinct, odistinct, worst);
    if (ok && mode == 2)
        printf("gpu coherence selftest: PASS %s software twin colour native: guest VRAM equals the twin-off run; "
               "the shown images equal the CPU presenter's; a CPU store shows the authentic pixel; off shows "
               "nothing from the twin\n", name);
    ge_twin_set_mode(-1);
    twin_soft_free(&off); twin_soft_free(&on); twin_soft_free(&off2);
    twin_run_free(&vk);
    coherence_reset_targets();
    coherence_reset_depths();
    return ok;
}

/* twin-key (`--twin-key on|off`): the player's switch end to end. The harness's Ace line carries
 * `twin=on` (`off`: `twin=off`, see below). With SR_GPU_TWIN_COLOR unset the line alone must turn the twin
 * on in the GPU renderer (full colour, decided in gegpu_init) and in the software renderer (decided at its first colour
 * write, ge_twin_set_mode(-1) first), and each must confirm it, so ace_finish's report holds no
 * "twin" entry under "unavailable". With the variable set it wins (`--twin 0` or `off` is no twin,
 * `--twin native` the twin with native colour), and the report then names the override. Each renderer
 * is checked against its own freshly made Ace state. A device that refuses the GPU twin is a SKIP
 * for the GPU half only after the report names the refusal. With `--twin-key off` the line asks
 * for nothing: `--twin 1` or `native` still turns the twin on, and the report must then say it is
 * on, as it runs (D-S1), with nothing named unavailable; with the variable unset or `0` no twin runs.
 * With `--ssaa 2` the GPU renderer also runs the twin for supersampling's samples, in native colour
 * unless the line or the variable asked for full colour, and that twin alone is not full colour:
 * the report names the twin as above, whatever supersampling ran. */
static int twin_key_report(int *effective, char *why, size_t n) {
    static char json[ACE_REPORT_MAX];
    why[0] = '\0';
    if (!ace_report_json(json, sizeof json)) return 0;
    const char *e = strstr(json, "\"twin\": ");        /* "effective" comes first */
    const char *u = strstr(json, "\"unavailable\": {");
    if (!e || !u) return 0;
    *effective = strncmp(e + 8, "true", 4) == 0;
    const char *end = strchr(u, '}');
    const char *r = strstr(u, "\"twin\": \"");
    if (r && end && r < end) {
        r += 9;
        size_t i = 0;
        for (; r[i] && r[i] != '"' && i + 1 < n; i++) why[i] = r[i];
        why[i] = '\0';
    }
    return 1;
}

/* The report for the renderer `who`: the twin is `want_on` or not, and the reason is named. */
static int twin_key_check_report(const char *who, int want_on, int refused_ok) {
    const int key = ace_requested()->twin;   /* the line asked for the twin */
    int eff = -1;
    char why[96];
    ace_finish();
    if (!twin_key_report(&eff, why, sizeof why)) {
        fprintf(stderr, "gpu coherence selftest [twin-key]: FAIL %s: the Ace report could not be read\n", who);
        return 0;
    }
    if (want_on && !why[0] && eff == 1) return 1;
    if (!want_on && eff == 0 && (key ? strstr(why, "SR_GPU_TWIN_COLOR") != NULL : !why[0])) return 1;
    if (refused_ok && eff == 0 && (why[0] || !key)) return 1;
    fprintf(stderr, "gpu coherence selftest [twin-key]: FAIL %s: the report names the twin %s%s (effective "
            "twin %s), want it %s\n", who, why[0] ? "unavailable: " : "available", why,
            eff == 1 ? "on" : "off", want_on ? "on and not unavailable"
            : key ? "off, unavailable by SR_GPU_TWIN_COLOR" : "off and not unavailable");
    return 0;
}

static int coherence_run_twin_key(void) {
    const char *name = "twin-key";
    const int key = ace_requested()->twin;       /* --twin-key on: the line asks for the twin */
    const char *k_line = key ? "v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;twin=on"
                             : "v1;af=0;text=off;readout=off;notice=off;wide=off;smooth=off;sas=off;tess=off;twin=off";
    const char *env = getenv("SR_GPU_TWIN_COLOR");
    const int overridden = env && env[0];
    const int env_mode = twin_mode_parse(env);   /* 0, 1 or 2; -1 refused */
    const int want = overridden ? (env_mode > 0 ? env_mode : 0) : key ? 1 : 0;
    int ok = 1;
    if (!g_ace_on) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL needs --twin-key on|off\n", name);
        return 0;
    }
    /* the GPU renderer, decided by gegpu_init from this Ace line */
    if (s_twin_unavailable) {
        if (!twin_key_check_report("gpu renderer", 1, 1)) ok = 0;
        printf("gpu coherence selftest: SKIP %s gpu renderer: this device refused the twin (the report names it)\n",
               name);
    } else {
        const int twin_want = want > 0 || s_ssaa > 1;   /* supersampling runs it in native colour */
        if (ace_requested()->ssaa > 1 && s_ssaa <= 1) {   /* else the --ssaa legs would test nothing */
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL gpu renderer: ssaa=%d was asked for and "
                    "supersampling did not run\n", name, ace_requested()->ssaa);
            ok = 0;
        }
        if (s_twin != twin_want || (twin_want && s_twin_color != (want == 1))) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL gpu renderer: the launch line's twin=on did not turn "
                    "the twin on (SR_GPU_TWIN_COLOR=%s): twin %s, colour %s, want %s\n", name, overridden ? env : "(unset)",
                    s_twin ? "on" : "off", s_twin_color ? "full" : "native",
                    !twin_want ? "off" : want == 1 ? "on, full" : "on, native");
            ok = 0;
        }
        if (!twin_key_check_report("gpu renderer", want > 0, 0)) ok = 0;
    }
    /* the software renderer: a fresh Ace state with the same line, the twin undecided */
    if (!ace_init_spec(k_line, NULL)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL the Ace line was refused\n", name);
        return 0;
    }
    TwinSoftRun run = {0};
    ge_twin_set_mode(-1);
    if (!twin_soft_run(&run)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL a software run did not run\n", name);
        ok = 0;
    } else {
        const int got = ge_twin_mode();
        if (got != want || run.presented != (want > 0 ? 3 * FB_H : 0)) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL software renderer: the launch line's twin=on did not "
                    "turn the twin on (SR_GPU_TWIN_COLOR=%s): mode %d, %d rows shown from it, want mode %d, %d rows\n",
                    name, overridden ? env : "(unset)", got, run.presented, want, want > 0 ? 3 * FB_H : 0);
            ok = 0;
        }
    }
    if (!twin_key_check_report("software renderer", want > 0, 0)) ok = 0;
    ge_twin_set_mode(-1);
    twin_soft_free(&run);
    coherence_reset_targets();
    coherence_reset_depths();
    if (ok)
        printf("gpu coherence selftest: PASS %s (line twin=%s) SR_GPU_TWIN_COLOR=%s: a twin %s in both renderers; "
               "the report is consistent\n", name, key ? "on" : "off", overridden ? env : "(unset)",
               want == 0 ? (key ? "off (the override named)" : "off") : want == 1 ? "in full colour" : "with native colour");
    return ok;
}

/* rtt-scale (#2052): a bloom-like pass into a quarter-size render target Q (128 x 68, stride 128,
 * 8888) is sampled back by one sprite onto the full-size target M (512 x 272), 4x, linear (a
 * bloom's filter), in the target's own format and stride. That is the render-to-texture bind in
 * build_state: Q's image is bound directly, so at render scale S Q is a 128S x 68S image and each
 * of its device pixels covers 4 device pixels of M, whatever S is. Read back through VRAM and
 * decoded at 1x (what SR_GPU_NORTT=1 forces for every draw, or a bind the direct rule refuses)
 * each Q texel covers 4S device pixels instead. (A nearest sample of a target takes the centre of
 * its guest texel by design, psp.frag, so only a linear one shows the target's own resolution.)
 *
 * Q holds a flat white quad whose top edge is long and shallow: 10 rows of rise over 128 columns,
 * so per device column it climbs one Q device row at most. In M's device image the edge is the
 * first row per column whose blend weight of Q's white is at least one half (the filter's 50%
 * crossing). The check is on the ramps' width, not on an exact colour, so the threshold sits
 * where no blend this sprite makes can reach it. M's device pixels step 1/(4S) of a guest texel
 * and psp.frag snaps positions to 1/16, so each weight along an axis is an odd number of eighths
 * at 1x and 2x and a whole number of quarters at 4x. A blend of white is then 255 * j / 64; over
 * every set of white texels in a 2x2 footprint, the values nearest one half are 119.5 and 135.5
 * (30/64 and 34/64, at 1x and 2x, where 32/64 cannot occur) and 127.5 (32/64, at 4x). Red >= 125
 * lies between them, more than one step from each, so a device that stores
 * a UNORM value as either neighbouring integer (Vulkan recommends rounding to nearest but does
 * not require it, and 127.5 is a tie) gives the same edge.
 * The edge stays on a plateau across a Q device pixel's run of columns and climbs one Q device
 * pixel, 4 device rows, in a ramp of the 4 columns where the filter blends two Q device columns:
 * so every ramp rises exactly 4 rows at 1x, 2x and 4x. (At 3x a device texel is 16/3 sixteenths,
 * the ramps are uneven, and the recipe does not run this case there.) Decoded at 1x, a Q texel is
 * 4S device pixels and the same climb is 4S rows (16 at 4x), here split in uneven ramps (1, 13
 * and 2 rows at 4x).
 * The case asserts that every ramp rises exactly 4 rows and that there are about 10 * S of them
 * (10 Q-guest rows, S device rows each, over the columns scanned); at scale 1 the two routes
 * agree, and the same 4 rows are the 1x lattice. The picture is also checked against the
 * software renderer running the same list: guest VRAM of M holds the top-left device sample of
 * each guest pixel (as sprite-scaled) and equals the software picture within 2 per channel,
 * except above 1x within 8 guest rows either side of its edge. There the scaled picture is not
 * the 1x one by design: a Q texel is 4 guest rows, the finer device staircase and the guest one
 * part by up to a texel, and the filter's blend reaches half a texel past that (6 rows; 8 leaves
 * half a texel of margin). s_cnt_rtt counts every draw that samples a target, direct or decoded,
 * so it shows only that the sprite sampled Q; the step size shows how. */
#define RTT_Q  0x00100000u
#define RTT_M  0x00060000u
typedef struct { uint32_t rgba; float x, y, z; } RttCVtx;
typedef struct { uint32_t steps, minjump, maxjump, bad, bad_x, y_first, y_last; } RttEdge;

#define RTT_LIT(px) (((px) & 0xFFu) >= 125u)        /* weight at least one half: see above */

/* Scan columns [x0, x1) of a w-wide image of h rows. Per column the edge is the first lit row; it
 * must lie in [ylo, yhi) and every row below it must be lit, and it never moves up. A filtered
 * edge climbs in ramps: runs of neighbouring columns where it rises, between plateaus where it
 * does not. Count the ramps (steps) and measure the smallest and largest total rise of one. */
static void rtt_scan_edge(const uint32_t *img, uint32_t w, uint32_t h, uint32_t x0, uint32_t x1,
                          uint32_t ylo, uint32_t yhi, RttEdge *e) {
    memset(e, 0, sizeof(*e));
    e->minjump = 0xFFFFFFFFu;
    uint32_t prev = 0, run = 0;
    for (uint32_t x = x0; x < x1; x++) {
        uint32_t y = 0;
        while (y < h && !RTT_LIT(img[(size_t)y * w + x])) y++;
        int bad = y < ylo || y >= yhi;
        for (uint32_t r = y; !bad && r < h; r++) bad = !RTT_LIT(img[(size_t)r * w + x]);
        if (bad && !e->bad++) e->bad_x = x;
        if (x == x0) e->y_first = y;
        if (x > x0 && y < prev && !e->bad++) e->bad_x = x;
        if (x > x0 && y > prev) {
            run += y - prev;
        } else if (run) {
            e->steps++;
            if (run < e->minjump) e->minjump = run;
            if (run > e->maxjump) e->maxjump = run;
            run = 0;
        }
        prev = y;
        e->y_last = y;
    }
    if (run) {
        e->steps++;
        if (run < e->minjump) e->minjump = run;
        if (run > e->maxjump) e->maxjump = run;
    }
}

static int coherence_run_rtt_scale(void) {
    const char *name = "rtt-scale";
    const uint32_t qa = 0x04000000u | RTT_Q, ma = 0x04000000u | RTT_M;
    const uint32_t qstride = 128u, qh = 68u, mstride = 512u;
    const uint32_t qbytes = qstride * FB_H * 4u, mbytes = mstride * FB_H * 4u;
    const uint32_t s = (uint32_t)s_scale, w = SCL_W, h = SCL_H;
    RttCVtx *cv = (RttCVtx *)SR_HOST(COH_SAMPLE_VERTS);
    RawSpriteVtx *sv = (RawSpriteVtx *)(cv + 6);
    const uint32_t cv_addr = COH_SAMPLE_VERTS, sv_addr = COH_SAMPLE_VERTS + 6u * (uint32_t)sizeof(RttCVtx);
    const uint32_t white = 0xFFFFFFFFu;
    /* the quad: top edge (0,30) to (128,40), as two triangles sharing the diagonal */
    cv[0] = (RttCVtx){ white, 0.0f, 30.0f, 0.0f };   cv[1] = (RttCVtx){ white, 128.0f, 40.0f, 0.0f };
    cv[2] = (RttCVtx){ white, 0.0f, 68.0f, 0.0f };   cv[3] = (RttCVtx){ white, 128.0f, 40.0f, 0.0f };
    cv[4] = (RttCVtx){ white, 128.0f, 68.0f, 0.0f }; cv[5] = (RttCVtx){ white, 0.0f, 68.0f, 0.0f };
    sv[0] = (RawSpriteVtx){ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    sv[1] = (RawSpriteVtx){ 128.0f, 68.0f, 512.0f, 272.0f, 0.0f };

    uint32_t *dl = (uint32_t *)SR_HOST(COH_SAMPLE_LIST);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, RTT_Q);                                  /* Q: 8888, stride 128, scissored to 128 x 68 */
    DL_EMIT(0x9D, qstride | (0x04u << 16));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0x9E, 0x000000u);                              /* ZBP: VRAM 0, below the targets */
    DL_EMIT(0x9F, 512u | (0x04u << 16));
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x1D, 0); DL_EMIT(0x1E, 0); DL_EMIT(0x1F, 0);
    DL_EMIT(0x22, 0); DL_EMIT(0x23, 0); DL_EMIT(0x24, 0);  /* no alpha, depth, stencil test */
    DL_EMIT(0x25, 0); DL_EMIT(0x27, 0); DL_EMIT(0x28, 0);  /* no AA, colour test, logic op */
    DL_EMIT(0xE7, 1); DL_EMIT(0xE8, 0); DL_EMIT(0xE9, 0);  /* no depth write, no masks */
    DL_EMIT(0x4C, 0); DL_EMIT(0x4D, 0); DL_EMIT(0xD6, 0); DL_EMIT(0xD7, 0xFFFFu);
    DL_EMIT(0x15, 0); DL_EMIT(0x16, ((qh - 1u) << 10) | (qstride - 1u));
    DL_EMIT(0xD4, 0); DL_EMIT(0xD5, ((qh - 1u) << 10) | (qstride - 1u));
    DL_EMIT(0x50, 1);                                      /* gouraud */
    DL_EMIT(0x20, 0);                                      /* dither off */
    DL_EMIT(0x21, 0);                                      /* blend off */
    DL_EMIT(0x12, (7u << 2) | (3u << 7) | (1u << 23));     /* 8888 colour, float pos, through */
    DL_EMIT(0x01, cv_addr);
    DL_EMIT(0x04, (3u << 16) | 6u);                        /* the two triangles */
    DL_EMIT(0x9C, RTT_M);                                  /* M: 8888, stride 512 */
    DL_EMIT(0x9D, mstride | (0x04u << 16));
    DL_EMIT(0x15, 0); DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0); DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0x1E, 1);                                      /* texture on: Q, its psm and stride */
    DL_EMIT(0xA0, RTT_Q);
    DL_EMIT(0xA8, qstride | (0x04u << 16));
    DL_EMIT(0xB8, 7u | (7u << 8));                         /* 128 x 128 */
    DL_EMIT(0xC0, 0); DL_EMIT(0xC8, 0);                    /* UV mapping, level 0 */
    DL_EMIT(0xC2, 0); DL_EMIT(0xC3, 3);                    /* 8888 */
    DL_EMIT(0xC6, 0x101u); DL_EMIT(0xC7, 0x101u);          /* linear, clamp */
    DL_EMIT(0xC9, 3u | 0x100u);                            /* replace, RGBA */
    DL_EMIT(0xCB, 0);
    DL_EMIT(0x12, 3u | (3u << 7) | (1u << 23));            /* float UV and position, through */
    DL_EMIT(0x01, sv_addr);
    DL_EMIT(0x04, (6u << 16) | 2u);                        /* one sprite, 4x */
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT

    uint32_t *dev = (uint32_t *)malloc((size_t)w * h * 4u);
    uint32_t *gpu_m = (uint32_t *)malloc(mbytes), *sw_m = (uint32_t *)malloc(mbytes);
    if (!dev || !gpu_m || !sw_m) { free(dev); free(gpu_m); free(sw_m); return 0; }
    coherence_reset_targets();
    coherence_reset_depths();
    memset(SR_HOST(qa), 0, qbytes);
    memset(SR_HOST(ma), 0, mbytes);
    sr_gpu_vram_dirty(qa, qbytes);
    sr_gpu_vram_dirty(ma, mbytes);
    ge_set_gpu_hooks(&k_hooks);
    const uint64_t rtt0 = s_cnt_rtt;
    int ok = ge_run_list(COH_SAMPLE_LIST, 0) == 0 && (!s_nbatch || submit_pending());
    GeGpuFbDescriptor desc = { .addr = ma, .format = 3, .stride = mstride, .width = 480, .height = FB_H };
    ok = ok && gegpu_sync_guest_fb(&desc) == GEGPU_SYNC_OK;
    Target *tm = ok ? target_find_by_fba(RTT_M) : NULL;
    ok = tm != NULL && coherence_read_device(tm->img, VK_IMAGE_ASPECT_COLOR_BIT, &tm->layout,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev);
    if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: draw or device read failed\n", name);
    if (ok) memcpy(gpu_m, SR_HOST(ma), mbytes);
    const Target *tq = ok ? target_find_by_fba(RTT_Q) : NULL;
    if (ok && (!tq || !tq->gpu_valid || s_cnt_rtt == rtt0)) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL Q is %s a GPU target and the sprite %s a render "
                "target\n", name, tq && tq->gpu_valid ? "" : "not", s_cnt_rtt != rtt0 ? "sampled" : "did not sample");
        ok = 0;
    }

    /* The software renderer, same list, from cleared VRAM. */
    if (ok) {
        memset(SR_HOST(qa), 0, qbytes);
        memset(SR_HOST(ma), 0, mbytes);
        sr_gpu_vram_dirty(qa, qbytes);
        sr_gpu_vram_dirty(ma, mbytes);
        ge_set_gpu_hooks(NULL);
        ok = ge_run_list(COH_SAMPLE_LIST, 0) == 0;
        ge_set_gpu_hooks(&k_hooks);
        if (ok) memcpy(sw_m, SR_HOST(ma), mbytes);
        else fprintf(stderr, "gpu coherence selftest [%s]: the software run failed\n", name);
    }

    RttEdge e = {0}, esw = {0};
    uint32_t steps_want = 0;
    if (ok) {
        /* Columns 4 to 507 of 512, away from the sprite's first and last pixel; the edge sits in
         * guest rows 120..160 (Q rows 30..40 at 4x), 4 either side allowed. */
        rtt_scan_edge(dev, w, h, 4u * s, 508u * s, 116u * s, 164u * s, &e);
        rtt_scan_edge(sw_m, mstride, FB_H, 4u, 480u, 116u, 164u, &esw);
        /* Q rises 10 rows over its 128 columns: over columns 4S..508S of M (Q columns 1..127 of 128)
         * that is 10 * 126 / 128 Q guest rows, each S Q device rows, each one step. */
        steps_want = (10u * 126u * s) / 128u;
        if (e.bad || e.minjump != 4u || e.maxjump != 4u || e.steps + 1u < steps_want || e.steps > steps_want + 1u) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL scale=%u: M's edge (first row per device "
                    "column with red at least 125, a weight of one half) %s: %u ramps rising %u..%u device rows "
                    "(rows %u to %u), want about "
                    "%u ramps of exactly 4 rows (one Q device pixel, stretched 4x); a 1x decode of Q climbs "
                    "%u rows per Q texel\n",
                    name, s, e.bad ? "is broken (a hole, out of range or moving up) from column" : "has the wrong lattice",
                    e.steps, e.minjump, e.maxjump, e.y_first, e.y_last, steps_want, 4u * s);
            if (e.bad) fprintf(stderr, "gpu coherence selftest [%s]: first bad device column %u\n", name, e.bad_x);
            ok = 0;
        }
        if (ok && (esw.bad || esw.minjump != 4u || esw.maxjump != 4u)) {
            fprintf(stderr, "gpu coherence selftest [%s]: FAIL the software picture's edge: %u steps of "
                    "%u..%u rows, bad %u\n", name, esw.steps, esw.minjump, esw.maxjump, esw.bad);
            ok = 0;
        }
    }
    /* Guest VRAM of M, but for its last Q texel row (guest rows 268 to 271): there the software
     * filter blends in the texture's unused row 68 (the texture is 128 high), which a device
     * sample taken further from the sprite's edge reaches less. */
    uint32_t differ = 0, outside = 0;
    for (uint32_t gy = 0; ok && gy < FB_H - 4u; gy++) {
        for (uint32_t gx = 0; gx < 480u; gx++) {
            const uint32_t got = gpu_m[(size_t)gy * mstride + gx] & 0x00FFFFFFu;
            const uint32_t top_left = dev[(size_t)(gy * s) * w + gx * s] & 0x00FFFFFFu;
            const uint32_t sw = sw_m[(size_t)gy * mstride + gx] & 0x00FFFFFFu;
            if (got != top_left && outside++ < 4)
                fprintf(stderr, "gpu coherence selftest [%s]: guest pixel (%u,%u) = %06x, the top-left device "
                        "sample is %06x\n", name, gx, gy, got, top_left);
            uint32_t dmax = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                const uint32_t a = (got >> sh) & 0xFFu, b = (sw >> sh) & 0xFFu;
                if ((a > b ? a - b : b - a) > dmax) dmax = a > b ? a - b : b - a;
            }
            if (dmax <= 2u) continue;
            differ++;
            /* the software picture's edge row in this column, from the first lit row */
            uint32_t edge = 0;
            while (edge < FB_H && !RTT_LIT(sw_m[(size_t)edge * mstride + gx])) edge++;
            const int in_band = gy + 8u >= edge && gy <= edge + 8u;
            if ((s == 1u || !in_band) && outside++ < 8)
                fprintf(stderr, "gpu coherence selftest [%s]: guest pixel (%u,%u) = %06x, software %06x "
                        "(software edge row %u, scale %u)\n", name, gx, gy, got, sw, edge, s);
        }
    }
    if (ok && outside) {
        fprintf(stderr, "gpu coherence selftest [%s]: FAIL %u guest VRAM pixel(s) of M are wrong at scale %u\n",
                name, outside, s);
        ok = 0;
    }
    free(dev); free(gpu_m); free(sw_m);
    coherence_reset_targets();
    coherence_reset_depths();
    if (ok) {
        printf("gpu coherence selftest: PASS %-22s scale=%u edge ramps %u of %u device rows (want about %u of 4; a 1x "
               "decode climbs %u rows per Q texel), software edge ramps %u of %u rows\n", name, s, e.steps,
               e.maxjump, steps_want, 4u * s, esw.steps, esw.maxjump);
        printf("gpu coherence selftest: %s guest VRAM of M is its top-left device samples and equals the software "
               "renderer's within 2 per channel (%u pixels differ more, all within 8 rows of the edge)\n",
               name, differ);
    }
    return ok;
}

/* ==== GE raster cells (PSP-GE-RASTER-001, #343) ====
 * This block is carried byte for byte by fixtures/psp_oracle/probe_ge_sample.c (the PSP
 * side, CASE=ge-raster), tests/native/hle_thread_selftest.c (the software GE side) and
 * src/rt/gpu_sdl3vk/ge_gpu_selftest.inc (the Vulkan side); tools/test_psp_oracle.py pins
 * the copies. Each cell is one display list of project-written
 * command words over generated patterns (no game data). It draws into the 16x4 window
 * x 16..31, y 8..11 of a 64x32 colour buffer at VRAM offset GR_FB_OFFSET whose other pixels
 * are 0 and whose window holds gr_dst() before the list runs. The base state turns every
 * per-fragment unit off and selects a 16x4 texture drawn 1:1 by a through-mode sprite with
 * REPLACE/RGBA, so a sprite's source colour is the texel; a cell's words then change only
 * what it measures. Transform-mode cells (GR_K_FOG, GR_K_XTRI) use identity world and view
 * matrices and a viewport that maps view x, y back to window pixels. The result is the
 * window's 64 pixels (16-bit formats zero-extended). */

#define GR_FB_OFFSET 0x00100000u   /* VRAM-relative colour buffer, stride 64, 32 rows */
#define GR_ZB_OFFSET 0x00110000u   /* VRAM-relative depth buffer, stride 64 */
#define GR_STRIDE 64u
#define GR_ROWS 32u
#define GR_WX 16u                  /* window x 16..31, y 8..11 */
#define GR_WY 8u
#define GR_WW 16u
#define GR_WH 4u
#define GR_N 64u                   /* pixels in the window, and texels in each texture */
#define GR_LIST_WORDS 256u
#define GR_VTX_WORDS 800u
#define GR_TEX_BYTES 512u          /* room for the largest texture (GR_T_8888W, 320 bytes) */
#define GR_CLUT_BYTES 1024u
#define GR_Z 32768.0f              /* depth of every tested through-mode primitive */
#define GR_RED 0xff0000ffu
#define GR_GREEN 0xff00ff00u

enum { GR_K_SPRITE, GR_K_ZSPRITE, GR_K_TRI, GR_K_WRAP, GR_K_CLEAR, GR_K_FOG, GR_K_XTRI,
       GR_K_LINE, GR_K_POINT };
enum { GR_T_8888, GR_T_5650, GR_T_5551, GR_T_4444, GR_T_CLUT8, GR_T_CLUT4, GR_T_CLUT4W,
       GR_T_8888W, GR_T_COUNT };
enum { GR_D_PATTERN, GR_D_WHITE };

/* kind: what is drawn (GR_K_*); psm: framebuffer format; tex: source texture (GR_T_*);
   dst: window contents before the draw (GR_D_*); arg: the kind's table row, the texture
   buffer width of a GR_K_SPRITE (0: 16) or, for GR_K_FOG, 1 to give every pixel colour vcol;
   vcol: vertex colour (ABGR); w: command words applied after the base state, 0 ends them. */
struct gr_cell {
    const char *name;
    uint8_t kind, psm, tex, dst, arg;
    uint32_t vcol;
    uint32_t w[6];
};

#define GR_CELL_COUNT 189u
static const struct gr_cell gr_cells[GR_CELL_COUNT] = {
    {"b-sa-isa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000032u}},
    {"b-fix", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000aau, 0xe040c0ffu, 0xe17f3f01u}},
    {"b-fix-ff", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000aau, 0xe0ffffffu, 0xe1ffffffu}},
    {"b-dc-sc", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000000u}},
    {"b-idc-isc", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000011u}},
    {"b-da-ida", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000054u}},
    {"b-2sa-2isa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000076u}},
    {"b-2isa-2sa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000067u}},
    {"b-2da-2ida", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000098u}},
    {"b-sub-sa-fix", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0001a2u, 0xe1806040u}},
    {"b-rsub-sa-isa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000232u}},
    {"b-min", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000332u}},
    {"b-max", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000432u}},
    {"b-abs", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000532u}},
    {"b-eq6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000632u}},
    {"b-eq7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000732u}},
    {"b-fac11-15", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000fbu, 0xe0102030u, 0xe1c0b0a0u}},
    {"b565-sa-isa", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000032u}},
    {"b5551-sa-isa", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000032u}},
    {"b4444-sa-isa", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000032u}},
    {"b565-da-ida", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000054u}},
    {"b-2sa-fix0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000a6u, 0xe1000000u}},
    {"b-2isa-fix0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000a7u, 0xe1000000u}},
    {"b-2da-fix0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000a8u, 0xe1000000u}},
    {"b-ffix-2isa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf00007au, 0xe0ffffffu}},
    {"b-ffix-2ida", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf00009au, 0xe0ffffffu}},
    {"b5551-da-ida", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000054u}},
    {"b4444-da-ida", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000054u}},
    {"at-f0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8000u}},
    {"at-f1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8001u}},
    {"at-f2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8002u}},
    {"at-f3", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8003u}},
    {"at-f4", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8004u}},
    {"at-f5", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8005u}},
    {"at-f6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8006u}},
    {"at-f7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8007u}},
    {"at-mask", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdb3c3007u}},
    {"at-gt0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff0006u}},
    {"ct-never", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000000u, 0xd9800000u, 0xda800000u}},
    {"ct-always", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000001u, 0xd9800000u, 0xda800000u}},
    {"ct-eq", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000002u, 0xd9800000u, 0xda800000u}},
    {"ct-ne", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000003u, 0xd9800000u, 0xda800000u}},
    {"ct-eq-mix", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000002u, 0xd9004080u, 0xda00c0c0u}},
    {"st-f0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8000u, 0xdd040003u}},
    {"st-f1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8001u, 0xdd040003u}},
    {"st-f2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8002u, 0xdd040003u}},
    {"st-f3", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8003u, 0xdd040003u}},
    {"st-f4", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8004u, 0xdd040003u}},
    {"st-f5", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8005u, 0xdd040003u}},
    {"st-f6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8006u, 0xdd040003u}},
    {"st-f7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8007u, 0xdd040003u}},
    {"st-op0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd000000u}},
    {"st-op1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd010000u}},
    {"st-op2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd020000u}},
    {"st-op3", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd030000u}},
    {"st-op4", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd040000u}},
    {"st-op5", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd050000u}},
    {"st-op6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd060000u}},
    {"st-op7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd070000u}},
    {"st-mask", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcf05002u, 0xdd050002u}},
    {"st4444-incr", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd040000u}},
    {"st4444-decr", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd050000u}},
    {"st4444-repl", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd020000u}},
    {"st4444-lt", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8004u, 0xdd040003u}},
    {"st5551-incr", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd040000u}},
    {"st5551-decr", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd050000u}},
    {"st5551-repl7f", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff7f01u, 0xdd020000u}},
    {"st5551-inv", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd030000u}},
    {"st565-repl", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd020000u}},
    {"st565-eq0", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff0002u, 0xdd000000u}},
    {"z-f0", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000000u}},
    {"z-f1", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000001u}},
    {"z-f2", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000002u}},
    {"z-f3", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000003u}},
    {"z-f4", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000004u}},
    {"z-f5", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000005u}},
    {"z-f6", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000006u}},
    {"z-f7", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000007u}},
    {"z-lt-st", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000004u, 0x24000001u, 0xdcff0001u, 0xdd040300u}},
    {"z-gt-st", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000006u, 0x24000001u, 0xdcff6006u, 0xdd040302u}},
    {"tf-mod-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000000u, 0xca30a0e0u}},
    {"tf-mod-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010000u, 0xca30a0e0u}},
    {"tf-mod-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000100u, 0xca30a0e0u}},
    {"tf-mod-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010100u, 0xca30a0e0u}},
    {"tf-dec-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000001u, 0xca30a0e0u}},
    {"tf-dec-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010001u, 0xca30a0e0u}},
    {"tf-dec-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000101u, 0xca30a0e0u}},
    {"tf-dec-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010101u, 0xca30a0e0u}},
    {"tf-bld-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000002u, 0xca30a0e0u}},
    {"tf-bld-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010002u, 0xca30a0e0u}},
    {"tf-bld-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000102u, 0xca30a0e0u}},
    {"tf-bld-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010102u, 0xca30a0e0u}},
    {"tf-rep-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000003u, 0xca30a0e0u}},
    {"tf-rep-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010003u, 0xca30a0e0u}},
    {"tf-rep-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000103u, 0xca30a0e0u}},
    {"tf-rep-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010103u, 0xca30a0e0u}},
    {"tf-add-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000004u, 0xca30a0e0u}},
    {"tf-add-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010004u, 0xca30a0e0u}},
    {"tf-add-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000104u, 0xca30a0e0u}},
    {"tf-add-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010104u, 0xca30a0e0u}},
    {"tf-mod-rgba-w", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0xffffffffu, {0xc9000100u}},
    {"tf-mod-rgba-1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x01010101u, {0xc9000100u}},
    {"tfa-mod-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000100u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-dec-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000101u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-bld-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000102u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-rep-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000103u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-add-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000104u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-mod-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000000u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tx-5650", GR_K_SPRITE, 3u, GR_T_5650, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000000u}},
    {"tx-5551", GR_K_SPRITE, 3u, GR_T_5551, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000001u}},
    {"tx-4444", GR_K_SPRITE, 3u, GR_T_4444, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000002u}},
    {"tx-c8-8888", GR_K_SPRITE, 3u, GR_T_CLUT8, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000005u, 0xc500ff03u}},
    {"tx-c8-sh4", GR_K_SPRITE, 3u, GR_T_CLUT8, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000005u, 0xc5020f13u}},
    {"tx-c8-5551", GR_K_SPRITE, 3u, GR_T_CLUT8, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000005u, 0xc500ff01u}},
    {"tx-c4-4444", GR_K_SPRITE, 3u, GR_T_CLUT4, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000004u, 0xc500ff02u}},
    {"tx-c4-off3", GR_K_SPRITE, 3u, GR_T_CLUT4, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000004u, 0xc503ff03u}},
    {"tx-c4-bw48", GR_K_SPRITE, 3u, GR_T_CLUT4W, GR_D_PATTERN, 48u, 0x9aff7f20u, {0xc3000004u, 0xc500ff02u}},
    {"tx-c4-bw64", GR_K_SPRITE, 3u, GR_T_CLUT4W, GR_D_PATTERN, 64u, 0x9aff7f20u, {0xc3000004u, 0xc500ff02u}},
    {"tx-8888-bw20", GR_K_SPRITE, 3u, GR_T_8888W, GR_D_PATTERN, 20u, 0x9aff7f20u, {0u}},
    {"tx-c8-bw24", GR_K_SPRITE, 3u, GR_T_8888W, GR_D_PATTERN, 24u, 0x9aff7f20u, {0xc3000005u, 0xc500ff03u}},
    {"wr-c-neg", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000101u}},
    {"wr-w-neg", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000000u}},
    {"wr-uw-vc", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000100u}},
    {"wr-c-big", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0x9aff7f20u, {0xc7000101u}},
    {"wr-w-big", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0x9aff7f20u, {0xc7000000u}},
    {"wr-w-min4", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 2u, 0x9aff7f20u, {0xc7000000u}},
    {"wr-w-neg-lin", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000000u, 0xc6000101u}},
    {"wr-c-neg-lin", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000101u, 0xc6000101u}},
    {"e-tri0", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri1", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri2", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 2u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri3", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 3u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri4", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 4u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri5", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 5u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri6", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 6u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri7", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 7u, 0x9aff7f20u, {0x1e000000u}},
    {"e-line0", GR_K_LINE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u}},
    {"e-line1", GR_K_LINE, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0x9aff7f20u, {0x1e000000u}},
    {"e-line2", GR_K_LINE, 3u, GR_T_8888, GR_D_PATTERN, 2u, 0x9aff7f20u, {0x1e000000u}},
    {"e-line3", GR_K_LINE, 3u, GR_T_8888, GR_D_PATTERN, 3u, 0x9aff7f20u, {0x1e000000u}},
    {"e-points", GR_K_POINT, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u}},
    {"fmt-565", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0u}},
    {"fmt-5551", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0u}},
    {"fmt-4444", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0u}},
    {"dth-565", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u}},
    {"dth-5551", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u}},
    {"dth-4444", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u}},
    {"dth-8888", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u}},
    {"dth-565-b", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u, 0x21000001u, 0xdf000032u}},
    {"msk-8888", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xe80f55f0u, 0xe90000c3u}},
    {"msk-565", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xe8f0f0f0u}},
    {"msk-4444", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xe8000f0fu, 0xe90000f0u}},
    {"msk-8888-st", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xe9000f0fu, 0x24000001u, 0xdcff5a01u, 0xdd020000u}},
    {"lo-0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000000u}},
    {"lo-1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000001u}},
    {"lo-2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000002u}},
    {"lo-3", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000003u}},
    {"lo-4", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000004u}},
    {"lo-5", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000005u}},
    {"lo-6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000006u}},
    {"lo-7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000007u}},
    {"lo-8", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000008u}},
    {"lo-9", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000009u}},
    {"lo-10", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000au}},
    {"lo-11", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000bu}},
    {"lo-12", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000cu}},
    {"lo-13", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000du}},
    {"lo-14", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000eu}},
    {"lo-15", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000fu}},
    {"lo-565-xor", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000006u}},
    {"clr-c", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000101u}},
    {"clr-a", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000201u}},
    {"clr-ca", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u}},
    {"clr-ca-msk", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0xe80f55f0u, 0xe90000c3u}},
    {"clr-ca-dth565", GR_K_CLEAR, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0x20000001u}},
    {"clr-ca-dth8888", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0x20000001u}},
    {"clr-c-565", GR_K_CLEAR, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000101u}},
    {"clr-ca-blend", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0x21000001u, 0xdf000032u}},
    {"clr-ca-atest", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0x22000001u, 0xdb000000u}},
    {"clr-a-4444", GR_K_CLEAR, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000201u}},
    {"xf-tri", GR_K_XTRI, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u}},
    {"xf-plain", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u}},
    {"fog-a", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u, 0x1f000001u, 0xcf8040c0u, 0xcdc08000u, 0xce3c9249u}},
    {"fog-b", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u, 0x1f000001u, 0xcf0080ffu, 0xcd000000u, 0xce3c8080u}},
    {"fog-c", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u, 0x1f000001u, 0xcfffffffu, 0xcd3f0000u, 0xce3c8000u}},
    {"fog-k0-b", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0xff000000u, {0x1e000000u, 0x1f000001u, 0xcfffffffu, 0xcd000000u, 0xce3c8080u}},
    {"fog-k0-c", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0xff000000u, {0x1e000000u, 0x1f000001u, 0xcfffffffu, 0xcd3f0000u, 0xce3c8000u}},
    {"fog-k1-b", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0xffffffffu, {0x1e000000u, 0x1f000001u, 0xcf000000u, 0xcd000000u, 0xce3c8080u}},
    {"fog-k1-c", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0xffffffffu, {0x1e000000u, 0x1f000001u, 0xcf000000u, 0xcd3f0000u, 0xce3c8000u}},
};

/* GR_K_WRAP texture rectangles: u0, v0 at the left/top edge, u1, v1 at the right/bottom. */
static const float gr_wrap_uv[3][4] = {
    {-8.0f, -2.0f, 8.0f, 2.0f}, {12.0f, 3.0f, 28.0f, 7.0f}, {-16.0f, -4.0f, 48.0f, 12.0f},
};

/* GR_K_TRI: two flat triangles (red, then green), x/y relative to the window origin. */
static const float gr_tris[8][12] = {
    {0.0f, 0.0f, 16.0f, 0.0f, 0.0f, 4.0f, 16.0f, 0.0f, 16.0f, 4.0f, 0.0f, 4.0f},
    {0.5f, 0.5f, 15.5f, 0.5f, 0.5f, 3.5f, 15.5f, 0.5f, 15.5f, 3.5f, 0.5f, 3.5f},
    {0.25f, 0.25f, 15.75f, 0.25f, 0.25f, 3.75f, 15.75f, 0.25f, 15.75f, 3.75f, 0.25f, 3.75f},
    {0.96875f, 0.96875f, 15.03125f, 0.03125f, 2.03125f, 3.03125f,
     8.53125f, 1.46875f, 15.96875f, 3.96875f, 4.46875f, 3.53125f},
    {0.0f, 0.5f, 16.0f, 1.0f, 0.0f, 1.5f, 0.0f, 2.25f, 16.0f, 2.0f, 16.0f, 3.75f},
    {0.0f, 0.0f, 7.5f, 0.0f, 7.5f, 4.0f, 7.5f, 0.0f, 16.0f, 4.0f, 7.5f, 4.0f},
    {0.0f, 2.0f, 8.0f, 0.0f, 16.0f, 2.0f, 0.0f, 2.0f, 16.0f, 2.0f, 8.0f, 4.0f},
    {0.0f, 0.0f, 0.0f, 4.0f, 16.0f, 0.0f, 16.0f, 0.0f, 0.0f, 4.0f, 16.0f, 4.0f},
};

/* GR_K_LINE: two lines (red, then green), x/y of both ends relative to the window origin. */
static const float gr_lines[4][8] = {
    {0.0f, 0.0f, 16.0f, 0.0f, 0.5f, 2.5f, 15.5f, 2.5f},
    {0.0f, 0.0f, 15.0f, 3.0f, 2.0f, 3.5f, 13.0f, 0.5f},
    {3.25f, 0.0f, 3.25f, 4.0f, 10.75f, 0.5f, 12.5f, 3.75f},
    {15.5f, 3.0f, 0.25f, 0.5f, 12.0f, 3.75f, 9.5f, 0.25f},
};

/* GR_K_POINT: eight points, alternately red and green. */
static const float gr_points[16] = {
    0.0f, 0.0f, 2.5f, 0.0f, 4.25f, 1.0f, 6.75f, 1.5f,
    8.0f, 2.0f, 10.5f, 2.5f, 12.375f, 3.0f, 14.625f, 3.625f,
};

/* Pattern byte `i` of stream `seed`: in every stream eight indices carry the edge values,
   the rest a fixed integer hash. */
static uint8_t gr_byte(uint32_t seed, uint32_t i)
{
    static const uint8_t edge[8] = {0x00u, 0xffu, 0x01u, 0xfeu, 0x7fu, 0x80u, 0x40u, 0xc0u};
    const uint32_t slot = (i * 5u + seed * 11u) & 63u;
    if (slot < 8u) {
        return edge[slot];
    }
    uint32_t x = seed * 0x9e3779b9u + i * 0x85ebca6bu + 0x165667b1u;
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    x ^= x >> 12;
    x *= 0x297a2d39u;
    x ^= x >> 15;
    return (uint8_t)(x >> 24);
}

/* An ABGR word from streams seed .. seed + 3 (R, G, B, A). */
static uint32_t gr_rgba(uint32_t seed, uint32_t i)
{
    return (uint32_t)gr_byte(seed, i) | ((uint32_t)gr_byte(seed + 1u, i) << 8) |
           ((uint32_t)gr_byte(seed + 2u, i) << 16) | ((uint32_t)gr_byte(seed + 3u, i) << 24);
}

/* Window pixel i before the draw, in the cell's framebuffer format. */
static uint32_t gr_dst(const struct gr_cell *c, uint32_t i)
{
    if (c->dst == GR_D_WHITE) {
        return c->psm == 3u ? 0xffffffffu : 0xffffu;
    }
    if (c->psm == 3u) {
        return gr_rgba(5u, i);
    }
    return (uint32_t)gr_byte(9u, i) | ((uint32_t)gr_byte(10u, i) << 8);
}

/* GR_K_ZSPRITE: the depth window pixel i holds before the tested z = 0x8000 sprite. */
static uint32_t gr_depth(uint32_t i)
{
    static const uint16_t near_ref[8] = {0x8000u, 0x7fffu, 0x8001u, 0x0000u,
                                         0xffffu, 0x7ffeu, 0x8002u, 0x8000u};
    if ((i & 3u) == 0u) {
        return near_ref[(i >> 2) & 7u];
    }
    return ((uint32_t)gr_byte(40u, i) << 8) | gr_byte(41u, i);
}

/* Texture t's bytes (16x4 texels at buffer width 16; GR_T_CLUT4W and GR_T_8888W carry 128
   and 320 bytes for wider buffers); returns the byte count. */
static uint32_t gr_texture(uint32_t t, uint8_t *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < GR_N; i++) {
        if (t == GR_T_8888) {
            const uint32_t v = gr_rgba(1u, i);
            for (uint32_t b = 0; b < 4u; b++) {
                out[n++] = (uint8_t)(v >> (8u * b));
            }
        } else if (t == GR_T_CLUT8) {
            out[n++] = gr_byte(13u, i);
        } else if (t == GR_T_CLUT4) {
            if (i < GR_N / 2u) {
                out[n++] = gr_byte(18u, i);
            }
        } else if (t == GR_T_CLUT4W) {
            out[n++] = gr_byte(44u, i);
            out[n++] = gr_byte(45u, i);
        } else if (t == GR_T_8888W) {
            for (uint32_t b = 0; b < 5u; b++) {
                out[n++] = gr_byte(46u + b, i);
            }
        } else {
            out[n++] = gr_byte(21u + t, i);
            out[n++] = gr_byte(25u + t, i);
        }
    }
    return n;
}

/* The CLUT the CLUT8/CLUT4 cells load: 256 8888 entries (512 16-bit ones). */
static void gr_clut(uint8_t *out)
{
    for (uint32_t k = 0; k < GR_CLUT_BYTES / 4u; k++) {
        const uint32_t v = gr_rgba(30u, k);
        for (uint32_t b = 0; b < 4u; b++) {
            out[k * 4u + b] = (uint8_t)(v >> (8u * b));
        }
    }
}

static uint32_t gr_fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

/* One through-mode vertex: float u, v; 8888 colour; float x, y, z (vertex type 0x80019f). */
static uint32_t gr_tvtx(uint32_t *out, float u, float v, uint32_t col, float x, float y, float z)
{
    out[0] = gr_fbits(u);
    out[1] = gr_fbits(v);
    out[2] = col;
    out[3] = gr_fbits(x);
    out[4] = gr_fbits(y);
    out[5] = gr_fbits(z);
    return 6u;
}

/* One transform-mode vertex: 8888 colour; float x, y, z (vertex type 0x19c). The x, y
   given in window pixels become the view-space x, y that the GR_K_FOG viewport maps back
   to those pixels: x = 32 + 32 * X and y = 16 + 16 * Y on the 64x32 buffer. */
static uint32_t gr_xvtx(uint32_t *out, uint32_t col, float x, float y, float z)
{
    out[0] = col;
    out[1] = gr_fbits((x - 32.0f) / 32.0f);
    out[2] = gr_fbits((y - 16.0f) / 16.0f);
    out[3] = gr_fbits(z);
    return 4u;
}

/* The cell's vertex words; returns the count. */
static uint32_t gr_vertices(const struct gr_cell *c, uint32_t *out)
{
    const float x0 = (float)GR_WX, y0 = (float)GR_WY;
    const float x1 = x0 + (float)GR_WW, y1 = y0 + (float)GR_WH;
    uint32_t n = 0;
    if (c->kind == GR_K_ZSPRITE || c->kind == GR_K_FOG) {
        /* One 1x1 sprite per window pixel: a depth in clear mode, or a colour at view z = i. */
        for (uint32_t i = 0; i < GR_N; i++) {
            const float x = x0 + (float)(i % GR_WW), y = y0 + (float)(i / GR_WW);
            if (c->kind == GR_K_ZSPRITE) {
                const float z = (float)gr_depth(i);
                n += gr_tvtx(out + n, 0.0f, 0.0f, 0u, x, y, z);
                n += gr_tvtx(out + n, 0.0f, 0.0f, 0u, x + 1.0f, y + 1.0f, z);
            } else {
                const uint32_t col = c->arg == 1u ? c->vcol : gr_rgba(1u, i);
                n += gr_xvtx(out + n, col, x, y, (float)i);
                n += gr_xvtx(out + n, col, x + 1.0f, y + 1.0f, (float)i);
            }
        }
    }
    if (c->kind == GR_K_TRI || c->kind == GR_K_XTRI) {
        for (uint32_t k = 0; k < 6u; k++) {
            const uint32_t col = k < 3u ? GR_RED : GR_GREEN;
            const float x = x0 + gr_tris[c->arg][2u * k], y = y0 + gr_tris[c->arg][2u * k + 1u];
            n += c->kind == GR_K_TRI ? gr_tvtx(out + n, 0.0f, 0.0f, col, x, y, GR_Z)
                                     : gr_xvtx(out + n, col, x, y, 0.0f);
        }
    } else if (c->kind == GR_K_LINE) {
        for (uint32_t k = 0; k < 4u; k++) {
            n += gr_tvtx(out + n, 0.0f, 0.0f, k < 2u ? GR_RED : GR_GREEN,
                         x0 + gr_lines[c->arg][2u * k], y0 + gr_lines[c->arg][2u * k + 1u], GR_Z);
        }
    } else if (c->kind == GR_K_POINT) {
        for (uint32_t k = 0; k < 8u; k++) {
            n += gr_tvtx(out + n, 0.0f, 0.0f, (k & 1u) ? GR_GREEN : GR_RED,
                         x0 + gr_points[2u * k], y0 + gr_points[2u * k + 1u], GR_Z);
        }
    } else if (c->kind == GR_K_WRAP) {
        const float *uv = gr_wrap_uv[c->arg];
        n += gr_tvtx(out + n, uv[0], uv[1], c->vcol, x0, y0, GR_Z);
        n += gr_tvtx(out + n, uv[2], uv[3], c->vcol, x1, y1, GR_Z);
    } else if (c->kind != GR_K_FOG) {
        n += gr_tvtx(out + n, 0.0f, 0.0f, c->vcol, x0, y0, GR_Z);
        n += gr_tvtx(out + n, (float)GR_WW, (float)GR_WH, c->vcol, x1, y1, GR_Z);
    }
    return n;
}

/* The cell's display list for vertex words at `vtx`, texture t at tex[t] and the CLUT at
   `clut` (physical addresses); returns the word count. Every state the draw reads is set. */
static uint32_t gr_list(const struct gr_cell *c, uint32_t vtx, const uint32_t *tex, uint32_t clut,
                        uint32_t *out)
{
    static const uint8_t off_units[12] = {0x17u, 0x1cu, 0x1du, 0x1fu, 0x20u, 0x21u,
                                          0x22u, 0x23u, 0x24u, 0x25u, 0x27u, 0x28u};
    static const float ident43[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    /* Clip z = 0 and w = 1 for every vertex, so the view z only feeds fog. */
    static const float proj[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                   0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const uint32_t t = tex[c->tex];
    const uint32_t full = (GR_STRIDE - 1u) | ((GR_ROWS - 1u) << 10);
    uint32_t n = 0;
#define GR_W(cmd, data) (out[n++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(data) & 0x00ffffffu))
    GR_W(0x10, (vtx >> 8) & 0x000f0000u);
    GR_W(0x9c, GR_FB_OFFSET);
    GR_W(0x9d, GR_STRIDE);
    GR_W(0xd2, c->psm);
    GR_W(0x9e, GR_ZB_OFFSET);
    GR_W(0x9f, GR_STRIDE);
    GR_W(0x15, 0);
    GR_W(0x16, full);
    GR_W(0xd4, 0);
    GR_W(0xd5, full);
    GR_W(0x4c, 0);
    GR_W(0x4d, 0);
    GR_W(0xd6, 0);
    GR_W(0xd7, 0xffffu);
    for (uint32_t i = 0; i < 12u; i++) {
        GR_W(off_units[i], 0);
    }
    GR_W(0x50, 0);              /* flat shading */
    GR_W(0xd3, 0);              /* clear mode off */
    GR_W(0xe7, 1);              /* depth writes off */
    GR_W(0xe8, 0);
    GR_W(0xe9, 0);
    GR_W(0xdb, 0x00ff0001u);    /* alpha test ALWAYS */
    GR_W(0xd8, 1);              /* colour test ALWAYS */
    GR_W(0xd9, 0);
    GR_W(0xda, 0);
    GR_W(0xdc, 0x00ff0001u);    /* stencil test ALWAYS */
    GR_W(0xdd, 0);              /* KEEP, KEEP, KEEP */
    GR_W(0xde, 1);              /* depth test ALWAYS */
    GR_W(0xdf, 0x32u);
    GR_W(0xe0, 0);
    GR_W(0xe1, 0);
    GR_W(0xe6, 3);              /* logic op COPY */
    GR_W(0xe2, 0x1d0cu);        /* the PSP SDK default dither matrix */
    GR_W(0xe3, 0xf3e2u);
    GR_W(0xe4, 0x0c1du);
    GR_W(0xe5, 0xe2f3u);
    GR_W(0xcf, 0);
    GR_W(0xcd, 0);
    GR_W(0xce, 0);
    GR_W(0x55, 0xffffffu);
    GR_W(0x58, 0xffu);
    GR_W(0x1e, 1);              /* texture on: UV, linear, one level, 8888 */
    GR_W(0xc0, 0);
    GR_W(0xc2, 0);
    GR_W(0xc3, 3);
    GR_W(0xa0, t);
    GR_W(0xa8, ((t >> 8) & 0x000f0000u) | (c->kind == GR_K_SPRITE && c->arg ? c->arg : GR_WW));
    GR_W(0xb8, (2u << 8) | 4u); /* 16x4 */
    GR_W(0xc6, 0);              /* nearest */
    GR_W(0xc7, 0x101u);         /* clamp u and v */
    GR_W(0xc8, 0);
    GR_W(0xc9, 0x103u);         /* REPLACE, RGBA */
    GR_W(0xca, 0);
    GR_W(0x48, gr_fbits(1.0f) >> 8);
    GR_W(0x49, gr_fbits(1.0f) >> 8);
    GR_W(0x4a, 0);
    GR_W(0x4b, 0);
    if (c->tex == GR_T_CLUT8 || c->tex == GR_T_CLUT4 || c->tex == GR_T_CLUT4W ||
        c->tex == GR_T_8888W) {
        GR_W(0xb0, clut);
        GR_W(0xb1, (clut >> 8) & 0x000f0000u);
        GR_W(0xc5, 0x00ff03u);
        GR_W(0xc4, GR_CLUT_BYTES / 32u);
    }
    GR_W(0xcb, 0);              /* texture flush */
    GR_W(0x12, 0x0080019fu);    /* through, float uv, 8888 colour, float position */
    GR_W(0x01, vtx);
    if (c->kind == GR_K_ZSPRITE) {
        GR_W(0xe7, 0);
        GR_W(0xd3, 0x401u);     /* clear mode, depth only: one 1x1 sprite per pixel */
        GR_W(0x04, (6u << 16) | (2u * GR_N));
        GR_W(0xd3, 0);
        GR_W(0xe7, 1);
        GR_W(0x01, vtx + 2u * GR_N * 24u);
    }
    for (uint32_t i = 0; i < 6u && c->w[i] != 0u; i++) {
        out[n++] = c->w[i];
    }
    if (c->kind == GR_K_FOG || c->kind == GR_K_XTRI) {
        GR_W(0x3a, 0);
        for (uint32_t i = 0; i < 12u; i++) {
            GR_W(0x3b, gr_fbits(ident43[i]) >> 8);
        }
        GR_W(0x3c, 0);
        for (uint32_t i = 0; i < 12u; i++) {
            GR_W(0x3d, gr_fbits(ident43[i]) >> 8);
        }
        GR_W(0x3e, 0);
        for (uint32_t i = 0; i < 16u; i++) {
            GR_W(0x3f, gr_fbits(proj[i]) >> 8);
        }
        GR_W(0x42, gr_fbits(32.0f) >> 8);   /* viewport: x = 32 + 32 X, y = 16 + 16 Y */
        GR_W(0x43, gr_fbits(16.0f) >> 8);
        GR_W(0x44, 0);
        GR_W(0x45, gr_fbits(2048.0f) >> 8);
        GR_W(0x46, gr_fbits(2048.0f) >> 8);
        GR_W(0x47, gr_fbits(32768.0f) >> 8);
        GR_W(0x4c, (2048u - 32u) << 4);
        GR_W(0x4d, (2048u - 16u) << 4);
        GR_W(0x12, 0x0000019cu);  /* transform, 8888 colour, float position */
        if (c->kind == GR_K_FOG) {
            GR_W(0x04, (6u << 16) | (2u * GR_N));
        } else {
            GR_W(0x04, (3u << 16) | 6u);
        }
    } else if (c->kind == GR_K_TRI) {
        GR_W(0x04, (3u << 16) | 6u);
    } else if (c->kind == GR_K_LINE) {
        GR_W(0x04, (1u << 16) | 4u);
    } else if (c->kind == GR_K_POINT) {
        GR_W(0x04, 8u);
    } else {
        GR_W(0x04, (6u << 16) | 2u);
    }
    GR_W(0x0f, 0);
    GR_W(0x0c, 0);
#undef GR_W
    return n;
}

/* FNV-1a 64 over the window's 64 result words, each little-endian. */
static uint64_t gr_digest(const uint32_t *px)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < GR_N; i++) {
        for (uint32_t b = 0; b < 4u; b++) {
            h ^= (px[i] >> (8u * b)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}
/* ==== end of the GE raster cells ==== */

/* PSP-GE-RASTER-001 (#343): the window digest each cell left on a PSP-3000 (firmware 6.6.1,
 * probe case ge-raster, docs/HARDWARE_ORACLE.md). Index k is gr_cells[k]. */
static const struct {
    const char *name;
    uint64_t psp;
} s_gr_psp[GR_CELL_COUNT] = {
    {"b-sa-isa", 0xe2e3dd342f1b7aedull},
    {"b-fix", 0x8ec220832fb9a609ull},
    {"b-fix-ff", 0xa4df29b706351f0aull},
    {"b-dc-sc", 0xae80e197b2487b80ull},
    {"b-idc-isc", 0xc158ab0c9e67248eull},
    {"b-da-ida", 0xd9612ab5d2b56816ull},
    {"b-2sa-2isa", 0xf00f5598f5e83a8dull},
    {"b-2isa-2sa", 0xd66d5d5af2f30cd9ull},
    {"b-2da-2ida", 0xa847911d0f1e673full},
    {"b-sub-sa-fix", 0x8759886443b6004full},
    {"b-rsub-sa-isa", 0x00f648218bafb8f8ull},
    {"b-min", 0xf21f885e6b14a8b6ull},
    {"b-max", 0xd1b0e597970e8debull},
    {"b-abs", 0x4d6f82babe9d8152ull},
    {"b-eq6", 0x61b0b02428e84441ull},
    {"b-eq7", 0x61b0b02428e84441ull},
    {"b-fac11-15", 0x4def31c864814aafull},
    {"b565-sa-isa", 0xa688406e73344322ull},
    {"b5551-sa-isa", 0xc13232bb3a0ebe30ull},
    {"b4444-sa-isa", 0x4aa79875c953edf2ull},
    {"b565-da-ida", 0x4f0a3a47648a81c7ull},
    {"b-2sa-fix0", 0x93349d9a477a8aa2ull},
    {"b-2isa-fix0", 0x0fc0361ab21e8438ull},
    {"b-2da-fix0", 0xc2b1ff28c04b6cfcull},
    {"b-ffix-2isa", 0x4753d8fff6a1bc9bull},
    {"b-ffix-2ida", 0x4973b119622d734cull},
    {"b5551-da-ida", 0x130ed831ea47c0d5ull},
    {"b4444-da-ida", 0x7eafc1cb089c2a0aull},
    {"at-f0", 0x6b417febbd03b2ecull},
    {"at-f1", 0x61b0b02428e84441ull},
    {"at-f2", 0xcdc817528c030710ull},
    {"at-f3", 0x3805351c8739f6fdull},
    {"at-f4", 0x75af15df6b356590ull},
    {"at-f5", 0xcd7d40eb15f0ab1cull},
    {"at-f6", 0x71901108fa18e3b1ull},
    {"at-f7", 0x2c3737c841f7433dull},
    {"at-mask", 0xcc38ed26427815fbull},
    {"at-gt0", 0x96354b2e62a1da37ull},
    {"ct-never", 0x6b417febbd03b2ecull},
    {"ct-always", 0x61b0b02428e84441ull},
    {"ct-eq", 0x10c0807ec120af51ull},
    {"ct-ne", 0x4475b5231e05efacull},
    {"ct-eq-mix", 0x4d0afb4fb8bf5a0aull},
    {"st-f0", 0xb3e5e86b6c945c14ull},
    {"st-f1", 0xa673adbc5a81af31ull},
    {"st-f2", 0xaa81ea4dcd87ab12ull},
    {"st-f3", 0x5e193d5ad9c2e8ebull},
    {"st-f4", 0xd4290d62c2eb2b52ull},
    {"st-f5", 0xc2ae2f09fb82c06cull},
    {"st-f6", 0xbd8efe4f49808d99ull},
    {"st-f7", 0xb80ae59808ab34d3ull},
    {"st-op0", 0x61b0b02428e84441ull},
    {"st-op1", 0x8cce44c95088fe39ull},
    {"st-op2", 0x8deef11e3a45a095ull},
    {"st-op3", 0xe28eaf938faee78dull},
    {"st-op4", 0xa673adbc5a81af31ull},
    {"st-op5", 0x2e98a36ee874b23cull},
    {"st-op6", 0x61b0b02428e84441ull},
    {"st-op7", 0x61b0b02428e84441ull},
    {"st-mask", 0xfc5213f372a980d8ull},
    {"st4444-incr", 0x761201d2693253d6ull},
    {"st4444-decr", 0xda56a206ea639db6ull},
    {"st4444-repl", 0xf6fa0571670cd636ull},
    {"st4444-lt", 0x89267a5400327090ull},
    {"st5551-incr", 0x99788ba9f6d725f6ull},
    {"st5551-decr", 0x11f6554bd26e93f6ull},
    {"st5551-repl7f", 0x11f6554bd26e93f6ull},
    {"st5551-inv", 0xb94551d4a09fff76ull},
    {"st565-repl", 0x12eb7b0d599b524eull},
    {"st565-eq0", 0x12eb7b0d599b524eull},
    {"z-f0", 0x6b417febbd03b2ecull},
    {"z-f1", 0x61b0b02428e84441ull},
    {"z-f2", 0x0d4abd68740baa99ull},
    {"z-f3", 0x8856970cd47244c8ull},
    {"z-f4", 0xcacab511e92ade03ull},
    {"z-f5", 0xe0adf135f4b58dfeull},
    {"z-f6", 0x9ce9403e66b0f8cfull},
    {"z-f7", 0xe27a10bc7d55a32eull},
    {"z-lt-st", 0x5e0dbd0e6ba47d80ull},
    {"z-gt-st", 0x9744cfbf2f4a20b2ull},
    {"tf-mod-rgb", 0x528021cf7dc9ab2cull},
    {"tf-mod-rgb-x2", 0xdfd3e8b5cf373522ull},
    {"tf-mod-rgba", 0x528021cf7dc9ab2cull},
    {"tf-mod-rgba-x2", 0xdfd3e8b5cf373522ull},
    {"tf-dec-rgb", 0x61b0b02428e84441ull},
    {"tf-dec-rgb-x2", 0x718a6690670a04bfull},
    {"tf-dec-rgba", 0xc3662b29f8e75d67ull},
    {"tf-dec-rgba-x2", 0xf87b6fb185779d15ull},
    {"tf-bld-rgb", 0xc322f1a42d989cc0ull},
    {"tf-bld-rgb-x2", 0xe2ca5f19c4d50788ull},
    {"tf-bld-rgba", 0xc322f1a42d989cc0ull},
    {"tf-bld-rgba-x2", 0xe2ca5f19c4d50788ull},
    {"tf-rep-rgb", 0x61b0b02428e84441ull},
    {"tf-rep-rgb-x2", 0x718a6690670a04bfull},
    {"tf-rep-rgba", 0x61b0b02428e84441ull},
    {"tf-rep-rgba-x2", 0x718a6690670a04bfull},
    {"tf-add-rgb", 0x3ff03fb3785db8fbull},
    {"tf-add-rgb-x2", 0x09e444041ba1e76aull},
    {"tf-add-rgba", 0x3ff03fb3785db8fbull},
    {"tf-add-rgba-x2", 0x09e444041ba1e76aull},
    {"tf-mod-rgba-w", 0x61b0b02428e84441ull},
    {"tf-mod-rgba-1", 0x1f26bf30dbbd11adull},
    {"tfa-mod-rgba", 0xdc8184067b497b4cull},
    {"tfa-dec-rgba", 0xd2cfdae5ad35a825ull},
    {"tfa-bld-rgba", 0xdc8184067b497b4cull},
    {"tfa-rep-rgba", 0xf50f75b899a90725ull},
    {"tfa-add-rgba", 0xdc8184067b497b4cull},
    {"tfa-mod-rgb", 0xd2cfdae5ad35a825ull},
    {"tx-5650", 0x55191b7986f94bc5ull},
    {"tx-5551", 0x50fd51ab4a3dc1bdull},
    {"tx-4444", 0x48a401c483f8a640ull},
    {"tx-c8-8888", 0xcc8e999943f08733ull},
    {"tx-c8-sh4", 0xa4539c4c08f2b348ull},
    {"tx-c8-5551", 0x672d960cefbf2f50ull},
    {"tx-c4-4444", 0xca1b5888bc0a73b8ull},
    {"tx-c4-off3", 0xe9f25d4569db9d85ull},
    {"tx-c4-bw48", 0xc6e98ff96fefe004ull},
    {"tx-c4-bw64", 0x7839e58d724c70b5ull},
    {"tx-8888-bw20", 0x9fa773b0a1d72d9bull},
    {"tx-c8-bw24", 0xb620ae02e7089a79ull},
    {"wr-c-neg", 0x7c9b1a47a3d70368ull},
    {"wr-w-neg", 0xb28ae2c44b1f4c09ull},
    {"wr-uw-vc", 0xc209862396d56a05ull},
    {"wr-c-big", 0x7e2190ed8778a2d5ull},
    {"wr-w-big", 0x8b682ff667ebaaedull},
    {"wr-w-min4", 0xa2e42f222f589b49ull},
    {"wr-w-neg-lin", 0xb28ae2c44b1f4c09ull},
    {"wr-c-neg-lin", 0x7c9b1a47a3d70368ull},
    {"e-tri0", 0xf5a0993ec9228d41ull},
    {"e-tri1", 0x0eaa42aa6b33ed99ull},
    {"e-tri2", 0x9a27cacbebcdd6f1ull},
    {"e-tri3", 0x58a0d3f1740cecf0ull},
    {"e-tri4", 0xbac04c9003489048ull},
    {"e-tri5", 0x6d8b51ec978358f3ull},
    {"e-tri6", 0x44c4675b69c3e467ull},
    {"e-tri7", 0xf5a0993ec9228d41ull},
    {"e-line0", 0x4a67f079217de8beull},
    {"e-line1", 0xa896e3153d5cc0bbull},
    {"e-line2", 0x5ce41b1df46da45aull},
    {"e-line3", 0x936dca4d32a5e7b7ull},
    {"e-points", 0x68c7b5aa88c253a7ull},
    {"fmt-565", 0x12eb7b0d599b524eull},
    {"fmt-5551", 0x673de6b93dd90776ull},
    {"fmt-4444", 0xe4ddad427dc21de6ull},
    {"dth-565", 0xf0ef3c90518c45fdull},
    {"dth-5551", 0x12406110445fa3b1ull},
    {"dth-4444", 0x127c48c1dccd5544ull},
    {"dth-8888", 0xdd0aecff5f0fa36bull},
    {"dth-565-b", 0x2f4ed56110282d97ull},
    {"msk-8888", 0xc554066569201d85ull},
    {"msk-565", 0xfb76050e8e1f9ebaull},
    {"msk-4444", 0xe4ddad427dc21de6ull},
    {"msk-8888-st", 0x324672ef2806c4a1ull},
    {"lo-0", 0xbe910e17deca3a2dull},
    {"lo-1", 0xb410e3e5c05ac6ebull},
    {"lo-2", 0x9f7d03d69cb73583ull},
    {"lo-3", 0x61b0b02428e84441ull},
    {"lo-4", 0x62b2029c0cf444d2ull},
    {"lo-5", 0x6b417febbd03b2ecull},
    {"lo-6", 0x8f3cc93ed2e25e10ull},
    {"lo-7", 0x41043d49cee09bd2ull},
    {"lo-8", 0x24b86f70ebc4230eull},
    {"lo-9", 0x3c44a6f726c2b570ull},
    {"lo-10", 0x07610972524b83a8ull},
    {"lo-11", 0xf50bc89ce82a2c72ull},
    {"lo-12", 0x274253721c649fbdull},
    {"lo-13", 0x272bb88f382da21bull},
    {"lo-14", 0x2254372563da19afull},
    {"lo-15", 0x74b58b05fdf3eaadull},
    {"lo-565-xor", 0x7b4ad7adbbf198bcull},
    {"clr-c", 0x5982dec5529ace6dull},
    {"clr-a", 0x7c24b06f87d84da0ull},
    {"clr-ca", 0x6635ffd176aa2b25ull},
    {"clr-ca-msk", 0xcec5ae97a8e37f30ull},
    {"clr-ca-dth565", 0x7341485be704cba5ull},
    {"clr-ca-dth8888", 0xa8195cd6205ffae5ull},
    {"clr-c-565", 0xf3d7b8b640c9a8a5ull},
    {"clr-ca-blend", 0x6635ffd176aa2b25ull},
    {"clr-ca-atest", 0x6635ffd176aa2b25ull},
    {"clr-a-4444", 0x24ffba3b563c2017ull},
    {"xf-tri", 0xf5a0993ec9228d41ull},
    {"xf-plain", 0x61b0b02428e84441ull},
    {"fog-a", 0xfc6d7b8405af2fa1ull},
    {"fog-b", 0x8545d166b94e44acull},
    {"fog-c", 0x5a6d3e6eceb6619bull},
    {"fog-k0-b", 0x89adaadf98387375ull},
    {"fog-k0-c", 0x578c3b95de454f25ull},
    {"fog-k1-b", 0xe3d831492858103dull},
    {"fog-k1-c", 0x75e634dc776d8575ull},
};

/* PSP-GE-RASTER-001 (#343) on the Vulkan path at render scale 1. Every cell of the block above
 * runs the probe's own list, vertices, textures and destination pattern through ge.c with the
 * GPU hooks on, and the window read back from the GPU is compared with the digest the PSP-3000
 * gave (s_gr_psp, the table test_ge_raster_cells_match_psp also reads). Each cell is in exactly
 * one class, and a cell that changes class fails the case, so neither a new fallback nor a new
 * divergence can hide:
 *  - drawn on the GPU and equal to the PSP (every cell not named below);
 *  - drawn on the GPU within a bound (k_gr_vk_divergent): the Vulkan blend and texture-function
 *    arithmetic most likely rounds in floating point where the PSP truncates integer products
 *    (not measured), so these may differ by 1 or 2 per channel; each pixel must stay within
 *    GR_VK_DIVERGENT_MAX_DELTA of ge.c's, which equals the PSP's (docs/HARDWARE_ORACLE.md, "GE raster pixel
 *    corpus"). Whether a cell lands on the PSP's value depends on the driver's rounding (lavapipe
 *    draws tf-mod-rgba-1 exact, an RTX 3080 does not), so equal passes too and is counted;
 *  - drawn by the software fallback (k_gr_vk_software): the backend refuses a primitive and
 *    ge.c draws it, which proves nothing about Vulkan's raster, but the window must still
 *    equal the PSP's after the readback. */
static const char *const k_gr_vk_divergent[] = {
    "b-sa-isa", "b-dc-sc", "b-idc-isc", "b-da-ida", "b-sub-sa-fix", "b-rsub-sa-isa",
    "tf-mod-rgb", "tf-mod-rgb-x2", "tf-mod-rgba", "tf-mod-rgba-x2", "tf-dec-rgba",
    "tf-dec-rgba-x2", "tf-bld-rgb", "tf-bld-rgb-x2", "tf-bld-rgba", "tf-bld-rgba-x2",
    "tf-mod-rgba-1", "tfa-mod-rgba", "tfa-bld-rgba", "tfa-add-rgba",
};
static const char *const k_gr_vk_software[] = {
    /* Z test with stencil, points, partial write masks (clear mode included) and logic ops. */
    "z-lt-st", "z-gt-st", "e-points", "msk-8888", "msk-565", "clr-ca-msk", "lo-0", "lo-1",
    "lo-2", "lo-3", "lo-4", "lo-5", "lo-6", "lo-7", "lo-8", "lo-9", "lo-10", "lo-11", "lo-12",
    "lo-13", "lo-14", "lo-15", "lo-565-xor",
};

static int gr_vk_named(const char *const *names, size_t n, const char *cell) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(names[i], cell) == 0) return 1;
    return 0;
}

/* The largest difference of one native colour field between two pixels of format psm. */
static uint32_t gr_vk_channel_delta(uint32_t psm, uint32_t a, uint32_t b) {
    static const uint8_t k_fields[4][4][2] = {
        {{0, 5}, {5, 6}, {11, 5}, {0, 0}}, {{0, 5}, {5, 5}, {10, 5}, {15, 1}},
        {{0, 4}, {4, 4}, {8, 4}, {12, 4}}, {{0, 8}, {8, 8}, {16, 8}, {24, 8}},
    };
    uint32_t worst = 0;
    for (int f = 0; f < 4; f++) {
        const uint32_t shift = k_fields[psm & 3u][f][0], bits = k_fields[psm & 3u][f][1];
        if (bits == 0) continue;
        const uint32_t va = (a >> shift) & ((1u << bits) - 1u), vb = (b >> shift) & ((1u << bits) - 1u);
        const uint32_t d = va > vb ? va - vb : vb - va;
        if (d > worst) worst = d;
    }
    return worst;
}

/* One cell from a clean window: its list through ge.c, with the GPU hooks when gpu is set (the
 * window is then read back from the GPU), into px. */
static int gr_vk_draw(const struct gr_cell *c, int gpu, const uint32_t *tex, uint32_t clut,
                      uint32_t px[GR_N], unsigned long long *fell, int *sync) {
    /* synthetic main RAM below the user partition (the purity gate keeps game addresses out) */
    const uint32_t list = 0x08600000u, vtx = 0x08604000u;
    const uint32_t fb = 0x04000000u + GR_FB_OFFSET;
    coherence_reset_targets();
    memset(SR_HOST(fb), 0, GR_STRIDE * GR_ROWS * 4u);
    for (uint32_t i = 0; i < GR_N; i++) {
        const uint32_t at = (GR_WY + i / GR_WW) * GR_STRIDE + GR_WX + i % GR_WW;
        if (c->psm == 3u) MEM_W32(fb + at * 4u, gr_dst(c, i));
        else MEM_W16(fb + at * 2u, (uint16_t)gr_dst(c, i));
    }
    sr_gpu_vram_dirty(fb, GR_STRIDE * GR_ROWS * 4u);
    (void)gr_vertices(c, (uint32_t *)SR_HOST(vtx));
    (void)gr_list(c, vtx, tex, clut, (uint32_t *)SR_HOST(list));
    const unsigned long long fallback0 = s_replay_stats.fallback_prims;
    ge_set_gpu_hooks(gpu ? &k_hooks : NULL);
    const int done = ge_run_list(list, 0) == 0;   /* 0: the list reached its END */
    ge_set_gpu_hooks(&k_hooks);
    *fell = s_replay_stats.fallback_prims - fallback0;
    *sync = GEGPU_SYNC_NO_TARGET;
    if (gpu) {
        GeGpuFbDescriptor desc = { .addr = fb, .format = c->psm, .stride = GR_STRIDE,
                                   .width = GR_STRIDE, .height = GR_ROWS };
        *sync = gegpu_sync_guest_fb(&desc);
    }
    for (uint32_t i = 0; i < GR_N; i++) {
        const uint32_t at = (GR_WY + i / GR_WW) * GR_STRIDE + GR_WX + i % GR_WW;
        px[i] = c->psm == 3u ? MEM_R32(fb + at * 4u) : (uint32_t)MEM_R16(fb + at * 2u);
    }
    return done;
}

/* Pinned cells differ from the PSP by at most this much in any native colour field. */
#define GR_VK_DIVERGENT_MAX_DELTA 2u

/* Clears through all-or-nothing pixel masks, one per framebuffer format: the GPU draws them
 * through its colour write mask and must give ge.c's window (which keeps every masked bit, as
 * the PSP does for clr-ca-msk). Before #343's fix the GPU wrote every bit of the clear colour. */
static int coherence_run_clear_masks(void) {
    static const struct gr_cell k_cells[] = {
        /* 8888: red and blue masked, alpha written. */
        {"clr-mask-8888", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu,
         {0xd3000301u, 0xe8ff00ffu, 0xe9000000u}},
        /* 8888: colour written, alpha masked. */
        {"clr-mask-8888-a", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu,
         {0xd3000301u, 0xe8000000u, 0xe90000ffu}},
        /* 565: red and blue masked (their top five bits), green written. */
        {"clr-mask-565", GR_K_CLEAR, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu,
         {0xd3000301u, 0xe8f803f8u}},
        /* 5551: green masked, the alpha bit masked. */
        {"clr-mask-5551", GR_K_CLEAR, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu,
         {0xd3000301u, 0xe800f800u, 0xe9000080u}},
        /* 4444: red and blue masked, green and alpha written (low nibbles are not stored). */
        {"clr-mask-4444", GR_K_CLEAR, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu,
         {0xd3000301u, 0xe8f00ff0u, 0xe900000fu}},
    };
    const uint32_t clut = 0x08618000u;
    uint32_t tex[GR_T_COUNT];
    for (uint32_t t = 0; t < GR_T_COUNT; t++) {
        tex[t] = 0x08610000u + t * 0x400u;
        (void)gr_texture(t, (uint8_t *)SR_HOST(tex[t]));
    }
    gr_clut((uint8_t *)SR_HOST(clut));
    GeState saved = *s_ge;
    int ok = 1;
    for (size_t k = 0; k < sizeof(k_cells) / sizeof(k_cells[0]); k++) {
        const struct gr_cell *c = &k_cells[k];
        uint32_t px[GR_N], ref[GR_N], plain[GR_N];
        unsigned long long fell = 0, ref_fell = 0, plain_fell = 0;
        int sync = 0, ref_sync = 0, plain_sync = 0;
        /* The same clear with no mask: ge.c's window must differ from it, or the cell keeps no
         * destination bit and tests nothing. */
        struct gr_cell unmasked = *c;
        for (size_t w = 1; w < sizeof(unmasked.w) / sizeof(unmasked.w[0]); w++)
            if ((unmasked.w[w] >> 24) == 0xe8u || (unmasked.w[w] >> 24) == 0xe9u)
                unmasked.w[w] &= 0xff000000u;
        const int done = gr_vk_draw(&unmasked, 0, tex, clut, plain, &plain_fell, &plain_sync) &&
                         gr_vk_draw(c, 0, tex, clut, ref, &ref_fell, &ref_sync) &&
                         gr_vk_draw(c, 1, tex, clut, px, &fell, &sync);
        unsigned differ = 0, kept = 0;
        for (uint32_t i = 0; i < GR_N; i++) {
            if (px[i] != ref[i]) differ++;
            if (ref[i] != plain[i]) kept++;
        }
        if (!done || sync != GEGPU_SYNC_OK || fell != 0u || differ != 0u || kept == 0u) {
            fprintf(stderr, "gpu coherence selftest [clear-masks/%s]: %s, fell %llu, sync %d, %u of %u pixels "
                            "differ from ge.c, ge.c's mask kept %u pixels\n", c->name,
                    done ? "drawn" : "the list did not reach its END", fell, sync, differ, (unsigned)GR_N, kept);
            ok = 0;
        }
    }
    *s_ge = saved;
    coherence_reset_targets();
    ge_set_gpu_hooks(&k_hooks);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s %u clears through all-or-nothing pixel masks drawn on "
               "the GPU equal ge.c\n", "clear-masks", (unsigned)(sizeof(k_cells) / sizeof(k_cells[0])));
    return ok;
}

static int coherence_run_psp_raster(void) {
    if (s_scale != 1) {
        fprintf(stderr, "gpu coherence selftest [psp-raster]: compares PSP pixels, so it runs only at render "
                        "scale 1, not %d\n", s_scale);
        return 0;
    }
    enum { GR_VK_SOFTWARE = sizeof(k_gr_vk_software) / sizeof(k_gr_vk_software[0]),
           GR_VK_DIVERGENT = sizeof(k_gr_vk_divergent) / sizeof(k_gr_vk_divergent[0]) };
    const uint32_t clut = 0x08618000u;
    uint32_t tex[GR_T_COUNT];
    for (uint32_t t = 0; t < GR_T_COUNT; t++) {
        tex[t] = 0x08610000u + t * 0x400u;
        (void)gr_texture(t, (uint8_t *)SR_HOST(tex[t]));
    }
    gr_clut((uint8_t *)SR_HOST(clut));
    GeState saved = *s_ge;
    unsigned gpu_match = 0, gpu_div = 0, sw_cells = 0, div_exact = 0;
    uint32_t worst_div = 0;
    int ok = 1;
    for (uint32_t k = 0; k < GR_CELL_COUNT; k++) {
        const struct gr_cell *c = &gr_cells[k];
        uint32_t px[GR_N], ref[GR_N];
        unsigned long long fell = 0, ref_fell = 0;
        int sync = 0, ref_sync = 0;
        if (strcmp(s_gr_psp[k].name, c->name) != 0) {
            fprintf(stderr, "gpu coherence selftest [psp-raster]: digest row %u names %s, not %s\n",
                    (unsigned)k, s_gr_psp[k].name, c->name);
            ok = 0;
            continue;
        }
        /* ge.c alone gives the PSP's window (test_ge_raster_cells_match_psp): the reference a
         * divergent GPU window is measured against, pixel by pixel. */
        if (!gr_vk_draw(c, 0, tex, clut, ref, &ref_fell, &ref_sync) ||
            !gr_vk_draw(c, 1, tex, clut, px, &fell, &sync)) {
            fprintf(stderr, "gpu coherence selftest [psp-raster/%s]: the list did not reach its END\n", c->name);
            ok = 0;
            continue;
        }
        const int want_sw = gr_vk_named(k_gr_vk_software, GR_VK_SOFTWARE, c->name);
        const int want_div = gr_vk_named(k_gr_vk_divergent, GR_VK_DIVERGENT, c->name);
        const int ref_ok = gr_digest(ref) == s_gr_psp[k].psp;
        const int matches = gr_digest(px) == s_gr_psp[k].psp;
        /* Drawn on the GPU: no primitive fell back and the GPU target was read back. Drawn by the
         * software fallback: some primitive fell back and the readback did not fail. Anything else
         * (a failed readback, or nothing drawn at all) is neither and fails. */
        const int on_gpu = sync == GEGPU_SYNC_OK && fell == 0u;
        const int on_sw = fell > 0u && sync != GEGPU_SYNC_FAILED;
        uint32_t delta = 0;
        for (uint32_t i = 0; i < GR_N; i++) {
            const uint32_t d = gr_vk_channel_delta(c->psm, px[i], ref[i]);
            if (d > delta) delta = d;
        }
        const char *got = on_gpu ? (matches ? "GPU, equal to the PSP" : "GPU, not equal to the PSP")
                        : on_sw ? (matches ? "software fallback, equal to the PSP"
                                           : "software fallback, not equal to the PSP")
                        : "neither drawn on the GPU nor by the fallback";
        int pass;
        if (!ref_ok) pass = 0;
        else if (want_sw) pass = on_sw && matches;
        else if (want_div) pass = on_gpu && delta <= GR_VK_DIVERGENT_MAX_DELTA;   /* equal included */
        else pass = on_gpu && matches;
        if (on_gpu && !matches) {
            gpu_div++;
            if (delta > worst_div) worst_div = delta;
        } else if (on_gpu) {
            gpu_match++;
            if (want_div) div_exact++;
        } else if (on_sw) {
            sw_cells++;
        }
        if (pass) continue;
        ok = 0;
        fprintf(stderr, "gpu coherence selftest [psp-raster/%s]: %s (fell %llu, sync %d), expected %s; "
                "largest field difference from ge.c %u; ge.c %s the PSP; gpu=0x%016llx psp=0x%016llx",
                c->name, got, fell, sync,
                want_sw ? "software fallback, equal to the PSP"
                        : want_div ? "GPU, within 2 per field of ge.c (which equals the PSP)" : "GPU, equal to the PSP",
                (unsigned)delta, ref_ok ? "equals" : "does not equal",
                (unsigned long long)gr_digest(px), (unsigned long long)s_gr_psp[k].psp);
        for (uint32_t i = 0; i < GR_N; i++)
            fprintf(stderr, "%s%08x", i % GR_WW ? " " : "\n  ", (unsigned)px[i]);
        fprintf(stderr, "\n");
    }
    for (size_t n = 0; n < GR_VK_SOFTWARE + GR_VK_DIVERGENT; n++) {
        const char *name = n < GR_VK_SOFTWARE ? k_gr_vk_software[n] : k_gr_vk_divergent[n - GR_VK_SOFTWARE];
        uint32_t k = 0;
        while (k < GR_CELL_COUNT && strcmp(gr_cells[k].name, name) != 0) k++;
        if (k == GR_CELL_COUNT) {
            fprintf(stderr, "gpu coherence selftest [psp-raster/%s]: no such cell\n", name);
            ok = 0;
        }
    }
    *s_ge = saved;
    coherence_reset_targets();
    ge_set_gpu_hooks(&k_hooks);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s PSP-GE-RASTER-001 at 1x: %u cells drawn on the GPU "
               "equal the PSP (%u of them pinned as rounding cells, exact on this driver), %u differ as "
               "pinned (at most %u per field), %u drawn by the software fallback equal the PSP (%u cells)\n",
               "psp-raster", gpu_match, div_exact, gpu_div, (unsigned)worst_div, sw_cells,
               (unsigned)GR_CELL_COUNT);
    return ok;
}

/* ---- host-depth: the 32-bit host depth buffer (Ace depth=32, #2051) ----------------------------
 *
 * Runs with the key off (`--case host-depth`) and on (`--depth 32 --case host-depth`), at the
 * render scales the recipe names. Every quad is a through-mode pair of triangles whose depth runs
 * left to right between two integer steps, so its pixels hold depth with a fraction (k/32 of a
 * step at 1x, never one half, so rounding to 16 bits has one answer at every scale). The first quad
 * of each strip is drawn with the test ALWAYS and depth writes on.
 *   - z-fight: a second surface with depth writes on and GEQUAL lies behind the first by between
 *     one step and a small fraction of one. At 16 bits most pixels round to the same step, GEQUAL
 *     passes, and the far surface covers the near one in stripes; with the key every pixel keeps
 *     the near one. This half fails without the key, which is the point of the key.
 *   - decal: a quad on the first one's plane (other vertices, depth writes off) with GEQUAL, and in
 *     a second strip with LEQUAL, covers every pixel it spans with the key on and off.
 *   - depth-equal: the first quad's own vertices again with EQUAL and depth writes off cover every
 *     pixel; the same quad one step nearer covers none.
 *   - upload: depth written by the CPU (guest memory, depth_from_cpu) for every 16-bit value reads
 *     back unchanged, and a quad at the step the CPU wrote passes EQUAL against it, one a step
 *     away does not.
 *   - readback: the scene's depth committed to guest VRAM. With SR_GPU_DEPTH_REF the key-off run
 *     writes its scale and the hash of those bytes and the key-on run must match them: the guest
 *     reads the same 16-bit depth either way. */
typedef struct { uint32_t c; float x0, x1, y0, y1, zl, zr; uint32_t zfunc, zwrite; } HdQuad;

#define HD_BASE (0x04000000u | 0x00160000u)
#define HD_ZBA 0x00080000u
#define HD_VTX 0x00102000u
#define HD_LIST 0x00100800u
#define HD_NEAR 0xFF3366CCu
#define HD_FAR 0xFF20C040u
#define HD_DECAL 0xFFC02080u
#define HD_EQUAL 0xFF80C0F0u

/* Draws the quads in order on the 8888 target at HD_BASE with depth at HD_ZBA, through the Vulkan
 * hooks. The depth surface keeps what it holds (the CPU's values on a fresh surface). */
static int hd_draw(const HdQuad *q, int n) {
    ThroughFloatVtx *v = (ThroughFloatVtx *)SR_HOST(HD_VTX);
    uint32_t *dl = (uint32_t *)SR_HOST(HD_LIST);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, HD_BASE & 0x00FFFFFFu);
    DL_EMIT(0x9D, 512u | ((HD_BASE & 0xFF000000u) >> 8));
    DL_EMIT(0x9E, HD_ZBA);
    DL_EMIT(0x9F, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x1E, 0);                                   /* no texture */
    DL_EMIT(0x1D, 0);                                   /* no culling */
    DL_EMIT(0x21, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x23, 1);                                   /* depth test */
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | 479);
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD6, 0);
    DL_EMIT(0xD7, 0xFFFF);
    DL_EMIT(0x12, (1u << 23) | (7u << 2) | (3u << 7)); /* through, 8888 colour, float pos */
    for (int i = 0; i < n; i++) {
        ThroughFloatVtx *t = v + 6 * i;
        t[0] = (ThroughFloatVtx){ q[i].c, q[i].x0, q[i].y0, q[i].zl };
        t[1] = (ThroughFloatVtx){ q[i].c, q[i].x1, q[i].y0, q[i].zr };
        t[2] = (ThroughFloatVtx){ q[i].c, q[i].x0, q[i].y1, q[i].zl };
        t[3] = (ThroughFloatVtx){ q[i].c, q[i].x1, q[i].y0, q[i].zr };
        t[4] = (ThroughFloatVtx){ q[i].c, q[i].x1, q[i].y1, q[i].zr };
        t[5] = (ThroughFloatVtx){ q[i].c, q[i].x0, q[i].y1, q[i].zl };
        DL_EMIT(0xDE, q[i].zfunc);
        DL_EMIT(0xE7, q[i].zwrite ? 0 : 1);
        DL_EMIT(0x01, (HD_VTX + (uint32_t)(i * 6) * (uint32_t)sizeof(ThroughFloatVtx)) & 0x00FFFFFFu);
        DL_EMIT(0x04, (3u << 16) | 6u);
    }
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(HD_LIST, 0);
    GeGpuFbDescriptor desc = { .addr = HD_BASE, .format = 3, .stride = 512u, .width = 480, .height = 272 };
    return gegpu_sync_guest_fb(&desc);
}

/* A fresh target and depth surface, the depth surface seeded from the CPU with `z` everywhere. */
static int hd_reset(uint16_t z) {
    coherence_reset_targets();
    coherence_reset_depths();
    memset(SR_HOST(HD_BASE), 0, (size_t)512u * FB_H * 4u);
    sr_gpu_vram_dirty(HD_BASE, 512u * FB_H * 4u);
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->zbp = 0x04000000u | HD_ZBA; s_ge->zbw = 512u;
    if (!ge_zbuf_prepare_current() || !(s_zbuf = ge_zbuf_ptr())) return 0;
    for (uint32_t i = 0; i < 512u * FB_H; i++) s_zbuf[i] = z;
    ge_zbuf_note_external_write();
    return 1;
}

/* Pixels of [x0,x1) x [y0,y1) in guest VRAM whose RGB is `c`. */
static uint32_t hd_count(uint32_t c, int x0, int x1, int y0, int y1) {
    const uint32_t *g = (const uint32_t *)SR_HOST(HD_BASE);
    uint32_t n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += (g[(uint32_t)y * 512u + (uint32_t)x] & 0x00FFFFFFu) == (c & 0x00FFFFFFu);
    return n;
}

static DepthEnt *hd_depth(void) {
    for (int i = 0; i < MAX_DEP; i++)
        if (s_deps[i].used && s_deps[i].zba == HD_ZBA && s_deps[i].zstride == 512u) return &s_deps[i];
    return NULL;
}

static int coherence_run_host_depth(void) {
    const char *name = "host-depth";
    const char *mode = s_depth32 ? "depth=32" : "16-bit";
    int ok = 1;
    if (g_ace_on && ace_effective()->depth == 32 && !s_depth32) {
        printf("gpu coherence selftest: SKIP %s: depth=32 was requested but this device refused it\n", name);
        return 1;
    }

    /* z-fight: near 1000 -> 1008 over 128 pixels, far 999 -> 1008: the gap closes from one step to
     * 1/256 of one across the strip. */
    {
        const HdQuad q[2] = {
            { HD_NEAR, 16.0f, 144.0f, 16.0f, 48.0f, 1000.0f, 1008.0f, 1u, 1u },
            { HD_FAR, 16.0f, 144.0f, 16.0f, 48.0f, 999.0f, 1008.0f, 7u, 1u },
        };
        ok = hd_reset(0) && hd_draw(q, 2);
        const uint32_t px = 128u * 32u, near = hd_count(HD_NEAR, 16, 144, 16, 48),
                       far = hd_count(HD_FAR, 16, 144, 16, 48);
        if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: z-fight did not draw\n", name);
        else if (near + far != px || (s_depth32 ? far != 0u : (far == 0u || near == 0u))) {
            fprintf(stderr, "gpu coherence selftest [%s]: z-fight (%s): near %u, far %u of %u pixels (want %s)\n",
                    name, mode, near, far, px,
                    s_depth32 ? "every pixel near" : "both: the far surface wins where the steps tie");
            ok = 0;
        } else {
            printf("gpu coherence selftest: %s z-fight (%s): near %u, far %u of %u pixels\n", name, mode, near,
                   far, px);
        }
    }

    /* decal: base 1000 -> 1008 over x 16..144; the decal over x 48..112 lies on it (1002 -> 1006). */
    for (int f = 0; ok && f < 2; f++) {
        const uint32_t func = f ? 5u : 7u;                       /* LEQUAL, GEQUAL */
        const HdQuad q[2] = {
            { HD_NEAR, 16.0f, 144.0f, 64.0f, 96.0f, 1000.0f, 1008.0f, 1u, 1u },
            { HD_DECAL, 48.0f, 112.0f, 64.0f, 96.0f, 1002.0f, 1006.0f, func, 0u },
        };
        ok = hd_reset(0) && hd_draw(q, 2);
        const uint32_t px = 64u * 32u, decal = ok ? hd_count(HD_DECAL, 48, 112, 64, 96) : 0u;
        if (!ok || decal != px) {
            fprintf(stderr, "gpu coherence selftest [%s]: decal %s (%s): %u of %u pixels show the decal\n", name,
                    f ? "LEQUAL" : "GEQUAL", mode, decal, px);
            ok = 0;
        }
    }

    /* depth-equal: the base's own vertices with EQUAL cover it; one step nearer covers nothing. */
    for (int f = 0; ok && f < 2; f++) {
        const HdQuad q[2] = {
            { HD_NEAR, 16.0f, 144.0f, 16.0f, 48.0f, 1000.0f, 1008.0f, 1u, 1u },
            { HD_EQUAL, 16.0f, 144.0f, 16.0f, 48.0f, 1000.0f + (float)f, 1008.0f + (float)f, 2u, 0u },
        };
        ok = hd_reset(0) && hd_draw(q, 2);
        const uint32_t px = 128u * 32u, eq = ok ? hd_count(HD_EQUAL, 16, 144, 16, 48) : 0u;
        if (!ok || eq != (f ? 0u : px)) {
            fprintf(stderr, "gpu coherence selftest [%s]: depth-equal%s (%s): %u of %u pixels pass EQUAL (want %u)\n",
                    name, f ? " one step nearer" : "", mode, eq, px, f ? 0u : px);
            ok = 0;
        }
    }

    /* upload: every 16-bit value through depth_from_cpu and back; then EQUAL against a CPU step. */
    if (ok) {
        ok = hd_reset(0);
        /* (k * 40503) mod 65536 over the 130560 visible pixels k: every 16-bit value, most twice */
        for (uint32_t y = 0; ok && y < FB_H; y++)
            for (uint32_t x = 0; x < FB_W; x++) s_zbuf[y * 512u + x] = (uint16_t)((y * FB_W + x) * 40503u);
        DepthEnt *d = ok ? depth_acquire(HD_ZBA, 512u) : NULL;
        uint8_t *dev = (uint8_t *)malloc((size_t)SCL_W * SCL_H * 4u);
        ok = d && dev && coherence_read_device(d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, DEPTH_BPP, dev);
        uint32_t seen = 0;
        for (uint32_t y = 0; ok && y < FB_H; y++)
            for (uint32_t x = 0; ok && x < FB_W; x++) {
                const uint16_t want = s_zbuf[y * 512u + x];
                const uint16_t got = depth_texel(dev, ((size_t)y * SCL_W + x) * (uint32_t)s_scale);
                if (got != want) {
                    fprintf(stderr, "gpu coherence selftest [%s]: upload (%s) (%u,%u): read 0x%04x, wrote 0x%04x\n",
                            name, mode, x, y, got, want);
                    ok = 0;
                }
                seen++;
            }
        free(dev);
        static const uint16_t steps[] = { 0u, 1u, 1000u, 12345u, 32768u, 65534u, 65535u };
        for (unsigned i = 0; ok && i < sizeof steps / sizeof steps[0]; i++)
            for (int off = 0; ok && off < 2; off++) {
                const uint16_t z = steps[i];
                const float qz = off ? (z < 65535u ? (float)z + 1.0f : (float)z - 1.0f) : (float)z;
                const HdQuad q = { HD_EQUAL, 16.0f, 48.0f, 16.0f, 48.0f, qz, qz, 2u, 0u };
                ok = hd_reset(z) && hd_draw(&q, 1);
                const uint32_t eq = ok ? hd_count(HD_EQUAL, 16, 48, 16, 48) : 0u;
                if (!ok || eq != (off ? 0u : 32u * 32u)) {
                    fprintf(stderr, "gpu coherence selftest [%s]: upload (%s): a quad at %.0f against the CPU's "
                            "step %u passes EQUAL at %u of 1024 pixels (want %u)\n", name, mode, (double)qz, z,
                            eq, off ? 0u : 1024u);
                    ok = 0;
                }
            }
        if (ok && seen != FB_W * FB_H) ok = 0;
    }

    /* readback: a z-fight scene's depth committed to guest VRAM. */
    uint64_t hash = 0xcbf29ce484222325ull;
    if (ok) {
        /* A z-fight whose outcome the key changes. Through mode cuts a vertex's z to a whole step
         * (ge.c), so fractions come only from interpolation. The near ramp climbs 8 steps a guest
         * pixel, which is a whole step at every device pixel centre at scales 1, 2 and 4; the far ramp
         * starts a step behind it and closes to a ninth of a step at the strip's end (it runs on to x 160,
         * past the near one, where it fails against the seed), so with depth=32 it fails by a clear
         * margin, not a few float ulps. At 16 bits the far surface passes
         * GEQUAL only where it rounds to the near one's step, and then writes that same step; with
         * depth=32 it fails everywhere and the near step stays. Either way each pixel stores the near
         * step, which a D16 attachment holds exactly, so no stored value lies near a half step (where a
         * D16 conversion's rounding is the driver's: lavapipe stores 17/32 of a step as the lower one).
         * The steep quad's depth is whole at every device pixel centre too. */
        const HdQuad q[3] = {
            { HD_NEAR, 16.0f, 144.0f, 16.0f, 48.0f, 1000.0f, 2024.0f, 1u, 1u },
            { HD_FAR, 16.0f, 160.0f, 16.0f, 48.0f, 999.0f, 2152.0f, 7u, 1u },
            { HD_NEAR, 160.0f, 400.0f, 100.0f, 260.0f, 60000.0f, 56160.0f, 1u, 1u },
        };
        ok = hd_reset(0x1234u) && hd_draw(q, 3);
        /* The scene must show the key's effect, or the hash would match whatever the key did. */
        const uint32_t far_px = ok ? hd_count(HD_FAR, 16, 144, 16, 48) : 0u;
        if (ok && (s_depth32 ? far_px != 0u : far_px == 0u)) {
            fprintf(stderr, "gpu coherence selftest [%s]: readback scene (%s): the far ramp shows at %u pixels "
                    "(want %s)\n", name, mode, far_px, s_depth32 ? "none" : "some, where the steps tie");
            ok = 0;
        }
        DepthEnt *d = ok ? hd_depth() : NULL;
        if (d) d->guest_dirty = 1;
        ok = d && depth_readback_to_guest(d);
        uint32_t first = 0, end = 0;
        ok = ok && ge_zbuf_guest_span_bounds(0x04000000u + HD_ZBA, 512u, &first, &end);
        const uint8_t *g = ok ? (const uint8_t *)SR_HOST_LINEAR(first) : NULL;
        for (uint32_t i = 0; ok && i < end - first; i++) { hash ^= g[i]; hash *= 0x100000001b3ull; }
        if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: readback (%s) did not run\n", name, mode);
    }
    const char *ref_path = getenv("SR_GPU_DEPTH_REF");
    /* The span's bytes go beside the reference (<ref>.raw), so a mismatch names its pixels. */
    char raw_path[600];
    snprintf(raw_path, sizeof(raw_path), "%s.raw", ref_path ? ref_path : "");
    uint32_t span_first = 0, span_end = 0;
    if (ok) (void)ge_zbuf_guest_span_bounds(0x04000000u + HD_ZBA, 512u, &span_first, &span_end);
    if (ok && ref_path && *ref_path) {
        if (!s_depth32) {
            FILE *f = fopen(ref_path, "w");
            ok = f && fprintf(f, "scale=%d hash=%016llx\n", s_scale, (unsigned long long)hash) > 0;
            if (f && fclose(f) != 0) ok = 0;
            FILE *r = ok ? fopen(raw_path, "wb") : NULL;
            if (r) {
                (void)fwrite(SR_HOST_LINEAR(span_first), 1, span_end - span_first, r);
                fclose(r);
            }
            if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: cannot write %s\n", name, ref_path);
        } else {
            FILE *f = fopen(ref_path, "r");
            int scale = 0;
            unsigned long long want = 0;
            ok = f && fscanf(f, "scale=%d hash=%llx", &scale, &want) == 2;
            if (f) fclose(f);
            if (!ok) fprintf(stderr, "gpu coherence selftest [%s]: no key-off reference in %s\n", name, ref_path);
            else if (scale != s_scale || want != (unsigned long long)hash) {
                fprintf(stderr, "gpu coherence selftest [%s]: readback with depth=32 at scale %d: hash %016llx, "
                        "the key-off run at scale %d: %016llx\n", name, s_scale, (unsigned long long)hash, scale,
                        want);
                FILE *r = fopen(raw_path, "rb");
                const uint32_t n = (span_end - span_first) / 2u;
                uint16_t *off = r ? (uint16_t *)malloc((size_t)n * 2u) : NULL;
                if (off && fread(off, 2u, n, r) == n) {
                    const uint16_t *on = (const uint16_t *)SR_HOST_LINEAR(span_first);
                    uint32_t diffs = 0;
                    for (uint32_t i = 0; i < n; i++) {
                        if (off[i] == on[i]) continue;
                        if (diffs++ < 12)
                            fprintf(stderr, "gpu coherence selftest [%s]:   x=%u y=%u key off %u, depth=32 %u\n", name,
                                    i % 512u, i / 512u, off[i], on[i]);
                    }
                    fprintf(stderr, "gpu coherence selftest [%s]:   %u of %u depth values differ\n", name, diffs, n);
                }
                free(off);
                if (r) fclose(r);
                ok = 0;
            }
        }
    }
    coherence_reset_targets();
    coherence_reset_depths();
    if (ok)
        printf("gpu coherence selftest: PASS %-22s %s scale=%d: z-fight %s, decals and depth-equal as at 16 "
               "bits, every 16-bit value uploads and reads back unchanged, readback hash %016llx%s\n", name, mode,
               s_scale, s_depth32 ? "gone" : "present", (unsigned long long)hash,
               ref_path && *ref_path ? (s_depth32 ? " (equals the key-off run)" : " (reference written)") : "");
    return ok;
}

static int coherence_selftest_cases(void) {
    /* SR_GPU_COHERENCE_CASE=scaled-upload (the harness's --case) runs that case alone, as the
     * recipe does at render scales 2 and 4: the other fixtures expect 1x pixels (a filtered sample
     * lands on other device pixels at a higher scale). Any other value fails. */
    const char *only = getenv("SR_GPU_COHERENCE_CASE");
    if (only && *only) {
        if (strcmp(only, "scaled-upload") == 0) return coherence_run_scaled_upload();
        if (strcmp(only, "mip-lod") == 0) return coherence_run_mip_lod();
        if (strcmp(only, "mip-clut") == 0) return coherence_run_mip_clut();
        if (strcmp(only, "readback-rows") == 0) return coherence_run_readback_rows();
        if (strcmp(only, "sprite-scaled") == 0) return coherence_run_sprite_scaled();
        if (strcmp(only, "uv-edge-clamp") == 0) return coherence_run_uv_edge_clamp();
        if (strcmp(only, "undrawn-rows") == 0) return coherence_run_undrawn_rows();
        if (strcmp(only, "through-float-z") == 0) return coherence_run_through_float_z();
        if (strcmp(only, "clut-stride-padding") == 0) return coherence_run_clut_stride_padding();
        if (strcmp(only, "depth-alias-no-zwrite") == 0) return coherence_run_depth_alias_no_zwrite();
        if (strcmp(only, "wide-fill") == 0) return coherence_run_wide_fill();
        if (strcmp(only, "patch-tess") == 0) return coherence_run_patch_tess();
        if (strcmp(only, "gpu-timestamps") == 0) return coherence_run_gpu_timestamps();
        if (strcmp(only, "gpu-timestamps-off") == 0) return coherence_run_gpu_timestamps_off();
        if (strcmp(only, "submit-batch") == 0) return coherence_run_submit_batch();   /* the snapbar mutants */
        if (strcmp(only, "depth-evict-newer") == 0) return coherence_run_depth_evict_newer();
        if (strcmp(only, "depth-zbp-switch") == 0) return coherence_run_depth_zbp_switch();
        if (strcmp(only, "depth-zbw-switch") == 0) return coherence_run_depth_zbw_switch();
        if (strcmp(only, "depth-evict-overlap") == 0) return coherence_run_depth_evict_overlap();
        if (strcmp(only, "depth-evict-holder") == 0) return coherence_run_depth_evict_holder();
        if (strcmp(only, "twin-scene") == 0) return coherence_run_twin_scene();
        if (strcmp(only, "ssaa-edge") == 0) return coherence_run_ssaa_edge();
        if (strcmp(only, "ssaa-sprite") == 0) return coherence_run_ssaa_sprite();
        if (strcmp(only, "ssaa-limit") == 0) return coherence_run_ssaa_limit();
        if (strcmp(only, "twin-sync") == 0) return coherence_run_twin_sync();
        if (strcmp(only, "twin-gate") == 0) return coherence_run_twin_gate();
        if (strcmp(only, "twin-alloc-fail") == 0) return coherence_run_twin_alloc_fail();
        if (strcmp(only, "twin-drain-fail") == 0) return coherence_run_twin_drain_fail();
        if (strcmp(only, "twin-soft") == 0) return coherence_run_twin_soft();
        if (strcmp(only, "twin-key") == 0) return coherence_run_twin_key();
        if (strcmp(only, "pipeline-cache-cold") == 0) return coherence_run_pipeline_cache(0);
        if (strcmp(only, "pipeline-cache-warm") == 0) return coherence_run_pipeline_cache(1);
        if (strcmp(only, "pipeline-cache-reject") == 0) return coherence_run_pipeline_cache_reject();
        if (strcmp(only, "dst-read") == 0) return coherence_run_dst_read();
        if (strncmp(only, "dst-read-", 9) == 0) return coherence_run_dst_read_one(only);
        if (strcmp(only, "stencil-once") == 0) return coherence_run_stencil_once();
        if (strcmp(only, "rtt-scale") == 0) return coherence_run_rtt_scale();
        if (strcmp(only, "color-test") == 0) return coherence_run_color_test();
        if (strcmp(only, "lines") == 0) return coherence_run_lines();
        if (strcmp(only, "host-depth") == 0) return coherence_run_host_depth();
        if (strcmp(only, "wide-raster") == 0) return coherence_run_wide_raster();
        if (strcmp(only, "psp-raster") == 0) return coherence_run_psp_raster();
        if (strcmp(only, "clear-masks") == 0) return coherence_run_clear_masks();
        fprintf(stderr, "gpu coherence selftest: unknown SR_GPU_COHERENCE_CASE '%s'\n", only);
        return 0;
    }
    static const CoherenceCase cases[] = {
        { "8888-middle", 3, 512, (91u * 512u + 137u) * 4u, 4, 0, 0, 0 },
        { "5650-unaligned", 0, 512, (42u * 512u + 61u) * 2u + 1u, 3, 0, 0, 0 },
        { "5551-unaligned", 1, 512, (43u * 512u + 62u) * 2u + 1u, 3, 0, 0, 0 },
        { "4444-unaligned", 2, 512, (44u * 512u + 63u) * 2u + 1u, 3, 0, 0, 0 },
        { "8888-unaligned", 3, 512, (45u * 512u + 64u) * 4u + 1u, 5, 0, 0, 0 },
        { "row-start", 3, 512, 10u * 512u * 4u, 4, 0, 0, 0 },
        { "row-end", 3, 512, (11u * 512u + 511u) * 4u, 4, 0, 0, 0 },
        { "row-crossing", 3, 512, (12u * 512u + 511u) * 4u + 2u, 6, 0, 0, 0 },
        { "multi-row", 3, 512, (20u * 512u + 500u) * 4u, (12u + 512u + 17u) * 4u, 0, 0, 0 },
        { "stride-500", 3, 500, (30u * 500u + 499u) * 4u + 1u, 11, 0, 0, 0 },
        { "pending-batch", 3, 512, (70u * 512u + 90u) * 4u, 8, 1, 0, 0 },
        { "pending-async", 3, 512, (71u * 512u + 91u) * 4u, 8, 0, 1, 0 },
        { "alias-vram", 3, 512, (72u * 512u + 92u) * 4u, 8, 0, 0, 1 },
    };
    int ok = 1;
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!coherence_run_case(&cases[i])) ok = 0;
    if (!coherence_run_clean_case("clean-aligned", 3, 512, (50u * 512u + 20u) * 4u, 16)) ok = 0;
    if (!coherence_run_clean_case("clean-unaligned", 3, 512, (51u * 512u + 21u) * 4u + 1u, 5)) ok = 0;
    if (!coherence_run_full_cover_case()) ok = 0;
    if (!coherence_run_cpu_write_coalesce()) ok = 0;
    if (!coherence_run_overlap_case()) ok = 0;
    if (!coherence_run_tflt_descriptor_capacity()) ok = 0;
    if (!coherence_run_sprite_semantics()) ok = 0;
    if (!coherence_run_software_sprite_semantics()) ok = 0;
    if (!coherence_run_wide_raster()) ok = 0;
    if (!coherence_run_linear_sprite()) ok = 0;
    if (!coherence_run_tflt_filters()) ok = 0;
    if (!coherence_run_ring_wrap()) ok = 0;
    if (!coherence_run_submit_batch()) ok = 0;
    if (!coherence_run_upload_sequence()) ok = 0;
    if (!coherence_run_texture_dirty_same_state()) ok = 0;
    if (!coherence_run_texture_clean_page_index()) ok = 0;
    if (!coherence_run_texture_ram_unnotified_change()) ok = 0;
    if (!coherence_run_clut_format_same_palette()) ok = 0;
    if (!coherence_run_clut_format_list()) ok = 0;
    if (!coherence_run_through_float_z()) ok = 0;
    if (!coherence_run_blend_parity()) ok = 0;
    if (!coherence_run_undrawn_rows()) ok = 0;
    if (!coherence_run_clut_stride_padding()) ok = 0;
    if (!coherence_run_depth_alias_no_zwrite()) ok = 0;
    if (!coherence_run_scalar_vram_read(0x04080000u)) ok = 0;
    /* zbp 0x0a2000: bit 13 set, so the absolute XOR differs from the within-buffer form (0x0a6854 vs
     * 0x0a2854 for pixel 10,10). The 0x0ce000 placement used by the software selftest would overlap the
     * colour surface at 0x100000 here. */
    if (!coherence_run_scalar_vram_read(0x040A2000u)) ok = 0;
    if (!coherence_run_native_bulk_copy()) ok = 0;
    if (!coherence_run_fallback_reasons()) ok = 0;
    /* Last: its chain-fallback case draws through the software fallback, which hands the
     * current depth surface to ge.c, and the fixtures above start from the depth state the
     * ones before them leave. */
    if (!coherence_run_mip_levels()) ok = 0;
    if (!coherence_run_psp_sample_points()) ok = 0;
    if (!coherence_run_psp_raster()) ok = 0;
    if (!coherence_run_clear_masks()) ok = 0;
    if (!coherence_run_sprite_scaled()) ok = 0;
    if (!coherence_run_uv_edge_clamp()) ok = 0;
    /* After the mip fixtures: it rewrites the depth buffer they start from. */
    if (!coherence_run_stencil_parity()) ok = 0;
    if (!coherence_run_stencil_constant()) ok = 0;
    if (!coherence_run_color_test()) ok = 0;
    if (!coherence_run_lines()) ok = 0;
    if (!coherence_run_dst_read()) ok = 0;
    if (!coherence_run_stencil_once()) ok = 0;
    if (!coherence_run_tnl_parity()) ok = 0;
    /* Last: it rewrites ge.c's depth surface. */
    if (!coherence_run_scaled_upload()) ok = 0;
    if (!coherence_run_readback_rows()) ok = 0;
    if (!coherence_run_stacked_targets()) ok = 0;
    if (!coherence_run_wide_fill()) ok = 0;
    if (!coherence_run_patch_tess()) ok = 0;
    /* Last: they free every depth surface the cases above share (the twin cases too). */
    if (!coherence_run_depth_evict_newer()) ok = 0;
    if (!coherence_run_depth_zbp_switch()) ok = 0;
    if (!coherence_run_depth_zbw_switch()) ok = 0;
    if (!coherence_run_depth_evict_overlap()) ok = 0;
    if (!coherence_run_depth_evict_holder()) ok = 0;
    if (!coherence_run_twin_gate()) ok = 0;
    if (!coherence_run_twin_scene()) ok = 0;
    if (s_twin && s_tscale == s_scale && !coherence_run_twin_sync()) ok = 0;
    if (!coherence_run_rtt_scale()) ok = 0;
    if (!coherence_run_host_depth()) ok = 0;
    if (!coherence_run_ssaa_edge()) ok = 0;
    if (!coherence_run_ssaa_sprite()) ok = 0;
    if (!coherence_run_ssaa_limit()) ok = 0;
    coherence_reset_targets();
    return ok;
}

/* Source-owned regression for the explicit snapshot boundary. It first proves that
 * ordinary presentation remains asynchronous (the pre-fix lag), then asks the same
 * target through gegpu_sync_guest_fb() and verifies that guest VRAM is current before
 * publication. Descriptor rejection and no-target answers are checked too. */
/* Ace Mode's texture filtering (ace.h), run with SR_ACE asking for 16x on a device that has it
 * (gpu-ace-selftest). Through-mode draws keep the guest's sampling exactly: the PSP-measured
 * sample points still hold and no anisotropic sampler is made for them. A transformed draw that
 * minifies linearly gets one anisotropic sampler, made once and reused; a nearest-filtered
 * texture, and the same entry drawn in through mode, keep the plain set. */
int gegpu_ace_af_selftest(void) {
    if (!s_ace_af) {
        fprintf(stderr, "gpu ace selftest: SKIP (this device has no anisotropic filtering)\n");
        return 77;
    }
    if (s_tex_pool_sets != 2u * MAX_TEX) {
        fprintf(stderr, "gpu ace selftest: FAIL texture pool has %u sets, expected %u\n",
                s_tex_pool_sets, 2u * MAX_TEX);
        return 0;
    }
    if (!coherence_run_psp_sample_points()) return 0;
    if (s_cnt_af_samplers != 0u) {
        fprintf(stderr, "gpu ace selftest: FAIL through-mode draws made %llu anisotropic samplers\n",
                (unsigned long long)s_cnt_af_samplers);
        return 0;
    }
    /* The list left the 32x32 texture bound. Draw it transformed, then nearest, then through. */
    GeState *g = s_ge;
    const uint32_t through = 1u << 23;
    g->vtype &= ~through;
    g->tex_filter = 0x101u;                                  /* linear min and mag */
    VkDescriptorSet aniso = tex_get();
    VkDescriptorSet again = tex_get();
    int ok = s_cnt_af_samplers == 1u && aniso == again && aniso != s_white_set;
    if (!ok) fprintf(stderr, "gpu ace selftest: FAIL transformed linear draw: samplers=%llu same=%d\n",
                     (unsigned long long)s_cnt_af_samplers, aniso == again);
    g->tex_filter = 0x100u;                                  /* nearest min, linear mag */
    VkDescriptorSet nearest = tex_get();
    if (ok && (s_cnt_af_samplers != 1u || nearest == aniso)) {
        fprintf(stderr, "gpu ace selftest: FAIL nearest minification got anisotropy\n");
        ok = 0;
    }
    g->tex_filter = 0x101u;
    g->vtype |= through;
    VkDescriptorSet plain = tex_get();
    if (ok && (s_cnt_af_samplers != 1u || plain == aniso)) {
        fprintf(stderr, "gpu ace selftest: FAIL a through-mode draw bound the anisotropic set\n");
        ok = 0;
    }
    if (ok) printf("gpu ace selftest: PASS af=%d through-mode exact, transformed linear filtered, "
                   "nearest kept\n", s_ace_af);
    return ok;
}

/* Ace smooth motion's HUD mask and depth (ace_hud.h), through ge_gpu.c's own hooks and a real
 * present: which draws ge_gpu.c notes as HUD (through mode, not clear mode, not sampling a
 * render target), that a box is in guest pixels (the mask covers exactly the pixels the draw
 * changed), that depth is handed on only after a draw wrote it (also when two display buffers
 * share one depth buffer), and that a published mask reaches only the image it was published
 * for. The show pass below stands in for gui.c's and takes what was published for each image. */
static struct {
    int calls, got;
    VkImage src;
    int have_bits;
    VkImage depth;
    uint32_t bits[ACE_SMOOTH_HUD_WORDS];
} s_ahc;

static void *ace_hud_cell_show(void *cmd, void *src, int *w, int *h) {
    (void)cmd; (void)w; (void)h;
    const AceSmoothAux *aux = ace_hud_take((VkImage)src);
    s_ahc.calls++;
    s_ahc.src = (VkImage)src;
    s_ahc.got = aux != NULL;
    s_ahc.have_bits = aux && aux->hud_bits;
    s_ahc.depth = aux ? aux->depth : VK_NULL_HANDLE;
    if (aux && aux->hud_bits) memcpy(s_ahc.bits, aux->hud_bits, sizeof(s_ahc.bits));
    else memset(s_ahc.bits, 0, sizeof(s_ahc.bits));
    return NULL;
}

static int ahc_bit(int x, int y) {
    uint32_t k = (uint32_t)(y * ACE_SMOOTH_HUD_W + x);
    return (int)((s_ahc.bits[k >> 5] >> (k & 31u)) & 1u);
}

static void ahc_state(uint32_t base, uint32_t zbp, int through) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = 512u;
    s_ge->fbfmt = 3u;
    s_ge->zbp = zbp;
    s_ge->zbw = 512u;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    if (through) s_ge->vtype = 1u << 23;
}

static int ahc_sprite(float x0, float y0, float x1, float y1, uint32_t c) {
    GeVtx p0, p1;
    ge_state_note_external_write();   /* the cell writes GE state directly, as a command would */
    wrap_vtx(&p0, x0, y0, c);
    wrap_vtx(&p1, x1, y1, c);
    p0.u = x0; p0.v = y0; p1.u = x1; p1.v = y1;
    return hook_sprite(&p0, &p1, 0);
}

/* Presents `base` and returns 1 when the show pass saw the image the present showed: the target's own,
 * or its host twin (#2050). gegpu_present keys the HUD mask by that image. */
static int ahc_present(uint32_t base, const char *step) {
    Target *t = target_find_by_fba(base & 0x00FFFFFFu);
    memset(&s_ahc, 0, sizeof(s_ahc));
    int pr = gegpu_present(base, 3, 512u);
    if (pr != GEGPU_PRESENT_SHOWN || !t || s_ahc.calls != 1 || s_ahc.src != present_image(t)) {
        fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: %s: present %d, show pass calls %d, "
                        "image %s\n", step, pr, s_ahc.calls, t && s_ahc.src == present_image(t) ? "right" : "wrong");
        return 0;
    }
    return 1;
}

static int coherence_run_ace_hud(void) {
    const uint32_t base_a = 0x04000000u, zbp = 0x04088000u, base_b = 0x040CC000u, base_c = 0x04154000u;
    const uint32_t hud = 0xFF332211u;
    int ok = 1;
    coherence_reset_targets();
    memset(SR_HOST(base_a), 0, 0x165000u);
    sr_gpu_vram_dirty(base_a, 0x165000u);
    ace_hud_enable(1);
    sdl3vk_set_show_pass(ace_hud_cell_show);
    if (!g_ace_hud_on) {
        fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: the HUD mask is not on\n");
        ok = 0;
    }

    /* A small render target the HUD frame will sample. */
    ok = ok && coherence_queue_g1_batch(base_c, 64u, 3u) && submit_pending();

    /* Frame 1 on A: a clear that fills depth, a through-mode sprite and triangle (HUD), a
     * transform-mode sprite (scene), and a through-mode sprite that samples target C. */
    ahc_state(base_a, zbp, 1);
    s_ge->clear = 1;
    s_ge->clear_mode = 0x501u;                  /* colour and depth */
    ok = ok && ahc_sprite(0.0f, 0.0f, 480.0f, 272.0f, 0x00000000u);
    s_ge->clear = 0;
    s_ge->clear_mode = 0;
    ok = ok && ahc_sprite(100.0f, 100.0f, 110.0f, 104.0f, hud);
    GeVtx ta, tb, tc;
    wrap_vtx(&ta, 40.0f, 40.0f, hud);
    wrap_vtx(&tb, 50.0f, 40.0f, hud);
    wrap_vtx(&tc, 40.0f, 50.0f, hud);
    ge_state_note_external_write();
    ok = ok && hook_tri(&ta, &tb, &tc, 0);
    s_ge->vtype = 0;
    ok = ok && ahc_sprite(200.0f, 50.0f, 210.0f, 60.0f, hud);
    s_ge->vtype = 1u << 23;
    s_ge->tex_enable = 1;
    s_ge->tex_addr = base_c;
    s_ge->tex_bufw = 64u;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 64;
    s_ge->tex_h = 64;
    s_ge->tex_func = 3;
    {
        /* The sprite binds target C's image directly (no readback): a post effect. */
        uint64_t rtt = s_cnt_rtt;
        ok = ok && ahc_sprite(300.0f, 200.0f, 320.0f, 210.0f, 0xFFFFFFFFu);
        if (ok && s_cnt_rtt != rtt + 1u) {
            fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: the sprite did not sample target C\n");
            ok = 0;
        }
    }
    s_ge->tex_enable = 0;
    ok = ok && submit_pending();
    Target *a = target_find_by_fba(base_a & 0x00FFFFFFu);
    DepthEnt *z = a ? a->dep : NULL;
    ok = ok && a && z && ahc_present(base_a, "frame 1");
    if (ok) {
        int wrong = 0;
        for (int y = 0; y < ACE_SMOOTH_HUD_H; y++)
            for (int x = 0; x < ACE_SMOOTH_HUD_W; x++) {
                int want = (x >= 100 && x < 110 && y >= 100 && y < 104) ||
                           (x >= 40 && x < 50 && y >= 40 && y < 50);
                wrong += ahc_bit(x, y) != want;
            }
        if (!s_ahc.have_bits || wrong || s_ahc.depth != z->img) {
            fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: frame 1: mask %d, %d bits wrong "
                            "(scene %d, post effect %d, clear %d), depth %s\n",
                    s_ahc.have_bits, wrong, ahc_bit(205, 55), ahc_bit(310, 205), ahc_bit(0, 0),
                    s_ahc.depth == z->img ? "handed on" : "missing");
            ok = 0;
        }
    }
    /* The box is in guest pixels: around the sprite, the mask is set exactly where it drew. */
    if (ok && target_readback(a)) {
        const uint32_t *px = (const uint32_t *)SR_HOST(base_a);
        int wrong = 0;
        for (int y = 90; y < 115; y++)
            for (int x = 90; x < 125; x++)
                wrong += ((px[y * 512 + x] & 0x00FFFFFFu) == (hud & 0x00FFFFFFu)) != ahc_bit(x, y);
        if (wrong) {
            fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: %d pixels around the HUD sprite differ "
                            "between what it drew and its mask\n", wrong);
            ok = 0;
        }
    } else if (ok) {
        fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: readback failed\n");
        ok = 0;
    }

    /* Frame 2 on B, which shares A's depth buffer, writes no depth: the depth image still
     * holds frame 1, so it is not handed on. */
    ahc_state(base_b, zbp, 1);
    ok = ok && ahc_sprite(10.0f, 10.0f, 20.0f, 20.0f, hud) && submit_pending();
    Target *b = target_find_by_fba(base_b & 0x00FFFFFFu);
    ok = ok && b && b->dep == z && ahc_present(base_b, "frame 2");
    if (ok && (!s_ahc.have_bits || !ahc_bit(15, 15) || ahc_bit(105, 102) || s_ahc.depth != VK_NULL_HANDLE)) {
        fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: frame 2: mask %d box %d stale box %d, "
                        "depth %s\n", s_ahc.have_bits, ahc_bit(15, 15), ahc_bit(105, 102),
                s_ahc.depth ? "handed on though no draw wrote it" : "withheld");
        ok = 0;
    }

    /* Frame 3 on A: a transform-mode draw with depth writes; depth is handed on again. */
    ahc_state(base_a, zbp, 0);
    s_ge->ztest_enable = 1;
    s_ge->ztest = 1;                            /* ALWAYS */
    ok = ok && ahc_sprite(60.0f, 60.0f, 90.0f, 90.0f, hud) && submit_pending();
    ok = ok && ahc_present(base_a, "frame 3");
    if (ok && (s_ahc.depth != z->img || ahc_bit(70, 70))) {
        fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: frame 3: depth %s, scene in mask %d\n",
                s_ahc.depth == z->img ? "handed on" : "missing after a depth write", ahc_bit(70, 70));
        ok = 0;
    }

    /* Frame 4 on A: its only depth write is a clear that clears depth. That is a write, so
     * depth is handed on, and the clear is not a HUD box. */
    ahc_state(base_a, zbp, 1);
    s_ge->clear = 1;
    s_ge->clear_mode = 0x401u;                  /* depth only */
    ok = ok && ahc_sprite(0.0f, 0.0f, 480.0f, 272.0f, 0x00000000u) && submit_pending();
    s_ge->clear = 0;
    s_ge->clear_mode = 0;
    ok = ok && ahc_present(base_a, "frame 4");
    if (ok && (s_ahc.depth != z->img || !s_ahc.have_bits || ahc_bit(240, 136))) {
        fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: frame 4: depth %s after a depth clear, mask %d, "
                        "clear in mask %d\n", s_ahc.depth == z->img ? "handed on" : "missing",
                s_ahc.have_bits, ahc_bit(240, 136));
        ok = 0;
    }

    /* A frame published while no show pass took it, then a CPU frame: the CPU frame gets
     * nothing, and the stale mask is counted, not carried over. */
    ahc_state(base_a, zbp, 1);
    ok = ok && ahc_sprite(100.0f, 100.0f, 110.0f, 104.0f, hud) && submit_pending();
    sdl3vk_set_show_pass(NULL);
    ok = ok && gegpu_present(base_a, 3, 512u) == GEGPU_PRESENT_SHOWN;
    sdl3vk_set_show_pass(ace_hud_cell_show);
    static uint32_t cpu_px[480 * 272];
    memset(&s_ahc, 0, sizeof(s_ahc));
    ok = ok && sdl3vk_present_rgba(cpu_px) == 1;
    if (ok && (s_ahc.calls != 1 || s_ahc.got)) {
        fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: a CPU frame took a GPU frame's mask "
                        "(calls %d, got %d)\n", s_ahc.calls, s_ahc.got);
        ok = 0;
    }

    AceSmoothAuxStats st;
    memset(&st, 0, sizeof(st));
    ace_hud_stats(&st);
    if (ok && (st.boxes_kept != 4u || st.boxes_target != 1u || st.boxes_large != 0u || st.boxes_full != 0u ||
               st.frames_mask != 5u || st.frames_depth != 3u || st.frames_depth_unwritten != 2u ||
               st.published_unshown != 1u)) {
        fprintf(stderr, "gpu snapshot sync selftest [ace-hud]: counts kept=%llu target=%llu large=%llu "
                        "full=%llu frames mask=%llu depth=%llu unwritten=%llu unshown=%llu\n",
                (unsigned long long)st.boxes_kept, (unsigned long long)st.boxes_target,
                (unsigned long long)st.boxes_large, (unsigned long long)st.boxes_full,
                (unsigned long long)st.frames_mask, (unsigned long long)st.frames_depth,
                (unsigned long long)st.frames_depth_unwritten, (unsigned long long)st.published_unshown);
        ok = 0;
    }
    sdl3vk_set_show_pass(NULL);
    ace_hud_enable(0);
    coherence_reset_targets();
    if (ok) printf("gpu snapshot sync selftest: ace_hud_mask_and_depth_from_the_ge\n");
    return ok;
}

int gegpu_snapshot_sync_selftest(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u, fmt = 3u;
    const uint32_t bytes = stride * FB_H * 4u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g1(fmt));
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu snapshot sync selftest: could not establish GPU target\n");
        coherence_reset_targets();
        return 0;
    }

    /* The image remains GPU G1, but guest VRAM is deliberately restored to stale G0. */
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g0(fmt));
    coherence_image_newer(t);
    if (!target_prepare_present(t)) {
        fprintf(stderr, "gpu snapshot sync selftest: present readback setup failed\n");
        coherence_reset_targets();
        return 0;
    }

    ReadbackSlot *pending = NULL;
    for (int i = 0; i < READBACK_FRAMES; i++) {
        if (s_readback[i].pending && s_readback[i].image == t->img) {
            pending = &s_readback[i];
            break;
        }
    }
    int stale = pending && pending->commit && t->clean_gen != t->render_gen &&
                memcmp(guest, &(uint32_t){ coherence_g0(fmt) }, sizeof(uint32_t)) == 0;
    if (!stale) {
        fprintf(stderr, "gpu snapshot sync selftest: no stale pre-commit snapshot observed "
                        "(render_gen=%llu clean_gen=%llu pending=%d)\n",
                (unsigned long long)t->render_gen, (unsigned long long)t->clean_gen,
                pending != NULL);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: ordinary_present_lag render_gen=%llu "
           "clean_gen=%llu pending=1 bytes=%u\n",
           (unsigned long long)t->render_gen, (unsigned long long)t->clean_gen, bytes);

    GeGpuFbDescriptor d = { base, fmt, stride, 480u, FB_H };
    int rc = gegpu_sync_guest_fb(&d);
    if (rc != GEGPU_SYNC_OK || t->clean_gen != t->render_gen ||
        memcmp(guest, &(uint32_t){ coherence_g1(fmt) }, sizeof(uint32_t)) != 0) {
        fprintf(stderr, "gpu snapshot sync selftest: explicit sync failed rc=%d "
                        "clean_gen=%llu render_gen=%llu\n", rc,
                (unsigned long long)t->clean_gen, (unsigned long long)t->render_gen);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: explicit_sync_ok render_gen=%llu\n",
           (unsigned long long)t->render_gen);

    GeGpuFbDescriptor bad = d;
    bad.width = 0;
    if (gegpu_sync_guest_fb(&bad) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: invalid descriptor was accepted\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor cross = d;
    cross.addr = 0x041f0000u;
    if (gegpu_sync_guest_fb(&cross) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: cross-aperture descriptor was accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* A caller that agrees on the address but disagrees on how the bytes are laid
     * out would misread the buffer it just synchronised, so the boundary refuses.
     * These two controls keep that refusal honest.  A real observer defect once
     * produced exactly this shape -- live target 5551, caller describing 8888 --
     * and the refusal is what surfaced it.  Any change that makes an observer
     * quieter must leave these failing closed. */
    GeGpuFbDescriptor wrong_fmt = d;
    wrong_fmt.format = fmt == 3u ? 1u : 3u;
    if (gegpu_sync_guest_fb(&wrong_fmt) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: pixel-format mismatch against a live "
                        "target was accepted (target fmt=%u, caller fmt=%u)\n",
                fmt, wrong_fmt.format);
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor wrong_stride = d;
    wrong_stride.stride = stride + 64u;
    if (gegpu_sync_guest_fb(&wrong_stride) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: stride mismatch against a live "
                        "target was accepted (target stride=%u, caller stride=%u)\n",
                stride, wrong_stride.stride);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: live_target_mismatch_refused fmt and stride\n");
    GeGpuFbDescriptor no_target = d;
    no_target.addr = 0x04010000u;
    if (gegpu_sync_guest_fb(&no_target) != GEGPU_SYNC_NO_TARGET) {
        fprintf(stderr, "gpu snapshot sync selftest: unmapped VRAM was not NO_TARGET\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor ram = d;
    ram.addr = 0x08010000u;
    if (gegpu_sync_guest_fb(&ram) != GEGPU_SYNC_NO_TARGET) {
        fprintf(stderr, "gpu snapshot sync selftest: main RAM was not NO_TARGET\n");
        coherence_reset_targets();
        return 0;
    }
    /* Declines that involve no GPU-valid target keep -1, so gui_present does not sync guest
     * VRAM for them. Only a GPU-valid target that fails to prepare or present reports
     * GEGPU_PRESENT_TARGET_FAILED (issue #827). */
    if (gegpu_present(0x08010000u, (int)fmt, stride) != GEGPU_PRESENT_NOT_RESIDENT ||
        gegpu_present(base, (int)fmt, FB_W + 64u) != GEGPU_PRESENT_NOT_RESIDENT) {
        fprintf(stderr, "gpu snapshot sync selftest: a present decline without a GPU target was not -1\n");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_declines_without_target_keep_code\n");

    if (!coherence_queue_g1_batch(base, stride, fmt) || !submit_pending() ||
        !target_prepare_present(t) || t->clean_gen == t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: ordinary second present lost async lag\n");
        coherence_reset_targets();
        return 0;
    }
    if (gegpu_sync_guest_fb(&d) != GEGPU_SYNC_OK || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: second explicit sync failed\n");
        coherence_reset_targets();
        return 0;
    }

    /* Present-side failure (issue #827): the target is GPU-valid and newer than guest VRAM.
     * The sdl3vk fault seam (selftest only) makes vkQueuePresentKHR report OUT_OF_DATE, which
     * present_common resolves as -1 after the target was prepared. gegpu_present() must report
     * GEGPU_PRESENT_TARGET_FAILED, and the sync gui_present runs before its fall-through must
     * make guest VRAM current with the newest generation. */
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g0(fmt));
    if (!coherence_queue_g1_batch(base, stride, fmt) || !submit_pending() ||
        t->clean_gen == t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: could not stage a newer GPU target\n");
        coherence_reset_targets();
        return 0;
    }
    sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
    int failed_present = gegpu_present(base, (int)fmt, stride);
    sdl3vk_present_fault_clear();
    if (failed_present != GEGPU_PRESENT_TARGET_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: failed present of a prepared target returned %d, "
                        "not GEGPU_PRESENT_TARGET_FAILED\n", failed_present);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_target_failure_reports_target_failed\n");
    if (gegpu_sync_guest_fb(&d) != GEGPU_SYNC_OK || t->clean_gen != t->render_gen ||
        memcmp((const uint8_t *)guest + (15u * stride + 15u) * 4u, &(uint32_t){ coherence_gbatch(fmt) },
               sizeof(uint32_t)) != 0) {
        fprintf(stderr, "gpu snapshot sync selftest: guest VRAM was not current after a target failure\n");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_target_failure_synced_guest_vram\n");

    /* Controls: a present that is shown, and one that is shown with an enqueued swapchain
     * rebuild (VK_SUBOPTIMAL_KHR), both still report 1. Only -1 is remapped. */
    sdl3vk_present_fault_inject(VK_SUBOPTIMAL_KHR);
    int rebuilt_present = gegpu_present(base, (int)fmt, stride);
    sdl3vk_present_fault_clear();
    if (rebuilt_present != GEGPU_PRESENT_SHOWN ||
        gegpu_present(base, (int)fmt, stride) != GEGPU_PRESENT_SHOWN) {
        fprintf(stderr, "gpu snapshot sync selftest: a shown present did not stay 1 (rebuild=%d)\n",
                rebuilt_present);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_shown_stays_1\n");
    coherence_reset_targets();
    return coherence_run_ace_hud();
}

/* With the host twin on (#2050), every case above also checks it: no twin may have been found
 * behind its image (a write that did not reach it), and, at equal scales, each twin left at the
 * end equals its image. */
int gegpu_coherence_selftest(void) {
    int ok = coherence_selftest_cases();
    if (!s_twin) return ok;
    twin_identity_check("at the end");
    if (s_twin_identity_failures || s_cnt_twin_reseed) {
        fprintf(stderr, "gpu coherence selftest [twin]: FAIL %u identity failure(s), %lu re-seed(s) "
                "(last lag: %s)\n", s_twin_identity_failures, s_cnt_twin_reseed,
                s_twin_lag_cause ? s_twin_lag_cause : "none");
        return 0;
    }
    printf("gpu coherence selftest: PASS twin %dx (authentic %dx): no twin fell behind%s\n", s_tscale,
           s_scale, s_tscale == s_scale ? ", every twin equals its image" : "");
    return ok;
}
