/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_MUSIC_H
#define PLAYER_UI_MUSIC_H

#include "player_ui_vorbis.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional launcher music (#2207): one Ogg Vorbis track that ships beside the
   player (assets/music/launcher/), off by default (the launcher_music setting).
   It plays only while the launcher shows: it fades in over 1.5 s, fades out
   over 0.5 s and releases the audio device before a game starts or when the
   launcher window loses focus, and picks up where it stopped when the launcher
   is back. The track is decoded on a worker thread and loops between its first
   and last non-silent frames, so the loop has no gap and no click. A game that
   starts cancels and joins a decode still running, and the decoded track (about
   22 MB) is freed while a game runs: it is decoded again on its worker (about
   half a second) when the launcher is back. The track is read and decoded only
   when it would be heard: the setting on, the launcher showing with focus, and a
   Music volume above 0. Otherwise no audio device is opened either. A
   missing or damaged file, or no audio device, is a named state, never a
   pretend play. */

typedef enum {
    PLAYER_UI_MUSIC_OFF = 0,     /* the setting is off: nothing loaded, no audio */
    PLAYER_UI_MUSIC_LOADING,     /* the track is being read and decoded */
    PLAYER_UI_MUSIC_PLAYING,     /* audible, or fading in */
    PLAYER_UI_MUSIC_FADING_OUT,  /* fading before a game starts, the window loses focus or
                                    the setting turns off */
    PLAYER_UI_MUSIC_PAUSED,      /* no focus, or Music volume 0: no audio device held */
    PLAYER_UI_MUSIC_STOPPED,     /* a game starts or runs: no audio device held */
    PLAYER_UI_MUSIC_MISSING,     /* the track file is not there */
    PLAYER_UI_MUSIC_DAMAGED,     /* the file could not be read, decoded, or is silent */
    PLAYER_UI_MUSIC_NO_DEVICE,   /* no audio device could be opened */
    PLAYER_UI_MUSIC_STATE_COUNT
} PlayerUiMusicState;

/* What the launcher wants this frame. */
typedef struct {
    bool enabled;      /* the launcher_music setting */
    bool in_launcher;  /* no game is starting or running */
    bool focused;      /* the launcher window has input focus */
    int volume;        /* launcher_music_volume, 0 to 100 */
} PlayerUiMusicRequest;

#define PLAYER_UI_MUSIC_FADE_IN_MS 1500u
#define PLAYER_UI_MUSIC_FADE_OUT_MS 500u
/* A volume step, and turning the setting off, ramps this long (or, during a
   fade, as long as the fade has left), so the change makes no click. */
#define PLAYER_UI_MUSIC_VOLUME_RAMP_MS 50u
/* After a fade-out the device is released once the music has rendered silence,
   or once the fade's time and this much more has passed, whichever is first:
   an audio device that stopped asking for data cannot hold a game back. A
   release at the time limit is reported (player_ui_music_take_release_timeout). */
#define PLAYER_UI_MUSIC_RELEASE_GRACE_MS 250u
/* A track file larger than this is refused as damaged before it is read. */
#define PLAYER_UI_MUSIC_MAX_FILE_BYTES (64u * 1024u * 1024u)
/* The decoded track (16-bit samples) is at most this large: a stream that
   would decode to more is refused as damaged before its buffer is allocated.
   The shipped track decodes to about 22 MB. */
#define PLAYER_UI_MUSIC_MAX_PCM_BYTES (64u * 1024u * 1024u)

/* The track, relative to the player's folder (staged by
   tools/stage_runtime_dlls.py with the launcher's fonts). A development run from
   the source tree finds it relative to the working folder instead. */
#define PLAYER_UI_MUSIC_TRACK "assets/music/launcher/ChillLofi.ogg"

typedef struct PlayerUiMusic PlayerUiMusic;

/* `path` is the file to play; `label` names it in messages (the path relative
   to the player's folder, so a log never shows a user's folders). */
