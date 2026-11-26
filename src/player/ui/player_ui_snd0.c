/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_snd0.h"

#include "player_ui_at3.h"
#include "player_ui_music.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frames the audio callback renders per SDL_PutAudioStreamData call. */
#define PLAYER_UI_SND0_FEED_FRAMES 1024
/* The disc image paths the launcher keeps are at most this long. */
#define PLAYER_UI_SND0_PATH_MAX 4096

struct PlayerUiSnd0 {
    PlayerUiSnd0Reader reader;
    void *reader_user;
    PlayerUiSnd0State state;
    char detail[256];
    /* The disc image of the game whose page is open. */
    char path[PLAYER_UI_SND0_PATH_MAX];
    /* The worker that reads and decodes a clip. It owns load_path and writes
       only the load_* fields, which this thread reads after SDL_WaitThread. */
    SDL_Thread *loader;
    SDL_AtomicInt loader_done;
    SDL_AtomicInt cancel;
    char load_path[PLAYER_UI_SND0_PATH_MAX];
    PlayerUiSnd0State load_failure;    /* IDLE (none), NO_CLIP or DAMAGED */
    char load_detail[160];
    PlayerUiPcm loaded;
    unsigned reads;
    /* The decoded clip of `path`, and its loop. */
    PlayerUiPcm pcm;
    bool have_pcm;
    bool stale;                        /* the clip is the last game's, still fading */
    PlayerUiSnd0State failure;         /* IDLE (none), NO_CLIP, DAMAGED or NO_DEVICE, for `path` */
    PlayerUiMusicMix mix;
    /* The audio stream, only while the clip is heard or fading. */
    SDL_AudioStream *stream;
    bool fading_out;
    uint64_t fade_started_ms;
    uint32_t fade_ms;
    float volume_gain;
};

const char *player_ui_snd0_state_name(PlayerUiSnd0State state) {
    switch (state) {
    case PLAYER_UI_SND0_OFF: return "off";
    case PLAYER_UI_SND0_IDLE: return "idle";
    case PLAYER_UI_SND0_LOADING: return "loading";
    case PLAYER_UI_SND0_PLAYING: return "playing";
    case PLAYER_UI_SND0_FADING_OUT: return "fading_out";
    case PLAYER_UI_SND0_STOPPED: return "stopped";
    case PLAYER_UI_SND0_NO_CLIP: return "no_clip";
    case PLAYER_UI_SND0_DAMAGED: return "damaged";
    case PLAYER_UI_SND0_NO_DEVICE: return "no_device";
    case PLAYER_UI_SND0_STATE_COUNT:
    default: return "unknown";
    }
}

PlayerUiSnd0 *player_ui_snd0_create(PlayerUiSnd0Reader reader, void *reader_user) {
    PlayerUiSnd0 *snd0;
    if (reader == NULL) return NULL;
    snd0 = (PlayerUiSnd0 *)calloc(1u, sizeof(*snd0));
    if (snd0 == NULL) return NULL;
    snd0->reader = reader;
    snd0->reader_user = reader_user;
    return snd0;
}

static bool player_ui_snd0_cancelled(void *user) {
    PlayerUiSnd0 *snd0 = (PlayerUiSnd0 *)user;
    return SDL_GetAtomicInt(&snd0->cancel) != 0;
}

/* The loader: reads the clip from the disc image and decodes it. */
static int SDLCALL player_ui_snd0_load(void *data) {
    PlayerUiSnd0 *snd0 = (PlayerUiSnd0 *)data;
    uint8_t *bytes = NULL;
    size_t size = 0u;
    PlayerUiSnd0ReadResult read;
    snd0->load_failure = PLAYER_UI_SND0_IDLE;
    snd0->load_detail[0] = '\0';
    read = snd0->reader(snd0->load_path, &bytes, &size, snd0->reader_user,
                        player_ui_snd0_cancelled, snd0);
    if (player_ui_snd0_cancelled(snd0)) goto done;
    if (read == PLAYER_UI_SND0_READ_MISSING) {
        snd0->load_failure = PLAYER_UI_SND0_NO_CLIP;
    } else if (read != PLAYER_UI_SND0_READ_OK || bytes == NULL) {
        snd0->load_failure = PLAYER_UI_SND0_DAMAGED;
        (void)snprintf(snd0->load_detail, sizeof(snd0->load_detail),
                       "the clip is too large or could not be read");
    } else {
        char reason[128];
        PlayerUiAt3Result result = player_ui_at3_decode(
            bytes, size, PLAYER_UI_SND0_MAX_PCM_BYTES, player_ui_snd0_cancelled, snd0,
            &snd0->loaded, reason, sizeof(reason));
        if (result != PLAYER_UI_AT3_OK) {
            /* A cancelled load is discarded by the thread that cancelled it. */
            snd0->load_failure = PLAYER_UI_SND0_DAMAGED;
            (void)snprintf(snd0->load_detail, sizeof(snd0->load_detail), "%s (%s)",
                           reason, player_ui_at3_result_name(result));
        }
    }
done:
    free(bytes);
    SDL_SetAtomicInt(&snd0->loader_done, 1);
    return 0;
}

