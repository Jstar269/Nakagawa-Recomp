/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* A game's startup clip on its page (SND0.AT3): synthetic RIFF files built here
   (no game data) decode through the player's ATRAC3 and ATRAC3+ path, damaged
   and hostile headers are refused by name, and the clip player reads nothing
   with no game's page open, off, away or at volume 0, stays silent with no
   error for a disc without a clip, fades out and lets go of the audio device
   when the page is left or a game starts, and joins a read still running
   before a game starts. Runs on SDL's dummy audio driver, with the module's
   test seams (PLAYER_UI_SND0_TEST_SEAMS). */

#ifndef PLAYER_UI_SND0_TEST_SEAMS
#define PLAYER_UI_SND0_TEST_SEAMS
#endif

#include "player_ui_at3.h"
#include "player_ui_snd0.h"
#include "player_ui_vorbis.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));      \
            return 0;                                                             \
        }                                                                         \
    } while (0)

/* The longest a state change may take here, on a slow runner. */
#define WAIT_LIMIT_MS 20000u

/* A synthetic one-channel ATRAC3+ frame whose coded spectrum is not empty, so it
   decodes to sound (SPECTRUM_UNIT_FRAME of tests/native/atrac3p_selftest.c). */
static const uint8_t AT3P_MONO_FRAME[] = {0x10, 0x5b, 0x7f, 0xcf, 0xf6, 0x7f, 0x67, 0xcb, 0x40};

/* A mono 96-byte ATRAC3 frame that decodes to silence: sound unit id 0x28, then
   zero coded bands (as tests/native/atrac3p_bridge_selftest.c). */
#define AT3_MONO_ALIGN 96u

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

/* RIFF WAVE with a "fmt " chunk of `fmt_bytes` (16, or 32 for ATRAC3's
   extension), a "LIST" chunk to skip, then "data" holding `frames` copies of
   `frame`. Returns the bytes written (0 when `cap` is too small). */
static size_t build_riff(uint8_t *out, size_t cap, uint32_t tag, uint32_t channels, uint32_t rate,
                         uint32_t align, uint32_t frames, const uint8_t *frame) {
    const uint32_t fmt_bytes = tag == 0x0270u ? 32u : 16u;
    const size_t data = (size_t)frames * align;
    const size_t total = 12u + 8u + fmt_bytes + 8u + 6u + 8u + data + (data & 1u);
    size_t off = 12u;
    if (total > cap) return 0u;
    memset(out, 0, total);
    memcpy(out, "RIFF", 4u);
    put32(out + 4, (uint32_t)(total - 8u));
    memcpy(out + 8, "WAVE", 4u);
    memcpy(out + off, "fmt ", 4u);
    put32(out + off + 4, fmt_bytes);
    put16(out + off + 8, tag);
    put16(out + off + 10, channels);
    put32(out + off + 12, rate);
    put32(out + off + 16, rate / 8u);
    put16(out + off + 20, align);
    if (tag == 0x0270u) {
        uint8_t *ext = out + off + 8 + 18;
        put16(out + off + 8 + 16, 14u);   /* cbSize */
        ext[0] = 1u;                       /* the decoder's extension: frame factor 1 */
        ext[10] = 1u;
    }
    off += 8u + fmt_bytes;
    memcpy(out + off, "LIST", 4u);         /* a chunk the reader skips */
    put32(out + off + 4, 6u);
    off += 8u + 6u;
    memcpy(out + off, "data", 4u);
    put32(out + off + 4, (uint32_t)data);
    off += 8u;
    for (uint32_t f = 0u; f < frames; ++f) memcpy(out + off + (size_t)f * align, frame, align);
    return total;
}

static size_t build_at3p(uint8_t *out, size_t cap, uint32_t frames) {
    return build_riff(out, cap, 0xFFFEu, 1u, 44100u, (uint32_t)sizeof(AT3P_MONO_FRAME), frames,
                      AT3P_MONO_FRAME);
}

static size_t build_at3_silent(uint8_t *out, size_t cap, uint32_t frames) {
    uint8_t frame[AT3_MONO_ALIGN];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xA0u;
    return build_riff(out, cap, 0x0270u, 1u, 44100u, AT3_MONO_ALIGN, frames, frame);
}

