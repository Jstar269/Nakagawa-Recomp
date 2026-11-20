/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_SHARED_HELPERS_H
#define NK_SHARED_HELPERS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h> /* memcpy, memset: the SHA-256 below */

/* Shared file-open primitive also declared by nk_platform.h for platform callers. */
FILE *nk_fopen_utf8(const char *path, const char *mode);

typedef enum NkSharedPathJoinMode {
    NK_SHARED_PATH_JOIN_ALWAYS,
    NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR,
    NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY
} NkSharedPathJoinMode;

enum {
    NK_SHARED_PATH_TRIM_LEFT_SEPARATORS = 1u << 0,
    NK_SHARED_PATH_NORMALIZE_FORWARD_SLASHES = 1u << 1,
    NK_SHARED_PATH_NORMALIZE_BOTH_SLASHES = 1u << 2,
    NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT = 1u << 3,
    NK_SHARED_PATH_REQUIRE_FIT = 1u << 4,
    NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH = 1u << 5
};

/* On ordinary overflow, like snprintf, writes a truncated NUL-terminated prefix.
 * REQUIRE_FIT leaves out untouched when the result does not fit. */
bool nk_shared_path_join(char *out, size_t out_capacity,
                         const char *left, const char *right, char separator,
                         NkSharedPathJoinMode mode, unsigned int flags);

/* Allocates the complete joined path, checking size arithmetic before allocation. */
char *nk_shared_path_join_alloc(const char *left, const char *right, char separator,
                                NkSharedPathJoinMode mode, unsigned int flags);

typedef enum NkSharedTextCopyMode {
    NK_SHARED_TEXT_COPY_TRUNCATE,
    NK_SHARED_TEXT_COPY_REQUIRE_FIT
} NkSharedTextCopyMode;

/* Truncating copies preserve strncpy padding; strict copies leave out untouched on failure. */
bool nk_shared_text_copy(char *out, size_t out_capacity, const char *text,
                         NkSharedTextCopyMode mode);

void nk_shared_format_size(char *out, size_t out_capacity, uint64_t bytes);

/* Writes the same formatted line to stderr_stream and, when path is nonempty, to an
 * append-binary event file. The caller supplies one timestamp for both records. */
void nk_shared_boot_event_vwrite(FILE *stderr_stream, uint64_t timestamp_ns,
                                 const char *format,
                                 va_list args);

/* Formats the player's process/sequence-specific event pathname. Failure clears out. */
int nk_shared_boot_event_format_path(char *out, size_t out_capacity,
                                    const char *cache_dir, char separator,
                                    unsigned long process_id, unsigned int sequence);

/* How a game's picture is drawn, as the runtime set it up (#2151): which presenter shows it,
 * whether the GPU GE renderer draws it, and the slower path taken, if any, because what was
 * asked for did not start. The runtime reports it once as a boot event,
 *   BOOT_EVENT phase=render_path presenter=<vulkan|gdi> ge=<gpu|software> fallback=<name>
 * and the player reads that line; both sides use the names below. */
typedef enum NkRenderFallback {
    NK_RENDER_FALLBACK_NONE,           /* what was asked for is what draws */
    NK_RENDER_FALLBACK_GDI_PRESENTER,  /* Vulkan did not start: the Windows GDI window shows a
                                          software-drawn picture */
    NK_RENDER_FALLBACK_SOFTWARE_GE,    /* the GPU GE renderer did not start: the Vulkan window
                                          shows a software-drawn picture */
    NK_RENDER_FALLBACK_COUNT
} NkRenderFallback;

typedef struct NkRenderPath {
    bool vulkan;                /* the SDL3/Vulkan presenter shows the picture (else GDI) */
    bool gpu_ge;                /* the GPU GE renderer draws it (else the software GE) */
    NkRenderFallback fallback;
} NkRenderPath;

/* The path from what the runtime asked for and what started. A presenter or renderer that was
 * not asked for (SR_VIDEO=gdi, SR_GPU_GE unset) is no fallback. */
NkRenderPath nk_render_path_from_init(bool vulkan_asked, bool vulkan_started,
                                      bool gpu_ge_asked, bool gpu_ge_started);

/* The fallback's name in the event ("none", "gdi_presenter", "software_ge"); NULL when out of
 * range. */
const char *nk_render_fallback_name(NkRenderFallback fallback);

/* Formats the event's text (without "t_ns"); returns its length, or 0 with out cleared when it
 * does not fit. */
int nk_render_path_event_format(const NkRenderPath *path, char *out, size_t out_capacity);

/* Reads a line the runtime wrote with nk_render_path_event_format (a trailing " t_ns=" and line
 * end are allowed). False, with out untouched, for any other line or an unknown value. */
bool nk_render_path_event_parse(const char *line, NkRenderPath *out);

/* What the GPU GE renderer, while it runs, hands to the software renderer instead of drawing it
 * (#2151), in the order of ge_gpu.h's GEGPU_FALLBACK_* reasons (ge_gpu.h checks that they
 * agree). When one of them costs enough for long enough (the rule below), the runtime reports
 * it once per run as
 *   BOOT_EVENT phase=render_fallback reason=<name> frames=<n> sw_prims=<n> gpu_prims=<n>
 *              readbacks=<n> share_pm=<n>
 * (one line): the presents the rule watched, the reason's software primitives, the primitives the
 * GPU drew, the reason's synchronous readbacks, and its share of all primitives in per mille. */
