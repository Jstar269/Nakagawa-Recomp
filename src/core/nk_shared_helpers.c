/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_shared_helpers.h"
#include "nk_platform.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

typedef struct NkSharedPathJoinPlan {
    size_t left_length;
    size_t right_length;
    size_t total_length;
    size_t separator_length;
} NkSharedPathJoinPlan;

static int path_separator_byte(char value) {
    return value == '/' || value == '\\';
}

static bool path_join_plan(const char *left, const char *right, char separator,
                           NkSharedPathJoinMode mode, unsigned int flags,
                           NkSharedPathJoinPlan *plan) {
    if (!left || !right || !plan ||
        (mode != NK_SHARED_PATH_JOIN_ALWAYS &&
         mode != NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR &&
         mode != NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY)) {
        return false;
    }
    if ((flags & NK_SHARED_PATH_REJECT_ABSOLUTE_RIGHT) != 0u &&
        (right[0] == '/' || right[0] == '\\' ||
         (right[0] != '\0' && right[1] == ':'))) {
        return false;
    }

    plan->left_length = strlen(left);
    plan->right_length = strlen(right);
    if ((flags & NK_SHARED_PATH_TRIM_LEFT_SEPARATORS) != 0u) {
        while (plan->left_length > 0u &&
               path_separator_byte(left[plan->left_length - 1u])) {
            plan->left_length--;
        }
    }

    int left_has_separator = plan->left_length > 0u &&
        path_separator_byte(left[plan->left_length - 1u]);
    switch (mode) {
        case NK_SHARED_PATH_JOIN_ALWAYS:
            plan->separator_length = 1u;
            break;
        case NK_SHARED_PATH_JOIN_IF_LEFT_NEEDS_SEPARATOR:
            plan->separator_length = left_has_separator ? 0u : 1u;
            break;
        case NK_SHARED_PATH_JOIN_IF_BOTH_NONEMPTY:
            plan->separator_length = plan->left_length > 0u &&
                plan->right_length > 0u && !left_has_separator ? 1u : 0u;
            break;
        default:
            return false;
    }

    if (plan->left_length > SIZE_MAX - plan->separator_length ||
        plan->left_length + plan->separator_length >
            SIZE_MAX - plan->right_length) {
        return false;
    }
    plan->total_length = plan->left_length + plan->separator_length + plan->right_length;
    (void)separator;
    return true;
}

static void path_join_write(char *out, size_t out_capacity,
                            const char *left, const char *right, char separator,
                            const NkSharedPathJoinPlan *plan, unsigned int flags) {
    size_t at = 0u;
    size_t limit = out_capacity - 1u;
    size_t copy_length = plan->left_length < limit ? plan->left_length : limit;
    if (copy_length > 0u) memcpy(out, left, copy_length);
    at += copy_length;

    if (plan->separator_length > 0u && at < limit) out[at++] = separator;

    size_t right_copy = plan->right_length < limit - at ? plan->right_length : limit - at;
    if (right_copy > 0u) memcpy(out + at, right, right_copy);
    at += right_copy;
    out[at] = '\0';

    if (plan->total_length >= out_capacity || plan->total_length > (size_t)INT_MAX) return;
    size_t suffix_start = plan->left_length + plan->separator_length;
    for (size_t i = suffix_start; i < plan->total_length; i++) {
        if ((flags & NK_SHARED_PATH_NORMALIZE_BOTH_SLASHES) != 0u &&
            path_separator_byte(out[i])) {
            out[i] = separator;
        } else if ((flags & NK_SHARED_PATH_NORMALIZE_FORWARD_SLASHES) != 0u &&
                   out[i] == '/') {
            out[i] = separator;
        }
    }
}

bool nk_shared_path_join(char *out, size_t out_capacity,
                         const char *left, const char *right, char separator,
                         NkSharedPathJoinMode mode, unsigned int flags) {
    NkSharedPathJoinPlan plan;
    if (!out || out_capacity == 0u ||
        !path_join_plan(left, right, separator, mode, flags, &plan)) {
        return false;
    }

    bool fits_capacity = plan.total_length < out_capacity;
    if (!fits_capacity && (flags & NK_SHARED_PATH_REQUIRE_FIT) != 0u) return false;

    path_join_write(out, out_capacity, left, right, separator, &plan, flags);
    if (!fits_capacity ||
        (plan.total_length > (size_t)INT_MAX &&
         (flags & NK_SHARED_PATH_REQUIRE_FIT) == 0u)) {
        return false;
    }
    return (flags & NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH) == 0u ||
           plan.total_length > 0u;
}

