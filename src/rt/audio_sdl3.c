// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Public-safe SDL3 host audio output backend.
 *
 * Provides a redistributable, public-safe host audio backend using SDL3
 * (already a project dependency). Consumes the runtime PCM/mix contract
 * from sceAudio / SAS without altering guest-visible timing.
 * If no host audio device is available or headless mode is selected,
 * fails closed gracefully with a clear diagnostic and open-loop guest pacing.
 */

#define SDL_MAIN_HANDLED 1

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>

#include "perf.h"

#define SR_AUDIO_CHANNELS 9
#define SR_AUDIO_OUTPUT2_CH 8
#define SR_AUDIO_SAMPLE_RATE 44100
#define SR_AUDIO_MAX_QUEUED_FRAMES (65536 * 4)

typedef enum {
    AUDIO_STATE_UNINITIALIZED = 0,
    AUDIO_STATE_ACTIVE,
    AUDIO_STATE_UNAVAILABLE,
    AUDIO_STATE_NULL
} AudioBackendState;

typedef struct {
    uint64_t total_pushed_frames;
    uint64_t channel_pushed_frames[SR_AUDIO_CHANNELS];
    uint64_t underrun_events;
    uint64_t overrun_events;
    uint64_t overrun_frames;   /* frames those events dropped (#2163) */
    uint64_t backpressure_events;
    uint64_t pause_enters;
    uint64_t pause_held_frames;
    uint64_t pause_stash_failures;
    uint32_t peak_queued_frames[SR_AUDIO_CHANNELS];
} AudioStats;

typedef struct {
    int16_t *samples;
    int frames;
} AudioPauseStash;

typedef struct {
    int16_t *samples;
    uint32_t capacity_frames;
    uint32_t head_frame;
    uint32_t frames;
    int valid;
} AudioShadowQueue;

#define SR_AUDIO_PAUSE_FADE_FRAMES 256

static AudioBackendState s_audio_state = AUDIO_STATE_UNINITIALIZED;
static SDL_AudioDeviceID s_device_id = 0;
static SDL_AudioStream *s_streams[SR_AUDIO_CHANNELS] = {NULL};
static AudioStats s_stats;
static AudioPauseStash s_pause_stash[SR_AUDIO_CHANNELS];
static AudioShadowQueue s_shadow_queues[SR_AUDIO_CHANNELS];
static int16_t s_pause_fade[SR_AUDIO_CHANNELS][SR_AUDIO_PAUSE_FADE_FRAMES * 2];
static int s_pause_active;
static int s_pause_device_fallback;
static int s_pause_device_was_paused;
static int s_pause_failure_reported;
static int s_master_volume_percent = 100;

/* SR_PERF audio-queue telemetry (#1296). Each accepted push records when it landed and the
 * queue depth it left, which dates the moment that audio runs out. A later push that finds
 * the queue empty measures the silence since then. A silence of a second or more is a stream
 * that stopped and started again (a sound effect channel between effects), not an underrun. */
typedef struct {
    uint64_t push_ns;
    uint32_t queued_frames;
} AudioPushMark;

#define SR_AUDIO_RESTART_GAP_NS 1000000000ull

static AudioPushMark s_push_marks[SR_AUDIO_CHANNELS];

static uint64_t audio_starved_ns(const AudioPushMark *mark, uint64_t now_ns,
                                 int queued_frames, int *underrun) {
    *underrun = 0;
    if (queued_frames > 0 || mark->push_ns == 0) return 0;
    uint64_t dry_ns = mark->push_ns +
        (uint64_t)mark->queued_frames * 1000000000ull / SR_AUDIO_SAMPLE_RATE;
    uint64_t starved = now_ns > dry_ns ? now_ns - dry_ns : 0;
    *underrun = starved < SR_AUDIO_RESTART_GAP_NS;
    return starved;
}
static int s_reported_unavailable = 0;
static int s_reported_init = 0;
static int s_cleanup_registered = 0;

/* ---- what the device takes from a channel per pull ---------------------------------------------
 *
 * The blocking output sizes its queue against the block the device takes at once (hle_audio.c).
 * SDL names a chunk size for the open device, but that is what SDL feeds the hardware, not what a
 * stream was seen to lose: a stream that converts the sample rate gives up a different count and
 * keeps a few frames back, and a back end can take several chunks in a row. So the streams report
 * every pull as it is made (SDL's get-callback, on the device thread, in the stream's own
 * 44.1 kHz frames):
 *   - the size of a pull is the most any channel reported for one: a converting stream that is
 *     idle asks for the same count every time and one that plays asks for a frame more on some
 *     pulls;
 *   - a stream that can give a pull nothing may still have frames queued, which its converter
 *     keeps back (7 on the reference PC from 44.1 to 48 kHz); the largest count seen is added;
 *   - pulls that come in less than half the time the one before them takes to play are a burst:
 *     the device was late and took its pulls together. The queue's display-period allowance
 *     covers that as it covers a late guest (441 + 736 frames queued leave 295 after two 441-frame
 *     pulls), so a burst does not change the size by itself. It does once it has cost audio: when
 *     a pull finds a playing channel short within one pull's time of a burst, the size becomes
 *     that burst's sum. On a busy host WASAPI took two pulls together in every one-minute run,
 *     and sizing for every such burst kept 10 ms more queued for the whole session.
 * Each figure is kept until SDL's chunk size or rate for the device changes, so the depth the
 * guest sees does not move with the host's moods. */
typedef struct {
    uint64_t last_ns;        /* when the last pull came */
    uint32_t last_frames;    /* how much it took */
    uint32_t burst_frames;   /* the burst it belongs to, summed */
    uint64_t burst_ns;       /* when a burst of two pulls or more last grew */
    uint32_t burst_last;     /* and its sum then */
    uint64_t short_ns;       /* when a pull last found a playing channel short; 0: never */
    uint32_t second_frames;  /* frames pulled since SDL's chunk size was last asked for */
    uint32_t widest;         /* the most any channel's stream gave up in one pull */
    uint32_t grown;          /* a burst that cost audio: the size since then */
    uint32_t kept_back;      /* queued frames no pull can take: largest seen on this device */
} AudioPullMeter;

static AudioPullMeter s_pull_meter;           /* written on the device thread only */
static SDL_AtomicInt s_pull_measured_frames;  /* its result, read on the scheduler thread */
static SDL_AtomicInt s_pull_chunk_stale;      /* time to ask SDL for its chunk size again */
static SDL_AtomicInt s_pull_meter_restart;    /* the device changed: the meter starts over */
static int s_pull_chunk_frames;               /* SDL's chunk size in mixer frames (scheduler thread) */
static int s_pull_chunk_device_frames, s_pull_chunk_device_freq;   /* and as SDL gave it */
static int s_pull_unmeasured;                 /* no stream reports its pulls: SDL's figure stands in */

static uint32_t audio_pull_add(uint32_t a, uint32_t b) {
    return a > (uint32_t)INT32_MAX - b ? (uint32_t)INT32_MAX : a + b;
}

/* The pull size to queue against, as the meter stands. */
static uint32_t audio_pull_size(const AudioPullMeter *m) {
    return audio_pull_add(m->widest > m->grown ? m->widest : m->grown, m->kept_back);
}

/* A burst and a short pull on a playing channel within one pull's time of each other, in either
 * order (the channels report in the device's order, not ours): the burst cost audio. A short
 * pull is any playing channel that drains, so a channel that ends on purpose (a track that
 * finishes, a guest that stops it, the fade at pause) within that time of a burst also grows
 * the size, for the rest of the session. That costs latency only, never audio; telling the two
 * apart needs the next push (a dropout is a push that finds the channel dry soon after), which
 * runs on the guest's thread, not this one. */
static void audio_pull_burst_cost(AudioPullMeter *m) {
    if (!m->burst_last || !m->short_ns) return;
    const uint64_t apart = m->burst_ns > m->short_ns ? m->burst_ns - m->short_ns : m->short_ns - m->burst_ns;
    const uint64_t pull_ns = (uint64_t)m->widest * 1000000000ull / SR_AUDIO_SAMPLE_RATE;
    if (apart <= pull_ns && m->burst_last > m->grown) m->grown = m->burst_last;
}

/* One pull of `frames`, made at `now_ns`, as channel 0 reports it. *second_ended is set when a
 * second of audio has been pulled since it was last set. */
static void audio_pull_note(AudioPullMeter *m, uint32_t frames, uint64_t now_ns, int *second_ended) {
    *second_ended = 0;
    if (frames > (uint32_t)INT32_MAX) frames = (uint32_t)INT32_MAX;
    const uint64_t last_play_ns = (uint64_t)m->last_frames * 1000000000ull / SR_AUDIO_SAMPLE_RATE;
    const int same_burst = m->last_frames && now_ns >= m->last_ns && now_ns - m->last_ns < last_play_ns / 2u;
    m->burst_frames = same_burst ? audio_pull_add(m->burst_frames, frames) : frames;
    m->last_ns = now_ns;
    m->last_frames = frames;
    if (same_burst) {
        m->burst_ns = now_ns;
        m->burst_last = m->burst_frames;
        audio_pull_burst_cost(m);
    }
    m->second_frames = audio_pull_add(m->second_frames, frames);
    if (m->second_frames >= SR_AUDIO_SAMPLE_RATE) {
        m->second_frames = 0;
        *second_ended = 1;
    }
}

/* A channel the device has played dry: a pull came while its stream could give nothing, or
 * nothing was ever pushed. Set on the device thread, cleared by the next push. Each channel has
 * its own, read from its own stream. */
static SDL_AtomicInt s_played_dry[SR_AUDIO_CHANNELS];

/* What a stream reports to its get-callback, as the dry flag and the meter need it. `available`
 * is SDL_GetAudioStreamAvailable before the pull: 0 means the stream gives this pull nothing,
 * whatever it still has queued (measured on the dummy driver and WASAPI: at 44.1 kHz it is 0 only
 * with an empty queue, from 44.1 to 48 kHz with 7 frames or fewer). SDL's own shortfall figure,
 * the callback's additional_amount, is not used: SDL documents that it may overestimate, and a
 * pull the stream did fill must not count as dry. */
static void audio_pull_report(int ch, int available, int queued_bytes, int total_bytes, uint64_t now_ns) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || total_bytes <= 0) return;
    AudioPullMeter *m = &s_pull_meter;
    if (SDL_CompareAndSwapAtomicInt(&s_pull_meter_restart, 1, 0)) memset(m, 0, sizeof(*m));
    /* Frames kept back are the converter's, the same for every stream: any channel shows them. */
    const uint32_t kept_back = available == 0 && queued_bytes > 0 ? (uint32_t)queued_bytes / 4u : 0u;
    if (kept_back > m->kept_back) m->kept_back = kept_back;
    if ((uint32_t)total_bytes / 4u > m->widest) m->widest = (uint32_t)total_bytes / 4u;
    /* A channel that was playing (pushed since it was last dry) and cannot fill this pull: with
     * nothing to give, or with less queued than the pull and the converter take. */
    const int64_t wanted = (int64_t)total_bytes + (int64_t)m->kept_back * 4;
    if (!SDL_GetAtomicInt(&s_played_dry[ch]) && (available == 0 || queued_bytes < wanted)) {
        m->short_ns = now_ns ? now_ns : 1u;
        audio_pull_burst_cost(m);
    }
    if (available == 0) SDL_SetAtomicInt(&s_played_dry[ch], 1);
    if (ch != 0) return;   /* one stream marks the pulls: the device comes to each stream once per pull */
    int second_ended;
    audio_pull_note(m, m->widest, now_ns, &second_ended);
    SDL_SetAtomicInt(&s_pull_measured_frames, (int)audio_pull_size(m));
    if (second_ended) SDL_SetAtomicInt(&s_pull_chunk_stale, 1);
}

/* SDL's get-callback for a channel's stream (userdata: the channel), on the device thread with the
 * stream locked: `total_amount` bytes of the stream's input format are about to be taken. */
static void SDLCALL sr_audio_pull_callback(void *userdata, SDL_AudioStream *stream, int additional_amount,
                                           int total_amount) {
    (void)additional_amount;
    audio_pull_report((int)(intptr_t)userdata, SDL_GetAudioStreamAvailable(stream),
                      SDL_GetAudioStreamQueued(stream), total_amount, SDL_GetTicksNS());
}

/* Silence queued ahead of a guest push since that channel's last push: the blocking output's hold
 * for a channel the device played dry (hle_audio.c). The push that follows reports the depth it
 * found without it, so a queue that ran dry is still counted as one, and adds the silence to the
 * time it reports the channel silent. */
static uint32_t s_primed_frames[SR_AUDIO_CHANNELS];

static void sr_audio_pull_reset(void) {
    memset(&s_pull_meter, 0, sizeof(s_pull_meter));
    SDL_SetAtomicInt(&s_pull_measured_frames, 0);
    SDL_SetAtomicInt(&s_pull_meter_restart, 0);
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) SDL_SetAtomicInt(&s_played_dry[i], 1);
    SDL_SetAtomicInt(&s_pull_chunk_stale, 1);
    s_pull_chunk_frames = 0;
    s_pull_chunk_device_frames = s_pull_chunk_device_freq = 0;
    s_pull_unmeasured = 0;
}


#ifdef SR_AUDIO_SELFTEST
static int s_test_fail_pause_stash_alloc;
static int s_test_device_freq;   /* open the device at this rate, so the streams convert */
static int s_test_pause_failure_messages;
static int s_test_device_open_attempts;   /* times sr_audio_init went on to open the device */
#endif

/* SR_AUDIOSTAT gate, matching the other backend: the queue/drift fields are diagnostics and
 * must not change the default output of a public build. */
static int audio_stat_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_AUDIOSTAT") != NULL;
    return on;
}

/* ---- SR_AUDIODUMP: debug-only capture of the device mix (off by default) ----------------
 *
 * With SR_AUDIODUMP=<path.wav> in the environment, every buffer SDL is about to submit to the
 * device -- the sum of all nine channel streams after the master gain -- is also written as
 * 16-bit PCM WAV at the device's own rate and channel count. That is the sound the host plays,
 * so it is the artefact an audit needs for silence, clipping, discontinuities and sample rate.
 * It is observation only: the callback never writes back into the buffer. It runs on SDL's
 * audio thread and does file I/O there, so a dump changes host pacing; never compare timing
 * with it enabled. The WAV header is patched once a second and at close, so an abnormal exit
 * still leaves a readable file up to its last patch. */
typedef struct {
    uint64_t frames;         /* sample frames written */
    uint64_t silent_frames;  /* frames where every channel was exactly zero */
    uint64_t clipped;        /* samples beyond full scale, before the 16-bit clamp */
    uint64_t dropped_frames; /* frames offered and not written: size limit, format change, failed write */
    uint32_t peak;           /* largest magnitude written, after the clamp */
} SrAudioDumpStats;

#define SR_AUDIO_DUMP_CHUNK 1024u
#define SR_AUDIO_DUMP_MAX_DATA 0xFFFFFF00u   /* stop before the RIFF size field overflows */

static FILE *s_dump_file = NULL;
static char s_dump_path[512];
static uint32_t s_dump_rate = 0, s_dump_channels = 0;
static uint64_t s_dump_data_bytes = 0, s_dump_last_patch = 0;
static SrAudioDumpStats s_dump_stats;
static int s_dump_write_failed = 0;    /* the first short write ends the dump */

static void sr_put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void sr_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Canonical 44-byte PCM WAV header for 16-bit samples. Pure, so the selftest checks it. */
static void sr_audio_wav_header(uint8_t out[44], uint32_t rate, uint32_t channels, uint32_t data_bytes) {
    memcpy(out + 0, "RIFF", 4);
    sr_put_le32(out + 4, 36u + data_bytes);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    sr_put_le32(out + 16, 16u);
    sr_put_le16(out + 20, 1u);                              /* PCM */
    sr_put_le16(out + 22, (uint16_t)channels);
    sr_put_le32(out + 24, rate);
    sr_put_le32(out + 28, rate * channels * 2u);            /* byte rate */
    sr_put_le16(out + 32, (uint16_t)(channels * 2u));       /* block align */
    sr_put_le16(out + 34, 16u);
    memcpy(out + 36, "data", 4);
    sr_put_le32(out + 40, data_bytes);
}

/* Float device mix (nominal full scale +-1.0) to 16-bit, rounding half away from zero. A sample
 * beyond the 16-bit range is counted as clipped and clamped; NaN reads as silence. Pure. */
