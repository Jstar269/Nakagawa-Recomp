/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_music.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frames the audio callback renders per SDL_PutAudioStreamData call. */
#define PLAYER_UI_MUSIC_FEED_FRAMES 1024

struct PlayerUiMusic {
    char *path;
    char *label;
    PlayerUiMusicState state;
    char detail[320];
    /* The worker that reads and decodes the track. It writes only the load_*
       fields, which this thread reads after SDL_WaitThread. */
    SDL_Thread *loader;
    SDL_AtomicInt loader_done;
    SDL_AtomicInt cancel;
    PlayerUiMusicState load_failure;   /* OFF, MISSING or DAMAGED */
    char load_detail[256];
    PlayerUiPcm loaded;
    /* The decoded track and its loop. */
    PlayerUiPcm pcm;
    bool have_pcm;
    PlayerUiMusicState failure;        /* OFF, MISSING or DAMAGED: sticky until turned off */
    PlayerUiMusicMix mix;
    /* The audio stream, only while the music is heard or fading. */
    SDL_AudioStream *stream;
    bool fading_out;
    uint64_t fade_started_ms;
    uint32_t fade_ms;                  /* the fade-out's length */
    float volume_gain;
    bool device_failed;                /* the last open failed while the music was wanted */
    bool was_audible;
    uint32_t release_timeout_ms;       /* a release at the time limit, not yet taken */
    /* Where the loop stood when the decoded track was freed for a game. */
    uint64_t resume_cursor;
    bool have_resume;
};

#if defined(NK_PLAYER_UI_REGRESSION_TEST) || defined(PLAYER_UI_MUSIC_TEST_SEAMS)
static SDL_AtomicInt player_ui_music_hold_decode;
static SDL_AtomicInt player_ui_music_stall_release;
static SDL_AtomicInt player_ui_music_no_thread;

void player_ui_music_test_hold_decode(bool on) {
    SDL_SetAtomicInt(&player_ui_music_hold_decode, on ? 1 : 0);
}

void player_ui_music_test_stall_release(bool on) {
    SDL_SetAtomicInt(&player_ui_music_stall_release, on ? 1 : 0);
}

void player_ui_music_test_no_thread(bool on) {
    SDL_SetAtomicInt(&player_ui_music_no_thread, on ? 1 : 0);
}

#define PLAYER_UI_MUSIC_HOLD_DECODE() (SDL_GetAtomicInt(&player_ui_music_hold_decode) != 0)
#define PLAYER_UI_MUSIC_STALL_RELEASE() (SDL_GetAtomicInt(&player_ui_music_stall_release) != 0)
#define PLAYER_UI_MUSIC_NO_THREAD() (SDL_GetAtomicInt(&player_ui_music_no_thread) != 0)
#else
#define PLAYER_UI_MUSIC_HOLD_DECODE() false
#define PLAYER_UI_MUSIC_STALL_RELEASE() false
#define PLAYER_UI_MUSIC_NO_THREAD() false
#endif

