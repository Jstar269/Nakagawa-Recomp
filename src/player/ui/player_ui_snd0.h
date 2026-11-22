/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_SND0_H
#define PLAYER_UI_SND0_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A game's own startup clip on highlight: while a game's card in the library is
   highlighted (it has focus, or the pointer is over it) the launcher plays that
   game's PSP_GAME/SND0.AT3, the clip a PSP's home menu plays for it. The clip is
   read from the user's own disc image only when it would be heard, decoded in
   memory on a worker thread (player_ui_at3.c) and never written anywhere.

   - It starts once the highlight has rested on the game for
     PLAYER_UI_SND0_REST_MS, so browsing past cards reads nothing, fades in over
     PLAYER_UI_SND0_FADE_IN_MS and loops (player_ui_music_mix_*).
   - It fades out over PLAYER_UI_SND0_FADE_OUT_MS, and releases the audio
     device, when the highlight moves, the window loses focus, the setting turns
     off or a game starts; a game's launch waits for that
     (player_ui_snd0_holds_audio, player_ui_snd0_decoding).
   - A disc without SND0.AT3 is silent with no error (PLAYER_UI_SND0_NO_CLIP). A
     file that cannot be read or decoded is silent for that game
     (PLAYER_UI_SND0_DAMAGED) and its reason is kept for the developer log.
   - The volume is the launcher's Music volume; at 0 nothing is read or opened. */

typedef enum {
    PLAYER_UI_SND0_OFF = 0,     /* the setting is off: nothing read, no audio */
    PLAYER_UI_SND0_IDLE,        /* no game highlighted, the highlight still resting,
                                   or the window away or the volume 0 */
    PLAYER_UI_SND0_LOADING,     /* the clip is being read and decoded */
    PLAYER_UI_SND0_PLAYING,     /* audible, or fading in */
    PLAYER_UI_SND0_FADING_OUT,  /* fading before the device is released */
    PLAYER_UI_SND0_STOPPED,     /* a game starts or runs: nothing held */
    PLAYER_UI_SND0_NO_CLIP,     /* the highlighted disc has no SND0.AT3 (or no image) */
    PLAYER_UI_SND0_DAMAGED,     /* the clip could not be read or decoded, or is silent */
    PLAYER_UI_SND0_NO_DEVICE,   /* no audio device could be opened */
    PLAYER_UI_SND0_STATE_COUNT
} PlayerUiSnd0State;

typedef struct {
    bool enabled;          /* the launcher_game_music setting */
    bool in_launcher;      /* no game is starting or running */
    bool focused;          /* the launcher window has input focus */
    int volume;            /* launcher_music_volume, 0 to 100 */
    const char *iso_path;  /* the highlighted game's disc image; NULL or "" for none */
} PlayerUiSnd0Request;

/* As the art rest (NK_UI_MOTION_ART_REST, 0.6 s). */
#define PLAYER_UI_SND0_REST_MS 600u
#define PLAYER_UI_SND0_FADE_IN_MS 300u
#define PLAYER_UI_SND0_FADE_OUT_MS 300u
/* A volume step, and turning the setting off, ramps this long. */
#define PLAYER_UI_SND0_VOLUME_RAMP_MS 50u
/* After a fade-out the device is released once the clip has rendered silence,
   or once the fade's time and this much more has passed. */
#define PLAYER_UI_SND0_RELEASE_GRACE_MS 250u
/* SND0.AT3 files larger than this are refused before they are read. */
#define PLAYER_UI_SND0_MAX_FILE_BYTES (4u * 1024u * 1024u)
/* The decoded clip (16-bit samples) is at most this large: about 3 minutes of
   44.1 kHz stereo. */
#define PLAYER_UI_SND0_MAX_PCM_BYTES (32u * 1024u * 1024u)
/* The clip's place on the disc. */
#define PLAYER_UI_SND0_PATH "PSP_GAME/SND0.AT3"

typedef enum {
    PLAYER_UI_SND0_READ_OK = 0,
    PLAYER_UI_SND0_READ_MISSING,   /* no image, or no clip on it: silent, not an error */
    PLAYER_UI_SND0_READ_FAILED     /* there, but too large or unreadable */
} PlayerUiSnd0ReadResult;

/* Reads the clip of `iso_path` (at most PLAYER_UI_SND0_MAX_FILE_BYTES) into a
   malloc'd buffer the caller frees. Runs on the worker thread. */
typedef PlayerUiSnd0ReadResult (*PlayerUiSnd0Reader)(const char *iso_path, uint8_t **bytes,
                                                     size_t *size, void *user);

typedef struct PlayerUiSnd0 PlayerUiSnd0;

PlayerUiSnd0 *player_ui_snd0_create(PlayerUiSnd0Reader reader, void *reader_user);
void player_ui_snd0_destroy(PlayerUiSnd0 *snd0);

/* Called once per launcher frame with what the launcher wants. */
void player_ui_snd0_update(PlayerUiSnd0 *snd0, const PlayerUiSnd0Request *request,
                           uint64_t now_ms);

PlayerUiSnd0State player_ui_snd0_state(const PlayerUiSnd0 *snd0);
/* "off", "idle", "loading", "playing", "fading_out", "stopped", "no_clip",
   "damaged", "no_device". */
const char *player_ui_snd0_state_name(PlayerUiSnd0State state);
/* What went wrong for DAMAGED and NO_DEVICE, in words (never a path); "" otherwise. */
const char *player_ui_snd0_detail(const PlayerUiSnd0 *snd0);
/* Whether the clip holds an audio stream (and SDL's audio subsystem). */
bool player_ui_snd0_holds_audio(const PlayerUiSnd0 *snd0);
/* Whether a worker is reading or decoding a clip. */
bool player_ui_snd0_decoding(const PlayerUiSnd0 *snd0);
/* Whether the launcher music should pause for a clip: one is loading or heard. */
bool player_ui_snd0_quiets_music(const PlayerUiSnd0 *snd0);
/* The milliseconds until the clip needs a frame (the rest's end, or a frame
   while it loads or fades), at most `idle_ms`. */
uint32_t player_ui_snd0_wait_ms(const PlayerUiSnd0 *snd0, uint64_t now_ms, uint32_t idle_ms);
/* The gain the clip is rendered at now, 0 to 1 (0 while it holds no stream). */
float player_ui_snd0_gain(const PlayerUiSnd0 *snd0);

#if defined(NK_PLAYER_UI_REGRESSION_TEST) || defined(PLAYER_UI_SND0_TEST_SEAMS)
/* The decoded clip's frames held (0 when none), and how many clips were read. */
uint64_t player_ui_snd0_test_pcm_frames(const PlayerUiSnd0 *snd0);
unsigned player_ui_snd0_test_reads(const PlayerUiSnd0 *snd0);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_SND0_H */
