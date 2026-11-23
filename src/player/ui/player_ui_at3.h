/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_AT3_H
#define PLAYER_UI_AT3_H

#include "player_ui_vorbis.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The decoder for a game's own startup clip, PSP_GAME/SND0.AT3 (the clip a PSP's
   home menu plays while the game is highlighted): a RIFF WAVE file holding
   ATRAC3 (format tag 0x0270) or ATRAC3+ (WAVE_FORMAT_EXTENSIBLE, 0xFFFE) frames.
   The frames are decoded from memory to 16-bit PCM through the project's ATRAC
   import (src/rt/atrac3p/, FFmpeg n4.4), the same decoder the runtime's
   sceAtrac uses. It is SDL-free, and the player builds it with the import's
   include paths on their own (player_ui_at3.c is the only player unit that
   sees them).

   Only "fmt " and "data" are read. The clip is decoded whole, so the launcher
   can loop it on an exact frame; its loop is the stretch between its first and
   last frames that are not digital silence (player_ui_music_mix_init), as for
   the launcher music. */

typedef enum {
    PLAYER_UI_AT3_OK = 0,
    PLAYER_UI_AT3_NOT_RIFF,      /* not a RIFF WAVE file */
    PLAYER_UI_AT3_UNSUPPORTED,   /* a format, channel count, rate or frame size the launcher refuses */
    PLAYER_UI_AT3_DAMAGED,       /* a chunk runs past the file, no frames, or a frame does not decode */
    PLAYER_UI_AT3_NO_MEMORY,
    PLAYER_UI_AT3_CANCELLED,
    PLAYER_UI_AT3_TOO_LARGE      /* the decoded samples would exceed the caller's limit */
} PlayerUiAt3Result;

/* Returns true when the decode should stop. May be NULL. */
typedef bool (*PlayerUiAt3Cancel)(void *user);

/* Decodes `size` bytes of SND0.AT3. The samples buffer is at most
   `max_pcm_bytes`: a file whose frames would decode to more is refused
   (PLAYER_UI_AT3_TOO_LARGE) before anything is allocated. On PLAYER_UI_AT3_OK
   `out` owns its samples (free them with player_ui_pcm_free); on any other
   result `out` is empty and `detail` says what was wrong, in words (never a
   path). */
PlayerUiAt3Result player_ui_at3_decode(const uint8_t *bytes, size_t size, uint64_t max_pcm_bytes,
                                       PlayerUiAt3Cancel cancel, void *cancel_user,
                                       PlayerUiPcm *out, char *detail, size_t detail_size);

/* "ok", "not_riff", "unsupported", "damaged", "no_memory", "cancelled", "too_large". */
const char *player_ui_at3_result_name(PlayerUiAt3Result result);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_AT3_H */
