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

#endif