PlayerUiMusic *player_ui_music_create(const char *path, const char *label);
void player_ui_music_destroy(PlayerUiMusic *music);

/* Called once per launcher frame with what the launcher wants. */
void player_ui_music_update(PlayerUiMusic *music, const PlayerUiMusicRequest *request,
                            uint64_t now_ms);

PlayerUiMusicState player_ui_music_state(const PlayerUiMusic *music);
/* "off", "loading", "playing", "fading_out", "paused", "stopped", "missing",
   "damaged", "no_device". */
const char *player_ui_music_state_name(PlayerUiMusicState state);
/* What went wrong for MISSING, DAMAGED and NO_DEVICE, in words; "" otherwise. */
const char *player_ui_music_detail(const PlayerUiMusic *music);
/* Whether the music holds an audio stream (and SDL's audio subsystem). */
bool player_ui_music_holds_audio(const PlayerUiMusic *music);
/* Whether a worker is reading or decoding the track. */
bool player_ui_music_decoding(const PlayerUiMusic *music);
/* When the last release closed the device at the time limit, before the music
   had rendered silence (the device stopped taking data): the milliseconds from
   the fade's start to the close, once; 0 otherwise. */
uint32_t player_ui_music_take_release_timeout(PlayerUiMusic *music);
/* The Settings row's words: "Off", "On", or what keeps the music from playing
   while it is on ("On: file missing", "On: file damaged", "On: no sound device"). */
const char *player_ui_music_row_value(bool on, PlayerUiMusicState state);
/* The gain the music is rendered at now, 0 to 1 (0 while it holds no stream). */
float player_ui_music_gain(const PlayerUiMusic *music);

#if defined(NK_PLAYER_UI_REGRESSION_TEST) || defined(PLAYER_UI_MUSIC_TEST_SEAMS)
/* Test seams, process-wide. hold_decode: a decode waits, before its first
   frame, until it is cancelled. stall_release: the audio callback stops
   rendering once the music is told to fade to silence, as a device that stopped
   taking data would. no_thread: the decode worker cannot start. */
void player_ui_music_test_hold_decode(bool on);
void player_ui_music_test_stall_release(bool on);
void player_ui_music_test_no_thread(bool on);
/* The decoded track's frames held (0 when none), the loop cursor, and the
   frames the current gain ramp has left. */
uint64_t player_ui_music_test_pcm_frames(const PlayerUiMusic *music);
uint64_t player_ui_music_test_cursor(const PlayerUiMusic *music);
uint64_t player_ui_music_test_ramp_frames(const PlayerUiMusic *music);
#endif

/* The loop and the fades, apart from SDL, so they can be checked frame by frame.
   The audio callback renders through these with the stream locked. */
typedef struct {
    const int16_t *samples;   /* interleaved; not owned */
    int channels;
    uint64_t loop_start;      /* first frame played */
    uint64_t loop_end;        /* one past the last frame played */
    uint64_t cursor;
    float gain;               /* current linear gain */
    float target;
    float step;               /* per frame, toward target */
    uint64_t ramp_left;       /* frames until the gain is exactly target */
} PlayerUiMusicMix;

/* Sets the loop to the first and last frames that are not digital silence and
   puts the cursor at its start, at gain 0. */
void player_ui_music_mix_init(PlayerUiMusicMix *mix, const PlayerUiPcm *pcm);
/* Whether the track has any sound to loop. */
bool player_ui_music_mix_has_sound(const PlayerUiMusicMix *mix);
/* Ramps the gain to `target` over `frames` frames (0: at once). */
void player_ui_music_mix_ramp(PlayerUiMusicMix *mix, float target, uint64_t frames);
/* Writes `frames` frames of the loop at the ramping gain. */
void player_ui_music_mix_render(PlayerUiMusicMix *mix, int16_t *out, uint32_t frames);
/* The gain has reached 0 and stays there. */
bool player_ui_music_mix_silent(const PlayerUiMusicMix *mix);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_MUSIC_H */