static char *player_ui_music_strdup(const char *text) {
    size_t length = strlen(text) + 1u;
    char *copy = (char *)malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

/* ---- the loop and the fades ---------------------------------------------- */

void player_ui_music_mix_init(PlayerUiMusicMix *mix, const PlayerUiPcm *pcm) {
    uint64_t first = 0u;
    uint64_t last = 0u;
    bool found = false;
    if (mix == NULL) return;
    memset(mix, 0, sizeof(*mix));
    if (pcm == NULL || pcm->samples == NULL || pcm->channels < 1) return;
    mix->samples = pcm->samples;
    mix->channels = pcm->channels;
    for (uint64_t frame = 0u; frame < pcm->frames && !found; ++frame) {
        for (int c = 0; c < pcm->channels; ++c) {
            if (pcm->samples[frame * (uint64_t)pcm->channels + (uint64_t)c] != 0) {
                first = frame;
                found = true;
                break;
            }
        }
    }
    if (!found) return;
    for (uint64_t frame = pcm->frames; frame > first; --frame) {
        bool sound = false;
        for (int c = 0; c < pcm->channels; ++c) {
            if (pcm->samples[(frame - 1u) * (uint64_t)pcm->channels + (uint64_t)c] != 0) {
                sound = true;
                break;
            }
        }
        if (sound) {
            last = frame;
            break;
        }
    }
    mix->loop_start = first;
    mix->loop_end = last;
    mix->cursor = first;
}

bool player_ui_music_mix_has_sound(const PlayerUiMusicMix *mix) {
    return mix != NULL && mix->samples != NULL && mix->loop_end > mix->loop_start;
}

void player_ui_music_mix_ramp(PlayerUiMusicMix *mix, float target, uint64_t frames) {
    if (mix == NULL) return;
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;
    mix->target = target;
    if (frames == 0u || mix->gain == target) {
        mix->gain = target;
        mix->step = 0.0f;
        mix->ramp_left = 0u;
        return;
    }
    mix->step = (target - mix->gain) / (float)frames;
    mix->ramp_left = frames;
}

void player_ui_music_mix_render(PlayerUiMusicMix *mix, int16_t *out, uint32_t frames) {
    if (mix == NULL || out == NULL) return;
    for (uint32_t f = 0u; f < frames; ++f) {
        int16_t *frame_out = out + (size_t)f * (size_t)(mix->channels > 0 ? mix->channels : 1);
        if (!player_ui_music_mix_has_sound(mix)) {
            for (int c = 0; c < (mix->channels > 0 ? mix->channels : 1); ++c) frame_out[c] = 0;
            continue;
        }
        if (mix->cursor < mix->loop_start || mix->cursor >= mix->loop_end) {
            mix->cursor = mix->loop_start;
        }
        for (int c = 0; c < mix->channels; ++c) {
            float value = (float)mix->samples[mix->cursor * (uint64_t)mix->channels +
                                              (uint64_t)c] * mix->gain;
            long rounded = (long)(value >= 0.0f ? value + 0.5f : value - 0.5f);
            if (rounded > 32767L) rounded = 32767L;
            if (rounded < -32768L) rounded = -32768L;
            frame_out[c] = (int16_t)rounded;
        }
        ++mix->cursor;
        if (mix->ramp_left > 0u) {
            /* Counted, not compared: the ramp ends on exactly its last frame. */
            --mix->ramp_left;
            if (mix->ramp_left == 0u) {
                mix->gain = mix->target;
                mix->step = 0.0f;
            } else {
                mix->gain += mix->step;
            }
        }
    }
}

bool player_ui_music_mix_silent(const PlayerUiMusicMix *mix) {
    return mix == NULL || (mix->gain == 0.0f && mix->target == 0.0f && mix->ramp_left == 0u);
}

/* ---- the player -------------------------------------------------------- */

const char *player_ui_music_state_name(PlayerUiMusicState state) {
    switch (state) {
    case PLAYER_UI_MUSIC_OFF: return "off";
    case PLAYER_UI_MUSIC_LOADING: return "loading";
    case PLAYER_UI_MUSIC_PLAYING: return "playing";
    case PLAYER_UI_MUSIC_FADING_OUT: return "fading_out";
    case PLAYER_UI_MUSIC_PAUSED: return "paused";
    case PLAYER_UI_MUSIC_STOPPED: return "stopped";
    case PLAYER_UI_MUSIC_MISSING: return "missing";
    case PLAYER_UI_MUSIC_DAMAGED: return "damaged";
    case PLAYER_UI_MUSIC_NO_DEVICE: return "no_device";
    case PLAYER_UI_MUSIC_STATE_COUNT:
    default: return "unknown";
    }
}

PlayerUiMusic *player_ui_music_create(const char *path, const char *label) {
    PlayerUiMusic *music;
    if (path == NULL || label == NULL) return NULL;
    music = (PlayerUiMusic *)calloc(1u, sizeof(*music));
    if (music == NULL) return NULL;
    music->path = player_ui_music_strdup(path);
    music->label = player_ui_music_strdup(label);
    if (music->path == NULL || music->label == NULL) {
        free(music->path);
        free(music->label);
        free(music);
        return NULL;
    }
    return music;
}

/* The decoder asks this before each 4096-frame chunk (about 93 ms of the
   track, a few milliseconds of decoding), so a cancelled decode stops promptly. */
static bool player_ui_music_cancelled(void *user) {
    PlayerUiMusic *music = (PlayerUiMusic *)user;
    while (PLAYER_UI_MUSIC_HOLD_DECODE() && SDL_GetAtomicInt(&music->cancel) == 0) {
        SDL_Delay(1);
    }
    return SDL_GetAtomicInt(&music->cancel) != 0;
}

/* The loader: reads the whole file and decodes it. */
static int SDLCALL player_ui_music_load(void *data) {
    PlayerUiMusic *music = (PlayerUiMusic *)data;
    size_t size = 0u;
    void *bytes = NULL;
    SDL_PathInfo info;
    music->load_failure = PLAYER_UI_MUSIC_OFF;
    music->load_detail[0] = '\0';
    if (!SDL_GetPathInfo(music->path, &info) || info.type == SDL_PATHTYPE_NONE) {
        music->load_failure = PLAYER_UI_MUSIC_MISSING;
        (void)snprintf(music->load_detail, sizeof(music->load_detail), "the file is not there");
    } else if (info.type != SDL_PATHTYPE_FILE) {
        music->load_failure = PLAYER_UI_MUSIC_DAMAGED;
        (void)snprintf(music->load_detail, sizeof(music->load_detail), "it is not a file");
    } else if (info.size > (Uint64)PLAYER_UI_MUSIC_MAX_FILE_BYTES) {
        music->load_failure = PLAYER_UI_MUSIC_DAMAGED;
        (void)snprintf(music->load_detail, sizeof(music->load_detail),
                       "the file is larger than %u MB", PLAYER_UI_MUSIC_MAX_FILE_BYTES >> 20);
    } else if ((bytes = SDL_LoadFile(music->path, &size)) == NULL) {
        music->load_failure = PLAYER_UI_MUSIC_DAMAGED;
        (void)snprintf(music->load_detail, sizeof(music->load_detail),
                       "the file could not be read (%s)", SDL_GetError());
    } else {
        PlayerUiVorbisResult result = player_ui_vorbis_decode(
            (const unsigned char *)bytes, size, player_ui_music_cancelled, music,
            &music->loaded, music->load_detail, sizeof(music->load_detail));
        SDL_free(bytes);
        if (result != PLAYER_UI_VORBIS_OK) {
            /* A cancelled load is discarded by the thread that cancelled it. */
            music->load_failure = PLAYER_UI_MUSIC_DAMAGED;
        }
    }
    SDL_SetAtomicInt(&music->loader_done, 1);
    return 0;
}

static void player_ui_music_close_device(PlayerUiMusic *music) {
    if (music->stream == NULL) return;
    /* Destroying a device stream stops its callback and closes the device. */
    SDL_DestroyAudioStream(music->stream);
    music->stream = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    music->fading_out = false;
    music->mix.gain = 0.0f;
    music->mix.target = 0.0f;
    music->mix.step = 0.0f;
    music->mix.ramp_left = 0u;
}

/* Cancels a running load and waits for its worker, discarding what it read. */
static void player_ui_music_cancel_load(PlayerUiMusic *music) {
    if (music->loader != NULL) {
        SDL_SetAtomicInt(&music->cancel, 1);
        SDL_WaitThread(music->loader, NULL);
        music->loader = NULL;
        player_ui_pcm_free(&music->loaded);
    }
    SDL_SetAtomicInt(&music->cancel, 0);
    SDL_SetAtomicInt(&music->loader_done, 0);
}

/* Frees the decoded track, keeping where the loop stood. */
static void player_ui_music_free_track(PlayerUiMusic *music) {
    if (!music->have_pcm) return;
    music->resume_cursor = music->mix.cursor;
    music->have_resume = true;
    player_ui_pcm_free(&music->pcm);
    memset(&music->mix, 0, sizeof(music->mix));
    music->have_pcm = false;
}

/* Back to the state of a fresh object: no thread, no device, no track. */
static void player_ui_music_reset(PlayerUiMusic *music) {
    player_ui_music_cancel_load(music);
    player_ui_music_close_device(music);
    player_ui_pcm_free(&music->pcm);
    memset(&music->mix, 0, sizeof(music->mix));
    music->have_pcm = false;
    music->have_resume = false;
    music->resume_cursor = 0u;
    music->failure = PLAYER_UI_MUSIC_OFF;
    music->detail[0] = '\0';
    music->device_failed = false;
    music->was_audible = false;
    music->volume_gain = 0.0f;
}

void player_ui_music_destroy(PlayerUiMusic *music) {
    if (music == NULL) return;
    player_ui_music_reset(music);
    free(music->path);
    free(music->label);
    free(music);
}

static void player_ui_music_set_detail(PlayerUiMusic *music, const char *reason) {
    (void)snprintf(music->detail, sizeof(music->detail), "%s: %s", music->label, reason);
}

/* Takes the finished load's result on this thread. */
static void player_ui_music_take_load(PlayerUiMusic *music) {
    SDL_SetAtomicInt(&music->loader_done, 0);
    if (music->load_failure != PLAYER_UI_MUSIC_OFF) {
        music->failure = music->load_failure;
        player_ui_music_set_detail(music, music->load_detail);
        player_ui_pcm_free(&music->loaded);
        return;
    }
    music->pcm = music->loaded;
    memset(&music->loaded, 0, sizeof(music->loaded));
    player_ui_music_mix_init(&music->mix, &music->pcm);
    if (!player_ui_music_mix_has_sound(&music->mix)) {
        music->failure = PLAYER_UI_MUSIC_DAMAGED;
        player_ui_music_set_detail(music, "the track decodes to silence");
        player_ui_pcm_free(&music->pcm);
        memset(&music->mix, 0, sizeof(music->mix));
        return;
    }
    if (music->have_resume && music->resume_cursor >= music->mix.loop_start &&
        music->resume_cursor < music->mix.loop_end) {
        music->mix.cursor = music->resume_cursor;
    }
    music->have_resume = false;
    music->have_pcm = true;
}

static void player_ui_music_start_load(PlayerUiMusic *music) {
    SDL_SetAtomicInt(&music->cancel, 0);
    SDL_SetAtomicInt(&music->loader_done, 0);
    music->loader = PLAYER_UI_MUSIC_NO_THREAD()
                        ? NULL
                        : SDL_CreateThread(player_ui_music_load, "nakagawa-music", music);
    if (music->loader == NULL) {
        /* No worker: decode here, as a launcher without threads would. */
        (void)player_ui_music_load(music);
        player_ui_music_take_load(music);
    }
}

static uint64_t player_ui_music_frames(const PlayerUiMusic *music, uint32_t ms) {
    return (uint64_t)music->pcm.sample_rate * (uint64_t)ms / 1000u;
}

/* Renders the next stretch of the loop whenever the device asks for more. */
static void SDLCALL player_ui_music_feed(void *user, SDL_AudioStream *stream,
                                         int additional_amount, int total_amount) {
    PlayerUiMusic *music = (PlayerUiMusic *)user;
    int16_t chunk[PLAYER_UI_MUSIC_FEED_FRAMES * 2];
    int frame_bytes = music->mix.channels * (int)sizeof(int16_t);
    (void)total_amount;
    if (PLAYER_UI_MUSIC_STALL_RELEASE() && music->mix.target == 0.0f) return;
    while (additional_amount > 0) {
        int frames = (additional_amount + frame_bytes - 1) / frame_bytes;
        if (frames > PLAYER_UI_MUSIC_FEED_FRAMES) frames = PLAYER_UI_MUSIC_FEED_FRAMES;
        player_ui_music_mix_render(&music->mix, chunk, (uint32_t)frames);
        if (!SDL_PutAudioStreamData(stream, chunk, frames * frame_bytes)) break;
        additional_amount -= frames * frame_bytes;
    }
}

static bool player_ui_music_open_device(PlayerUiMusic *music, float gain) {
    SDL_AudioSpec spec;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        char reason[256];
        (void)snprintf(reason, sizeof(reason), "no audio (%s)", SDL_GetError());
        player_ui_music_set_detail(music, reason);
        return false;
    }
    spec.format = SDL_AUDIO_S16;
    spec.channels = music->pcm.channels;
    spec.freq = music->pcm.sample_rate;
    music->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                              player_ui_music_feed, music);
    if (music->stream == NULL) {
        char reason[256];
        (void)snprintf(reason, sizeof(reason), "no audio device (%s)", SDL_GetError());
        player_ui_music_set_detail(music, reason);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    /* The device starts paused, so the callback cannot run yet. */
    music->mix.gain = 0.0f;
    player_ui_music_mix_ramp(&music->mix, gain,
                             player_ui_music_frames(music, PLAYER_UI_MUSIC_FADE_IN_MS));
    if (!SDL_ResumeAudioStreamDevice(music->stream)) {
        char reason[256];
        (void)snprintf(reason, sizeof(reason), "the audio device did not start (%s)",
                       SDL_GetError());
        player_ui_music_set_detail(music, reason);
        player_ui_music_close_device(music);
        return false;
    }
    return true;
}