static void player_ui_snd0_close_device(PlayerUiSnd0 *snd0) {
    if (snd0->stream == NULL) return;
    /* Destroying a device stream stops its callback and closes the device. */
    SDL_DestroyAudioStream(snd0->stream);
    snd0->stream = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    snd0->fading_out = false;
    snd0->mix.gain = 0.0f;
    snd0->mix.target = 0.0f;
    snd0->mix.step = 0.0f;
    snd0->mix.ramp_left = 0u;
}

static void player_ui_snd0_cancel_load(PlayerUiSnd0 *snd0) {
    if (snd0->loader != NULL) {
        SDL_SetAtomicInt(&snd0->cancel, 1);
        SDL_WaitThread(snd0->loader, NULL);
        snd0->loader = NULL;
        player_ui_pcm_free(&snd0->loaded);
    }
    SDL_SetAtomicInt(&snd0->cancel, 0);
    SDL_SetAtomicInt(&snd0->loader_done, 0);
}

static void player_ui_snd0_free_clip(PlayerUiSnd0 *snd0) {
    player_ui_pcm_free(&snd0->pcm);
    memset(&snd0->mix, 0, sizeof(snd0->mix));
    snd0->have_pcm = false;
    snd0->stale = false;
}

void player_ui_snd0_destroy(PlayerUiSnd0 *snd0) {
    if (snd0 == NULL) return;
    player_ui_snd0_cancel_load(snd0);
    player_ui_snd0_close_device(snd0);
    player_ui_snd0_free_clip(snd0);
    free(snd0);
}

static void player_ui_snd0_take_load(PlayerUiSnd0 *snd0) {
    SDL_SetAtomicInt(&snd0->loader_done, 0);
    if (snd0->load_failure != PLAYER_UI_SND0_IDLE) {
        snd0->failure = snd0->load_failure;
        (void)snprintf(snd0->detail, sizeof(snd0->detail), "%s", snd0->load_detail);
        player_ui_pcm_free(&snd0->loaded);
        return;
    }
    snd0->pcm = snd0->loaded;
    memset(&snd0->loaded, 0, sizeof(snd0->loaded));
    player_ui_music_mix_init(&snd0->mix, &snd0->pcm);
    if (!player_ui_music_mix_has_sound(&snd0->mix)) {
        snd0->failure = PLAYER_UI_SND0_DAMAGED;
        (void)snprintf(snd0->detail, sizeof(snd0->detail), "the clip decodes to silence");
        player_ui_snd0_free_clip(snd0);
        return;
    }
    snd0->have_pcm = true;
}

static void player_ui_snd0_start_load(PlayerUiSnd0 *snd0) {
    (void)snprintf(snd0->load_path, sizeof(snd0->load_path), "%s", snd0->path);
    SDL_SetAtomicInt(&snd0->cancel, 0);
    SDL_SetAtomicInt(&snd0->loader_done, 0);
    ++snd0->reads;
    snd0->loader = SDL_CreateThread(player_ui_snd0_load, "nakagawa-snd0", snd0);
    if (snd0->loader == NULL) {
        /* No worker: read and decode here, as a launcher without threads would. */
        (void)player_ui_snd0_load(snd0);
        player_ui_snd0_take_load(snd0);
    }
}

static uint64_t player_ui_snd0_frames(const PlayerUiSnd0 *snd0, uint32_t ms) {
    return (uint64_t)snd0->pcm.sample_rate * (uint64_t)ms / 1000u;
}

static void SDLCALL player_ui_snd0_feed(void *user, SDL_AudioStream *stream,
                                        int additional_amount, int total_amount) {
    PlayerUiSnd0 *snd0 = (PlayerUiSnd0 *)user;
    int16_t chunk[PLAYER_UI_SND0_FEED_FRAMES * 2];
    int frame_bytes = snd0->mix.channels * (int)sizeof(int16_t);
    (void)total_amount;
    while (additional_amount > 0) {
        int frames = (additional_amount + frame_bytes - 1) / frame_bytes;
        if (frames > PLAYER_UI_SND0_FEED_FRAMES) frames = PLAYER_UI_SND0_FEED_FRAMES;
        player_ui_music_mix_render(&snd0->mix, chunk, (uint32_t)frames);
        if (!SDL_PutAudioStreamData(stream, chunk, frames * frame_bytes)) break;
        additional_amount -= frames * frame_bytes;
    }
}