static int16_t sr_audio_dump_sample(float x, uint64_t *clipped) {
    float scaled = x * 32768.0f;
    if (isnan(scaled)) return 0;
    if (scaled > 32767.0f) { (*clipped)++; return 32767; }
    if (scaled < -32768.0f) { (*clipped)++; return -32768; }
    return (int16_t)(int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

/* The first short write ends the dump: the header keeps its last good patch, later blocks are
 * dropped, and the close line says so. One message, not one per buffer. */
static void sr_audio_dump_write_failed(const char *what) {
    if (s_dump_write_failed) return;
    s_dump_write_failed = 1;
    fprintf(stderr, "AUDIODUMP: %s write to %s failed (%s); the dump stops here\n",
            what, s_dump_path, strerror(errno));
}

/* A block refused whole, past the size limit or in a format the header does not describe, is
 * counted in dropped_frames. The first refusal prints one line; the close line has the total. */
static void sr_audio_dump_refuse(uint64_t frames, const char *why) {
    if (!s_dump_stats.dropped_frames && frames) {
        fprintf(stderr, "AUDIODUMP: %s %s; device blocks from here on are dropped and counted\n",
                s_dump_path, why);
    }
    s_dump_stats.dropped_frames += frames;
}

static void sr_audio_dump_patch_header(void) {
    uint8_t hdr[44];
    sr_audio_wav_header(hdr, s_dump_rate, s_dump_channels, (uint32_t)s_dump_data_bytes);
    if (fseek(s_dump_file, 0, SEEK_SET) != 0 || fwrite(hdr, 1, sizeof(hdr), s_dump_file) != sizeof(hdr)) {
        sr_audio_dump_write_failed("header");
    }
    if (fseek(s_dump_file, 0, SEEK_END) != 0) sr_audio_dump_write_failed("seek");
    fflush(s_dump_file);
    s_dump_last_patch = s_dump_data_bytes;
}

/* Convert and write `samples` interleaved float values (a whole number of frames). Returns the
 * number of samples written; fewer than `samples` only after a failed write. */
static size_t sr_audio_dump_block(const float *in, size_t samples) {
    int16_t out[SR_AUDIO_DUMP_CHUNK];
    size_t chunk = SR_AUDIO_DUMP_CHUNK - (SR_AUDIO_DUMP_CHUNK % s_dump_channels);
    size_t done = 0;
    for (; done < samples;) {
        size_t k = samples - done < chunk ? samples - done : chunk;
        for (size_t i = 0; i < k; i++) {
            int16_t v = sr_audio_dump_sample(in[done + i], &s_dump_stats.clipped);
            out[i] = v;
            uint32_t a = v < 0 ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
            if (a > s_dump_stats.peak) s_dump_stats.peak = a;
        }
        if (fwrite(out, sizeof(int16_t), k, s_dump_file) != k) {
            sr_audio_dump_write_failed("data");
            return done;
        }
        for (size_t f = 0; f < k; f += s_dump_channels) {
            int any = 0;
            for (uint32_t c = 0; c < s_dump_channels; c++) any |= out[f + c] != 0;
            if (!any) s_dump_stats.silent_frames++;
        }
        s_dump_stats.frames += k / s_dump_channels;
        done += k;
    }
    return done;
}

/* Runs on SDL's audio thread. Thread contract: s_dump_file, s_dump_rate, s_dump_channels and
 * s_dump_path are written on the main thread before SDL_SetAudioPostmixCallback registers this
 * callback and after it is removed; SDL's registration and removal are the synchronization
 * points, so this callback never runs concurrently with those writes. s_dump_data_bytes,
 * s_dump_last_patch, s_dump_stats and s_dump_write_failed are written only here while the
 * callback is registered, and the close path reads them only after removing it. */
static void sr_audio_dump_postmix(void *userdata, const SDL_AudioSpec *spec, float *buffer, int buflen) {
    (void)userdata;
    if (!s_dump_file || !buffer || !spec || buflen < (int)sizeof(float)) return;
    size_t samples = (size_t)buflen / sizeof(float);
    /* A device whose format changed mid-run keeps the header it was opened with. */
    if ((uint32_t)spec->channels != s_dump_channels || (uint32_t)spec->freq != s_dump_rate) {
        if (spec->channels > 0) sr_audio_dump_refuse(samples / (size_t)spec->channels, "changed format");
        return;
    }
    samples -= samples % s_dump_channels;
    if (!samples) return;
    if (s_dump_write_failed) {
        s_dump_stats.dropped_frames += samples / s_dump_channels;
        return;
    }
    if (s_dump_data_bytes + samples * 2u > SR_AUDIO_DUMP_MAX_DATA) {
        sr_audio_dump_refuse(samples / s_dump_channels, "reached its size limit");
        return;
    }
    /* A failed write stops inside the block: the frames it did not reach are dropped with the rest. */
    size_t written = sr_audio_dump_block(buffer, samples);
    s_dump_data_bytes += written * 2u;
    s_dump_stats.dropped_frames += (samples - written) / s_dump_channels;
    if (s_dump_write_failed) return;
    if (s_dump_data_bytes - s_dump_last_patch >= (uint64_t)s_dump_rate * s_dump_channels * 2u)
        sr_audio_dump_patch_header();
}

static void sr_audio_dump_start(SDL_AudioDeviceID dev) {
    const char *path = getenv("SR_AUDIODUMP");
    if (!path || !*path || s_dump_file || !dev) return;
    SDL_AudioSpec spec;
    if (!SDL_GetAudioDeviceFormat(dev, &spec, NULL) || spec.channels <= 0 || spec.freq <= 0) {
        fprintf(stderr, "AUDIODUMP: device format unavailable (%s); no dump written\n", SDL_GetError());
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "AUDIODUMP: cannot open %s; no dump written\n", path);
        return;
    }
    snprintf(s_dump_path, sizeof(s_dump_path), "%s", path);
    s_dump_file = f;
    s_dump_rate = (uint32_t)spec.freq;
    s_dump_channels = (uint32_t)spec.channels;
    s_dump_data_bytes = s_dump_last_patch = 0;
    s_dump_write_failed = 0;
    memset(&s_dump_stats, 0, sizeof(s_dump_stats));
    sr_audio_dump_patch_header();
    if (s_dump_write_failed) {
        fclose(s_dump_file);
        s_dump_file = NULL;
        return;
    }
    if (!SDL_SetAudioPostmixCallback(dev, sr_audio_dump_postmix, NULL)) {
        fprintf(stderr, "AUDIODUMP: postmix hook refused (%s); no dump written\n", SDL_GetError());
        fclose(s_dump_file);
        s_dump_file = NULL;
        return;
    }
    fprintf(stderr, "AUDIODUMP: writing device mix to %s (%uHz, %u ch, s16le)\n",
            s_dump_path, s_dump_rate, s_dump_channels);
}

static void sr_audio_dump_stop(SDL_AudioDeviceID dev) {
    if (!s_dump_file) return;
    if (dev) SDL_SetAudioPostmixCallback(dev, NULL, NULL);
    sr_audio_dump_patch_header();
    fclose(s_dump_file);
    s_dump_file = NULL;
    fprintf(stderr, "AUDIODUMP: %s rate=%u channels=%u frames=%llu seconds=%.3f silent_frames=%llu "
                    "clipped_samples=%llu peak=%u dropped_frames=%llu\n",
            s_dump_path, s_dump_rate, s_dump_channels, (unsigned long long)s_dump_stats.frames,
            s_dump_rate ? (double)s_dump_stats.frames / (double)s_dump_rate : 0.0,
            (unsigned long long)s_dump_stats.silent_frames, (unsigned long long)s_dump_stats.clipped,
            (unsigned)s_dump_stats.peak, (unsigned long long)s_dump_stats.dropped_frames);
    if (s_dump_write_failed) {
        fprintf(stderr, "AUDIODUMP: the dump ended early on a failed write; the header covers %llu data bytes\n",
                (unsigned long long)s_dump_last_patch);
    }
    fflush(stderr);
}

/* The SR_AUDIODUMP close for the end-of-run capture point, which exits with _Exit and so skips
 * atexit. It prints no stats line and never reads SR_AUDIOSTAT; once the dump is closed it is a
 * no-op. Without SR_AUDIODUMP no dump is open, so it does nothing. */
void sr_audio_dump_finish(void) {
    sr_audio_dump_stop(s_device_id);
}

static void sr_audio_cleanup(void) {
    if (s_audio_state == AUDIO_STATE_ACTIVE) {
        sr_audio_dump_stop(s_device_id);
        for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
            if (s_streams[i]) {
                SDL_DestroyAudioStream(s_streams[i]);
                s_streams[i] = NULL;
            }
        }
        if (s_device_id) {
            SDL_CloseAudioDevice(s_device_id);
            s_device_id = 0;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        free(s_pause_stash[i].samples);
        s_pause_stash[i].samples = NULL;
        s_pause_stash[i].frames = 0;
        free(s_shadow_queues[i].samples);
        memset(&s_shadow_queues[i], 0, sizeof(s_shadow_queues[i]));
    }
    sr_audio_pull_reset();
    memset(s_primed_frames, 0, sizeof(s_primed_frames));
    s_pause_active = 0;
    s_pause_device_fallback = 0;
    s_pause_device_was_paused = 0;
    s_audio_state = AUDIO_STATE_UNINITIALIZED;
}

/* Master-volume gain from SR_MASTER_VOLUME (player Settings > Master Volume,
 * 0..100). Pure and clamped: 0 mutes, 100 is unity. Callers pass 100 when the
 * variable is unset so a direct runtime launch keeps full volume. */
static float sr_audio_master_gain(int percent) {
    if (percent <= 0) return 0.0f;
    if (percent >= 100) return 1.0f;
    return (float)percent / 100.0f;
}

/* One sample at a channel volume, where 0x8000 is unity gain, clamped to 16 bits. The
 * product is 64-bit: the Output2/SRC channel accepts volumes up to 0xFFFFF (hle_audio.c,
 * SCE_AUDIO_OUTPUT2_VOLUME_MAX), and a full-scale sample times that overflows 32 bits.
 * PPSSPP's mixer also widens the product to 64 bits (hrydgard/ppsspp caa91b54b535,
 * Core/HLE/__sceAudio.cpp, ApplyChannelVolume), but clamps once after summing the channels;
 * this mixer hands each channel to its own stream, so it clamps per sample. */
static int16_t sr_audio_scale_sample(int16_t sample, int volume) {
    int64_t scaled = ((int64_t)sample * volume) >> 15;
    return (int16_t)(scaled < -32768 ? -32768 : (scaled > 32767 ? 32767 : scaled));
}

static int sr_audio_apply_master_volume(int channel_volume) {
    if (s_master_volume_percent >= 100) return channel_volume;
    if (s_master_volume_percent <= 0) return 0;
    return (int)(((int64_t)channel_volume * s_master_volume_percent) / 100);
}

/* ---- Ace Mode's true-peak limiter (peak=limit, #2137, docs/ENHANCEMENT_CONTRACT.md 10.22) ------
 *
 * Off unless the run turns it on (sr_audio_set_true_peak_limiter); off, sr_audio_push is what it
 * was. On, every push of every channel passes through one limiter per channel instead of the
 * per-sample clamp, and a push may carry the SAS grain's unsaturated sum (sr_audio_push_wide,
 * from hle_audio.c) in place of the int16 copy the guest holds. The guest sees nothing of it: the
 * guest's buffers are only read, and a push of n frames still queues n frames.
 *
 * The limiter, per channel, in mixer frames at 44.1 kHz:
 *   - detection: each frame's peak is its own sample and the inter-sample peaks on either side of
 *     it, read at 4x through a windowed-sinc interpolator (SR_TPL_TAPS taps a phase);
 *   - the gain a frame needs is ceiling / peak, or 1 below the ceiling (SR_TPL_CEILING, -0.3 dBTP);
 *   - a minimum over the next SR_TPL_LOOKAHEAD frames, then an exponential release
 *     (SR_TPL_RELEASE_S), then a moving average over SR_TPL_LOOKAHEAD frames. Every frame the
 *     average covers looks ahead past the peak, so the gain at a peak is at most what that peak
 *     needs and the gain falls over SR_TPL_LOOKAHEAD frames before it, never in one step;
 *   - the output is the input SR_TPL_DELAY frames late (1.6 ms), so the channel's queue holds the
 *     same number of frames as without the limiter. Frames below the ceiling, with no peak within
 *     the attack or release of them, come out bit-exact (a gain of exactly 1).
 * A stream that stops leaves its last SR_TPL_DELAY frames in the limiter until the channel's next
 * push or hold of silence (sr_audio_push_silence), which plays them first, in order. A push or hold
 * that finds the channel played dry drops them instead, as a hold drops the converter's kept-back
 * frames: after the gap they would play as a fragment ahead of the new sound. Nothing is invented. */
#define SR_TPL_TAPS 16                       /* interpolator taps a phase; it reads 8 frames ahead */
#define SR_TPL_PHASES 4                      /* 4x oversampled peak detection */
#define SR_TPL_LOOKAHEAD 64                  /* attack, in frames (1.45 ms) */
#define SR_TPL_DELAY (SR_TPL_LOOKAHEAD - 1 + SR_TPL_TAPS / 2)   /* 71 frames, 1.61 ms */
#define SR_TPL_RING 256u                     /* a power of two above every reach back */
#define SR_TPL_CEILING (0.9660508789898133 * 32768.0)   /* -0.3 dBTP, in int16 counts */
#define SR_TPL_RELEASE_S 0.050
#define SR_TPL_WIDE_LIMIT (1 << 24)          /* the widest sample a push can carry (float-exact) */

typedef struct {
    uint64_t next;                       /* index of the next input frame (starts at SR_TPL_RING) */
    float x[SR_TPL_RING][2];             /* input, in int16 counts after the channel volume */
    float isp[SR_TPL_RING];              /* inter-sample peak between frame i and i + 1 */
    double need[SR_TPL_RING];            /* the gain frame i needs */
    double held[SR_TPL_RING];            /* minimum ahead, released */
} AudioPeakLimiter;

typedef struct {
    uint64_t pushes_wide;     /* pushes that carried an unsaturated SAS sum */
    uint64_t frames_wide;     /* frames of those whose sum differs from the guest's copy */
    uint64_t frames_limited;  /* output frames whose bytes the gain changed */
    double min_gain;          /* the lowest gain applied; 1 when none was */
} AudioPeakStats;

static int s_tpl_on;
static AudioPeakLimiter s_tpl[SR_AUDIO_CHANNELS];
static AudioPeakStats s_tpl_stats = {0, 0, 0, 1.0};
static double s_tpl_fir[SR_TPL_PHASES][SR_TPL_TAPS];
static double s_tpl_release;

/* One channel's limiter back to silence: no frames held, unity gain. */
static void sr_audio_tpl_reset_channel(int ch) {
    AudioPeakLimiter *l = &s_tpl[ch];
    memset(l, 0, sizeof(*l));
    l->next = SR_TPL_RING;
    for (uint32_t i = 0; i < SR_TPL_RING; i++) l->need[i] = l->held[i] = 1.0;
}

static void sr_audio_tpl_reset(void) {
    for (int ch = 0; ch < SR_AUDIO_CHANNELS; ch++) sr_audio_tpl_reset_channel(ch);
    memset(&s_tpl_stats, 0, sizeof(s_tpl_stats));
    s_tpl_stats.min_gain = 1.0;
}

/* Phase k reads the value at fraction k / SR_TPL_PHASES past frame m from frames
 * m - SR_TPL_TAPS / 2 + 1 .. m + SR_TPL_TAPS / 2: a Blackman-windowed sinc, each phase scaled
 * to a DC gain of 1. Phase 0 is the frame itself and is not used. */
static void sr_audio_tpl_design(void) {
    const double pi = 3.14159265358979323846;
    for (int k = 0; k < SR_TPL_PHASES; k++) {
        const double f = (double)k / SR_TPL_PHASES;
        double sum = 0.0;
        for (int t = 0; t < SR_TPL_TAPS; t++) {
            const double u = f - (double)(t - (SR_TPL_TAPS / 2 - 1));   /* distance to tap */
            const double sinc = fabs(u) < 1e-12 ? 1.0 : sin(pi * u) / (pi * u);
            const double w = 0.42 + 0.5 * cos(2.0 * pi * u / SR_TPL_TAPS) +
                             0.08 * cos(4.0 * pi * u / SR_TPL_TAPS);
            s_tpl_fir[k][t] = sinc * w;
            sum += s_tpl_fir[k][t];
        }
        for (int t = 0; t < SR_TPL_TAPS; t++) s_tpl_fir[k][t] /= sum;
    }
    s_tpl_release = exp(-1.0 / (SR_TPL_RELEASE_S * SR_AUDIO_SAMPLE_RATE));
}

/* Turns the limiter on or off for the run, before the first push (driver.c, from the Ace line).
 * Returns 1: this backend carries it. */
int sr_audio_set_true_peak_limiter(int on) {
    s_tpl_on = on ? 1 : 0;
    sr_audio_tpl_design();
    sr_audio_tpl_reset();
    return 1;
}

/* The run's totals, for the Ace report (driver.c hands them to ace.c at exit). */
void sr_audio_true_peak_stats(uint64_t *pushes_wide, uint64_t *frames_wide, uint64_t *frames_limited,
                              double *min_gain) {
    if (pushes_wide) *pushes_wide = s_tpl_stats.pushes_wide;
    if (frames_wide) *frames_wide = s_tpl_stats.frames_wide;
    if (frames_limited) *frames_limited = s_tpl_stats.frames_limited;
    if (min_gain) *min_gain = s_tpl_stats.min_gain;
}

static float sr_audio_tpl_scaled(int64_t sample, int volume) {
    int64_t v = (sample * volume) >> 15;   /* the authentic scale, before its clamp */
    if (v > SR_TPL_WIDE_LIMIT) v = SR_TPL_WIDE_LIMIT;
    if (v < -SR_TPL_WIDE_LIMIT) v = -SR_TPL_WIDE_LIMIT;
    return (float)v;
}

/* Feeds one frame and returns the frame SR_TPL_DELAY earlier, limited. */
static void sr_audio_tpl_frame(AudioPeakLimiter *l, float in_l, float in_r, int16_t out[2]) {
    const uint32_t M = SR_TPL_RING - 1u;
    const uint64_t j = l->next++;
    l->x[j & M][0] = in_l;
    l->x[j & M][1] = in_r;

    /* Frame m's inter-sample peak toward m + 1: the interpolator now has every frame it reads. */
    const uint64_t m = j - SR_TPL_TAPS / 2;
    float isp = 0.0f;
    for (int k = 1; k < SR_TPL_PHASES; k++) {
        for (int c = 0; c < 2; c++) {
            double y = 0.0;
            for (int t = 0; t < SR_TPL_TAPS; t++)
                y += s_tpl_fir[k][t] * l->x[(m - (SR_TPL_TAPS / 2 - 1) + (uint64_t)t) & M][c];
            const float a = (float)fabs(y);
            if (a > isp) isp = a;
        }
    }
    l->isp[m & M] = isp;
    float peak = fabsf(l->x[m & M][0]);
    if (fabsf(l->x[m & M][1]) > peak) peak = fabsf(l->x[m & M][1]);
    if (isp > peak) peak = isp;
    if (l->isp[(m - 1u) & M] > peak) peak = l->isp[(m - 1u) & M];
    l->need[m & M] = peak > SR_TPL_CEILING ? SR_TPL_CEILING / peak : 1.0;

    /* Frame q's minimum over q .. q + SR_TPL_LOOKAHEAD - 1 (= m), released from frame q - 1's. */
    const uint64_t q = m - (SR_TPL_LOOKAHEAD - 1);
    double h = 1.0;
    for (uint64_t i = q; i <= m; i++)
        if (l->need[i & M] < h) h = l->need[i & M];
    double released = 1.0 - (1.0 - l->held[(q - 1u) & M]) * s_tpl_release;
    if (1.0 - released < 1e-9) released = 1.0;
    l->held[q & M] = h < released ? h : released;

    /* The gain at q: the mean of the held gains over the SR_TPL_LOOKAHEAD frames up to q. A run of
     * ones sums to exactly SR_TPL_LOOKAHEAD, so a frame no peak reaches keeps its value. */
    double sum = 0.0;
    for (uint64_t i = q - (SR_TPL_LOOKAHEAD - 1); i <= q; i++) sum += l->held[i & M];
    const double gain = sum / SR_TPL_LOOKAHEAD;
    if (gain < s_tpl_stats.min_gain) s_tpl_stats.min_gain = gain;
    int changed = 0;
    for (int c = 0; c < 2; c++) {
        double y = (double)l->x[q & M][c] * gain;
        y = y < 0.0 ? y - 0.5 : y + 0.5;               /* to nearest, ties away from zero */
        const long v = (long)y;
        out[c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        if ((float)out[c] != l->x[q & M][c]) changed = 1;   /* the gain changed this frame's bytes */
    }
    if (changed) s_tpl_stats.frames_limited++;
}

/* n frames of `lr` (or of `wide` where given) at the channel volumes, through the channel's
 * limiter into `buf`. */
static void sr_audio_tpl_block(int ch, const int16_t *lr, const int32_t *wide, int nframes,
                               int volL, int volR, int16_t *buf) {
    AudioPeakLimiter *l = &s_tpl[ch];
    if (wide) {
        s_tpl_stats.pushes_wide++;
        for (int i = 0; i < nframes; i++)
            if (wide[i * 2] != lr[i * 2] || wide[i * 2 + 1] != lr[i * 2 + 1]) s_tpl_stats.frames_wide++;
    }
    for (int i = 0; i < nframes; i++) {
        const int64_t sl = wide ? wide[i * 2] : lr[i * 2];
        const int64_t sr = wide ? wide[i * 2 + 1] : lr[i * 2 + 1];
        sr_audio_tpl_frame(l, sr_audio_tpl_scaled(sl, volL), sr_audio_tpl_scaled(sr, volR), buf + i * 2);
    }
}

static int sr_audio_shadow_reserve(AudioShadowQueue *queue, uint32_t needed_frames) {
    if (!queue || !queue->valid || needed_frames > SR_AUDIO_MAX_QUEUED_FRAMES) return 0;
    if (needed_frames <= queue->capacity_frames) return 1;
    uint32_t capacity = queue->capacity_frames ? queue->capacity_frames : 1024u;
    while (capacity < needed_frames) {
        if (capacity > SR_AUDIO_MAX_QUEUED_FRAMES / 2u) {
            capacity = SR_AUDIO_MAX_QUEUED_FRAMES;
            break;
        }
        capacity *= 2u;
    }
    int16_t *samples = (int16_t *)malloc((size_t)capacity * 4u);
    if (!samples) return 0;
    for (uint32_t i = 0; i < queue->frames; i++) {
        const uint32_t src = (queue->head_frame + i) % queue->capacity_frames;
        samples[i * 2u] = queue->samples[src * 2u];
        samples[i * 2u + 1u] = queue->samples[src * 2u + 1u];
    }
    free(queue->samples);
    queue->samples = samples;
    queue->capacity_frames = capacity;
    queue->head_frame = 0;
    return 1;
}

static void sr_audio_shadow_sync(int ch, uint32_t queued_frames) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || s_pause_active) return;
    AudioShadowQueue *queue = &s_shadow_queues[ch];
    if (!queued_frames) {
        queue->head_frame = 0;
        queue->frames = 0;
        queue->valid = 1;
        return;
    }
    if (!queue->valid || queued_frames > SR_AUDIO_MAX_QUEUED_FRAMES) {
        queue->valid = 0;
        return;
    }
    if (queue->frames < queued_frames) {
        queue->valid = 0;
        return;
    }
    const uint32_t consumed = queue->frames - queued_frames;
    if (consumed && queue->capacity_frames)
        queue->head_frame = (queue->head_frame + consumed) % queue->capacity_frames;
    queue->frames = queued_frames;
}