static void player_ui_music_locked_ramp(PlayerUiMusic *music, float target, uint32_t ms) {
    if (SDL_LockAudioStream(music->stream)) {
        player_ui_music_mix_ramp(&music->mix, target, player_ui_music_frames(music, ms));
        (void)SDL_UnlockAudioStream(music->stream);
    }
}

/* A new level: over the ramp's own short time, or over what is left of a fade
   that is still running, so a step during the fade-in does not cut it short. */
static void player_ui_music_locked_retarget(PlayerUiMusic *music, float target) {
    if (SDL_LockAudioStream(music->stream)) {
        uint64_t frames = player_ui_music_frames(music, PLAYER_UI_MUSIC_VOLUME_RAMP_MS);
        if (music->mix.ramp_left > frames) frames = music->mix.ramp_left;
        player_ui_music_mix_ramp(&music->mix, target, frames);
        (void)SDL_UnlockAudioStream(music->stream);
    }
}

/* Fades the music to silence over `fade_ms` (a fade already running keeps its
   own length) and closes the device once the music has rendered silence, or
   once the fade's time and PLAYER_UI_MUSIC_RELEASE_GRACE_MS have passed. A close
   at that limit is kept for player_ui_music_take_release_timeout. Returns
   whether the device is closed. */
static bool player_ui_music_release(PlayerUiMusic *music, uint32_t fade_ms, uint64_t now_ms) {
    bool silent = false;
    uint64_t elapsed;
    if (music->stream == NULL) return true;
    if (!music->fading_out) {
        player_ui_music_locked_ramp(music, 0.0f, fade_ms);
        music->fading_out = true;
        music->fade_started_ms = now_ms;
        music->fade_ms = fade_ms;
    }
    if (SDL_LockAudioStream(music->stream)) {
        silent = player_ui_music_mix_silent(&music->mix);
        (void)SDL_UnlockAudioStream(music->stream);
    }
    elapsed = now_ms - music->fade_started_ms;
    if (!silent) {
        if (elapsed < (uint64_t)music->fade_ms + PLAYER_UI_MUSIC_RELEASE_GRACE_MS) return false;
        /* The device stopped taking data: the music closes it unfaded. */
        music->release_timeout_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    }
    player_ui_music_close_device(music);
    return true;
}