typedef enum NkRenderEffect {
    NK_RENDER_EFFECT_STENCIL,      /* stencil tests and operations the GPU path cannot match */
    NK_RENDER_EFFECT_COLOR_TEST,   /* colour tests on a colour the shader computes inexactly */
    NK_RENDER_EFFECT_LOGIC_OP,     /* logic operations */
    NK_RENDER_EFFECT_WRITE_MASK,   /* partial colour write masks */
    NK_RENDER_EFFECT_RAM_FB,       /* drawing into a frame buffer in main memory */
    NK_RENDER_EFFECT_WIDE_STRIDE,  /* frame buffers wider than 512 pixels */
    NK_RENDER_EFFECT_MIPS,         /* mipmap levels no Vulkan texture holds */
    NK_RENDER_EFFECT_LINE,         /* lines the GPU path does not draw */
    NK_RENDER_EFFECT_POINT,        /* points */
    NK_RENDER_EFFECT_COUNT
} NkRenderEffect;

typedef struct NkRenderFallbackEvent {
    NkRenderEffect reason;
    uint32_t frames;
    uint64_t sw_prims;
    uint64_t gpu_prims;
    uint64_t readbacks;
    uint32_t share_pm;
} NkRenderFallbackEvent;

/* The reason's name in the event ("stencil", "color_test", ...); NULL when out of range. */
const char *nk_render_effect_name(NkRenderEffect effect);

/* Formats the event's text (without "t_ns"); returns its length, or 0 with out cleared when the
 * reason is out of range or the text does not fit. */
int nk_render_fallback_event_format(const NkRenderFallbackEvent *event, char *out,
                                    size_t out_capacity);

/* Reads a line the runtime wrote with nk_render_fallback_event_format (a trailing " t_ns=" and
 * line end are allowed). False, with out untouched, for any other line, an unknown reason, or a
 * missing or malformed number. */
bool nk_render_fallback_event_parse(const char *line, NkRenderFallbackEvent *out);

/* The cumulative counts a GPU renderer keeps of what it hands to the software renderer
 * (gegpu_fallback_counts in src/rt/gpu_sdl3vk/ge_gpu.c fills one): ready while that renderer
 * runs, the primitives it drew itself (wrapping at 2^32), and by effect the primitives it handed
 * over and the synchronous readbacks those took. */
typedef struct NkRenderFallbackCounts {
    int ready;
    uint32_t gpu_prims;
    uint64_t sw_prims[NK_RENDER_EFFECT_COUNT];
    uint64_t readbacks[NK_RENDER_EFFECT_COUNT];
} NkRenderFallbackCounts;

/* When an effect is reported: tumbling windows of NK_RENDER_FALLBACK_WINDOW_FRAMES accepted
 * presents. An effect is over the rule in a window when its estimated cost there, its readbacks
 * times NK_RENDER_FALLBACK_READBACK_US plus its software primitives times NK_RENDER_FALLBACK_PRIM_US,
 * reaches NK_RENDER_FALLBACK_BUDGET_US (nk_render_fallback_cost_us). The budget is per effect:
 * costs of different effects do not add up, so each report names an effect that costs the budget
 * on its own. Over the rule in NK_RENDER_FALLBACK_WINDOWS windows in a row, an effect is reported
 * once per run. Each value is measured, derived (with its arithmetic) or chosen, as
 * src/rt/gpu_sdl3vk/README.md ("Software fallback report") sets out. */
#define NK_RENDER_FALLBACK_WINDOW_FRAMES 120u /* chosen: 2 s at 60 presents a second */
#define NK_RENDER_FALLBACK_WINDOWS 3u         /* chosen: at least 6 s in a row */
#define NK_RENDER_FALLBACK_BUDGET_US 96000u   /* chosen: 0.8 ms a present x 120 presents */
#define NK_RENDER_FALLBACK_READBACK_US 3200u  /* chosen in the derived 1,800-3,500 us */
#define NK_RENDER_FALLBACK_PRIM_US 24u        /* chosen in the derived 22-28 us */
/* Derived: one kind alone at the budget, 96,000 / 3,200 = 30 readbacks and 96,000 / 24 = 4,000
 * software primitives a window. */
#define NK_RENDER_FALLBACK_READBACKS (NK_RENDER_FALLBACK_BUDGET_US / NK_RENDER_FALLBACK_READBACK_US)
#define NK_RENDER_FALLBACK_MIN_PRIMS (NK_RENDER_FALLBACK_BUDGET_US / NK_RENDER_FALLBACK_PRIM_US)

/* An effect's estimated cost in a window, in microseconds: readbacks x NK_RENDER_FALLBACK_READBACK_US
 * + software primitives x NK_RENDER_FALLBACK_PRIM_US, UINT64_MAX when that does not fit. */
uint64_t nk_render_fallback_cost_us(uint64_t sw_prims, uint64_t readbacks);

