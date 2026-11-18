/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_ART_H
#define PLAYER_UI_ART_H

#include "nk_ui_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Disc art for the next UI: each library title's ICON0.PNG, and PIC1.PNG for
   the title being viewed, read from the user's own disc image and decoded on a
   background thread, one disc at a time. The images live in memory only: they
   are never written to disk, cached in a file, or sent anywhere. A disc
   without an image, an unreadable disc, or a PNG over the limits below simply
   has no image, and the card keeps its placeholder; nothing is retried in the
   same session. */

/* The PSP's own sizes are 144 x 80 (ICON0) and 480 x 272 (PIC1); anything far
   larger is refused before it is decoded. The encoded size is already bounded
   by the ISO reader (NK_ICON_MAX_BYTES). */
#define PLAYER_UI_ART_ICON_MAX_SIDE 512u
#define PLAYER_UI_ART_PICTURE_MAX_SIDE 1024u
#define PLAYER_UI_ART_MAX_BYTES (1024u * 1024u)
/* Downloaded box cover art (thumbnails.libretro.com PSP scans ~512 x 885 up
   to several MB) uses separate bounds from disc ICON0/PIC1 art. */
#define PLAYER_UI_ART_COVER_MAX_SIDE 2048u
#define PLAYER_UI_ART_COVER_MAX_BYTES (8u * 1024u * 1024u)
/* Discs whose art is kept; the least recently shown goes first. */
#define PLAYER_UI_ART_SLOTS 64u

enum {
    PLAYER_UI_ART_ICON = 1u << 0,
    PLAYER_UI_ART_PICTURE = 1u << 1,
    PLAYER_UI_ART_COVER = 1u << 2
};

/* Reads the wanted images' PNG bytes from a disc image (bit set in `wanted`),
   returning malloc'd buffers the caller frees; a missing image is NULL / 0.
   The player uses the ISO reader; tests pass their own. Runs on the worker. */
typedef void (*PlayerUiArtReader)(const char *iso_path, unsigned wanted,
                                  uint8_t **icon, size_t *icon_size,
                                  uint8_t **picture, size_t *picture_size);

typedef struct PlayerUiArt PlayerUiArt;

/* A reader is required: a NULL reader is refused (returns NULL), as is a
   creation that runs out of memory. The player passes its ISO reader. */
PlayerUiArt *player_ui_art_create(PlayerUiArtReader reader);
/* Waits for a running read, then frees every image. */
void player_ui_art_destroy(PlayerUiArt *art);

/* Once per frame, before the frame's player_ui_art_get calls: takes a
   finished read's images, then starts the next wanted read. Images returned
   by player_ui_art_get stay valid until the next pump. */
void player_ui_art_pump(PlayerUiArt *art);

/* The images for a disc image path, asking for them on first sight: always
   the icon, and the picture too when want_picture. Either out pointer gets
   NULL until its image is decoded, and for good when there is none. */
void player_ui_art_get(PlayerUiArt *art, const char *iso_path, bool want_picture,
                       const NkUiImage **icon, const NkUiImage **picture);

/* Same as player_ui_art_get, optionally decoding a cached cover PNG at
   `cover_path` (NULL or "" omits cover art and frees any stale cover). */
void player_ui_art_get_title(PlayerUiArt *art, const char *iso_path, const char *cover_path,
                             bool want_picture, const NkUiImage **icon,
                             const NkUiImage **picture, const NkUiImage **cover);

/* Clear all decoded cover art thumbnails so subsequent requests reload from
   the cache folder (used when Refresh cover art clears the cache). */
void player_ui_art_invalidate_covers(PlayerUiArt *art);

/* True while the worker may still have a cached cover file open: a cache delete
   waits for it to finish (Windows refuses to delete an open file). */
bool player_ui_art_reading_cover(const PlayerUiArt *art);

/* True while a read runs or one is waiting to start (tests wait on it). */
bool player_ui_art_busy(const PlayerUiArt *art);

/* The host loop's wait, at most wait_ms: while the loader is busy, about one
   60 Hz frame (PLAYER_UI_ART_POLL_MS), so each finished read is collected and
   the next disc's read started on the next frame instead of at the idle wake;
   otherwise wait_ms. */
#define PLAYER_UI_ART_POLL_MS 16u
uint32_t player_ui_art_wait_ms(const PlayerUiArt *art, uint32_t wait_ms);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_ART_H */