void player_ui_music_update(PlayerUiMusic *music, const PlayerUiMusicRequest *request,
                            uint64_t now_ms) {
    bool audible;
    float gain;
    if (music == NULL || request == NULL) return;
    if (!request->enabled) {
        /* Turned off: a short fade first, so the stop makes no click. */
        if (!player_ui_music_release(music, PLAYER_UI_MUSIC_VOLUME_RAMP_MS, now_ms)) {
            music->state = PLAYER_UI_MUSIC_FADING_OUT;
            return;
        }
        player_ui_music_reset(music);
        music->state = PLAYER_UI_MUSIC_OFF;
        return;
    }
    if (music->loader != NULL && !request->in_launcher) {
        /* A game is starting: the decode stops, and its worker is joined, before
           the launch may start the game's process (player_ui_music_decoding).
           The track loads again when the launcher is back. */
        player_ui_music_cancel_load(music);
        music->state = PLAYER_UI_MUSIC_STOPPED;
        return;
    }
    if (music->loader != NULL) {
        if (SDL_GetAtomicInt(&music->loader_done) == 0) {
            music->state = PLAYER_UI_MUSIC_LOADING;
            return;
        }
        SDL_WaitThread(music->loader, NULL);
        music->loader = NULL;
        player_ui_music_take_load(music);
    }
    if (music->failure != PLAYER_UI_MUSIC_OFF) {
        music->state = music->failure;
        return;
    }
    if (!music->have_pcm) {
        /* Nothing is read or decoded while a game runs: the track loads when the
           launcher is back. */
        if (!request->in_launcher) {
            music->state = PLAYER_UI_MUSIC_STOPPED;
            return;
        }
        player_ui_music_start_load(music);
        if (music->failure != PLAYER_UI_MUSIC_OFF) {
            music->state = music->failure;
            return;
        }
        if (music->loader != NULL) {
            music->state = PLAYER_UI_MUSIC_LOADING;
            return;
        }
        /* Decoded here, with no worker: the track plays from this frame on. */
    }
    gain = (float)(request->volume < 0 ? 0 : request->volume > 100 ? 100 : request->volume) /
           100.0f;
    /* At Music volume 0 the music holds no device, as when the window is away. */
    audible = request->in_launcher && request->focused && gain > 0.0f;
    if (audible) {
        if (music->stream == NULL) {
            if (music->device_failed && music->was_audible) {
                music->state = PLAYER_UI_MUSIC_NO_DEVICE;
                return;
            }
            music->was_audible = true;
            if (!player_ui_music_open_device(music, gain)) {
                music->device_failed = true;
                music->state = PLAYER_UI_MUSIC_NO_DEVICE;
                return;
            }
            music->device_failed = false;
            music->detail[0] = '\0';
        } else if (music->fading_out) {
            player_ui_music_locked_ramp(music, gain, PLAYER_UI_MUSIC_FADE_IN_MS);
        } else if (gain != music->volume_gain) {
            player_ui_music_locked_retarget(music, gain);
        }
        music->fading_out = false;
        music->volume_gain = gain;
        music->state = PLAYER_UI_MUSIC_PLAYING;
        return;
    }
    music->was_audible = false;
    if (!player_ui_music_release(music, PLAYER_UI_MUSIC_FADE_OUT_MS, now_ms)) {
        music->state = PLAYER_UI_MUSIC_FADING_OUT;
        return;
    }
    if (!request->in_launcher) {
        /* A game runs: the decoded track is not kept for it. */
        player_ui_music_free_track(music);
        music->state = PLAYER_UI_MUSIC_STOPPED;
        return;
    }
    music->state = PLAYER_UI_MUSIC_PAUSED;
}

