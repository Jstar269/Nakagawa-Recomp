/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_ISO_READER_H
#define NAKAGAWA_ISO_READER_H

#include "player_state.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool success;
    char disc_id[MAX_DISC_ID_LEN];
    char title_name[MAX_TITLE_LEN];
    char disc_version[16];
    char error_message[256];
    char boundary_code[48];
    uint64_t file_size;
    bool is_supported;
    bool param_sfo_parsed;
    NkIsoExecutableReport executables;
    char matched_title_id[65];
    GameSupportStatus status;
    /* NkIsoMetadata.disc_key_version and legacy_disc_id: which key disc_id is, and
       the v1 key of a v2-keyed disc (nk_iso_library_key_match). */
    uint32_t disc_key_version;
    char legacy_disc_id[MAX_DISC_ID_LEN];
} IsoInspectResult;

/* Inspects a raw PSP ISO9660 disc image directly */
bool iso_inspect_file(const char *iso_path, IsoInspectResult *out_result);

/* Keys an inspected image as the library holds it (nk_iso_library_key_for_image)
 * and returns that key's version. When an entry made before v2 holds the disc's
 * v1 key, result is re-keyed under it and the title catalog searched again under
 * that key (nk_iso_catalog_match), so a manifest kept under the v1 key goes on
 * matching the disc; otherwise result is left as it is. */
uint32_t iso_inspect_result_for_library(IsoInspectResult *result, const NkGameEntry *entries,
                                        int entry_count);

/* Icon and image loading from ISO disc image */
typedef enum {
    NK_ICON_OK = 0,
    NK_ICON_ERR_MISSING,
    NK_ICON_ERR_OVERSIZED,
    NK_ICON_ERR_CORRUPT,
    NK_ICON_ERR_INVALID_PARAM,
    NK_ICON_ERR_CANCELLED
} NkIconStatus;

#define NK_ICON_MAX_BYTES (1024 * 1024) /* 1 MiB bound */

typedef struct {
    NkIconStatus status;
    uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
} NkIsoImageData;

/* Validates a PNG buffer in memory:
 * - Checks PNG 8-byte signature: \x89PNG\r\n\x1a\n
 * - Parses IHDR chunk (length 13, type "IHDR")
 * - Extracts width and height (big-endian 32-bit unsigned int)
 * - Validates dimensions: width > 0 && height > 0 && width <= 2048 && height <= 2048
 * Returns NK_ICON_OK on success, or NK_ICON_ERR_CORRUPT.
 */
NkIconStatus nk_iso_validate_png_header(const uint8_t *data, size_t size, uint32_t *out_w, uint32_t *out_h);

/* Reads an image file (e.g. "PSP_GAME/ICON0.PNG" or "PSP_GAME/PIC1.PNG")
 * from an ISO9660 disc image into memory.
 * - Enforces 1 MiB size limit (returns NK_ICON_ERR_OVERSIZED if > 1 MiB).
 * - Validates PNG header and dimensions.
 * - Allocates *out_data with malloc (caller must free()).
 */
NkIconStatus nk_iso_read_image_entry(const char *iso_path, const char *rel_path,
                                     uint8_t **out_data, size_t *out_size,
                                     uint32_t *out_w, uint32_t *out_h);

/* Reads any file from an ISO9660 disc image into memory, unvalidated, for a
 * reader that checks its own format (the launcher's PSP_GAME/SND0.AT3 clip).
 * NK_ICON_ERR_MISSING: no image, or no such file (or it is a directory);
 * NK_ICON_ERR_OVERSIZED: larger than max_bytes; NK_ICON_ERR_CORRUPT: empty or
 * unreadable. The optional cancel callback is checked before payload I/O,
 * between chunks, and before handing off a complete result. Cancellation
 * returns NK_ICON_ERR_CANCELLED. *out_data is malloc'd (caller frees) only
 * on NK_ICON_OK. */
NkIconStatus nk_iso_read_bounded_entry(const char *iso_path, const char *rel_path,
                                       size_t max_bytes, uint8_t **out_data, size_t *out_size,
                                       bool (*cancel)(void *), void *cancel_user);

/* Which library images an attempt still needs. A disc without one of them is
 * common, so a retry only asks for the image whose texture is still absent
 * instead of re-reading the other one. */
enum {
    NK_ISO_ART_ICON = 1u << 0,
    NK_ISO_ART_PICTURE = 1u << 1
};

/* Read the wanted library images while opening/indexing the ISO once. An image
 * whose bit is absent from `wanted` is left as not read
 * (NK_ICON_ERR_INVALID_PARAM, no buffer) and the ISO is still opened only when
 * at least one image was wanted. Returned image buffers are owned by the caller
 * and must be freed. */
void nk_iso_read_game_art(const char *iso_path, unsigned wanted,
                          NkIsoImageData *icon, NkIsoImageData *background);

#endif /* NAKAGAWA_ISO_READER_H */