char *nk_shared_path_join_alloc(const char *left, const char *right, char separator,
                                NkSharedPathJoinMode mode, unsigned int flags) {
    NkSharedPathJoinPlan plan;
    if (!path_join_plan(left, right, separator, mode, flags, &plan) ||
        plan.total_length == SIZE_MAX) {
        return NULL;
    }
    char *out = (char *)malloc(plan.total_length + 1u);
    if (!out) return NULL;
    path_join_write(out, plan.total_length + 1u, left, right, separator, &plan, flags);
    if ((flags & NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH) != 0u &&
        plan.total_length == 0u) {
        free(out);
        return NULL;
    }
    return out;
}

bool nk_shared_text_copy(char *out, size_t out_capacity, const char *text,
                         NkSharedTextCopyMode mode) {
    if (!out || out_capacity == 0u ||
        (mode != NK_SHARED_TEXT_COPY_TRUNCATE &&
         mode != NK_SHARED_TEXT_COPY_REQUIRE_FIT)) {
        return false;
    }
    if (!text) {
        if (mode == NK_SHARED_TEXT_COPY_TRUNCATE) {
            out[0] = '\0';
            return true;
        }
        return false;
    }

    if (mode == NK_SHARED_TEXT_COPY_REQUIRE_FIT) {
        size_t length = strlen(text);
        if (length >= out_capacity) return false;
        memcpy(out, text, length + 1u);
        return true;
    }

    strncpy(out, text, out_capacity - 1u);
    out[out_capacity - 1u] = '\0';
    return true;
}

/* This common-source primitive is also linked by tools that do not link a host backend. */
FILE *nk_fopen_utf8(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide_path[32768];
    WCHAR wide_mode[32];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide_path, (int)(sizeof(wide_path) / sizeof(wide_path[0]))) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1,
                            wide_mode, (int)(sizeof(wide_mode) / sizeof(wide_mode[0]))) <= 0) {
        return NULL;
    }
    return _wfopen(wide_path, wide_mode);
#else
    return fopen(path, mode);
#endif
}

void nk_shared_format_size(char *out, size_t out_capacity, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_capacity, "%.2f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_capacity, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_capacity, "%llu bytes", (unsigned long long)bytes);
    }
}

static void boot_event_write_line(FILE *stream, const char *format, uint64_t timestamp_ns,
                                  va_list args) {
    va_list copy;
    va_copy(copy, args);
    vfprintf(stream, format, copy);
    va_end(copy);
    if (timestamp_ns != 0u) {
        fprintf(stream, " t_ns=%llu\n", (unsigned long long)timestamp_ns);
    } else {
        fputc('\n', stream);
    }
}

void nk_shared_boot_event_vwrite(FILE *stderr_stream, uint64_t timestamp_ns,
                                 const char *format,
                                 va_list args) {
    if (!stderr_stream) stderr_stream = stderr;
    boot_event_write_line(stderr_stream, format, timestamp_ns, args);
    fflush(stderr_stream);

    const char *path = getenv("SR_BOOT_EVENT_FILE");
    if (!path || !*path) return;
    FILE *file = fopen(path, "ab");
    if (!file) return;
    boot_event_write_line(file, format, timestamp_ns, args);
    fclose(file);
}

static const char *const k_render_fallback_names[NK_RENDER_FALLBACK_COUNT] = {
    "none", "gdi_presenter", "software_ge"
};

NkRenderPath nk_render_path_from_init(bool vulkan_asked, bool vulkan_started,
                                      bool gpu_ge_asked, bool gpu_ge_started) {
    NkRenderPath path;
    /* The GPU GE renderer draws only into the Vulkan presenter. */
    path.vulkan = vulkan_asked && vulkan_started;
    path.gpu_ge = path.vulkan && gpu_ge_asked && gpu_ge_started;
    if (vulkan_asked && !vulkan_started) {
        path.fallback = NK_RENDER_FALLBACK_GDI_PRESENTER;
    } else if (path.vulkan && gpu_ge_asked && !gpu_ge_started) {
        path.fallback = NK_RENDER_FALLBACK_SOFTWARE_GE;
    } else {
        path.fallback = NK_RENDER_FALLBACK_NONE;
    }
    return path;
}

