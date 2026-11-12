/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_VORBIS_H
#define PLAYER_UI_VORBIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The launcher music's decoder: Ogg Vorbis from memory to 16-bit PCM, through
   the vendored stb_vorbis (third_party/stb/stb_vorbis.c, built only into the
   player by player_ui_vorbis.c). The whole stream is decoded at once, so the
   loop can wrap on an exact frame. */

typedef enum {
    PLAYER_UI_VORBIS_OK = 0,
    PLAYER_UI_VORBIS_NOT_VORBIS,   /* not an Ogg Vorbis stream stb_vorbis can open */
    PLAYER_UI_VORBIS_UNSUPPORTED,  /* channels, rate or length outside what the launcher plays */
    PLAYER_UI_VORBIS_INCOMPLETE,   /* fewer frames decoded than the stream declares */
    PLAYER_UI_VORBIS_NO_MEMORY,
    PLAYER_UI_VORBIS_CANCELLED
} PlayerUiVorbisResult;

/* Interleaved signed 16-bit frames. */
typedef struct {
    int16_t *samples;
    uint64_t frames;
    int channels;     /* 1 or 2 */
    int sample_rate;  /* Hz */
} PlayerUiPcm;

/* The longest stream decoded: the launcher keeps the whole track in memory. */
#define PLAYER_UI_VORBIS_MAX_SECONDS 600u

/* Returns true when the decode should stop (the music was turned off or the
   launcher is closing). May be NULL. */
typedef bool (*PlayerUiVorbisCancel)(void *user);

/* Decodes `size` bytes. On PLAYER_UI_VORBIS_OK `out` owns its samples (free
   them with player_ui_pcm_free); on any other result `out` is empty and
   `detail` says what was wrong, in words. */
PlayerUiVorbisResult player_ui_vorbis_decode(const unsigned char *bytes, size_t size,
                                             PlayerUiVorbisCancel cancel, void *cancel_user,
                                             PlayerUiPcm *out, char *detail,
                                             size_t detail_size);

void player_ui_pcm_free(PlayerUiPcm *pcm);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_VORBIS_H */