static bool pcm_has_sound(const PlayerUiPcm *pcm) {
    for (uint64_t i = 0u; i < pcm->frames * (uint64_t)pcm->channels; ++i) {
        if (pcm->samples[i] != 0) return true;
    }
    return false;
}

static bool stop_now(void *user) {
    (void)user;
    return true;
}

static int test_decode(void) {
    static uint8_t buf[4096];
    char detail[128];
    PlayerUiPcm pcm;
    PlayerUiPcm again;
    size_t n = build_at3p(buf, sizeof(buf), 8u);
    CHECK(n != 0u, "the ATRAC3+ fixture builds");
    CHECK(player_ui_at3_decode(buf, n, PLAYER_UI_SND0_MAX_PCM_BYTES, NULL, NULL, &pcm, detail,
                               sizeof(detail)) == PLAYER_UI_AT3_OK,
          "an ATRAC3+ SND0 decodes");
    CHECK(pcm.frames == 8u * 2048u && pcm.channels == 1 && pcm.sample_rate == 44100,
          "ATRAC3+: 2048 frames per frame, mono, 44100 Hz");
    CHECK(pcm_has_sound(&pcm), "the ATRAC3+ canary decodes to sound");
    CHECK(player_ui_at3_decode(buf, n, PLAYER_UI_SND0_MAX_PCM_BYTES, NULL, NULL, &again, detail,
                               sizeof(detail)) == PLAYER_UI_AT3_OK &&
              again.frames == pcm.frames &&
              memcmp(again.samples, pcm.samples, (size_t)pcm.frames * sizeof(int16_t)) == 0,
          "the decode is deterministic");
    player_ui_pcm_free(&again);
    player_ui_pcm_free(&pcm);

    n = build_at3_silent(buf, sizeof(buf), 4u);
    CHECK(n != 0u, "the ATRAC3 fixture builds");
    CHECK(player_ui_at3_decode(buf, n, PLAYER_UI_SND0_MAX_PCM_BYTES, NULL, NULL, &pcm, detail,
                               sizeof(detail)) == PLAYER_UI_AT3_OK,
          "an ATRAC3 SND0 decodes");
    CHECK(pcm.frames == 4u * 1024u && pcm.channels == 1 && !pcm_has_sound(&pcm),
          "ATRAC3: 1024 frames per frame, and the silent frame is silent");
    player_ui_pcm_free(&pcm);

    /* A data chunk that says it is longer than the file keeps its whole frames. */
    n = build_at3p(buf, sizeof(buf), 3u);
    CHECK(player_ui_at3_decode(buf, n - 4u, PLAYER_UI_SND0_MAX_PCM_BYTES, NULL, NULL, &pcm,
                               detail, sizeof(detail)) == PLAYER_UI_AT3_OK &&
              pcm.frames == 2u * 2048u,
          "a cut-short file keeps the whole frames it has");
    player_ui_pcm_free(&pcm);
    return 1;
}

static int expect_refused(const uint8_t *buf, size_t n, uint64_t max_pcm, PlayerUiAt3Result want,
                          const char *what) {
    char detail[128] = "";
    PlayerUiPcm pcm;
    PlayerUiAt3Result got = player_ui_at3_decode(buf, n, max_pcm, NULL, NULL, &pcm, detail,
                                                 sizeof(detail));
    if (got != want || pcm.samples != NULL || pcm.frames != 0u || detail[0] == '\0') {
        fprintf(stderr, "FAIL %s: got %s (%s)\n", what, player_ui_at3_result_name(got), detail);
        return 0;
    }
    return 1;
}