static int sr_audio_shadow_append(int ch, const int16_t *samples, int frames) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || !samples || frames < 0) return 0;
    AudioShadowQueue *queue = &s_shadow_queues[ch];
    if (!queue->valid) return 0;
    if ((uint32_t)frames > SR_AUDIO_MAX_QUEUED_FRAMES - queue->frames ||
        !sr_audio_shadow_reserve(queue, queue->frames + (uint32_t)frames)) {
        queue->valid = 0;
        return 0;
    }
    if (!frames) return 1;
    const uint32_t tail = (queue->head_frame + queue->frames) % queue->capacity_frames;
    uint32_t first = queue->capacity_frames - tail;
    if (first > (uint32_t)frames) first = (uint32_t)frames;
    memcpy(queue->samples + tail * 2u, samples, (size_t)first * 4u);
    if ((uint32_t)frames > first)
        memcpy(queue->samples, samples + first * 2u, (size_t)((uint32_t)frames - first) * 4u);
    queue->frames += (uint32_t)frames;
    return 1;
}

static int sr_audio_shadow_copy(int ch, int16_t *out, uint32_t frames) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || !out) return 0;
    const AudioShadowQueue *queue = &s_shadow_queues[ch];
    if (!queue->valid || queue->frames != frames) return 0;
    if (!frames) return 1;
    uint32_t first = queue->capacity_frames - queue->head_frame;
    if (first > frames) first = frames;
    memcpy(out, queue->samples + queue->head_frame * 2u, (size_t)first * 4u);
    if (frames > first)
        memcpy(out + first * 2u, queue->samples, (size_t)(frames - first) * 4u);
    return 1;
}

/* A device block of `device_frames` at `device_freq`, in mixer frames (SR_AUDIO_SAMPLE_RATE),
 * rounded up: a pull takes that many frames from each channel's stream. 0 for an unknown block. */
static uint32_t sr_audio_pull_in_mixer_frames(int device_frames, int device_freq) {
    if (device_frames <= 0 || device_freq <= 0) return 0u;
    const uint64_t scaled = (uint64_t)(uint32_t)device_frames * SR_AUDIO_SAMPLE_RATE +
                            (uint32_t)device_freq - 1u;
    const uint64_t frames = scaled / (uint32_t)device_freq;
    return frames > (uint64_t)INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)frames;
}

int sr_audio_init(void) {
    if (s_audio_state == AUDIO_STATE_ACTIVE) {
        return 0;
    }
    if (s_audio_state == AUDIO_STATE_UNAVAILABLE || s_audio_state == AUDIO_STATE_NULL) {
        return -1;
    }

    /* Check environment configuration for explicit null/disabled audio */
    const char *env_backend = getenv("SR_AUDIO_BACKEND");
    const char *env_disabled = getenv("SR_AUDIO_DISABLED");
    const char *env_headless = getenv("SR_HEADLESS");
    if ((env_backend && (strcmp(env_backend, "null") == 0 ||
                         strcmp(env_backend, "none") == 0 ||
                         strcmp(env_backend, "0") == 0 ||
                         strcmp(env_backend, "off") == 0)) ||
        (env_disabled && strcmp(env_disabled, "1") == 0) ||
        (env_headless && strcmp(env_headless, "1") == 0 && !env_backend)) {
        s_audio_state = AUDIO_STATE_NULL;
        if (!s_reported_unavailable) {
            fprintf(stderr, "audio: host audio output disabled by environment (null backend)\n");
            s_reported_unavailable = 1;
        }
        return -1;
    }

    if (env_backend && strcmp(env_backend, "dummy") == 0) {
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    }

#ifdef SR_AUDIO_SELFTEST
    s_test_device_open_attempts++;
#endif
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        s_audio_state = AUDIO_STATE_UNAVAILABLE;
        if (!s_reported_unavailable) {
            fprintf(stderr, "audio: host audio output unavailable: %s\n", SDL_GetError());
            s_reported_unavailable = 1;
        }
        return -1;
    }

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = SR_AUDIO_SAMPLE_RATE;

    sr_audio_pull_reset();
    SDL_AudioSpec device_request = spec;
#ifdef SR_AUDIO_SELFTEST
    if (s_test_device_freq) device_request.freq = s_test_device_freq;
#endif
    s_device_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device_request);
    if (!s_device_id) {
        s_audio_state = AUDIO_STATE_UNAVAILABLE;
        if (!s_reported_unavailable) {
            fprintf(stderr, "audio: host audio output unavailable (no audio device: %s)\n", SDL_GetError());
            s_reported_unavailable = 1;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        s_streams[i] = SDL_CreateAudioStream(&spec, &spec);
        /* Each stream reports the device's pulls (sr_audio_pull_callback). Without the report the
         * queue is sized against SDL's chunk size alone, read on every query, and no channel is
         * seen played dry after its first push. */
        if (s_streams[i] &&
            !SDL_SetAudioStreamGetCallback(s_streams[i], sr_audio_pull_callback, (void *)(intptr_t)i) &&
            !s_pull_unmeasured) {
            fprintf(stderr, "audio: device pulls are not measured (%s): SDL's chunk size stands in\n", SDL_GetError());
            s_pull_unmeasured = 1;
        }
        if (!s_streams[i] || !SDL_BindAudioStream(s_device_id, s_streams[i])) {
            s_audio_state = AUDIO_STATE_UNAVAILABLE;
            if (!s_reported_unavailable) {
                fprintf(stderr, "audio: host audio output stream creation failed: %s\n", SDL_GetError());
                s_reported_unavailable = 1;
            }
            sr_audio_cleanup();
            return -1;
        }
        s_shadow_queues[i].valid = 1;
    }

    /* Apply master volume in the integer push path. SDL3 applies stream gain
     * when data leaves a stream, which would also scale the temporary pause
     * stash when it is read back. The stream gain remains at its default 1.0. */
    {
        const char *mv = getenv("SR_MASTER_VOLUME");
        const int requested = (mv && mv[0]) ? atoi(mv) : 100;
        const float gain = sr_audio_master_gain(requested);
        s_master_volume_percent = (int)(gain * 100.0f + 0.5f);
    }

    s_audio_state = AUDIO_STATE_ACTIVE;
    sr_audio_dump_start(s_device_id);
    if (!s_reported_init) {
        const char *driver = SDL_GetCurrentAudioDriver();
        fprintf(stderr, "audio: initialized SDL3 audio output (driver: %s, %uHz stereo 16-bit)\n",
                driver ? driver : "unknown", SR_AUDIO_SAMPLE_RATE);
        /* What the blocking output sizes its queue against, until pulls are measured
         * (sr_audio_host_pull_frames). */
        SDL_AudioSpec device_spec;
        int device_frames = 0;
        if (SDL_GetAudioDeviceFormat(s_device_id, &device_spec, &device_frames) && device_frames > 0 &&
            device_spec.freq > 0)
            fprintf(stderr, "audio: the host device takes %d frames at %d Hz per pull (%.1f ms, %u mixer frames)\n",
                    device_frames, device_spec.freq, 1000.0 * device_frames / device_spec.freq,
                    sr_audio_pull_in_mixer_frames(device_frames, device_spec.freq));
        else
            fprintf(stderr, "audio: the host device does not report how many frames it takes per pull\n");
        s_reported_init = 1;
    }

    if (!s_cleanup_registered) {
        atexit(sr_audio_cleanup);
        s_cleanup_registered = 1;
    }

    return 0;
}

/* 1 when the device has played `ch` dry and nothing has been pushed since: a pull came while its
 * stream could give nothing, and it still can give nothing. A queue restored after a pause is not
 * dry, whatever the device asked for while it was held. Opens the device like a first push, so a
 * channel's first output asks a live one. 0 with no host output. */
int sr_audio_played_dry(int ch) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS) {
        return 0;
    }
    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) {
        sr_audio_init();
    }
    if (s_audio_state != AUDIO_STATE_ACTIVE || !SDL_GetAtomicInt(&s_played_dry[ch])) {
        return 0;
    }
    return SDL_GetAudioStreamAvailable(s_streams[ch]) == 0;
}

/* Of `queued_frames` on `ch`, the frames a pull can still take: all of them, or none when the
 * stream can give nothing. A stream that converts the rate keeps its last few frames back until
 * more arrive (7 from 44.1 to 48 kHz on the reference PC), and those are never played on their
 * own, so they are not queued audio as the guest means it. Warship Gunner 2 Portable waits for
 * sceAudioGetChannelRestLength to read 0 at the end of a stream; on a 48 kHz device it read 7
 * for good and the title stopped there. The dropout count needs the same reading: on such a
 * device a queue that has run dry is not empty. */
static int sr_audio_takeable_frames(int ch, int queued_frames) {
    return SDL_GetAudioStreamAvailable(s_streams[ch]) == 0 ? 0 : queued_frames;
}

void sr_audio_push_silence(int ch, int nframes) {
    static const int16_t zeros[1024 * 2];
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || nframes <= 0 || s_audio_state != AUDIO_STATE_ACTIVE) {
        return;
    }
    const int queued_bytes = SDL_GetAudioStreamQueued(s_streams[ch]);
    if (queued_bytes < 0) {
        return;
    }
    sr_audio_shadow_sync(ch, (uint32_t)(queued_bytes / 4));
    if (queued_bytes / 4 >= SR_AUDIO_MAX_QUEUED_FRAMES - nframes) {
        return;   /* a hold the mixer cannot take: the pushes that follow are refused and counted */
    }
    /* A stream that can give nothing may still hold the last frames of what it played, kept back
     * by its converter. Ahead of the silence they would play as a fragment between two silences,
     * so they go: nothing a pull could take is lost. The frames Ace Mode's limiter holds back go
     * for the same reason; on a channel still playing they stay, and leave ahead of the silence. */
    const int dry = queued_bytes == 0 || sr_audio_takeable_frames(ch, queued_bytes / 4) == 0;
    if (s_tpl_on && dry) sr_audio_tpl_reset_channel(ch);
    if (queued_bytes > 0 && dry) {
        if (!SDL_ClearAudioStream(s_streams[ch])) {
            fprintf(stderr, "audio: SDL_ClearAudioStream failed: %s\n", SDL_GetError());
            return;
        }
        sr_audio_shadow_sync(ch, 0u);
    }
    /* Before the data: a pull that comes in between then leaves the flag set over a queue that
     * has frames, which is not dry. */
    SDL_SetAtomicInt(&s_played_dry[ch], 0);
    while (nframes > 0) {
        const int chunk = nframes < 1024 ? nframes : 1024;
        /* With Ace Mode's limiter on, the hold is pushed through it like any frames: the last
         * SR_TPL_DELAY frames of what played before leave ahead of the silence, in order. */
        int16_t limited[1024 * 2];
        const int16_t *data = zeros;
        if (s_tpl_on) {
            sr_audio_tpl_block(ch, zeros, NULL, chunk, 0x8000, 0x8000, limited);
            data = limited;
        }
        if (!SDL_PutAudioStreamData(s_streams[ch], data, chunk * 4)) {
            fprintf(stderr, "audio: SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
            return;
        }
        (void)sr_audio_shadow_append(ch, data, chunk);
        s_primed_frames[ch] += (uint32_t)chunk;
        nframes -= chunk;
    }
}

/* A push the full mixer drops is host playback falling behind the guest (a run faster than real
 * time). The guest does not see it: under SR_DETERMINISTIC_CLOCK=1 it paces against its own model
 * (hle_audio.c). It stays visible here with counts, on the first drop and then each time the
 * count of dropped pushes doubles, and in AUDIOSTAT_HOST at exit. */
static void audio_note_overrun(int ch, int nframes) {
    const uint64_t n = s_stats.overrun_events;
    if (n & (n - 1u)) return;
    fprintf(stderr, "audio: host mixer queue is full (%d frames) on channel %d: dropped %d frames; "
                    "%llu pushes and %llu frames dropped so far (the guest is not affected under "
                    "SR_DETERMINISTIC_CLOCK=1)\n",
            SR_AUDIO_MAX_QUEUED_FRAMES, ch, nframes, (unsigned long long)n,
            (unsigned long long)s_stats.overrun_frames);
}

/* The other side: a push that finds a channel that was playing already dry is host playback that
 * ran ahead of the guest. Counted in underrun_events; logged the same way. */
static void audio_note_underrun(int ch) {
    const uint64_t n = s_stats.underrun_events;
    if (n & (n - 1u)) return;
    fprintf(stderr, "audio: host mixer underrun on channel %d: the queue ran dry; %llu so far (the guest "
                    "is not affected under SR_DETERMINISTIC_CLOCK=1)\n",
            ch, (unsigned long long)n);
}

static void sr_audio_push_frames(int ch, const int16_t *lr, const int32_t *wide, int nframes,
                                 int volL, int volR);

void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR) {
    sr_audio_push_frames(ch, lr, NULL, nframes, volL, volR);
}

/* sr_audio_push with the SAS grain's unsaturated sum for the same frames (hle_audio.c, Ace Mode's
 * peak=limit). `lr` is still the guest's copy, for everything but the sound: the recorder and the
 * counters. Without the limiter on, the wide values are not used. */