/* One accepted present's share of the counts. */
typedef struct NkRenderFallbackDelta {
    uint32_t gpu_prims;
    uint64_t sw_prims[NK_RENDER_EFFECT_COUNT];
    uint64_t readbacks[NK_RENDER_EFFECT_COUNT];
} NkRenderFallbackDelta;

/* The windows over the cumulative counts; all zero to start. */
typedef struct NkRenderFallbackWindow {
    int primed;                                     /* `last` holds a sample */
    NkRenderFallbackCounts last;
    uint32_t frames;                                /* accepted presents in this window */
    uint64_t gpu, sw[NK_RENDER_EFFECT_COUNT], rb[NK_RENDER_EFFECT_COUNT];
    uint32_t streak[NK_RENDER_EFFECT_COUNT];        /* windows in a row over the rule */
    /* Those windows' sums, for the event. */
    uint64_t span_gpu[NK_RENDER_EFFECT_COUNT], span_all_sw[NK_RENDER_EFFECT_COUNT];
    uint64_t span_sw[NK_RENDER_EFFECT_COUNT], span_rb[NK_RENDER_EFFECT_COUNT];
    uint16_t reported;                              /* 1 << effect: already reported */
} NkRenderFallbackWindow;

/* One accepted present, with the counts read after it. Fills `delta` (when not NULL) with its
 * share, zero when the counts are not ready or started over (either restarts the window), and
 * returns the effects that crossed the rule now (1 << effect), each once per run. */
uint16_t nk_render_fallback_window_step(NkRenderFallbackWindow *window,
                                        const NkRenderFallbackCounts *now,
                                        NkRenderFallbackDelta *delta);

/* The event for an effect nk_render_fallback_window_step just returned: the presents of the
 * windows in a row that crossed (always NK_RENDER_FALLBACK_WINDOWS x
 * NK_RENDER_FALLBACK_WINDOW_FRAMES under this rule; written out so a reader needs no
 * constants), the effect's software primitives, the primitives the GPU drew, its readbacks,
 * and its share of all primitives in per mille (reported, not part of the rule). */
NkRenderFallbackEvent nk_render_fallback_window_event(const NkRenderFallbackWindow *window,
                                                      NkRenderEffect effect);

/* SHA-256 (FIPS 180-4), the one implementation the player and the runtime share: the disc,
 * package and profile identities (nk_iso.c, nk_title_manifest.c, nk_pbp_homebrew.c), the
 * imported font identities (nk_font.c), and the runtime's table, save-state, checkpoint,
 * enhancement and texture digests (src/rt/vfpu_tables.c: sr_vfpu_sha256 and
 * sr_vfpu_hmac_sha256 call it). It is defined here, static inline, rather than in
 * nk_shared_helpers.c, because src/rt/vfpu_tables.c is linked without nk_shared_helpers.c by
 * many self-test recipes and hosted CI steps; any file that includes this header gets the
 * same code. Keep it the only copy: the known
 * answers in tests/native/test_shared_helpers.c (the core side) and
 * tests/native/vfpu_tables_selftest.c (the runtime side) run through it. */
typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_len;
} NkSha256;

static inline uint32_t nk_sha256_rotr(uint32_t value, unsigned int amount) {
    return (value >> amount) | (value << (32u - amount));
}

static inline void nk_sha256_transform(NkSha256 *ctx, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t words[64];
    for (size_t i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = nk_sha256_rotr(words[i - 15], 7) ^
                      nk_sha256_rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = nk_sha256_rotr(words[i - 2], 17) ^
                      nk_sha256_rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t sum1 = nk_sha256_rotr(e, 6) ^ nk_sha256_rotr(e, 11) ^ nk_sha256_rotr(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t t1 = h + sum1 + choose + k[i] + words[i];
        uint32_t sum0 = nk_sha256_rotr(a, 2) ^ nk_sha256_rotr(a, 13) ^ nk_sha256_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = sum0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static inline void nk_sha256_init(NkSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bit_count = 0;
    ctx->block_len = 0;
}

static inline void nk_sha256_update(NkSha256 *ctx, const uint8_t *data, size_t len) {
    ctx->bit_count += (uint64_t)len * 8u;
    while (len > 0) {
        size_t room = sizeof(ctx->block) - ctx->block_len;
        size_t take = len < room ? len : room;
        memcpy(ctx->block + ctx->block_len, data, take);
        ctx->block_len += take;
        data += take;
        len -= take;
        if (ctx->block_len == sizeof(ctx->block)) {
            nk_sha256_transform(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

static inline void nk_sha256_finish(NkSha256 *ctx, uint8_t digest[32]) {
    uint64_t bits = ctx->bit_count;
    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > 56) {
        memset(ctx->block + ctx->block_len, 0, sizeof(ctx->block) - ctx->block_len);
        nk_sha256_transform(ctx, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56 - ctx->block_len);
    for (size_t i = 0; i < 8; i++) {
        ctx->block[63 - i] = (uint8_t)(bits >> (i * 8));
    }
    nk_sha256_transform(ctx, ctx->block);
    for (size_t i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

#endif