static int test_refusals(void) {
    static uint8_t buf[4096];
    static uint8_t bad[4096];
    PlayerUiPcm pcm;
    char detail[128];
    size_t n = build_at3p(buf, sizeof(buf), 4u);
    CHECK(n != 0u, "the fixture builds");
    /* fmt at 12, LIST at 36, data at 50; the data's frames at 58. */
    memcpy(bad, buf, n);
    memcpy(bad, "RIFX", 4u);
    if (!expect_refused(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_NOT_RIFF,
                        "another container")) return 0;
    if (!expect_refused(buf, 11u, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_NOT_RIFF,
                        "a file shorter than its header")) return 0;
    memcpy(bad, buf, n);
    put16(bad + 20, 0x0001u);
    if (!expect_refused(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_UNSUPPORTED,
                        "PCM in place of ATRAC")) return 0;
    memcpy(bad, buf, n);
    put16(bad + 22, 3u);
    if (!expect_refused(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_UNSUPPORTED,
                        "three channels")) return 0;
    memcpy(bad, buf, n);
    put32(bad + 24, 96000u);
    if (!expect_refused(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_UNSUPPORTED,
                        "a 96 kHz rate")) return 0;
    memcpy(bad, buf, n);
    put16(bad + 32, 0u);
    if (!expect_refused(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_UNSUPPORTED,
                        "a zero frame size")) return 0;
    memcpy(bad, buf, n);
    put32(bad + 16, 0xFFFFFFF0u);
    if (!expect_refused(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_DAMAGED,
                        "a fmt chunk that runs past the file")) return 0;
    memcpy(bad, buf, n);
    put32(bad + 40, 0xFFFFFFFFu);
    if (!expect_refused(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_DAMAGED,
                        "a chunk size that would wrap the cursor")) return 0;
    for (uint32_t riff = 0u; riff < 4u; ++riff) {
        /* A RIFF size of 0 to 3 ends the file inside its header; with a data
           chunk claiming far more than is there, nothing may be read past the
           buffer (the cursor must not wrap). */
        /* Whole frames follow past the file's end inside this buffer, so a
           parser that wrapped would decode them and report OK. */
        memcpy(bad, buf, n);
        for (size_t f = 0u; f < 64u; ++f) {
            memcpy(bad + 58u + f * sizeof(AT3P_MONO_FRAME), AT3P_MONO_FRAME, sizeof(AT3P_MONO_FRAME));
        }
        put32(bad + 4, riff);
        put32(bad + 54, 64u * (uint32_t)sizeof(AT3P_MONO_FRAME));
        if (!expect_refused(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_DAMAGED,
                            "a RIFF size that ends inside the header")) return 0;
    }
    memcpy(bad, buf, 12u);
    put32(bad + 4, 0u);
    if (!expect_refused(bad, 12u, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_DAMAGED,
                        "a 12-byte file with a RIFF size of 0")) return 0;
    put32(bad + 4, 4u);
    if (!expect_refused(bad, 12u, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_DAMAGED,
                        "a 12-byte file with no chunks")) return 0;
    memcpy(bad, buf, n);
    put32(bad + 54, 73728u);
    {
        PlayerUiAt3Result got = player_ui_at3_decode(bad, n, PLAYER_UI_SND0_MAX_PCM_BYTES, NULL,
                                                     NULL, &pcm, detail, sizeof(detail));
        CHECK(got == PLAYER_UI_AT3_OK && pcm.frames == 4u * 2048u,
              "a data chunk larger than the file keeps only the frames in the file");
        player_ui_pcm_free(&pcm);
    }
    if (!expect_refused(buf, 58u + 4u, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_DAMAGED,
                        "a data chunk without a whole frame")) return 0;
    if (!expect_refused(buf, 36u, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_DAMAGED,
                        "no data chunk")) return 0;
    if (!expect_refused(buf, n, 4u * 2048u * 2u - 1u, PLAYER_UI_AT3_TOO_LARGE,
                        "more samples than the limit")) return 0;
    {
        /* An ATRAC3 frame size off the decoder's table. */
        uint8_t at3[2048];
        size_t m = build_at3_silent(at3, sizeof(at3), 2u);
        put16(at3 + 32, 100u);
        if (!expect_refused(at3, m, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_UNSUPPORTED,
                            "an ATRAC3 frame size off the table")) return 0;
        m = build_at3_silent(at3, sizeof(at3), 2u);
        put16(at3 + 12 + 8 + 16, 10u);
        if (!expect_refused(at3, m, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_UNSUPPORTED,
                            "an ATRAC3 extension that is not 14 bytes")) return 0;
        m = build_at3_silent(at3, sizeof(at3), 2u);
        memset(at3 + m - AT3_MONO_ALIGN, 0, AT3_MONO_ALIGN);   /* unit id 0: invalid */
        if (!expect_refused(at3, m, PLAYER_UI_SND0_MAX_PCM_BYTES, PLAYER_UI_AT3_DAMAGED,
                            "a frame that does not decode")) return 0;
    }
    CHECK(player_ui_at3_decode(buf, n, PLAYER_UI_SND0_MAX_PCM_BYTES, stop_now, NULL, &pcm, detail,
                               sizeof(detail)) == PLAYER_UI_AT3_CANCELLED &&
              pcm.samples == NULL,
          "a cancelled decode keeps nothing");
    return 1;
}

