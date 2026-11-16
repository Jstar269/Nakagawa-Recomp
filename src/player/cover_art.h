/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Opt-in cover art from the libretro thumbnails set (issue #2632).
 *
 * Disabled by default: the player makes no connection unless the user explicitly
 * enables "Download cover art". When enabled, only Redump-verified discs are
 * matched by exact Redump <game name="..."> against Named_Boxarts for
 * "Sony - PlayStation Portable" over HTTPS on thumbnails.libretro.com, using
 * the shared transport (package_builder.h). A clean fallback seam
 * (NkCoverArtProviders) allows RetroAchievements (#1316) to supply box art when
 * the primary set has no match. Downloaded PNGs are validated and cached under
 * <data_root>/cache/cover-art/. */

#ifndef NK_COVER_ART_H
#define NK_COVER_ART_H

#include "nk_disc_verify.h"
#include "package_builder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Only host contacted for primary box art; docs/SETUP.md "Network use" and
   tools/test_prereq_fetcher.py mirror these constants. */
#define NK_COVER_ART_HOST "thumbnails.libretro.com"
#define NK_COVER_ART_SYSTEM "Sony - PlayStation Portable"
#define NK_COVER_ART_BOXARTS_URL_PREFIX \
    "https://" NK_COVER_ART_HOST "/Sony%20-%20PlayStation%20Portable/Named_Boxarts/"

#define NK_COVER_ART_MAX_BYTES (8u * 1024u * 1024u)
#define NK_COVER_ART_MAX_SIDE 2048u
#define NK_COVER_ART_URL_MAX 1536u
#define NK_COVER_ART_CACHE_DIR "cache"
#define NK_COVER_ART_CACHE_SUBDIR "cover-art"

typedef enum {
    NK_COVER_ART_OK = 0,
    NK_COVER_ART_NO_MATCH,
    NK_COVER_ART_NOT_FOUND,
    NK_COVER_ART_INVALID_PNG,
    NK_COVER_ART_TOO_LARGE,
    NK_COVER_ART_CANCELLED,
    NK_COVER_ART_UNSUPPORTED,
    NK_COVER_ART_IO_ERROR
} NkCoverArtStatus;

typedef enum {
    NK_COVER_ART_SOURCE_NONE = 0,
    NK_COVER_ART_SOURCE_LIBRETRO,
    NK_COVER_ART_SOURCE_RETROACHIEVEMENTS
} NkCoverArtSource;

/* Clean fallback seam for RetroAchievements (#1316): invoked when primary
   Redump/libretro box art lookup has no match or image. Implementations populate
   an HTTPS URL in out_url and ';' separated allowed_hosts in out_allowed_hosts
   and return true when an image URL is available. */
typedef bool (*NkCoverArtFallbackFn)(void *context,
                                     const char *disc_id,
                                     const char *sha1_hex,
                                     const char *crc32_hex,
                                     char *out_url, size_t out_url_size,
                                     char *out_allowed_hosts, size_t out_hosts_size);

typedef struct {
    NkCoverArtFallbackFn fallback;
    void *fallback_context;
} NkCoverArtProviders;

typedef bool (*NkCoverArtCancelFn)(void *context);

/* Replace every character in `& * / : < > ? \ | "` with `_` per libretro's
   thumbnail naming specification. Fails on NULL, empty input, ASCII control
   bytes, invalid UTF-8, or an undersized output buffer. */
bool nk_cover_art_sanitize_name(const char *redump_name, char *out, size_t out_size);

/* Build the HTTPS Named_Boxarts PNG URL for `redump_name` using RFC 3986
   percent-encoding after libretro sanitization. */
bool nk_cover_art_boxart_url(const char *redump_name, char *out_url, size_t out_url_size);

/* Cache folder `<data_root>/cache/cover-art` and per-disc `<sha1>.png` file. */
bool nk_cover_art_cache_dir(const char *data_root, char *out_dir, size_t out_size);
bool nk_cover_art_cache_path(const char *data_root, const char *sha1_hex,
                             char *out_path, size_t out_size);

/* Total bytes stored in `<data_root>/cache/cover-art` (0 if missing). */
bool nk_cover_art_cache_size(const char *data_root, uint64_t *out_bytes);
void nk_cover_art_format_size(uint64_t bytes, char *out, size_t out_size);

/* Delete cached files in `<data_root>/cache/cover-art`. */
bool nk_cover_art_clear_cache(const char *data_root,
                              char *error_code, size_t error_code_size,
                              char *error_message, size_t error_message_size);

/* Check that `bytes` carries a valid PNG signature and IHDR width/height within
   1..max_side. */
bool nk_cover_art_validate_png_header(const uint8_t *bytes, size_t size,
                                      uint32_t max_side,
                                      uint32_t *out_width, uint32_t *out_height);

/* Fetch a PNG from an HTTPS URL allowed by `allowed_hosts` and atomically write
   it to `dest_path`. Pass transport == NULL for the platform native transport. */
NkCoverArtStatus nk_cover_art_fetch_url(PackageHttpOpen transport, void *transport_context,
                                        const char *url, const char *allowed_hosts,
                                        const char *dest_path,
                                        NkCoverArtCancelFn should_cancel, void *cancel_ctx);

/* Resolve a Redump-verified disc by sha1/crc32 in `dat`, fetch its box art from
   thumbnails.libretro.com (or `providers->fallback` when absent), validate and
   save to `<data_root>/cache/cover-art/<sha1>.png`. */
NkCoverArtStatus nk_cover_art_fetch_for_disc(PackageHttpOpen transport, void *transport_context,
                                             const NkDiscDat *dat,
                                             const char *data_root,
                                             const char *disc_id,
                                             const char *sha1_hex,
                                             const char *crc32_hex,
                                             const NkCoverArtProviders *providers,
                                             NkCoverArtCancelFn should_cancel, void *cancel_ctx,
                                             NkCoverArtSource *out_source);

const char *nk_cover_art_status_name(NkCoverArtStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NK_COVER_ART_H */