void sr_audio_push_wide(int ch, const int16_t *lr, const int32_t *wide, int nframes, int volL, int volR) {
    sr_audio_push_frames(ch, lr, s_tpl_on ? wide : NULL, nframes, volL, volR);
}

static void sr_audio_push_frames(int ch, const int16_t *lr, const int32_t *wide, int nframes,
                                 int volL, int volR) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS || !lr || nframes <= 0) {
        return;
    }

    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) {
        sr_audio_init();
    }
    if (s_audio_state != AUDIO_STATE_ACTIVE) {
        return;
    }

    volL = sr_audio_apply_master_volume(volL);
    volR = sr_audio_apply_master_volume(volR);

    int queued_bytes = SDL_GetAudioStreamQueued(s_streams[ch]);
    if (queued_bytes < 0) {
        return;
    }
    int cur_queued_frames = queued_bytes / 4;
    sr_audio_shadow_sync(ch, (uint32_t)cur_queued_frames);
    /* What this push found queued: frames a pull can take (sr_audio_takeable_frames), less the
     * silence sr_audio_push_silence() put there for it. */
    const uint32_t primed_frames = s_primed_frames[ch];
    const int takeable_frames = sr_audio_takeable_frames(ch, cur_queued_frames);
    const int found_frames = (uint32_t)takeable_frames > primed_frames
                                 ? takeable_frames - (int)primed_frames : 0;
    s_primed_frames[ch] = 0;

    /* Instrumentation: underrun detection (queue had completely dried up after previous activity) */
    if (found_frames == 0 && s_stats.channel_pushed_frames[ch] > 0) {
        s_stats.underrun_events++;
        audio_note_underrun(ch);
    }

    /* Instrumentation: backpressure detection (queue has more than two periods of lead) */
    if (cur_queued_frames > 2 * nframes) {
        s_stats.backpressure_events++;
    }

    /* Backpressure ceiling: drop samples if queue has grown excessively to protect memory */
    if (cur_queued_frames >= SR_AUDIO_MAX_QUEUED_FRAMES) {
        s_stats.overrun_events++;
        s_stats.overrun_frames += (uint64_t)nframes;
        audio_note_overrun(ch, nframes);
        return;
    }

    const int16_t *submitted = lr;
    int push_ok;
    /* Before the data, as in sr_audio_push_silence. Played dry: a device pull found nothing (an
     * empty queue a pull just emptied is still playing). Read and cleared in one step, so a pull
     * that finds nothing in between is not lost. */
    const int played_dry = SDL_SetAtomicInt(&s_played_dry[ch], 0) && takeable_frames == 0;
    /* Volume scaling and submission */
    if (s_tpl_on) {
        /* Ace Mode's limiter in place of the per-sample clamp: n frames in, n frames out. On a
         * channel played dry with no hold ahead of this push, the frames the limiter still holds
         * would play as a fragment ahead of the new sound, so they go (as in the hold). A queue the
         * last pull emptied without coming up short is still playing: its frames stay. */
        if (played_dry) sr_audio_tpl_reset_channel(ch);
        int16_t stack_buf[1024 * 2];
        int16_t *buf = stack_buf;
        if (nframes > 1024) {
            buf = (int16_t *)malloc((size_t)nframes * 4);
            if (!buf) return;
        }
        sr_audio_tpl_block(ch, lr, wide, nframes, volL, volR, buf);
        push_ok = SDL_PutAudioStreamData(s_streams[ch], buf, nframes * 4);
        if (push_ok) (void)sr_audio_shadow_append(ch, buf, nframes);
        if (buf != stack_buf) free(buf);
    } else if (volL == 0x8000 && volR == 0x8000) {
        push_ok = SDL_PutAudioStreamData(s_streams[ch], lr, nframes * 4);
    } else {
        int16_t stack_buf[1024 * 2];
        int16_t *buf = stack_buf;
        if (nframes > 1024) {
            buf = (int16_t *)malloc((size_t)nframes * 4);
            if (!buf) return;
        }
        for (int i = 0; i < nframes; i++) {
            buf[i * 2]     = sr_audio_scale_sample(lr[i * 2], volL);
            buf[i * 2 + 1] = sr_audio_scale_sample(lr[i * 2 + 1], volR);
        }
        submitted = buf;
        push_ok = SDL_PutAudioStreamData(s_streams[ch], buf, nframes * 4);
        if (push_ok) (void)sr_audio_shadow_append(ch, submitted, nframes);
        if (buf != stack_buf) {
            free(buf);
        }
    }
    if (!push_ok) {
        fprintf(stderr, "audio: SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
        return;
    }
    if (!s_tpl_on && volL == 0x8000 && volR == 0x8000)
        (void)sr_audio_shadow_append(ch, submitted, nframes);
    s_stats.total_pushed_frames += (uint64_t)nframes;
    s_stats.channel_pushed_frames[ch] += (uint64_t)nframes;

    const uint64_t now_ns = SDL_GetTicksNS();
    int underrun;
    uint64_t starved_ns = audio_starved_ns(&s_push_marks[ch], now_ns, found_frames, &underrun);
    /* A dropout is silent until this push is heard: the gap the device padded, and the hold
     * queued as silence ahead of the push. */
    if (underrun) starved_ns += (uint64_t)primed_frames * 1000000000ull / SR_AUDIO_SAMPLE_RATE;
    sr_perf_audio_queue(ch, (uint32_t)found_frames, starved_ns, underrun);
    s_push_marks[ch].push_ns = now_ns;
    s_push_marks[ch].queued_frames = (uint32_t)(cur_queued_frames + nframes);

    const int new_queue_bytes = SDL_GetAudioStreamQueued(s_streams[ch]);
    if (new_queue_bytes >= 0) {
        const int new_q = new_queue_bytes / 4;
        sr_audio_shadow_sync(ch, (uint32_t)new_q);
        s_push_marks[ch].queued_frames = (uint32_t)new_q;
        if (new_q > (int)s_stats.peak_queued_frames[ch])
            s_stats.peak_queued_frames[ch] = (uint32_t)new_q;
    }
}

static void sr_audio_pause_unlock(const int locked[SR_AUDIO_CHANNELS]) {
    for (int i = SR_AUDIO_CHANNELS - 1; i >= 0; i--)
        if (locked[i]) SDL_UnlockAudioStream(s_streams[i]);
}

static void sr_audio_pause_free_stashes(void) {
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        free(s_pause_stash[i].samples);
        s_pause_stash[i].samples = NULL;
        s_pause_stash[i].frames = 0;
    }
}

static void *sr_audio_pause_alloc(size_t bytes) {
#ifdef SR_AUDIO_SELFTEST
    if (s_test_fail_pause_stash_alloc) {
        s_test_fail_pause_stash_alloc = 0;
        return NULL;
    }
#endif
    return malloc(bytes);
}

static void sr_audio_pause_failure_message(int failure_kind) {
    if (failure_kind == 1) {
        if (s_pause_failure_reported) return;
        s_pause_failure_reported = 1;
        const int written = fprintf(stderr,
            "AUDIO_PAUSE: stash allocation failed; device paused without fade\n");
#ifdef SR_AUDIO_SELFTEST
        if (written > 0) s_test_pause_failure_messages++;
#else
        (void)written;
#endif
    } else if (failure_kind == 2) {
        fprintf(stderr, "AUDIO_PAUSE: queued audio mirror unavailable; device paused without fade\n");
    } else {
        fprintf(stderr, "AUDIO_PAUSE: stream operation failed; device paused without fade (%s)\n",
                SDL_GetError());
    }
    fflush(stderr);
}

static void sr_audio_pause_fallback(uint64_t queued_frames, int failure_kind) {
    s_stats.pause_stash_failures++;
    s_stats.pause_held_frames += queued_frames;
    sr_audio_pause_failure_message(failure_kind);
    s_pause_device_was_paused = SDL_AudioDevicePaused(s_device_id);
    if (!s_pause_device_was_paused && !SDL_PauseAudioDevice(s_device_id)) {
        fprintf(stderr, "AUDIO_PAUSE: could not pause the device for fallback (%s)\n", SDL_GetError());
        fflush(stderr);
    }
    s_pause_device_fallback = 1;
    s_pause_active = 1;
    s_stats.pause_enters++;
}

static void sr_audio_pause_restore_unfaded(const int locked[SR_AUDIO_CHANNELS]) {
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (!locked[i] || !s_pause_stash[i].frames) continue;
        SDL_ClearAudioStream(s_streams[i]);
        if (!SDL_PutAudioStreamData(s_streams[i], s_pause_stash[i].samples,
                                    s_pause_stash[i].frames * 4)) {
            fprintf(stderr, "AUDIO_PAUSE: could not restore channel %d after stream failure (%s)\n",
                    i, SDL_GetError());
            fflush(stderr);
        }
    }
}

static void sr_audio_pause_scale_prefix(const int16_t *source, int16_t *destination,
                                        int frames, int fade_in) {
    for (int frame = 0; frame < frames; frame++) {
        int numerator, denominator;
        if (frames <= 1) {
            numerator = fade_in ? 1 : 0;
            denominator = 1;
        } else {
            denominator = frames - 1;
            numerator = fade_in ? frame : (frames - 1 - frame);
        }
        for (int channel = 0; channel < 2; channel++) {
            destination[frame * 2 + channel] = (int16_t)(
                ((int32_t)source[frame * 2 + channel] * numerator) / denominator);
        }
    }
}

void sr_audio_pause_enter(void) {
    if (s_audio_state != AUDIO_STATE_ACTIVE || s_pause_active) return;

    int locked[SR_AUDIO_CHANNELS] = {0};
    int queued[SR_AUDIO_CHANNELS] = {0};
    uint64_t queued_frames = 0;
    int failure = 0;
    int failure_kind = 0;

    /* Lock every stream before reading any depth, so SDL's device thread cannot
     * consume a later channel while an earlier channel is being stashed. */
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (!s_streams[i] || !SDL_LockAudioStream(s_streams[i])) {
            failure = 1;
            break;
        }
        locked[i] = 1;
    }
    if (failure) {
        sr_audio_pause_unlock(locked);
        sr_audio_pause_fallback(0, 3);
        return;
    }

    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        const int bytes = SDL_GetAudioStreamQueued(s_streams[i]);
        if (bytes < 0 || (bytes & 3) != 0) {
            failure = 1;
            break;
        }
        queued[i] = bytes / 4;
        queued_frames += (uint64_t)queued[i];
        sr_audio_shadow_sync(i, (uint32_t)queued[i]);
        if (!s_shadow_queues[i].valid || s_shadow_queues[i].frames != (uint32_t)queued[i]) {
            failure = 1;
            failure_kind = 2;
            break;
        }
        if (queued[i] > 0) {
            s_pause_stash[i].samples = (int16_t *)sr_audio_pause_alloc((size_t)bytes);
            s_pause_stash[i].frames = queued[i];
            if (!s_pause_stash[i].samples) {
                failure = 1;
                failure_kind = 1;
                break;
            }
            if (!sr_audio_shadow_copy(i, s_pause_stash[i].samples, (uint32_t)queued[i])) {
                failure = 1;
                failure_kind = 2;
                break;
            }
        }
    }
    if (failure) {
        sr_audio_pause_free_stashes();
        sr_audio_pause_unlock(locked);
        sr_audio_pause_fallback(queued_frames, failure_kind);
        return;
    }

    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (!SDL_ClearAudioStream(s_streams[i])) { failure = 1; break; }
    }
    if (!failure) {
        for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
            const int frames = queued[i] < SR_AUDIO_PAUSE_FADE_FRAMES
                ? queued[i] : SR_AUDIO_PAUSE_FADE_FRAMES;
            if (!frames) continue;
            sr_audio_pause_scale_prefix(s_pause_stash[i].samples, s_pause_fade[i], frames, 0);
            if (!SDL_PutAudioStreamData(s_streams[i], s_pause_fade[i], frames * 4)) {
                failure = 1;
                break;
            }
        }
    }

    if (failure) sr_audio_pause_restore_unfaded(locked);
    sr_audio_pause_unlock(locked);
    if (failure) {
        sr_audio_pause_free_stashes();
        sr_audio_pause_fallback(queued_frames, 3);
        return;
    }

    s_pause_active = 1;
    s_pause_device_fallback = 0;
    s_stats.pause_enters++;
    s_stats.pause_held_frames += queued_frames;
}

void sr_audio_pause_leave(void) {
    if (s_audio_state != AUDIO_STATE_ACTIVE || !s_pause_active) return;
    if (s_pause_device_fallback) {
        if (!s_pause_device_was_paused && !SDL_ResumeAudioDevice(s_device_id)) {
            fprintf(stderr, "AUDIO_PAUSE: could not resume fallback device (%s)\n", SDL_GetError());
            fflush(stderr);
        }
        s_pause_device_fallback = 0;
        s_pause_active = 0;
        return;
    }

    int locked[SR_AUDIO_CHANNELS] = {0};
    int failure = 0;
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (!s_streams[i] || !SDL_LockAudioStream(s_streams[i])) {
            failure = 1;
            break;
        }
        locked[i] = 1;
    }
    if (failure) {
        sr_audio_pause_unlock(locked);
        fprintf(stderr, "AUDIO_PAUSE: could not lock streams while restoring audio (%s)\n", SDL_GetError());
        fflush(stderr);
        sr_audio_pause_free_stashes();
        s_pause_active = 0;
        return;
    }

    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        const int frames = s_pause_stash[i].frames;
        const int fade_frames = frames < SR_AUDIO_PAUSE_FADE_FRAMES
            ? frames : SR_AUDIO_PAUSE_FADE_FRAMES;
        if (!SDL_ClearAudioStream(s_streams[i])) {
            failure = 1;
            break;
        }
        if (!frames) continue;
        sr_audio_pause_scale_prefix(s_pause_stash[i].samples, s_pause_fade[i], fade_frames, 1);
        if (!SDL_PutAudioStreamData(s_streams[i], s_pause_fade[i], fade_frames * 4) ||
            (frames > fade_frames && !SDL_PutAudioStreamData(s_streams[i],
                s_pause_stash[i].samples + fade_frames * 2, (frames - fade_frames) * 4))) {
            failure = 1;
            SDL_ClearAudioStream(s_streams[i]);
            if (!SDL_PutAudioStreamData(s_streams[i], s_pause_stash[i].samples, frames * 4))
                fprintf(stderr, "AUDIO_PAUSE: could not restore channel %d after fade failure (%s)\n",
                        i, SDL_GetError());
            break;
        }
    }
    sr_audio_pause_unlock(locked);
    if (failure) {
        fprintf(stderr, "AUDIO_PAUSE: fade-in restore failed (%s)\n", SDL_GetError());
        fflush(stderr);
    }
    sr_audio_pause_free_stashes();
    s_pause_active = 0;
}

int sr_audio_queued(int ch) {
    if (ch < 0 || ch >= SR_AUDIO_CHANNELS) {
        return -1;
    }
    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) {
        sr_audio_init();
    }
    if (s_audio_state != AUDIO_STATE_ACTIVE) {
        return -1;
    }

    int bytes = SDL_GetAudioStreamQueued(s_streams[ch]);
    if (bytes < 0) {
        return -1;
    }
    const int frames = bytes / 4;
    sr_audio_shadow_sync(ch, (uint32_t)frames);
    return sr_audio_takeable_frames(ch, frames);
}

/* The frames the host output device takes from a channel in one pull, in mixer frames: 441 from
 * WASAPI and 1024 from DirectSound on the reference PC. It is the larger of what the pulls were
 * measured to take (above) and SDL's chunk size for the device, which stands alone until the
 * first pull and is asked for again once a second of audio has been pulled. A default device
 * that changes mid-run brings its own figures and the measurement starts over. 0 when there is
 * no host output or nothing is known. The
 * blocking output keeps whole pulls of this size queued (hle_audio.c). */
int sr_audio_host_pull_frames(void) {
    if (s_audio_state != AUDIO_STATE_ACTIVE) {
        return 0;
    }
    if (SDL_CompareAndSwapAtomicInt(&s_pull_chunk_stale, 1, 0) || s_pull_unmeasured) {
        SDL_AudioSpec spec;
        int device_frames = 0;
        if (!SDL_GetAudioDeviceFormat(s_device_id, &spec, &device_frames)) {
            device_frames = 0;
            spec.freq = 0;
        }
        if (device_frames != s_pull_chunk_device_frames || spec.freq != s_pull_chunk_device_freq) {
            const int chunk = (int)sr_audio_pull_in_mixer_frames(device_frames, spec.freq);
            if (s_pull_chunk_device_frames > 0) {
                /* Another device, or the same one reconfigured: what was measured on the old one
                 * no longer holds. The meter starts over at its next pull. */
                fprintf(stderr, "audio: the host device now takes %d frames at %d Hz per pull (%d mixer frames, was %d)\n",
                        device_frames, spec.freq, chunk, s_pull_chunk_frames);
                SDL_SetAtomicInt(&s_pull_meter_restart, 1);
                SDL_SetAtomicInt(&s_pull_measured_frames, 0);
            }
            s_pull_chunk_frames = chunk;
            s_pull_chunk_device_frames = device_frames;
            s_pull_chunk_device_freq = spec.freq;
        }
    }
    const int measured = SDL_GetAtomicInt(&s_pull_measured_frames);
    return measured > s_pull_chunk_frames ? measured : s_pull_chunk_frames;
}

/* SR_AUDIOSTAT: the value the blocking output paces against, and the drift it is accumulating.
 * `worst` is the largest per-channel sr_audio_queued() in this backend -- the lead the slowest
 * channel carries -- and `lead_ms` is every channel's un-consumed audio in milliseconds.
 * The arithmetic is separated from the device query so it can be checked exactly, with no
 * device and no race against the driver draining the queue as it is read. */
