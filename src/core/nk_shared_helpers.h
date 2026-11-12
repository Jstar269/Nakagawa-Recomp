/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_SHARED_HELPERS_H
#define NK_SHARED_HELPERS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
 * presents. An effect is over the rule in a window when its software primitives reach
 * NK_RENDER_FALLBACK_MIN_PRIMS or its readbacks reach NK_RENDER_FALLBACK_READBACKS, both a cost
 * budget a window; over the rule in NK_RENDER_FALLBACK_WINDOWS windows in a row, it is reported
 * once per run. Each value is measured, derived (with its arithmetic) or chosen, as
 * src/rt/gpu_sdl3vk/README.md ("Software fallback report") sets out. */
#define NK_RENDER_FALLBACK_WINDOW_FRAMES 120u /* chosen: 2 s at 60 presents a second */
#define NK_RENDER_FALLBACK_WINDOWS 3u         /* chosen: at least 6 s in a row */
#define NK_RENDER_FALLBACK_READBACKS 30u      /* derived: 0.45-0.88 ms a present */
#define NK_RENDER_FALLBACK_MIN_PRIMS 4000u    /* derived: 0.73-0.93 ms a present */

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

#endif