/* The discs the player's reader sees, by name. */
static uint8_t g_clip[8192];
static size_t g_clip_size;
static uint8_t g_silent[4096];
static size_t g_silent_size;
static SDL_AtomicInt g_reads_started;

static PlayerUiSnd0ReadResult test_reader(const char *iso_path, uint8_t **bytes, size_t *size,
                                          void *user) {
    const uint8_t *src = NULL;
    size_t n = 0u;
    (void)user;
    *bytes = NULL;
    *size = 0u;
    SDL_AddAtomicInt(&g_reads_started, 1);
    if (strcmp(iso_path, "clip.iso") == 0 || strcmp(iso_path, "slow.iso") == 0) {
        if (strcmp(iso_path, "slow.iso") == 0) SDL_Delay(300);
        src = g_clip;
        n = g_clip_size;
    } else if (strcmp(iso_path, "silent.iso") == 0) {
        src = g_silent;
        n = g_silent_size;
    } else if (strcmp(iso_path, "garbage.iso") == 0) {
        static const uint8_t garbage[] = "not a clip at all";
        src = garbage;
        n = sizeof(garbage);
    } else if (strcmp(iso_path, "huge.iso") == 0) {
        return PLAYER_UI_SND0_READ_FAILED;
    } else {
        return PLAYER_UI_SND0_READ_MISSING;
    }
    *bytes = (uint8_t *)malloc(n);
    if (*bytes == NULL) return PLAYER_UI_SND0_READ_FAILED;
    memcpy(*bytes, src, n);
    *size = n;
    return PLAYER_UI_SND0_READ_OK;
}

static PlayerUiSnd0Request request_for(const char *iso_path) {
    PlayerUiSnd0Request request;
    request.enabled = true;
    request.in_launcher = true;
    request.focused = true;
    request.volume = 80;
    request.iso_path = iso_path;
    return request;
}

/* Updates every few milliseconds until the state is `want`, or the limit. */
static bool wait_for(PlayerUiSnd0 *snd0, const PlayerUiSnd0Request *request,
                     PlayerUiSnd0State want) {
    uint64_t deadline = SDL_GetTicks() + WAIT_LIMIT_MS;
    while (SDL_GetTicks() < deadline) {
        player_ui_snd0_update(snd0, request, SDL_GetTicks());
        if (player_ui_snd0_state(snd0) == want) return true;
        SDL_Delay(5);
    }
    fprintf(stderr, "  waiting for %s, the clip is %s\n", player_ui_snd0_state_name(want),
            player_ui_snd0_state_name(player_ui_snd0_state(snd0)));
    return false;
}