const char *nk_render_fallback_name(NkRenderFallback fallback) {
    return (unsigned)fallback < NK_RENDER_FALLBACK_COUNT ? k_render_fallback_names[fallback]
                                                          : NULL;
}

int nk_render_path_event_format(const NkRenderPath *path, char *out, size_t out_capacity) {
    const char *fallback = path ? nk_render_fallback_name(path->fallback) : NULL;
    if (!out || out_capacity == 0u) return 0;
    out[0] = '\0';
    if (!fallback) return 0;
    int written = snprintf(out, out_capacity,
                           "BOOT_EVENT phase=render_path presenter=%s ge=%s fallback=%s",
                           path->vulkan ? "vulkan" : "gdi", path->gpu_ge ? "gpu" : "software",
                           fallback);
    if (written <= 0 || (size_t)written >= out_capacity) {
        out[0] = '\0';
        return 0;
    }
    return written;
}

/* The value after " key=" in line, up to the next space or line end; false when absent. */
static bool event_value(const char *line, const char *key, const char **value, size_t *length) {
    size_t key_length = strlen(key);
    for (const char *at = strchr(line, ' '); at != NULL; at = strchr(at + 1, ' ')) {
        if (strncmp(at + 1, key, key_length) == 0 && at[1 + key_length] == '=') {
            *value = at + 2 + key_length;
            *length = strcspn(*value, " \r\n");
            return true;
        }
    }
    return false;
}

static bool event_value_is(const char *value, size_t length, const char *word) {
    return length == strlen(word) && strncmp(value, word, length) == 0;
}

bool nk_render_path_event_parse(const char *line, NkRenderPath *out) {
    static const char prefix[] = "BOOT_EVENT phase=render_path ";
    const char *value;
    size_t length;
    NkRenderPath path;
    if (!line || !out || strncmp(line, prefix, sizeof(prefix) - 1u) != 0) return false;
    if (!event_value(line, "presenter", &value, &length)) return false;
    if (event_value_is(value, length, "vulkan")) {
        path.vulkan = true;
    } else if (event_value_is(value, length, "gdi")) {
        path.vulkan = false;
    } else {
        return false;
    }
    if (!event_value(line, "ge", &value, &length)) return false;
    if (event_value_is(value, length, "gpu")) {
        path.gpu_ge = true;
    } else if (event_value_is(value, length, "software")) {
        path.gpu_ge = false;
    } else {
        return false;
    }
    if (!event_value(line, "fallback", &value, &length)) return false;
    path.fallback = NK_RENDER_FALLBACK_COUNT;
    for (int i = 0; i < NK_RENDER_FALLBACK_COUNT; ++i) {
        if (event_value_is(value, length, k_render_fallback_names[i])) {
            path.fallback = (NkRenderFallback)i;
        }
    }
    if (path.fallback == NK_RENDER_FALLBACK_COUNT) return false;
    *out = path;
    return true;
}

static const char *const k_render_effect_names[NK_RENDER_EFFECT_COUNT] = {
    "stencil", "color_test", "logic_op", "write_mask", "ram_fb", "wide_stride", "mips", "line",
    "point"
};

const char *nk_render_effect_name(NkRenderEffect effect) {
    return (unsigned)effect < NK_RENDER_EFFECT_COUNT ? k_render_effect_names[effect] : NULL;
}

int nk_render_fallback_event_format(const NkRenderFallbackEvent *event, char *out,
                                    size_t out_capacity) {
    const char *reason = event ? nk_render_effect_name(event->reason) : NULL;
    if (!out || out_capacity == 0u) return 0;
    out[0] = '\0';
    if (!reason) return 0;
    int written = snprintf(out, out_capacity,
                           "BOOT_EVENT phase=render_fallback reason=%s frames=%lu sw_prims=%llu "
                           "gpu_prims=%llu readbacks=%llu share_pm=%lu",
                           reason, (unsigned long)event->frames,
                           (unsigned long long)event->sw_prims,
                           (unsigned long long)event->gpu_prims,
                           (unsigned long long)event->readbacks,
                           (unsigned long)event->share_pm);
    if (written <= 0 || (size_t)written >= out_capacity) {
        out[0] = '\0';
        return 0;
    }
    return written;
}