PlayerUiMusicState player_ui_music_state(const PlayerUiMusic *music) {
    return music != NULL ? music->state : PLAYER_UI_MUSIC_OFF;
}

const char *player_ui_music_detail(const PlayerUiMusic *music) {
    if (music == NULL) return "";
    switch (music->state) {
    case PLAYER_UI_MUSIC_MISSING:
    case PLAYER_UI_MUSIC_DAMAGED:
    case PLAYER_UI_MUSIC_NO_DEVICE:
        return music->detail;
    default:
        return "";
    }
}

const char *player_ui_music_row_value(bool on, PlayerUiMusicState state) {
    if (!on) return "Off";
    switch (state) {
    case PLAYER_UI_MUSIC_MISSING: return "On: file missing";
    case PLAYER_UI_MUSIC_DAMAGED: return "On: file damaged";
    case PLAYER_UI_MUSIC_NO_DEVICE: return "On: no sound device";
    default: return "On";
    }
}

bool player_ui_music_holds_audio(const PlayerUiMusic *music) {
    return music != NULL && music->stream != NULL;
}

bool player_ui_music_decoding(const PlayerUiMusic *music) {
    return music != NULL && music->loader != NULL;
}

uint32_t player_ui_music_take_release_timeout(PlayerUiMusic *music) {
    uint32_t elapsed;
    if (music == NULL) return 0u;
    elapsed = music->release_timeout_ms;
    music->release_timeout_ms = 0u;
    return elapsed;
}