static void audio_lead_from(const int *queued_frames, int channels, int *worst, long *lead_ms) {
    uint64_t lead = 0;
    int q = 0;
    for (int i = 0; i < channels; i++) {
        int frames = queued_frames[i] > 0 ? queued_frames[i] : 0;
        if (frames > q) q = frames;
        lead += (uint64_t)frames;
    }
    *worst = q;
    *lead_ms = (long)(lead / (SR_AUDIO_SAMPLE_RATE / 1000u));
}

static void audio_lead(int *worst, long *lead_ms) {
    /* Without a device there is no queue to be ahead of, so both stay -1 ("not measured")
     * rather than a zero that would read as "no drift". */
    *worst = -1;
    *lead_ms = -1;
    if (s_audio_state != AUDIO_STATE_ACTIVE) return;
    int queued[SR_AUDIO_CHANNELS];
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        int bytes = SDL_GetAudioStreamQueued(s_streams[i]);
        queued[i] = bytes > 0 ? sr_audio_takeable_frames(i, bytes / 4) : 0;
    }
    audio_lead_from(queued, SR_AUDIO_CHANNELS, worst, lead_ms);
}

void sr_audio_dump_stats(void) {
    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) {
        sr_audio_init();
    }
    uint32_t peak = 0;
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        if (s_stats.peak_queued_frames[i] > peak) {
            peak = s_stats.peak_queued_frames[i];
        }
    }
    const char *driver = (s_audio_state == AUDIO_STATE_ACTIVE) ? SDL_GetCurrentAudioDriver() : NULL;
    const char *state_str = (s_audio_state == AUDIO_STATE_ACTIVE) ? "active" :
                            (s_audio_state == AUDIO_STATE_NULL) ? "null" : "unavailable";
    /* SR_AUDIOSTAT: the value the blocking output paces against, and the drift it is
     * accumulating (see audio_lead). */
    long lead_ms = -1;
    int worst = -1;
    if (audio_stat_on()) audio_lead(&worst, &lead_ms);
    fprintf(stderr, "AUDIOSTAT_HOST: state=%s driver=%s pushed=%llu underruns=%llu overruns=%llu "
                    "backpressure=%llu pause_enters=%llu pause_held_frames=%llu pause_stash_failures=%llu "
                    "peak_q=%u queued=%d lead_ms=%ld\n",
            state_str,
            driver ? driver : "none",
            (unsigned long long)s_stats.total_pushed_frames,
            (unsigned long long)s_stats.underrun_events,
            (unsigned long long)s_stats.overrun_events,
            (unsigned long long)s_stats.backpressure_events,
            (unsigned long long)s_stats.pause_enters,
            (unsigned long long)s_stats.pause_held_frames,
            (unsigned long long)s_stats.pause_stash_failures,
            (unsigned)peak, worst, lead_ms);
    if (s_stats.overrun_events)   /* a separate line: tools/soak_audit.py parses the one above */
        fprintf(stderr, "AUDIOSTAT_HOST_DROPPED: pushes=%llu frames=%llu\n",
                (unsigned long long)s_stats.overrun_events,
                (unsigned long long)s_stats.overrun_frames);
    fflush(stderr);
    /* The dump is closed after the stats line, so the WAV header is finalised. The hle.c _Exit path
     * reaches this only with SR_AUDIOSTAT set and always calls sr_audio_dump_finish, so whichever
     * runs second is a no-op. A no-op without SR_AUDIODUMP. */
    sr_audio_dump_stop(s_device_id);
}

int sr_audio_is_active(void) {
    return s_audio_state == AUDIO_STATE_ACTIVE;
}

int sr_audio_pause_script_ready(void) {
    if (s_audio_state == AUDIO_STATE_UNINITIALIZED) return 0;
    if (s_audio_state != AUDIO_STATE_ACTIVE) return 1;
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        const int queued = SDL_GetAudioStreamQueued(s_streams[i]);
        if (queued > 0 && sr_audio_takeable_frames(i, queued / 4) > 0) return 1;
    }
    return 0;
}

#ifdef SR_AUDIO_SELFTEST
#include <assert.h>
#include <math.h>

static void set_test_env(const char *key, const char *value);

static void test_reset(void) {
    sr_audio_cleanup();
    s_audio_state = AUDIO_STATE_UNINITIALIZED;
    s_reported_unavailable = 0;
    s_reported_init = 0;
    s_pause_failure_reported = 0;
    s_pause_device_fallback = 0;
    s_pause_device_was_paused = 0;
    s_pause_active = 0;
    s_master_volume_percent = 100;
    s_test_fail_pause_stash_alloc = 0;
    s_test_pause_failure_messages = 0;
    s_test_device_freq = 0;
    s_test_device_open_attempts = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_push_marks, 0, sizeof(s_push_marks));
    s_tpl_on = 0;
    sr_audio_tpl_reset();
    set_test_env("SR_MASTER_VOLUME", "");
}

static void test_dummy_mixer_handoff(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    int rc = sr_audio_init();
    assert(rc == 0);
    assert(s_audio_state == AUDIO_STATE_ACTIVE);

    /* The dummy driver drains bound streams in real time on its own thread, so exact
     * queue depths are only observable while the device is paused; otherwise a loaded
     * host can consume frames between a push and the assertion that counts them. */
    assert(SDL_PauseAudioDevice(s_device_id));

    /* Initial state: 0 frames queued on all channels */
    for (int i = 0; i < SR_AUDIO_CHANNELS; i++) {
        assert(sr_audio_queued(i) == 0);
    }

    /* Generate 512 frames of 440Hz test sine tone */
    int16_t tone[512 * 2];
    for (int i = 0; i < 512; i++) {
        int16_t sample = (int16_t)(sin(2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * 16000.0);
        tone[i * 2]     = sample;
        tone[i * 2 + 1] = sample;
    }

    /* Push to channel 0 */
    sr_audio_push(0, tone, 512, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == 512);
    assert(sr_audio_queued(1) == 0); /* Channel isolation */
    assert(sr_audio_queued(8) == 0);

    /* Push to channel 8 (AudioOutput2) with panned volume */
    sr_audio_push(8, tone, 256, 0x4000, 0x8000);
    assert(sr_audio_queued(8) == 256);
    assert(sr_audio_queued(0) == 512);

    assert(SDL_ResumeAudioDevice(s_device_id));

    /* The dummy driver consumes queued data in real time. Poll against a
     * generous deadline rather than a fixed sleep so a loaded CI host
     * cannot turn scheduling jitter into a failure. */
    Uint64 deadline = SDL_GetTicks() + 5000;
    while ((sr_audio_queued(0) != 0 || sr_audio_queued(8) != 0) &&
           SDL_GetTicks() < deadline) {
        SDL_Delay(5);
    }
    assert(sr_audio_queued(0) == 0);
    assert(sr_audio_queued(8) == 0);

    /* Second push after drain triggers underrun tracking */
    sr_audio_push(0, tone, 128, 0x8000, 0x8000);
    assert(s_stats.underrun_events >= 1);

    /* SR_AUDIOSTAT drift telemetry: the published worst-channel lead is the value the
     * blocking output paces against, and the drift is every channel's un-consumed audio. */
    int worst = -2;
    long lead_ms = -2;
    audio_lead_from((const int[SR_AUDIO_CHANNELS]){0}, SR_AUDIO_CHANNELS, &worst, &lead_ms);
    assert(worst == 0);
    assert(lead_ms == 0);

    const int led[] = { 4410, 0, 0, 2205, 0, 0, 0, 0, 8820 };
    audio_lead_from(led, SR_AUDIO_CHANNELS, &worst, &lead_ms);
    assert(worst == 8820);
    assert(lead_ms == 350);
    assert(worst == led[8]);

    /* A negative reading from the device is not a negative lead. */
    const int odd[] = { -1, -100, 0 };
    audio_lead_from(odd, 3, &worst, &lead_ms);
    assert(worst == 0);
    assert(lead_ms == 0);

    /* With the device draining in real time the exact value races, but the published number
     * is still one of this backend's own sr_audio_queued() readings and never exceeds it. */
    audio_lead(&worst, &lead_ms);
    assert(worst >= 0);
    assert(worst >= sr_audio_queued(0));

    sr_audio_dump_stats();
    printf("test_dummy_mixer_handoff: PASS\n");
}

/* #2163: a run faster than real time fills the mixer to its cap and the next pushes are dropped.
 * The drop is host playback falling behind, so it must be visible with counts (and the guest, which
 * paces against its own model under SR_DETERMINISTIC_CLOCK=1, never feels it: hle_thread_selftest).
 * The device is paused so the dummy driver cannot drain the queue between the pushes.
 *
 * MUTANT: delete the overrun_frames accounting in sr_audio_push -- the frame count stays 0. */
static void test_full_mixer_drops_are_counted(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));
    static int16_t block[4096 * 2];   /* silence is enough: only the queue depth matters */
    for (int i = 0; i < SR_AUDIO_MAX_QUEUED_FRAMES / 4096; i++)
        sr_audio_push(0, block, 4096, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == SR_AUDIO_MAX_QUEUED_FRAMES);
    assert(s_stats.overrun_events == 0 && s_stats.overrun_frames == 0);
    sr_audio_push(0, block, 4096, 0x8000, 0x8000);   /* the mixer is full: dropped, and counted */
    sr_audio_push(0, block, 1000, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == SR_AUDIO_MAX_QUEUED_FRAMES);
    assert(s_stats.overrun_events == 2);
    assert(s_stats.overrun_frames == 4096u + 1000u);
    assert(s_stats.total_pushed_frames == (uint64_t)SR_AUDIO_MAX_QUEUED_FRAMES);
    /* A channel that was playing and ran dry is an underrun: counted and logged. */
    SDL_ClearAudioStream(s_streams[0]);
    assert(sr_audio_queued(0) == 0);
    sr_audio_push(0, block, 64, 0x8000, 0x8000);
    assert(s_stats.underrun_events == 1);
    sr_audio_dump_stats();
    printf("test_full_mixer_drops_are_counted: PASS\n");
}

static void test_no_device_path(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "nonexistent_driver_xyz", SDL_HINT_OVERRIDE);

    int rc = sr_audio_init();
    assert(rc == -1);
    assert(s_audio_state == AUDIO_STATE_UNAVAILABLE);
    assert(sr_audio_queued(0) == -1);

    /* No device means no queue, so drift is not measurable: -1, never 0. */
    int worst = 0;
    long lead_ms = 0;
    audio_lead(&worst, &lead_ms);
    assert(worst == -1);
    assert(lead_ms == -1);

    /* Push must fail-closed safely without crash */
    int16_t dummy[128 * 2] = {0};
    sr_audio_push(0, dummy, 128, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == -1);
    sr_audio_pause_enter();
    sr_audio_pause_leave();
    assert(!s_pause_active);

    sr_audio_dump_stats();
    printf("test_no_device_path: PASS\n");
}

static void test_null_backend_env(void) {
    test_reset();
#if defined(_WIN32) || defined(_WIN64)
    _putenv("SR_AUDIO_BACKEND=null");
#else
    setenv("SR_AUDIO_BACKEND", "null", 1);
#endif

    int rc = sr_audio_init();
    assert(rc == -1);
    assert(s_audio_state == AUDIO_STATE_NULL);
    assert(sr_audio_queued(0) == -1);
    sr_audio_pause_enter();
    sr_audio_pause_leave();
    assert(!s_pause_active);

#if defined(_WIN32) || defined(_WIN64)
    _putenv("SR_AUDIO_BACKEND=");
#else
    unsetenv("SR_AUDIO_BACKEND");
#endif
    printf("test_null_backend_env: PASS\n");
}

/* #2361: the back-end behaviour the driver's start-up open relies on. It does not run the
 * driver (tools/test_native_driver_hardening.py checks where the driver calls sr_audio_init),
 * and it passes on the code before #2361 too: it pins what was already true. The seam counts
 * the times sr_audio_init goes past its state checks to SDL_InitSubSystem. After a successful
 * init a second init, a push, sr_audio_queued and sr_audio_played_dry open nothing; with no
 * init the first push opens the device once (a run without the scheduler); after a refused
 * open the state stays UNAVAILABLE, reported once, and a push does not try again; under a
 * null back end the state is NULL and nothing reaches the open.
 *
 * MUTANT: sr_audio_init does not return early when ACTIVE -- the second init opens again and
 *   the attempts read 2.
 * MUTANT: sr_audio_init does not return early when UNAVAILABLE -- the second init after the
 *   refused open (a LoadExec restart's, were the driver's call inside its loop) tries again
 *   and the attempts read 2. A push cannot show it: push, queued and played_dry call
 *   sr_audio_init only while the state is UNINITIALIZED. */
static void test_start_up_open_is_the_only_open(void) {
    static const int16_t block[64 * 2];
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);   /* the driver's call */
    assert(s_test_device_open_attempts == 1);
    (void)sr_audio_played_dry(0);
    assert(sr_audio_queued(0) == 0);
    sr_audio_push(0, block, 64, 0x8000, 0x8000);
    sr_audio_push(8, block, 64, 0x8000, 0x8000);
    assert(s_stats.total_pushed_frames == 128u);
    assert(sr_audio_init() == 0);
    assert(s_test_device_open_attempts == 1);

    /* No start-up open: the first push opens the device, and only that one. */
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    sr_audio_push(0, block, 64, 0x8000, 0x8000);
    assert(s_audio_state == AUDIO_STATE_ACTIVE && s_test_device_open_attempts == 1);
    sr_audio_push(0, block, 64, 0x8000, 0x8000);
    assert(s_test_device_open_attempts == 1 && s_stats.total_pushed_frames == 128u);

    /* No device at start-up: reported once, and the run goes on with no output. */
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "nonexistent_driver_xyz", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == -1 && s_audio_state == AUDIO_STATE_UNAVAILABLE);
    assert(s_reported_unavailable == 1 && s_test_device_open_attempts == 1);
    sr_audio_push(0, block, 64, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == -1 && sr_audio_played_dry(0) == 0);
    assert(s_audio_state == AUDIO_STATE_UNAVAILABLE && s_reported_unavailable == 1);
    assert(s_test_device_open_attempts == 1 && s_stats.total_pushed_frames == 0u);
    assert(sr_audio_init() == -1 && s_test_device_open_attempts == 1);   /* a second init does not retry */

    /* A null back end: nothing is opened, at start-up or after. */
    test_reset();
    set_test_env("SR_AUDIO_BACKEND", "null");
    assert(sr_audio_init() == -1 && s_audio_state == AUDIO_STATE_NULL);
    sr_audio_push(0, block, 64, 0x8000, 0x8000);
    assert(s_audio_state == AUDIO_STATE_NULL && s_test_device_open_attempts == 0);
    set_test_env("SR_AUDIO_BACKEND", "");
    test_reset();
    printf("test_start_up_open_is_the_only_open: PASS\n");
}

/* The device block the blocking output sizes its queue against: converted to mixer frames
 * rounding up, read from the open device, and 0 (nothing to hold for) with no host output.
 *
 * MUTANT: sr_audio_pull_in_mixer_frames rounds down -- a 1024-frame block at 48000 Hz reads 940,
 *   one frame short of what the pull takes.
 * MUTANT: sr_audio_host_pull_frames returns the mixer's own period or a constant -- the dummy
 *   device opened with 441 and then 1024 frames per pull reads the same both times. */