/* A value of only decimal digits that fits max; false for an empty or longer one. */
static bool event_number(const char *line, const char *key, uint64_t max, uint64_t *out) {
    const char *value;
    size_t length;
    uint64_t n = 0u;
    if (!event_value(line, key, &value, &length) || length == 0u) return false;
    for (size_t i = 0u; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        unsigned digit = (unsigned)(value[i] - '0');
        if (n > (max - digit) / 10u) return false;
        n = n * 10u + digit;
    }
    *out = n;
    return true;
}

bool nk_render_fallback_event_parse(const char *line, NkRenderFallbackEvent *out) {
    static const char prefix[] = "BOOT_EVENT phase=render_fallback ";
    const char *value;
    size_t length;
    uint64_t frames, share_pm;
    NkRenderFallbackEvent event;
    if (!line || !out || strncmp(line, prefix, sizeof(prefix) - 1u) != 0) return false;
    if (!event_value(line, "reason", &value, &length)) return false;
    event.reason = NK_RENDER_EFFECT_COUNT;
    for (int i = 0; i < NK_RENDER_EFFECT_COUNT; ++i) {
        if (event_value_is(value, length, k_render_effect_names[i])) {
            event.reason = (NkRenderEffect)i;
        }
    }
    if (event.reason == NK_RENDER_EFFECT_COUNT) return false;
    if (!event_number(line, "frames", UINT32_MAX, &frames) ||
        !event_number(line, "sw_prims", UINT64_MAX, &event.sw_prims) ||
        !event_number(line, "gpu_prims", UINT64_MAX, &event.gpu_prims) ||
        !event_number(line, "readbacks", UINT64_MAX, &event.readbacks) ||
        !event_number(line, "share_pm", 1000u, &share_pm)) {
        return false;
    }
    event.frames = (uint32_t)frames;
    event.share_pm = (uint32_t)share_pm;
    *out = event;
    return true;
}

/* Compile-time checks in the C99 form (this file is also built with -std=c99 -Wpedantic, where
 * _Static_assert is not available): the reported effects fit a uint16_t, and each kind alone
 * reaches the budget at a whole count. */
typedef char nk_render_effects_fit_u16_check[(NK_RENDER_EFFECT_COUNT <= 16) ? 1 : -1];
typedef char nk_render_fallback_whole_counts_check
    [(NK_RENDER_FALLBACK_BUDGET_US % NK_RENDER_FALLBACK_READBACK_US == 0u &&
      NK_RENDER_FALLBACK_BUDGET_US % NK_RENDER_FALLBACK_PRIM_US == 0u &&
      NK_RENDER_FALLBACK_READBACKS == 30u && NK_RENDER_FALLBACK_MIN_PRIMS == 4000u) ? 1 : -1];

uint64_t nk_render_fallback_cost_us(uint64_t sw_prims, uint64_t readbacks) {
    if (readbacks > UINT64_MAX / NK_RENDER_FALLBACK_READBACK_US) return UINT64_MAX;
    const uint64_t rb_us = readbacks * NK_RENDER_FALLBACK_READBACK_US;
    if (sw_prims > (UINT64_MAX - rb_us) / NK_RENDER_FALLBACK_PRIM_US) return UINT64_MAX;
    return rb_us + sw_prims * NK_RENDER_FALLBACK_PRIM_US;
}