static bool player_ui_snd0_open_device(PlayerUiSnd0 *snd0, float gain) {
    SDL_AudioSpec spec;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        (void)snprintf(snd0->detail, sizeof(snd0->detail), "no audio (%s)", SDL_GetError());
        return false;
    }
    spec.format = SDL_AUDIO_S16;
    spec.channels = snd0->pcm.channels;
    spec.freq = snd0->pcm.sample_rate;
    snd0->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                             player_ui_snd0_feed, snd0);
    if (snd0->stream == NULL) {
        (void)snprintf(snd0->detail, sizeof(snd0->detail), "no audio device (%s)",
                       SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    /* The device starts paused, so the callback cannot run yet. */
    snd0->mix.gain = 0.0f;
    player_ui_music_mix_ramp(&snd0->mix, gain,
                             player_ui_snd0_frames(snd0, PLAYER_UI_SND0_FADE_IN_MS));
    if (!SDL_ResumeAudioStreamDevice(snd0->stream)) {
        (void)snprintf(snd0->detail, sizeof(snd0->detail),
                       "the audio device did not start (%s)", SDL_GetError());
        player_ui_snd0_close_device(snd0);
        return false;
    }
    return true;
}

static void player_ui_snd0_locked_ramp(PlayerUiSnd0 *snd0, float target, uint32_t ms) {
    if (SDL_LockAudioStream(snd0->stream)) {
        uint64_t frames = player_ui_snd0_frames(snd0, ms);
        /* A step during the fade-in does not cut the fade short. */
        if (target > 0.0f && snd0->mix.ramp_left > frames) frames = snd0->mix.ramp_left;
        player_ui_music_mix_ramp(&snd0->mix, target, frames);
        (void)SDL_UnlockAudioStream(snd0->stream);
    }
}

/* Fades the clip to silence over `fade_ms` (a fade already running keeps its
   own length) and closes the device once it has rendered silence, or once the
   fade's time and PLAYER_UI_SND0_RELEASE_GRACE_MS have passed: a device that
   stopped taking data cannot hold a game back. Returns whether it is closed. */
static bool player_ui_snd0_release(PlayerUiSnd0 *snd0, uint32_t fade_ms, uint64_t now_ms) {
    bool silent = false;
    if (snd0->stream == NULL) return true;
    if (!snd0->fading_out) {
        player_ui_snd0_locked_ramp(snd0, 0.0f, fade_ms);
        snd0->fading_out = true;
        snd0->fade_started_ms = now_ms;
        snd0->fade_ms = fade_ms;
    }
    if (SDL_LockAudioStream(snd0->stream)) {
        silent = player_ui_music_mix_silent(&snd0->mix);
        (void)SDL_UnlockAudioStream(snd0->stream);
    }
    if (!silent &&
        now_ms - snd0->fade_started_ms < (uint64_t)snd0->fade_ms + PLAYER_UI_SND0_RELEASE_GRACE_MS) {
        return false;
    }
    player_ui_snd0_close_device(snd0);
    return true;
}

void player_ui_snd0_update(PlayerUiSnd0 *snd0, const PlayerUiSnd0Request *request,
                           uint64_t now_ms) {
    const char *want;
    float gain;
    bool audible;
    if (snd0 == NULL || request == NULL) return;
    if (!request->enabled || !request->in_launcher) {
        /* Turned off, or a game starts: the read stops and its worker is joined
           before the launch may start the game's process, then the clip fades
           and lets go of the device. Nothing is kept for the game. */
        player_ui_snd0_cancel_load(snd0);
        if (!player_ui_snd0_release(snd0, request->enabled ? PLAYER_UI_SND0_FADE_OUT_MS
                                                          : PLAYER_UI_SND0_VOLUME_RAMP_MS,
                                    now_ms)) {
            snd0->state = PLAYER_UI_SND0_FADING_OUT;
            return;
        }
        player_ui_snd0_free_clip(snd0);
        snd0->path[0] = '\0';
        snd0->failure = PLAYER_UI_SND0_IDLE;
        snd0->detail[0] = '\0';
        snd0->state = request->enabled ? PLAYER_UI_SND0_STOPPED : PLAYER_UI_SND0_OFF;
        return;
    }
    want = request->iso_path != NULL ? request->iso_path : "";
    if (strlen(want) >= sizeof(snd0->path)) want = "";
    if (strcmp(want, snd0->path) != 0) {
        /* Another page: the last game's read stops, and its clip fades out
           before the next one may open the device. */
        player_ui_snd0_cancel_load(snd0);
        if (snd0->stream != NULL) snd0->stale = true;
        else player_ui_snd0_free_clip(snd0);
        snd0->failure = PLAYER_UI_SND0_IDLE;
        snd0->detail[0] = '\0';
        (void)snprintf(snd0->path, sizeof(snd0->path), "%s", want);
    }
    gain = (float)(request->volume < 0 ? 0 : request->volume > 100 ? 100 : request->volume) /
           100.0f;
    audible = request->focused && gain > 0.0f && snd0->path[0] != '\0';
    if (snd0->stream != NULL && (snd0->stale || !audible)) {
        if (!player_ui_snd0_release(snd0, PLAYER_UI_SND0_FADE_OUT_MS, now_ms)) {
            snd0->state = PLAYER_UI_SND0_FADING_OUT;
            return;
        }
    }
    if (snd0->stale) player_ui_snd0_free_clip(snd0);
    if (snd0->loader != NULL) {
        if (SDL_GetAtomicInt(&snd0->loader_done) == 0) {
            snd0->state = PLAYER_UI_SND0_LOADING;
            return;
        }
        SDL_WaitThread(snd0->loader, NULL);
        snd0->loader = NULL;
        player_ui_snd0_take_load(snd0);
    }
    if (snd0->failure != PLAYER_UI_SND0_IDLE) {
        snd0->state = snd0->failure;
        return;
    }
    if (!audible) {
        /* Away or at volume 0, the decoded clip is kept for its game, so coming
           back plays it again without another read. */
        snd0->state = PLAYER_UI_SND0_IDLE;
        return;
    }
    if (!snd0->have_pcm) {
        player_ui_snd0_start_load(snd0);
        if (snd0->loader != NULL) {
            snd0->state = PLAYER_UI_SND0_LOADING;
            return;
        }
        if (snd0->failure != PLAYER_UI_SND0_IDLE) {
            snd0->state = snd0->failure;
            return;
        }
    }
    if (snd0->stream == NULL) {
        if (!player_ui_snd0_open_device(snd0, gain)) {
            /* Sticky for this page: opening a game's page again tries again. */
            snd0->failure = PLAYER_UI_SND0_NO_DEVICE;
            snd0->state = PLAYER_UI_SND0_NO_DEVICE;
            return;
        }
    } else if (snd0->fading_out) {
        /* Audible again before a fade-out finished (focus back mid-fade): the
           device is still open, so the clip fades back in from where it is. */
        snd0->fading_out = false;
        player_ui_snd0_locked_ramp(snd0, gain, PLAYER_UI_SND0_FADE_IN_MS);
    } else if (gain != snd0->volume_gain) {
        player_ui_snd0_locked_ramp(snd0, gain, PLAYER_UI_SND0_VOLUME_RAMP_MS);
    }
    snd0->volume_gain = gain;
    snd0->state = PLAYER_UI_SND0_PLAYING;
}

PlayerUiSnd0State player_ui_snd0_state(const PlayerUiSnd0 *snd0) {
    return snd0 != NULL ? snd0->state : PLAYER_UI_SND0_OFF;
}

const char *player_ui_snd0_detail(const PlayerUiSnd0 *snd0) {
    if (snd0 == NULL) return "";
    return snd0->state == PLAYER_UI_SND0_DAMAGED || snd0->state == PLAYER_UI_SND0_NO_DEVICE
               ? snd0->detail
               : "";
}

bool player_ui_snd0_holds_audio(const PlayerUiSnd0 *snd0) {
    return snd0 != NULL && snd0->stream != NULL;
}

bool player_ui_snd0_decoding(const PlayerUiSnd0 *snd0) {
    return snd0 != NULL && snd0->loader != NULL;
}

bool player_ui_snd0_quiets_music(const PlayerUiSnd0 *snd0) {
    return snd0 != NULL && ((snd0->stream != NULL && !snd0->fading_out) || snd0->loader != NULL);
}

uint32_t player_ui_snd0_wait_ms(const PlayerUiSnd0 *snd0, uint32_t idle_ms) {
    if (snd0 == NULL || (snd0->loader == NULL && !snd0->fading_out)) return idle_ms;
    return idle_ms < 16u ? idle_ms : 16u;
}

float player_ui_snd0_gain(const PlayerUiSnd0 *snd0) {
    float gain = 0.0f;
    if (snd0 == NULL || snd0->stream == NULL) return 0.0f;
    if (SDL_LockAudioStream(snd0->stream)) {
        gain = snd0->mix.gain;
        (void)SDL_UnlockAudioStream(snd0->stream);
    }
    return gain;
}

#if defined(NK_PLAYER_UI_REGRESSION_TEST) || defined(PLAYER_UI_SND0_TEST_SEAMS)
uint64_t player_ui_snd0_test_pcm_frames(const PlayerUiSnd0 *snd0) {
    return snd0 != NULL && snd0->have_pcm ? snd0->pcm.frames : 0u;
}

unsigned player_ui_snd0_test_reads(const PlayerUiSnd0 *snd0) {
    return snd0 != NULL ? snd0->reads : 0u;
}
#endif