#if defined(NK_PLAYER_UI_REGRESSION_TEST) || defined(PLAYER_UI_MUSIC_TEST_SEAMS)
uint64_t player_ui_music_test_pcm_frames(const PlayerUiMusic *music) {
    return music != NULL && music->have_pcm ? music->pcm.frames : 0u;
}

uint64_t player_ui_music_test_cursor(const PlayerUiMusic *music) {
    uint64_t cursor = 0u;
    if (music == NULL) return 0u;
    if (music->stream != NULL && SDL_LockAudioStream(music->stream)) {
        cursor = music->mix.cursor;
        (void)SDL_UnlockAudioStream(music->stream);
        return cursor;
    }
    return music->have_pcm ? music->mix.cursor : music->resume_cursor;
}

uint64_t player_ui_music_test_ramp_frames(const PlayerUiMusic *music) {
    uint64_t frames = 0u;
    if (music == NULL || music->stream == NULL) return 0u;
    if (SDL_LockAudioStream(music->stream)) {
        frames = music->mix.ramp_left;
        (void)SDL_UnlockAudioStream(music->stream);
    }
    return frames;
}
#endif

float player_ui_music_gain(const PlayerUiMusic *music) {
    float gain = 0.0f;
    if (music == NULL || music->stream == NULL) return 0.0f;
    if (SDL_LockAudioStream(music->stream)) {
        gain = music->mix.gain;
        (void)SDL_UnlockAudioStream(music->stream);
    }
    return gain;
}