static void test_host_pull_frames(void) {
    assert(sr_audio_pull_in_mixer_frames(441, 44100) == 441u);
    assert(sr_audio_pull_in_mixer_frames(480, 48000) == 441u);
    assert(sr_audio_pull_in_mixer_frames(1024, 48000) == 941u);
    assert(sr_audio_pull_in_mixer_frames(1024, 44100) == 1024u);
    assert(sr_audio_pull_in_mixer_frames(0, 44100) == 0u);
    assert(sr_audio_pull_in_mixer_frames(-1, 44100) == 0u);
    assert(sr_audio_pull_in_mixer_frames(512, 0) == 0u);

    /* The pull meter, fed as the callbacks feed it: 441-frame pulls 10 ms apart. Channel 0 is idle
     * and marks the pulls; channel 2 plays. */
    const uint64_t ms = 1000000ull;
    const int full = 441 * 4;
    AudioPullMeter *const m = &s_pull_meter;
    test_reset();
    SDL_SetAtomicInt(&s_played_dry[2], 0);                       /* channel 2 has been pushed */
    audio_pull_report(0, 0, 0, full, 100 * ms);
    audio_pull_report(2, full, 2000 * 4, full, 100 * ms);
    assert(audio_pull_size(m) == 441u);
    assert(SDL_GetAtomicInt(&s_pull_measured_frames) == 441);    /* published by channel 0's report */
    audio_pull_report(0, 0, 0, full, 110 * ms);
    audio_pull_report(2, full, 1559 * 4, full, 110 * ms);
    assert(audio_pull_size(m) == 441u && m->burst_last == 0u);   /* steady pulls are not a burst */

    audio_pull_report(0, 0, 0, full, 112 * ms);                  /* 2 ms behind: a burst of two */
    audio_pull_report(2, full, 1118 * 4, full, 112 * ms);        /* which the channel fills */
    assert(m->burst_last == 882u);
    assert(audio_pull_size(m) == 441u);                          /* so the size stays */
    audio_pull_report(0, 0, 0, full, 117 * ms);                  /* 5 ms is half its play time: a new pull */
    assert(m->burst_last == 882u && m->burst_frames == 441u);

    audio_pull_report(0, 0, 0, full, 310 * ms);                  /* much later, a pull finds the playing */
    audio_pull_report(2, 100 * 4, 100 * 4, full, 310 * ms);      /* channel short, with no burst near */
    assert(audio_pull_size(m) == 441u);
    assert(SDL_GetAtomicInt(&s_played_dry[2]) == 0);             /* it gave what it had: not dry */

    audio_pull_report(0, 0, 0, full, 500 * ms);
    audio_pull_report(2, full, 741 * 4, full, 500 * ms);
    audio_pull_report(0, 0, 0, full, 502 * ms);                  /* a burst of two */
    audio_pull_report(2, 300 * 4, 300 * 4, full, 502 * ms);      /* that finds the channel short */
    assert(audio_pull_size(m) == 882u);                          /* it cost audio: the size is the burst */
    for (int i = 1; i <= 300; i++) audio_pull_report(0, 0, 0, full, (502 + 10 * (uint64_t)i) * ms);
    assert(audio_pull_size(m) == 882u);                          /* and stays, on this device */
    assert(SDL_GetAtomicInt(&s_pull_measured_frames) == 882);

    /* The channels report in the device's order: the short pull may be seen before the pull is
     * known to be part of a burst. */
    SDL_SetAtomicInt(&s_pull_meter_restart, 1);
    audio_pull_report(2, full, 900 * 4, full, 4000 * ms);
    audio_pull_report(0, 0, 0, full, 4000 * ms);
    assert(audio_pull_size(m) == 441u && SDL_GetAtomicInt(&s_pull_meter_restart) == 0);
    audio_pull_report(2, 459 * 4, 459 * 4, full, 4002 * ms);
    audio_pull_report(2, 18 * 4, 18 * 4, full, 4003 * ms);       /* short, before channel 0 reports */
    assert(audio_pull_size(m) == 441u);
    audio_pull_report(0, 0, 0, full, 4003 * ms);
    assert(audio_pull_size(m) == 882u);

    /* A channel that is not playing never counts: its empty pulls are not dropouts. A playing
     * channel's first empty pull is one, and leaves it dry. */
    SDL_SetAtomicInt(&s_pull_meter_restart, 1);
    audio_pull_report(0, 0, 0, full, 6000 * ms);
    audio_pull_report(5, 0, 0, full, 6000 * ms);
    audio_pull_report(0, 0, 0, full, 6002 * ms);
    audio_pull_report(5, 0, 0, full, 6002 * ms);                 /* channel 5 was never pushed */
    assert(audio_pull_size(m) == 441u);
    SDL_SetAtomicInt(&s_played_dry[4], 0);
    audio_pull_report(0, 0, 0, full, 6100 * ms);
    audio_pull_report(0, 0, 0, full, 6102 * ms);
    audio_pull_report(4, 0, 0, full, 6102 * ms);                 /* channel 4 was playing: now it has nothing */
    assert(audio_pull_size(m) == 882u);
    assert(SDL_GetAtomicInt(&s_played_dry[4]) == 1);
    audio_pull_report(0, 0, 0, full, 6200 * ms);
    audio_pull_report(0, 0, 0, full, 6201 * ms);
    audio_pull_report(0, 0, 0, full, 6202 * ms);                 /* a burst of three */
    audio_pull_report(4, 0, 0, full, 6202 * ms);                 /* channel 4 is dry already: no dropout */
    assert(audio_pull_size(m) == 882u);

    /* Frames a converter keeps back are learned from any channel, and a wider pull from a stream
     * that plays sizes the pulls channel 0 marks. */
    SDL_SetAtomicInt(&s_pull_meter_restart, 1);
    audio_pull_report(0, 0, 0, full, 8000 * ms);
    audio_pull_report(5, 0, 7 * 4, full, 8000 * ms);             /* channel 5 kept 7 frames back */
    assert(audio_pull_size(m) == 448u);
    audio_pull_report(5, 480 * 8, 900 * 4, 442 * 4, 8010 * ms);  /* a playing stream gave up 442 */
    assert(audio_pull_size(m) == 449u);
    SDL_SetAtomicInt(&s_played_dry[6], 0);
    audio_pull_report(6, 480 * 8, 448 * 4, 441 * 4, 8020 * ms);  /* 441 and the 7 kept back: filled */
    audio_pull_report(0, 0, 0, full, 8020 * ms);
    audio_pull_report(0, 0, 0, full, 8021 * ms);
    assert(m->burst_last == 884u && audio_pull_size(m) == 449u);
    audio_pull_report(6, 473 * 8, 441 * 4, 441 * 4, 8021 * ms);  /* 441 queued is 7 short of a pull */
    assert(audio_pull_size(m) == 891u);

    /* SDL is asked for its chunk size again once a second of audio has been pulled; reports that
     * are not pulls change nothing. */
    SDL_SetAtomicInt(&s_pull_meter_restart, 1);
    SDL_SetAtomicInt(&s_pull_chunk_stale, 0);
    for (int i = 0; i < 99; i++) audio_pull_report(0, 0, 0, full, (9000 + 10 * (uint64_t)i) * ms);
    assert(SDL_GetAtomicInt(&s_pull_chunk_stale) == 0);
    audio_pull_report(0, 0, 0, full, 9990 * ms);
    assert(SDL_GetAtomicInt(&s_pull_chunk_stale) == 1);
    audio_pull_report(0, 480 * 4, 480 * 4, 0, 10000 * ms);
    audio_pull_report(9, 0, 0, 480 * 4, 10000 * ms);
    audio_pull_report(-1, 0, 0, 480 * 4, 10000 * ms);
    assert(audio_pull_size(m) == 441u && SDL_GetAtomicInt(&s_pull_measured_frames) == 441);

    /* The dummy device: nothing is known before it opens; then SDL's chunk size stands until the
     * stream's own pulls are measured, and they agree with it (no conversion, no bursts). The
     * hint asks for two sizes; whatever SDL gives is what is compared. */
    static const char *const hints[] = { "441", "1024" };
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        test_reset();
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, hints[i], SDL_HINT_OVERRIDE);
        assert(sr_audio_host_pull_frames() == 0);   /* it does not open the device */
        assert(s_audio_state == AUDIO_STATE_UNINITIALIZED);
        assert(sr_audio_init() == 0);
        SDL_AudioSpec spec;
        int device_frames = 0;
        assert(SDL_GetAudioDeviceFormat(s_device_id, &spec, &device_frames) && device_frames > 0);
        const int chunk = (int)sr_audio_pull_in_mixer_frames(device_frames, spec.freq);
        assert(sr_audio_host_pull_frames() == chunk);
        const Uint64 deadline = SDL_GetTicks() + 5000;
        while (SDL_GetAtomicInt(&s_pull_measured_frames) == 0 && SDL_GetTicks() < deadline) SDL_Delay(2);
        assert(SDL_GetAtomicInt(&s_pull_measured_frames) == chunk);
        assert(sr_audio_host_pull_frames() == chunk);

        /* With no stream reporting, SDL's figure is read on every query, and one that differs from
         * the last (a changed device) starts the measurement over. */
        assert(SDL_PauseAudioDevice(s_device_id));   /* no pull takes the restart before it is read */
        s_pull_unmeasured = 1;
        s_pull_chunk_device_frames = device_frames + 64;
        assert(sr_audio_host_pull_frames() == chunk);
        assert(s_pull_chunk_device_frames == device_frames);
        assert(SDL_GetAtomicInt(&s_pull_meter_restart) == 1);
    }
    test_reset();
    assert(SDL_GetAtomicInt(&s_pull_measured_frames) == 0);   /* a closed device leaves nothing behind */
    SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);

    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "nonexistent_driver_xyz", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == -1);
    assert(sr_audio_host_pull_frames() == 0);
    assert(s_audio_state == AUDIO_STATE_UNAVAILABLE);
    test_reset();
    printf("test_host_pull_frames: PASS\n");
}

/* A channel is played dry when a pull comes while its own stream can give nothing, and only then.
 * SDL's shortfall estimate does not decide it, a stream that converts the rate is dry with the
 * frames it keeps back still queued, and the answer is each channel's own.
 *
 * On a device opened at 48 kHz every stream converts from 44.1 kHz. The case that failed before
 * is a channel other than 0 there: its stream ends with frames queued that no pull can take. */
static void test_played_dry_is_exact_and_per_channel(void) {
    const uint64_t ms = 1000000ull;
    test_reset();
    for (int ch = 0; ch < SR_AUDIO_CHANNELS; ch++) SDL_SetAtomicInt(&s_played_dry[ch], 0);
    audio_pull_report(3, 480 * 8, 448 * 4, 441 * 4, 10 * ms);   /* a pull the stream fills */
    assert(SDL_GetAtomicInt(&s_played_dry[3]) == 0);
    audio_pull_report(3, 186 * 8, 177 * 4, 441 * 4, 20 * ms);   /* a short one: it gives what it has */
    assert(SDL_GetAtomicInt(&s_played_dry[3]) == 0);
    audio_pull_report(3, 0, 7 * 4, 441 * 4, 30 * ms);           /* nothing to give, 7 frames kept back */
    assert(SDL_GetAtomicInt(&s_played_dry[3]) == 1);
    for (int ch = 0; ch < SR_AUDIO_CHANNELS; ch++)
        if (ch != 3) assert(SDL_GetAtomicInt(&s_played_dry[ch]) == 0);

    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    s_test_device_freq = 48000;
    assert(sr_audio_init() == 0);
    SDL_AudioSpec src, dst;
    assert(SDL_GetAudioStreamFormat(s_streams[3], &src, &dst));
    assert(src.freq == 44100 && dst.freq == 48000);              /* the streams convert */
    assert(sr_audio_played_dry(3) == 1 && sr_audio_played_dry(0) == 1);   /* never pushed */

    int16_t tone[600 * 2];
    for (int i = 0; i < 600 * 2; i++) tone[i] = (int16_t)(2000 + i);
    assert(SDL_PauseAudioDevice(s_device_id));
    sr_audio_push(3, tone, 600, 0x8000, 0x8000);
    assert(sr_audio_played_dry(3) == 0);
    assert(SDL_ResumeAudioDevice(s_device_id));
    const Uint64 deadline = SDL_GetTicks() + 5000;
    while (!sr_audio_played_dry(3) && SDL_GetTicks() < deadline) SDL_Delay(2);
    assert(sr_audio_played_dry(3) == 1);
    const int kept = SDL_GetAudioStreamQueued(s_streams[3]) / 4;
    assert(kept > 0 && kept < 64);                               /* dry, with frames still in the stream */
    assert(SDL_GetAudioStreamAvailable(s_streams[3]) == 0);
    assert(sr_audio_queued(3) == 0);                             /* which no pull can take: nothing is queued */

    /* The pull to queue against includes them, learned from channel 3: channel 0 was never used. */
    SDL_AudioSpec spec;
    int device_frames = 0;
    assert(SDL_GetAudioDeviceFormat(s_device_id, &spec, &device_frames) && spec.freq == 48000);
    const int chunk = (int)sr_audio_pull_in_mixer_frames(device_frames, spec.freq);
    const Uint64 deadline2 = SDL_GetTicks() + 5000;
    while (sr_audio_host_pull_frames() < chunk + kept && SDL_GetTicks() < deadline2) SDL_Delay(2);
    assert(sr_audio_host_pull_frames() >= chunk + kept);

    /* The hold's silence replaces the frames the converter kept back, which would otherwise play
     * as a fragment ahead of it, and the push that follows found nothing queued: the dropout is
     * counted. */
    assert(SDL_PauseAudioDevice(s_device_id));
    assert(s_stats.underrun_events == 0);
    sr_audio_push_silence(3, 672);
    assert(SDL_GetAudioStreamQueued(s_streams[3]) == 672 * 4);   /* the kept-back frames went first */
    sr_audio_push(3, tone, 600, 0x8000, 0x8000);
    assert(s_stats.underrun_events == 1);
    assert(sr_audio_played_dry(3) == 0);
    assert(sr_audio_queued(3) == 672 + 600);
    sr_audio_push(3, tone, 600, 0x8000, 0x8000);
    assert(s_stats.underrun_events == 1);
    s_test_device_freq = 0;
    test_reset();
    printf("test_played_dry_is_exact_and_per_channel: PASS\n");
}

/* sr_audio_push_silence queues the blocking output's hold ahead of a push: it is in the queue and
 * in the pause copy, it is not counted as guest audio, and the push behind it still reports the
 * empty queue it found. sr_audio_played_dry says when that hold is due: a channel never pushed,
 * or one the device has since taken a short pull from, and not one that still has frames. */
static void test_push_silence_is_not_a_push(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_played_dry(0) == 1);         /* opens the device, as a first push does */
    assert(s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));   /* exact depths need a device that does not drain */

    int16_t tone[256 * 2];
    for (int i = 0; i < 256 * 2; i++) tone[i] = (int16_t)(1000 + i);
    static int16_t held[1756 * 2];

    sr_audio_push_silence(0, 1500);              /* more than one 1024-frame piece */
    assert(sr_audio_queued(0) == 1500);
    assert(sr_audio_played_dry(0) == 0);
    assert(s_stats.total_pushed_frames == 0 && s_stats.channel_pushed_frames[0] == 0);
    sr_audio_push(0, tone, 256, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == 1756);
    assert(s_stats.channel_pushed_frames[0] == 256);
    assert(s_stats.underrun_events == 0);        /* a channel's first push is a start */
    assert(sr_audio_shadow_copy(0, held, 1756));
    for (int i = 0; i < 1500 * 2; i++) assert(held[i] == 0);
    assert(held[1500 * 2] == tone[0] && held[1755 * 2 + 1] == tone[255 * 2 + 1]);

    assert(SDL_ClearAudioStream(s_streams[0]));  /* as if the device had played it all */
    assert(sr_audio_queued(0) == 0);
    sr_audio_push_silence(0, 500);
    sr_audio_push(0, tone, 256, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == 756);
    assert(s_stats.underrun_events == 1);        /* the queue ran dry: counted, silence or not */
    sr_audio_push(0, tone, 256, 0x8000, 0x8000);
    assert(s_stats.underrun_events == 1);        /* only the push right behind the silence is told */

    sr_audio_push_silence(1, 0);
    sr_audio_push_silence(9, 64);
    sr_audio_push_silence(-1, 64);
    for (int ch = 1; ch < SR_AUDIO_CHANNELS; ch++) assert(sr_audio_queued(ch) == 0);
    sr_audio_push_silence(1, SR_AUDIO_MAX_QUEUED_FRAMES);   /* more than the mixer takes: refused whole */
    assert(sr_audio_queued(1) == 0);
    assert(sr_audio_played_dry(1) == 1 && sr_audio_played_dry(9) == 0 && sr_audio_played_dry(-1) == 0);

    /* A pushed channel is not dry until the device takes a pull it cannot fill. */
    sr_audio_push(2, tone, 64, 0x8000, 0x8000);
    assert(sr_audio_played_dry(2) == 0);
    assert(SDL_ClearAudioStream(s_streams[2]));  /* emptied behind the device's back: no pull saw it */
    assert(sr_audio_played_dry(2) == 0);
    sr_audio_push(2, tone, 64, 0x8000, 0x8000);
    assert(SDL_ResumeAudioDevice(s_device_id));
    const Uint64 deadline = SDL_GetTicks() + 5000;
    while (!sr_audio_played_dry(2) && SDL_GetTicks() < deadline) SDL_Delay(2);
    assert(sr_audio_played_dry(2) == 1);
    assert(sr_audio_queued(2) == 0);

    /* A queue that has frames is not dry whatever the device asked for while it was held back
     * (a pause puts the queue back after the device ran on without it). */
    assert(SDL_PauseAudioDevice(s_device_id));
    assert(SDL_PutAudioStreamData(s_streams[2], tone, 64 * 4));
    assert(SDL_GetAtomicInt(&s_played_dry[2]) == 1);
    assert(sr_audio_played_dry(2) == 0);

    test_reset();
    sr_audio_push_silence(0, 64);                /* no open device: nothing happens, nothing opens */
    assert(s_audio_state == AUDIO_STATE_UNINITIALIZED);
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "nonexistent_driver_xyz", SDL_HINT_OVERRIDE);
    assert(sr_audio_played_dry(0) == 0);         /* no host output: nothing to prime */
    test_reset();
    printf("test_push_silence_is_not_a_push: PASS\n");
}

/* Master-volume gain mapping (SR_MASTER_VOLUME): pure, window-free. */
static void test_master_volume_gain(void) {
    assert(sr_audio_master_gain(0) == 0.0f);
    assert(sr_audio_master_gain(-5) == 0.0f);
    assert(sr_audio_master_gain(100) == 1.0f);
    assert(sr_audio_master_gain(250) == 1.0f);
    assert(sr_audio_master_gain(50) == 0.5f);
    assert(sr_audio_master_gain(80) > 0.7999f && sr_audio_master_gain(80) < 0.8001f);
}

/* SR_PERF underrun classification (#1296): pure, window-free. A push that left 441 frames
 * (10 ms) at t = 1 s dates the queue's end at 1.010 s. */
static void test_underrun_classification(void) {
    const AudioPushMark mark = { 1000000000ull, 441u };
    const AudioPushMark none = { 0u, 0u };
    int underrun = -1;
    /* Audio still queued: no starvation, whatever the clock says. */
    assert(audio_starved_ns(&mark, 5000000000ull, 1, &underrun) == 0 && underrun == 0);
    /* Empty 20 ms after the queue ran out: a 20 ms underrun of a playing stream. */
    assert(audio_starved_ns(&mark, 1030000000ull, 0, &underrun) == 20000000ull && underrun == 1);
    /* Empty before the estimated end (the device pulled ahead): an underrun with no silence. */
    assert(audio_starved_ns(&mark, 1005000000ull, 0, &underrun) == 0 && underrun == 1);
    /* Empty a second or more after the end: a stream that stopped and restarted. */
    assert(audio_starved_ns(&mark, 2010000000ull, 0, &underrun) == 1000000000ull && underrun == 0);
    /* The first push a channel ever gets is not an underrun. */
    assert(audio_starved_ns(&none, 1030000000ull, 0, &underrun) == 0 && underrun == 0);
    printf("test_underrun_classification: PASS\n");
}