uint16_t nk_render_fallback_window_step(NkRenderFallbackWindow *w,
                                        const NkRenderFallbackCounts *now,
                                        NkRenderFallbackDelta *delta) {
    NkRenderFallbackDelta d;
    memset(&d, 0, sizeof(d));
    if (delta) *delta = d;
    if (!w || !now) return 0u;
    if (!now->ready) {
        w->primed = 0;
        return 0u;
    }
    bool restart = !w->primed;
    for (int r = 0; r < NK_RENDER_EFFECT_COUNT && !restart; ++r) {
        if (now->sw_prims[r] < w->last.sw_prims[r] || now->readbacks[r] < w->last.readbacks[r]) {
            restart = true;                   /* the counts started over: so does the window */
        }
    }
    if (restart) {
        const uint16_t reported = w->reported;
        memset(w, 0, sizeof(*w));
        w->reported = reported;
        w->last = *now;
        w->primed = 1;
        return 0u;
    }
    d.gpu_prims = now->gpu_prims - w->last.gpu_prims;
    w->gpu += d.gpu_prims;
    for (int r = 0; r < NK_RENDER_EFFECT_COUNT; ++r) {
        d.sw_prims[r] = now->sw_prims[r] - w->last.sw_prims[r];
        d.readbacks[r] = now->readbacks[r] - w->last.readbacks[r];
        w->sw[r] += d.sw_prims[r];
        w->rb[r] += d.readbacks[r];
    }
    w->last = *now;
    if (delta) *delta = d;
    if (++w->frames < NK_RENDER_FALLBACK_WINDOW_FRAMES) return 0u;
    uint64_t all_sw = 0u;
    for (int r = 0; r < NK_RENDER_EFFECT_COUNT; ++r) all_sw += w->sw[r];
    uint16_t crossed = 0u;
    for (int r = 0; r < NK_RENDER_EFFECT_COUNT; ++r) {
        /* Per effect: another effect's cost never adds to this one's. */
        const bool over = nk_render_fallback_cost_us(w->sw[r], w->rb[r]) >=
                          NK_RENDER_FALLBACK_BUDGET_US;
        if (!over) {
            w->streak[r] = 0u;
            w->span_gpu[r] = w->span_all_sw[r] = w->span_sw[r] = w->span_rb[r] = 0u;
            continue;
        }
        w->streak[r]++;
        w->span_gpu[r] += w->gpu;
        w->span_all_sw[r] += all_sw;
        w->span_sw[r] += w->sw[r];
        w->span_rb[r] += w->rb[r];
        if (w->streak[r] >= NK_RENDER_FALLBACK_WINDOWS && !(w->reported & (1u << r))) {
            w->reported |= (uint16_t)(1u << r);
            crossed |= (uint16_t)(1u << r);
        }
    }
    w->frames = 0u;
    w->gpu = 0u;
    memset(w->sw, 0, sizeof(w->sw));
    memset(w->rb, 0, sizeof(w->rb));
    return crossed;
}

NkRenderFallbackEvent nk_render_fallback_window_event(const NkRenderFallbackWindow *w,
                                                      NkRenderEffect effect) {
    NkRenderFallbackEvent e;
    memset(&e, 0, sizeof(e));
    e.reason = effect;
    if (!w || (unsigned)effect >= NK_RENDER_EFFECT_COUNT) return e;
    const uint64_t all = w->span_gpu[effect] + w->span_all_sw[effect];
    e.frames = w->streak[effect] * NK_RENDER_FALLBACK_WINDOW_FRAMES;
    e.sw_prims = w->span_sw[effect];
    e.gpu_prims = w->span_gpu[effect];
    e.readbacks = w->span_rb[effect];
    e.share_pm = all ? (uint32_t)(1000u * w->span_sw[effect] / all) : 0u;
    return e;
}

int nk_shared_boot_event_format_path(char *out, size_t out_capacity,
                                    const char *cache_dir, char separator,
                                    unsigned long process_id, unsigned int sequence) {
    if (!out || out_capacity == 0u) return 0;
    if (!cache_dir) {
        out[0] = '\0';
        return 0;
    }
    int written = snprintf(out, out_capacity, "%s%cplayer-boot-%lu-%u.events",
                           cache_dir, separator, process_id, sequence);
    if (written <= 0 || (size_t)written >= out_capacity) {
        out[0] = '\0';
        return 0;
    }
    return written;
}

/* SHA-256 (FIPS 180-4); declared in nk_shared_helpers.h. */
static uint32_t nk_sha256_rotr(uint32_t value, unsigned int amount) {
    return (value >> amount) | (value << (32u - amount));
}

static void nk_sha256_transform(NkSha256 *ctx, const uint8_t block[64]) {
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

void nk_sha256_init(NkSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bit_count = 0;
    ctx->block_len = 0;
}

void nk_sha256_update(NkSha256 *ctx, const uint8_t *data, size_t len) {
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

void nk_sha256_finish(NkSha256 *ctx, uint8_t digest[32]) {
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