static int test_player(void) {
    PlayerUiSnd0 *snd0 = player_ui_snd0_create(test_reader, NULL);
    PlayerUiSnd0Request request;
    uint64_t t0;
    CHECK(snd0 != NULL, "the clip player is made");
    CHECK(player_ui_snd0_create(NULL, NULL) == NULL, "a clip player needs a reader");

    /* Off: nothing is read and no device is held, whatever page is open. */
    request = request_for("clip.iso");
    request.enabled = false;
    t0 = SDL_GetTicks();
    player_ui_snd0_update(snd0, &request, t0);
    player_ui_snd0_update(snd0, &request, t0 + 5000u);
    CHECK(player_ui_snd0_state(snd0) == PLAYER_UI_SND0_OFF && player_ui_snd0_test_reads(snd0) == 0u &&
              !player_ui_snd0_holds_audio(snd0),
          "off reads nothing and holds no audio");

    /* No game's page open (the library list): nothing is read. */
    t0 = SDL_GetTicks();
    request = request_for(NULL);
    player_ui_snd0_update(snd0, &request, t0);
    player_ui_snd0_update(snd0, &request, t0 + 5000u);
    CHECK(player_ui_snd0_state(snd0) == PLAYER_UI_SND0_IDLE && player_ui_snd0_test_reads(snd0) == 0u &&
              player_ui_snd0_wait_ms(snd0, 1000u) == 1000u,
          "with no game's page open nothing is read and the launcher may sleep");

    /* At volume 0 or away from the window, a page reads nothing either. */
    request = request_for("clip.iso");
    request.volume = 0;
    player_ui_snd0_update(snd0, &request, t0 + 5000u);
    CHECK(player_ui_snd0_test_reads(snd0) == 0u && player_ui_snd0_state(snd0) == PLAYER_UI_SND0_IDLE,
          "Music volume 0 reads nothing");
    request.volume = 80;
    request.focused = false;
    player_ui_snd0_update(snd0, &request, t0 + 5000u);
    CHECK(player_ui_snd0_test_reads(snd0) == 0u, "an unfocused window reads nothing");

    /* The page opens: the clip is read once, decoded and heard. */
    request = request_for("clip.iso");
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_PLAYING), "the open page plays its clip");
    CHECK(player_ui_snd0_test_reads(snd0) == 1u && player_ui_snd0_holds_audio(snd0) &&
              player_ui_snd0_test_pcm_frames(snd0) == 16u * 2048u &&
              player_ui_snd0_quiets_music(snd0),
          "one read, a stream on the (dummy) device, and the launcher music quiet");
    {
        uint64_t deadline = SDL_GetTicks() + WAIT_LIMIT_MS;
        while (player_ui_snd0_gain(snd0) <= 0.0f && SDL_GetTicks() < deadline) {
            player_ui_snd0_update(snd0, &request, SDL_GetTicks());
            SDL_Delay(5);
        }
        CHECK(player_ui_snd0_gain(snd0) > 0.0f, "the device pulls the fade-in");
    }

    /* The window loses focus: the clip fades out and lets go, and is kept. */
    request.focused = false;
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_IDLE), "away, the clip fades out");
    CHECK(!player_ui_snd0_holds_audio(snd0) && player_ui_snd0_test_pcm_frames(snd0) != 0u,
          "away holds no audio and keeps the decoded clip");
    request.focused = true;
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_PLAYING) && player_ui_snd0_test_reads(snd0) == 1u,
          "back, it plays again without another read");

    /* Focus lost and back during the fade-out: the clip fades back in, keeps
       the launcher music quiet, and the launcher may sleep again. */
    {
        uint64_t deadline = SDL_GetTicks() + WAIT_LIMIT_MS;
        while (player_ui_snd0_gain(snd0) < 0.79f && SDL_GetTicks() < deadline) {
            player_ui_snd0_update(snd0, &request, SDL_GetTicks());
            SDL_Delay(5);
        }
        CHECK(player_ui_snd0_gain(snd0) >= 0.79f, "the clip is up to the Music volume");
    }
    request.focused = false;
    player_ui_snd0_update(snd0, &request, SDL_GetTicks());
    CHECK(player_ui_snd0_state(snd0) == PLAYER_UI_SND0_FADING_OUT &&
              !player_ui_snd0_quiets_music(snd0),
          "away, the fade-out starts");
    request.focused = true;
    player_ui_snd0_update(snd0, &request, SDL_GetTicks());
    CHECK(player_ui_snd0_state(snd0) == PLAYER_UI_SND0_PLAYING && player_ui_snd0_holds_audio(snd0) &&
              player_ui_snd0_quiets_music(snd0) && player_ui_snd0_wait_ms(snd0, 1000u) == 1000u,
          "back mid-fade, it plays on and the launcher music stays quiet");
    {
        uint64_t deadline = SDL_GetTicks() + WAIT_LIMIT_MS;
        while (player_ui_snd0_gain(snd0) < 0.79f && SDL_GetTicks() < deadline) {
            player_ui_snd0_update(snd0, &request, SDL_GetTicks());
            SDL_Delay(5);
        }
        CHECK(player_ui_snd0_gain(snd0) >= 0.79f && player_ui_snd0_state(snd0) == PLAYER_UI_SND0_PLAYING,
              "back mid-fade, it fades back up to the Music volume");
    }

    /* Another game's page, whose disc has no clip: a fade, then silence, no error. */
    request.iso_path = "missing.iso";
    player_ui_snd0_update(snd0, &request, SDL_GetTicks());
    CHECK(player_ui_snd0_state(snd0) == PLAYER_UI_SND0_FADING_OUT ||
              !player_ui_snd0_holds_audio(snd0),
          "another page fades the last clip out");
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_NO_CLIP), "a disc without SND0.AT3 is silent");
    CHECK(!player_ui_snd0_holds_audio(snd0) && player_ui_snd0_test_pcm_frames(snd0) == 0u &&
              player_ui_snd0_detail(snd0)[0] == '\0' && !player_ui_snd0_quiets_music(snd0),
          "no clip: no audio, nothing kept, no message, the launcher music back");

    /* Damaged, unreadable and silent clips are silent too, each with its reason. */
    request.iso_path = "garbage.iso";
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_DAMAGED), "a damaged clip is refused");
    CHECK(strstr(player_ui_snd0_detail(snd0), "not_riff") != NULL &&
              strstr(player_ui_snd0_detail(snd0), "garbage") == NULL &&
              !player_ui_snd0_holds_audio(snd0),
          "the reason names the state, never the path");
    request.iso_path = "huge.iso";
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_DAMAGED) &&
              strstr(player_ui_snd0_detail(snd0), "too large") != NULL,
          "an unreadable clip is refused");
    request.iso_path = "silent.iso";
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_DAMAGED) &&
              strstr(player_ui_snd0_detail(snd0), "silence") != NULL &&
              !player_ui_snd0_holds_audio(snd0),
          "a clip that decodes to silence opens no device");

    /* A game starts while a clip plays: it fades and lets go, and nothing is kept. */
    request.iso_path = "clip.iso";
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_PLAYING), "the clip plays again");
    request.in_launcher = false;
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_STOPPED), "a game start stops the clip");
    CHECK(!player_ui_snd0_holds_audio(snd0) && !player_ui_snd0_decoding(snd0) &&
              player_ui_snd0_test_pcm_frames(snd0) == 0u,
          "a running game: no audio, no worker, no clip");
    player_ui_snd0_update(snd0, &request, SDL_GetTicks() + 10000u);
    CHECK(player_ui_snd0_state(snd0) == PLAYER_UI_SND0_STOPPED && !player_ui_snd0_decoding(snd0),
          "nothing is read while a game runs");

    /* A game starts while a read runs: the worker is joined on that frame. */
    request = request_for("slow.iso");
    t0 = SDL_GetTicks();
    player_ui_snd0_update(snd0, &request, t0);
    player_ui_snd0_update(snd0, &request, t0);
    CHECK(player_ui_snd0_decoding(snd0) && player_ui_snd0_quiets_music(snd0),
          "the slow clip is being read");
    request.in_launcher = false;
    player_ui_snd0_update(snd0, &request, t0 + 1u);
    CHECK(!player_ui_snd0_decoding(snd0) && !player_ui_snd0_holds_audio(snd0) &&
              player_ui_snd0_state(snd0) == PLAYER_UI_SND0_STOPPED,
          "a game start joins the read before the game may start");

    /* Turned off while playing: a short fade, then off. */
    request = request_for("clip.iso");
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_PLAYING), "plays before turning off");
    request.enabled = false;
    CHECK(wait_for(snd0, &request, PLAYER_UI_SND0_OFF) && !player_ui_snd0_holds_audio(snd0),
          "turning the setting off lets go of the device");
    player_ui_snd0_destroy(snd0);
    player_ui_snd0_destroy(NULL);
    return 1;
}

int main(void) {
    (void)SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    g_clip_size = build_at3p(g_clip, sizeof(g_clip), 16u);
    g_silent_size = build_at3_silent(g_silent, sizeof(g_silent), 4u);
    if (g_clip_size == 0u || g_silent_size == 0u) {
        fprintf(stderr, "FAIL: the fixtures do not build\n");
        return 1;
    }
    if (!test_decode() || !test_refusals() || !test_player()) return 1;
    printf("test_player_ui_snd0: all checks passed\n");
    return 0;
}