static void set_test_env(const char *key, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
    char buf[600];
    snprintf(buf, sizeof(buf), "%s=%s", key, value);
    _putenv(buf);
#else
    if (value && *value) setenv(key, value, 1);
    else unsetenv(key);
#endif
}

static uint32_t test_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t test_le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* SR_AUDIODUMP conversion: the 16-bit mapping, the clip count and NaN handling, pure. */
static void test_dump_conversion(void) {
    uint64_t clipped = 0;
    assert(sr_audio_dump_sample(0.0f, &clipped) == 0);
    assert(sr_audio_dump_sample(0.5f, &clipped) == 16384);
    assert(sr_audio_dump_sample(-0.5f, &clipped) == -16384);
    assert(sr_audio_dump_sample(0.25f / 32768.0f, &clipped) == 0);        /* rounds to nearest */
    assert(sr_audio_dump_sample(0.75f / 32768.0f, &clipped) == 1);
    assert(clipped == 0);
    assert(sr_audio_dump_sample(1.0f, &clipped) == 32767);                /* 32768 is out of range */
    assert(clipped == 1);
    assert(sr_audio_dump_sample(-1.0f, &clipped) == -32768);              /* exactly representable */
    assert(clipped == 1);
    assert(sr_audio_dump_sample(1.5f, &clipped) == 32767);
    assert(sr_audio_dump_sample(-2.0f, &clipped) == -32768);
    assert(clipped == 3);
    uint32_t nan_bits = 0x7fc00000u;
    float nan_value;
    memcpy(&nan_value, &nan_bits, sizeof(nan_value));
    assert(sr_audio_dump_sample(nan_value, &clipped) == 0);
    assert(clipped == 3);
    printf("test_dump_conversion: PASS\n");
}

/* SR_AUDIODUMP header: the canonical 44-byte PCM layout the Python audit reads. */
static void test_dump_wav_header(void) {
    uint8_t h[44];
    sr_audio_wav_header(h, 44100u, 2u, 1000u);
    assert(memcmp(h + 0, "RIFF", 4) == 0);
    assert(memcmp(h + 8, "WAVE", 4) == 0);
    assert(memcmp(h + 12, "fmt ", 4) == 0);
    assert(memcmp(h + 36, "data", 4) == 0);
    assert(test_le32(h + 4) == 1036u);       /* 36 + data */
    assert(test_le32(h + 16) == 16u);
    assert(test_le16(h + 20) == 1u);         /* PCM */
    assert(test_le16(h + 22) == 2u);
    assert(test_le32(h + 24) == 44100u);
    assert(test_le32(h + 28) == 176400u);    /* 44100 * 2 * 2 */
    assert(test_le16(h + 32) == 4u);
    assert(test_le16(h + 34) == 16u);
    assert(test_le32(h + 40) == 1000u);
    printf("test_dump_wav_header: PASS\n");
}

/* SR_AUDIODUMP is off by default: an unset variable writes nothing and opens no file. */
static void test_dump_off_by_default(void) {
    test_reset();
    set_test_env("SR_AUDIODUMP", "");
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0);
    assert(s_dump_file == NULL);
    test_reset();
    printf("test_dump_off_by_default: PASS\n");
}

/* With SR_AUDIODUMP set, the device mix is written as a valid WAV at the device rate, the
 * header matches the data, and a pushed tone appears in it. */
static void test_dump_writes_wav(void) {
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/sr_audio_dump_selftest.wav", dir);
    remove(path);
    set_test_env("SR_AUDIODUMP", path);
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0);
    assert(s_dump_file != NULL);

    int16_t tone[2048 * 2];
    for (int i = 0; i < 2048; i++) {
        int16_t sample = (int16_t)(sin(2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * 8000.0);
        tone[i * 2] = sample;
        tone[i * 2 + 1] = sample;
    }
    sr_audio_push(0, tone, 2048, 0x8000, 0x8000);

    Uint64 deadline = SDL_GetTicks() + 5000;
    while ((sr_audio_queued(0) != 0 || s_dump_stats.frames < 4096) && SDL_GetTicks() < deadline)
        SDL_Delay(5);
    test_reset();   /* sr_audio_cleanup stops the dump and patches the header */

    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t h[44];
    assert(size >= 44 && fread(h, 1, sizeof(h), f) == sizeof(h));
    assert(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 36, "data", 4) == 0);
    assert(test_le32(h + 4) == (uint32_t)size - 8u);
    assert(test_le16(h + 22) == 2u);
    assert(test_le32(h + 24) == 44100u);
    uint32_t data_bytes = test_le32(h + 40);
    assert(data_bytes == (uint32_t)size - 44u);
    assert(data_bytes >= 4096u * 4u);
    int nonzero = 0;
    int16_t s;
    while (fread(&s, sizeof(s), 1, f) == 1)
        if (s != 0) nonzero++;
    fclose(f);
    assert(nonzero > 0);
    remove(path);
    set_test_env("SR_AUDIODUMP", "");
    printf("test_dump_writes_wav: PASS\n");
}

/* The _Exit path closes the dump through sr_audio_dump_finish and never calls the stats gate. The
 * finaliser does not read SR_AUDIOSTAT, so this holds whatever the host environment sets. The
 * close must finalise the header to every byte written, and a second close must change nothing. */
static void test_dump_finish_without_stats(void) {
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/sr_audio_dump_finish_selftest.wav", dir);
    remove(path);
    set_test_env("SR_AUDIODUMP", path);
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0);
    assert(s_dump_file != NULL);

    int16_t tone[2048 * 2];
    for (int i = 0; i < 2048; i++) {
        int16_t sample = (int16_t)(sin(2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * 8000.0);
        tone[i * 2] = sample;
        tone[i * 2 + 1] = sample;
    }
    sr_audio_push(0, tone, 2048, 0x8000, 0x8000);

    Uint64 deadline = SDL_GetTicks() + 5000;
    while ((sr_audio_queued(0) != 0 || s_dump_stats.frames < 4096) && SDL_GetTicks() < deadline)
        SDL_Delay(5);
    assert(s_dump_stats.frames >= 4096);

    /* The postmix hook is removed inside the close, so the byte count is stable after it. The
     * checks below hold whether or not a once-a-second header patch ran before the close. */
    sr_audio_dump_finish();
    assert(s_dump_file == NULL);
    uint64_t written = s_dump_data_bytes;
    assert(written >= 4096u * 4u);

    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t h[44];
    assert(size >= 44 && fread(h, 1, sizeof(h), f) == sizeof(h));
    fclose(f);
    assert(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 36, "data", 4) == 0);
    assert((uint64_t)size == 44u + written);
    assert(test_le32(h + 4) == (uint32_t)size - 8u);
    assert(test_le32(h + 40) == (uint32_t)written);
    assert(test_le32(h + 40) == (uint32_t)size - 44u);

    sr_audio_dump_finish();   /* a second close is a no-op */
    uint8_t again[44];
    f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    assert(ftell(f) == size);
    fseek(f, 0, SEEK_SET);
    assert(fread(again, 1, sizeof(again), f) == sizeof(again));
    fclose(f);
    assert(memcmp(again, h, sizeof(h)) == 0);

    test_reset();
    remove(path);
    set_test_env("SR_AUDIODUMP", "");
    printf("test_dump_finish_without_stats: PASS\n");
}

/* A block the dump refuses is counted in dropped_frames and never written: past the size limit,
 * in a format the header does not describe, and after a failed write. The writer is driven
 * directly through the postmix callback SDL calls, with no device, so the counts are exact. */
static void test_dump_counts_refused_blocks(void) {
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/sr_audio_dump_drop_selftest.wav", dir);
    remove(path);
    test_reset();
    snprintf(s_dump_path, sizeof(s_dump_path), "%s", path);
    s_dump_file = fopen(path, "wb");
    assert(s_dump_file != NULL);
    s_dump_rate = 44100;
    s_dump_channels = 2;
    s_dump_data_bytes = s_dump_last_patch = 0;
    s_dump_write_failed = 0;
    memset(&s_dump_stats, 0, sizeof(s_dump_stats));
    sr_audio_dump_patch_header();

    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = 44100;
    float block[512 * 2];   /* 512 stereo frames: 2048 bytes once written as s16 */
    for (int i = 0; i < 512 * 2; i++) block[i] = 0.25f;
    const int blen = (int)sizeof(block);

    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_stats.frames == 512);
    assert(s_dump_stats.dropped_frames == 0);

    /* The data is at the limit, so the next block would pass it: each whole block is refused. */
    s_dump_data_bytes = SR_AUDIO_DUMP_MAX_DATA - 1024u;
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_stats.dropped_frames == 1024);
    assert(s_dump_data_bytes == SR_AUDIO_DUMP_MAX_DATA - 1024u);
    assert(s_dump_stats.frames == 512);

    /* A device that changed format is refused too: 1024 mono frames in this buffer. */
    spec.channels = 1;
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_stats.dropped_frames == 2048);
    spec.channels = 2;

    /* Reopen read-only so the next write fails for real: that block and every later one is
     * counted, with no per-block message. */
    fclose(s_dump_file);
    s_dump_file = fopen(path, "rb");
    assert(s_dump_file != NULL);
    s_dump_data_bytes = 2048u;
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_write_failed == 1);
    assert(s_dump_stats.dropped_frames == 2560);
    sr_audio_dump_postmix(NULL, &spec, block, blen);
    assert(s_dump_stats.dropped_frames == 3072);
    assert(s_dump_stats.frames == 512);

    sr_audio_dump_finish();
    assert(s_dump_file == NULL);
    assert(s_dump_stats.dropped_frames == 3072);
    remove(path);
    printf("test_dump_counts_refused_blocks: PASS\n");
}

static void test_pause_stream_readback(const int16_t *input, int frames, int16_t *output) {
    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = SR_AUDIO_SAMPLE_RATE;
    SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, &spec);
    assert(stream != NULL);
    assert(SDL_SetAudioStreamGain(stream, s_streams[0]
        ? SDL_GetAudioStreamGain(s_streams[0]) : 1.0f));
    assert(SDL_PutAudioStreamData(stream, input, frames * 4));
    assert(SDL_GetAudioStreamData(stream, output, frames * 4) == frames * 4);
    SDL_DestroyAudioStream(stream);
}

static void test_pause_queue_hold_and_fades(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));

    int16_t dc0[4096 * 2];
    int16_t dc8[300 * 2];
    for (int i = 0; i < 4096 * 2; i++) dc0[i] = 16000;
    for (int i = 0; i < 300 * 2; i++) dc8[i] = 16000;
    sr_audio_push(0, dc0, 4096, 0x8000, 0x8000);
    sr_audio_push(8, dc8, 300, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == 4096 && sr_audio_queued(8) == 300);

    sr_audio_pause_enter();
    assert(s_pause_active && !s_pause_device_fallback);
    const int held0 = sr_audio_queued(0);
    const int held8 = sr_audio_queued(8);
    if (held0 != SR_AUDIO_PAUSE_FADE_FRAMES || held8 != SR_AUDIO_PAUSE_FADE_FRAMES)
        fprintf(stderr, "AUDIO_PAUSE_TEST: held queue depths ch0=%d ch8=%d\n", held0, held8);
    assert(held0 == SR_AUDIO_PAUSE_FADE_FRAMES);
    assert(held8 == SR_AUDIO_PAUSE_FADE_FRAMES);
    assert(s_stats.pause_enters == 1 && s_stats.pause_held_frames == 4396);

    int16_t fade_out[SR_AUDIO_PAUSE_FADE_FRAMES * 2];
    test_pause_stream_readback(s_pause_fade[0], SR_AUDIO_PAUSE_FADE_FRAMES, fade_out);
    for (int i = 1; i < SR_AUDIO_PAUSE_FADE_FRAMES; i++) {
        assert(fade_out[i * 2] <= fade_out[(i - 1) * 2]);
        assert(fade_out[i * 2 + 1] <= fade_out[(i - 1) * 2 + 1]);
    }
    assert(fade_out[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2] <= 63);
    assert(fade_out[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2 + 1] <= 63);

    sr_audio_pause_leave();
    assert(!s_pause_active);
    assert(sr_audio_queued(0) == 4096 && sr_audio_queued(8) == 300);
    int16_t restored0[SR_AUDIO_PAUSE_FADE_FRAMES * 2];
    int16_t restored8[SR_AUDIO_PAUSE_FADE_FRAMES * 2];
    test_pause_stream_readback(s_pause_fade[0], SR_AUDIO_PAUSE_FADE_FRAMES, restored0);
    test_pause_stream_readback(s_pause_fade[8], SR_AUDIO_PAUSE_FADE_FRAMES, restored8);
    assert(restored0[0] <= 63 && restored0[1] <= 63);
    for (int i = 1; i < SR_AUDIO_PAUSE_FADE_FRAMES; i++) {
        assert(restored0[i * 2] >= restored0[(i - 1) * 2]);
        assert(restored0[i * 2 + 1] >= restored0[(i - 1) * 2 + 1]);
    }
    assert(restored0[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2] == 16000);
    assert(restored0[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2 + 1] == 16000);
    for (int i = SR_AUDIO_PAUSE_FADE_FRAMES; i < 4096; i++) {
        const uint32_t pos = (s_shadow_queues[0].head_frame + (uint32_t)i) % s_shadow_queues[0].capacity_frames;
        assert(s_shadow_queues[0].samples[pos * 2u] == 16000 &&
               s_shadow_queues[0].samples[pos * 2u + 1u] == 16000);
    }
    assert(restored8[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2] == 16000);
    assert(restored8[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2 + 1] == 16000);
    test_reset();
    printf("test_pause_queue_hold_and_fades: PASS\n");
}

static void test_pause_master_gain_applied_once(void) {
    test_reset();
    set_test_env("SR_MASTER_VOLUME", "50");
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));
    int16_t dc[4096 * 2];
    int16_t sample[2];
    for (int i = 0; i < 4096 * 2; i++) dc[i] = 16000;
    sr_audio_push(0, dc, 4096, 0x8000, 0x8000);
    assert(s_shadow_queues[0].samples[0] == 8000 && s_shadow_queues[0].samples[1] == 8000);
    /* Read a copy through the production stream's gain setting. SDL applies
     * stream gain during reads, so an accidental second master gain changes
     * these 8000 samples to 4000. */
    test_pause_stream_readback(s_shadow_queues[0].samples, 4096, (int16_t *)dc);
    sample[0] = dc[0]; sample[1] = dc[1];
    assert(sample[0] == 8000 && sample[1] == 8000);

    sr_audio_pause_enter();
    assert(s_pause_stash[0].samples[0] == 8000 && s_pause_stash[0].samples[1] == 8000);
    sr_audio_pause_leave();
    int16_t restored[SR_AUDIO_PAUSE_FADE_FRAMES * 2];
    test_pause_stream_readback(s_pause_fade[0], SR_AUDIO_PAUSE_FADE_FRAMES, restored);
    assert(restored[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2] == 8000);
    assert(restored[(SR_AUDIO_PAUSE_FADE_FRAMES - 1) * 2 + 1] == 8000);
    assert(s_shadow_queues[0].samples[0] == 8000 && s_shadow_queues[0].samples[1] == 8000);
    test_reset();
    printf("test_pause_master_gain_applied_once: PASS\n");
}

/* The Output2/SRC channel accepts volumes up to 0xFFFFF (hle_audio.c,
 * SCE_AUDIO_OUTPUT2_VOLUME_MAX). A full-scale sample at that volume amplifies past 16 bits
 * and is clamped. A 32-bit product overflows instead: 32767 * 0xFFFFF wraps to -1081343, so
 * the mixer submitted -33 where the clamp gives 32767, and -32768 * 0xFFFFF wraps to 32768,
 * giving 1 where the clamp gives -32768. */
static void test_volume_above_16_bits_is_clamped(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));
    int16_t block[64 * 2];
    for (int i = 0; i < 64; i++) {
        block[i * 2] = 32767;
        block[i * 2 + 1] = -32768;
    }
    block[2] = 100;
    block[3] = -1;
    sr_audio_push(0, block, 64, 0xFFFFF, 0xFFFFF);
    const int16_t *out = s_shadow_queues[0].samples;
    assert(out[0] == 32767 && out[1] == -32768);
    /* 100 * 0xFFFFF >> 15 and -1 * 0xFFFFF >> 15 (arithmetic shift). */
    assert(out[2] == 3199 && out[3] == -32);
    test_reset();
    printf("test_volume_above_16_bits_is_clamped: PASS\n");
}

/* ---- #2137: Ace Mode's peak=limit, the host side ------------------------------------------------ */

/* Frame i of what channel ch queued (the shadow copy of the SDL stream, device paused). */
static int16_t test_queued_sample(int ch, uint32_t frame, int c) {
    const AudioShadowQueue *q = &s_shadow_queues[ch];
    assert(q->valid && frame < q->frames);
    return q->samples[((q->head_frame + frame) % q->capacity_frames) * 2u + (uint32_t)c];
}

/* The true peak of a stereo int16 run, read at 16x through a 64-tap Blackman-windowed sinc a
 * phase: a far finer reading than the limiter's own 4x detector, so the bound below is the
 * limiter's and not this check's. */
static double test_true_peak(int ch, uint32_t from, uint32_t to) {
    const double pi = 3.14159265358979323846;
    double peak = 0.0;
    for (int c = 0; c < 2; c++)
        for (uint32_t i = from + 32u; i + 32u < to; i++)
            for (int k = 0; k < 16; k++) {
                const double f = k / 16.0;
                double y = 0.0;
                for (int n = -31; n <= 32; n++) {
                    const double u = f - n;
                    const double sinc = fabs(u) < 1e-12 ? 1.0 : sin(pi * u) / (pi * u);
                    const double w = 0.42 + 0.5 * cos(2.0 * pi * u / 64.0) + 0.08 * cos(4.0 * pi * u / 64.0);
                    y += sinc * w * test_queued_sample(ch, (uint32_t)((int)i + n), c);
                }
                if (fabs(y) > peak) peak = fabs(y);
            }
    return peak;
}

/* Off (every run but peak=limit), a wide push is the plain push of the guest's copy: the same
 * bytes reach the stream, the per-sample clamp included. This is the Authentic path. */
static void test_peak_limiter_off_is_the_plain_push(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));
    static int16_t lr[256 * 2];
    static int32_t wide[256 * 2];
    for (int i = 0; i < 256 * 2; i++) {
        wide[i] = (i & 1 ? -1 : 1) * (i * 300);
        lr[i] = (int16_t)(wide[i] > 32767 ? 32767 : wide[i] < -32768 ? -32768 : wide[i]);
    }
    sr_audio_push_wide(0, lr, wide, 256, 0x8000, 0x8000);
    sr_audio_push(1, lr, 256, 0x8000, 0x8000);
    sr_audio_push_wide(2, lr, wide, 256, 0x6000, 0xA000);
    sr_audio_push(3, lr, 256, 0x6000, 0xA000);
    assert(sr_audio_queued(0) == 256 && sr_audio_queued(2) == 256);
    for (uint32_t i = 0; i < 256u; i++)
        for (int c = 0; c < 2; c++) {
            assert(test_queued_sample(0, i, c) == lr[i * 2 + (uint32_t)c]);
            assert(test_queued_sample(0, i, c) == test_queued_sample(1, i, c));
            assert(test_queued_sample(2, i, c) == test_queued_sample(3, i, c));
        }
    uint64_t pushes = 1, frames = 1, limited = 1;
    double min_gain = 0.0;
    sr_audio_true_peak_stats(&pushes, &frames, &limited, &min_gain);
    assert(pushes == 0 && frames == 0 && limited == 0 && min_gain == 1.0);
    test_reset();
    printf("test_peak_limiter_off_is_the_plain_push: PASS\n");
}

/* On, audio under the ceiling with no peak near it comes out bit-exact, SR_TPL_DELAY frames late,
 * and a push of n frames still queues n frames (the guest's queue depth is unchanged).
 * The sines start at zero and peak at 30000 and 28000 counts, 0.47 dB and more under the ceiling.
 * MUTANT: SR_TPL_CEILING at -1 dBTP -- the sine is turned down and the bit-exact cell fails. */
static void test_peak_limiter_passes_quiet_audio_bit_exact(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));
    assert(sr_audio_set_true_peak_limiter(1) == 1);
    enum { N = 4000, BLOCK = 500 };
    static int16_t in[N * 2];
    for (int i = 0; i < N; i++) {
        in[i * 2] = (int16_t)lrint(30000.0 * sin(2.0 * 3.14159265358979323846 * 1000.0 * i / 44100.0));
        in[i * 2 + 1] = (int16_t)lrint(28000.0 * sin(2.0 * 3.14159265358979323846 * 1500.0 * i / 44100.0));
    }
    for (int b = 0; b < N; b += BLOCK) {
        sr_audio_push(0, in + b * 2, BLOCK, 0x8000, 0x8000);
        assert(sr_audio_queued(0) == b + BLOCK);
    }
    for (uint32_t i = 0; i < (uint32_t)N; i++)
        for (int c = 0; c < 2; c++) {
            const int16_t want = i < SR_TPL_DELAY ? 0 : in[(i - SR_TPL_DELAY) * 2u + (uint32_t)c];
            assert(test_queued_sample(0, i, c) == want);
        }
    uint64_t limited = 1;
    sr_audio_true_peak_stats(NULL, NULL, &limited, NULL);
    assert(limited == 0);
    test_reset();
    printf("test_peak_limiter_passes_quiet_audio_bit_exact: PASS (delay %d frames)\n", SR_TPL_DELAY);
}

/* The overflow fixture: a 997 Hz sine whose sum reaches 70000 counts (6.6 dB past 16 bits) for
 * 4000 frames between two passages at 8000 counts, pushed as the SAS mixer hands it over (the
 * guest's saturated copy beside the sum). The PSP's copy clips; the limiter's output must not:
 * no sample past the -0.3 dBTP ceiling, a true peak under full scale, the passage
 * before the overflow bit-exact, and the one after it, once the release is over, at the input's
 * level within 0.1 dB.
 * MUTANT: use the gain frame q needs (l->need) instead of the moving average -- the gain no longer
 *   falls before the overflow arrives and the attack cell fails.
 * The true-peak bound is full scale, not the ceiling: at the fixture's hard 62000-count steps
 * the 16x reading here shows more ringing than the limiter's 4x detector (16 taps a phase,
 * finer than BS.1770's 12) and lands 0.13 dB over the ceiling (measured), still under 0 dBTP. */
static void test_peak_limiter_rounds_off_an_overflow(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));
    assert(sr_audio_set_true_peak_limiter(1) == 1);
    enum { N = 30208, BLOCK = 256, OVER_FROM = 4000, OVER_TO = 8000 };
    static int32_t wide[N * 2];
    static int16_t lr[N * 2];
    uint32_t psp_clipped = 0;
    for (int i = 0; i < N; i++) {
        const double a = i >= OVER_FROM && i < OVER_TO ? 70000.0 : 8000.0;
        for (int c = 0; c < 2; c++) {
            const double ph = 2.0 * 3.14159265358979323846 * 997.0 * i / 44100.0 + c * 0.7;
            wide[i * 2 + c] = (int32_t)lrint(a * sin(ph));
            const int32_t w = wide[i * 2 + c];
            lr[i * 2 + c] = (int16_t)(w > 32767 ? 32767 : w < -32768 ? -32768 : w);
            if (lr[i * 2 + c] == 32767 || lr[i * 2 + c] == -32768) psp_clipped++;
        }
    }
    assert(psp_clipped > 1000);   /* the fixture overflows 16 bits, as the PSP's mix would */
    for (int b = 0; b < N; b += BLOCK)
        sr_audio_push_wide(0, lr + b * 2, wide + b * 2, BLOCK, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == N);

    int out_max = 0;
    for (uint32_t i = 0; i < (uint32_t)N; i++)
        for (int c = 0; c < 2; c++) {
            const int v = abs(test_queued_sample(0, i, c));
            if (v > out_max) out_max = v;
        }
    assert(out_max < 32767);
    assert(out_max <= (int)(SR_TPL_CEILING + 0.5));   /* the ceiling, to the nearest count */
    const double tp = test_true_peak(0, 0u, (uint32_t)N);
    const double tp_db = 20.0 * log10(tp / 32768.0);
    assert(tp_db < 0.0);

    /* Before the attack reaches it, the quiet passage is the input, SR_TPL_DELAY frames late. */
    for (uint32_t i = 0; i < SR_TPL_DELAY; i++)
        assert(test_queued_sample(0, i, 0) == 0 && test_queued_sample(0, i, 1) == 0);
    for (uint32_t i = 0; i < OVER_FROM - SR_TPL_LOOKAHEAD - SR_TPL_TAPS; i++)
        for (int c = 0; c < 2; c++)
            assert(test_queued_sample(0, i + SR_TPL_DELAY, c) == lr[i * 2u + (uint32_t)c]);
    /* The attack: frames 16 to 48 before the overflow, out of the interpolator's reach of it, are
     * already turned down, so the gain falls over the look-ahead instead of in one step. */
    uint32_t attacked = 0;
    for (uint32_t i = OVER_FROM - 48u; i < OVER_FROM - 16u; i++)
        for (int c = 0; c < 2; c++) {
            const int x = lr[i * 2u + (uint32_t)c], y = test_queued_sample(0, i + SR_TPL_DELAY, c);
            if (abs(x) > 1000 && abs(y) < abs(x) - 1) attacked++;
        }
    assert(attacked > 32u);
    /* A quarter second after the overflow (five release time constants), the level is back. */
    double sum_in = 0.0, sum_out = 0.0;
    for (uint32_t i = OVER_TO + 11025u; i + SR_TPL_DELAY < (uint32_t)N; i++)
        for (int c = 0; c < 2; c++) {
            const double x = wide[i * 2u + (uint32_t)c];
            const double y = test_queued_sample(0, i + SR_TPL_DELAY, c);
            sum_in += x * x;
            sum_out += y * y;
        }
    const double level_db = 10.0 * log10(sum_out / sum_in);
    assert(fabs(level_db) <= 0.1);

    uint64_t pushes = 0, frames = 0, limited = 0;
    double min_gain = 1.0;
    sr_audio_true_peak_stats(&pushes, &frames, &limited, &min_gain);
    assert(pushes == (uint64_t)(N / BLOCK) && frames > 1000u && limited > (uint64_t)(OVER_TO - OVER_FROM));
    assert(min_gain < 0.5);
    test_reset();
    printf("test_peak_limiter_rounds_off_an_overflow: PASS (PSP clipped %u samples; output peak %d, "
           "true peak %.2f dBTP, level after %.3f dB, lowest gain %.3f)\n",
           psp_clipped, out_max, tp_db, level_db, min_gain);
}

/* A hold of silence (sr_audio_push_silence) with the limiter on. On a channel still playing it
 * keeps the order: what played before, then the silence, then the new push, each SR_TPL_DELAY
 * frames late. Pushed past the limiter, the silence played first and the old stream's last frames
 * came after it. On a channel the device played dry (the hold's real case, hle_audio.c), the held
 * frames go, as the converter's kept-back frames do: after the gap they were a 1.6 ms fragment
 * between two silences.
 * MUTANT: put the zeros straight into the stream -- the order cell fails.
 * A push to a dry channel with no hold ahead of it (hle_audio.c holds only when the pull is long
 * against the block) drops them the same way.
 * MUTANT: drop the limiter reset on a dry channel in sr_audio_push_silence -- the dry cell fails.
 * A queue a pull emptied without coming up short is still playing: a push there keeps the frames.
 * MUTANT: drop it in sr_audio_push_frames -- the no-hold cell fails.
 * MUTANT: reset on any empty queue, played dry or not -- the still-playing cell fails. */
static void test_peak_limiter_keeps_a_hold_in_order(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    assert(SDL_PauseAudioDevice(s_device_id));
    assert(sr_audio_set_true_peak_limiter(1) == 1);
    static int16_t a[300 * 2], b[200 * 2];
    for (int i = 0; i < 300 * 2; i++) a[i] = 10000;
    for (int i = 0; i < 200 * 2; i++) b[i] = -5000;
    sr_audio_push(0, a, 300, 0x8000, 0x8000);
    sr_audio_push_silence(0, 500);
    sr_audio_push(0, b, 200, 0x8000, 0x8000);
    assert(sr_audio_queued(0) == 1000);
    for (uint32_t i = 0; i < 1000u; i++) {
        const uint32_t in = i < SR_TPL_DELAY ? 0xFFFFFFFFu : i - SR_TPL_DELAY;
        const int16_t want = in == 0xFFFFFFFFu ? 0 : in < 300u ? 10000 : in < 800u ? 0 : -5000;
        assert(test_queued_sample(0, i, 0) == want && test_queued_sample(0, i, 1) == want);
    }
    /* Dry: the device took everything queued (cleared here); the limiter still holds a's last
     * SR_TPL_DELAY frames. The hold is silence only, and b follows it. */
    sr_audio_push(1, a, 300, 0x8000, 0x8000);
    assert(SDL_ClearAudioStream(s_streams[1]));
    sr_audio_push_silence(1, 500);
    sr_audio_push(1, b, 200, 0x8000, 0x8000);
    assert(sr_audio_queued(1) == 700);
    for (uint32_t i = 0; i < 700u; i++) {
        const int16_t want = i < 500u + SR_TPL_DELAY ? 0 : -5000;
        assert(test_queued_sample(1, i, 0) == want && test_queued_sample(1, i, 1) == want);
    }
    /* Dry with no hold (a pull found nothing): b follows the limiter's delay at once, with none
     * of a's frames ahead. */
    sr_audio_push(2, a, 300, 0x8000, 0x8000);
    assert(SDL_ClearAudioStream(s_streams[2]));
    SDL_SetAtomicInt(&s_played_dry[2], 1);
    sr_audio_push(2, b, 200, 0x8000, 0x8000);
    assert(sr_audio_queued(2) == 200);
    for (uint32_t i = 0; i < 200u; i++) {
        const int16_t want = i < SR_TPL_DELAY ? 0 : -5000;
        assert(test_queued_sample(2, i, 0) == want && test_queued_sample(2, i, 1) == want);
    }
    /* Still playing: a pull took the last queued frame without coming up short. a's last frames
     * still play ahead of b, with no hole between them. */
    sr_audio_push(3, a, 300, 0x8000, 0x8000);
    assert(SDL_ClearAudioStream(s_streams[3]));
    assert(SDL_GetAtomicInt(&s_played_dry[3]) == 0);
    sr_audio_push(3, b, 200, 0x8000, 0x8000);
    assert(sr_audio_queued(3) == 200);
    for (uint32_t i = 0; i < 200u; i++) {
        const int16_t want = i < SR_TPL_DELAY ? 10000 : -5000;
        assert(test_queued_sample(3, i, 0) == want && test_queued_sample(3, i, 1) == want);
    }
    test_reset();
    printf("test_peak_limiter_keeps_a_hold_in_order: PASS\n");
}

static void test_pause_fade_has_no_dumped_click(void) {
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/sr_audio_pause_selftest.wav", dir);
    remove(path);

    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    int16_t *dc = (int16_t *)malloc(44100u * 2u * sizeof(int16_t));
    assert(dc != NULL);
    for (int i = 0; i < 44100 * 2; i++) dc[i] = 16000;
    sr_audio_push(0, dc, 44100, 0x8000, 0x8000);
    free(dc);
    SDL_Delay(100);  /* establish a steady DC output before starting the captured window */
    set_test_env("SR_AUDIODUMP", path);
    sr_audio_dump_start(s_device_id);
    sr_audio_pause_enter();
    SDL_Delay(200);
    sr_audio_pause_leave();
    SDL_Delay(200);  /* stop on the restored steady signal, before the queue drains */
    sr_audio_dump_stop(s_device_id);
    test_reset();
    set_test_env("SR_AUDIODUMP", "");

    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    uint8_t header[44];
    assert(fread(header, 1, sizeof(header), f) == sizeof(header));
    assert(memcmp(header, "RIFF", 4) == 0 && memcmp(header + 36, "data", 4) == 0);
    int16_t previous = 0, sample = 0;
    int have_previous = 0;
    unsigned samples = 0;
    unsigned max_step = 0;
    while (fread(&sample, sizeof(sample), 1, f) == 1) {
        if (have_previous) {
            unsigned step = (unsigned)abs((int)sample - (int)previous);
            if (step > max_step) max_step = step;
        }
        previous = sample;
        have_previous = 1;
        samples++;
    }
    fclose(f);
    remove(path);
    assert(samples > SR_AUDIO_PAUSE_FADE_FRAMES * 4u);
    assert(max_step <= 64u);
    printf("test_pause_fade_has_no_dumped_click: PASS (max_step=%u)\n", max_step);
}

static void test_pause_stash_failure_is_visible(void) {
    test_reset();
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    assert(sr_audio_init() == 0 && s_audio_state == AUDIO_STATE_ACTIVE);
    static int16_t dc[8192 * 2];
    for (int i = 0; i < 8192 * 2; i++) dc[i] = 8000;

    for (int failure = 1; failure <= 2; failure++) {
        assert(SDL_PauseAudioDevice(s_device_id));
        sr_audio_push(0, dc, 8192, 0x8000, 0x8000);
        assert(SDL_ResumeAudioDevice(s_device_id));
        s_test_fail_pause_stash_alloc = 1;
        sr_audio_pause_enter();
        assert(s_pause_active && s_pause_device_fallback);
        assert(SDL_AudioDevicePaused(s_device_id));
        assert(s_stats.pause_stash_failures == (uint64_t)failure);
        assert(s_test_pause_failure_messages == 1);
        /* The fallback holds the guest's queue where the pause found it: the device thread
         * consumes nothing while the guest is stopped, so the depth the guest reads after
         * resume is the depth it left (8192 frames last 186 ms; the hold lasts 60 ms). */
        const int held = sr_audio_queued(0);
        assert(held > 0);
        SDL_Delay(60);
        if (sr_audio_queued(0) != held)
            fprintf(stderr, "AUDIO_PAUSE_TEST: fallback queue moved %d -> %d\n", held, sr_audio_queued(0));
        assert(sr_audio_queued(0) == held);
        sr_audio_pause_leave();
        assert(!s_pause_active && !SDL_AudioDevicePaused(s_device_id));
    }
    test_reset();
    printf("test_pause_stash_failure_is_visible: PASS\n");
}

int main(int argc, char **argv) {
    sr_perf_init();
    (void)argc;
    (void)argv;
    printf("--- Running audio selftest ---\n");
    test_master_volume_gain();
    test_underrun_classification();
    test_dummy_mixer_handoff();
    test_pause_queue_hold_and_fades();
    test_pause_master_gain_applied_once();
    test_volume_above_16_bits_is_clamped();
    test_peak_limiter_off_is_the_plain_push();
    test_peak_limiter_passes_quiet_audio_bit_exact();
    test_peak_limiter_rounds_off_an_overflow();
    test_peak_limiter_keeps_a_hold_in_order();
    test_pause_fade_has_no_dumped_click();
    test_full_mixer_drops_are_counted();
    test_no_device_path();
    test_null_backend_env();
    test_start_up_open_is_the_only_open();
    test_host_pull_frames();
    test_played_dry_is_exact_and_per_channel();
    test_push_silence_is_not_a_push();
    test_pause_stash_failure_is_visible();
    test_dump_conversion();
    test_dump_wav_header();
    test_dump_off_by_default();
    test_dump_writes_wav();
    test_dump_finish_without_stats();
    test_dump_counts_refused_blocks();
    test_reset();
    printf("ALL AUDIO HOST TESTS PASSED\n");
    return 0;
}
#endif
