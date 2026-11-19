/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The launcher music (#2207): the shipped track decodes through the vendored
   stb_vorbis, loops between its first and last non-silent frames with no step
   at the seam, fades in and out over the frames it is asked to, and the player
   object reports a missing or damaged file by name, opens no audio while off,
   and lets go of the audio device (and its reference on SDL's audio subsystem)
   when a game starts or the window loses focus. A game that starts during the
   decode cancels it and joins its worker; a device that stops taking data is
   released at the time limit and reported; the decoded track is freed while a
   game runs. Runs on SDL's dummy audio driver, with the module's test seams
   (PLAYER_UI_MUSIC_TEST_SEAMS). Usage: test_player_ui_music <path to ChillLofi.ogg> */

/* The module's test seams (the module itself is built with the same define). */
#ifndef PLAYER_UI_MUSIC_TEST_SEAMS
#define PLAYER_UI_MUSIC_TEST_SEAMS
#endif

#include "player_ui_music.h"
#include "player_ui_vorbis.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));      \
            return 0;                                                             \
        }                                                                         \
    } while (0)

/* The longest a state change may take here: decoding the track on a slow runner. */
#define WAIT_LIMIT_MS 120000u

static unsigned char *read_file(const char *path, size_t *size) {
    void *data = SDL_LoadFile(path, size);
    return (unsigned char *)data;
}

static bool frame_silent(const PlayerUiPcm *pcm, uint64_t frame) {
    for (int c = 0; c < pcm->channels; ++c) {
        if (pcm->samples[frame * (uint64_t)pcm->channels + (uint64_t)c] != 0) return false;
    }
    return true;
}

static int abs_i(int v) { return v < 0 ? -v : v; }

static int test_track_decodes_and_loops(const char *track) {
    size_t size = 0u;
    unsigned char *bytes = read_file(track, &size);
    PlayerUiPcm pcm;
    PlayerUiMusicMix mix;
    char detail[256];
    int16_t out[16 * 2];
    uint64_t decode_started;
    CHECK(bytes != NULL, "the shipped track is readable");
    decode_started = SDL_GetTicks();
    {
        /* The decoded size is bounded before anything is allocated: the track
           (about 22 MB of samples) against a 1 MB limit. */
        PlayerUiPcm refused;
        CHECK(player_ui_vorbis_decode(bytes, size, 1024u * 1024u, NULL, NULL, &refused, detail,
                                      sizeof(detail)) == PLAYER_UI_VORBIS_TOO_LARGE &&
                  refused.samples == NULL && strstr(detail, "more than 1 MB") != NULL,
              "a stream that decodes past the limit is refused by name");
    }
    CHECK(player_ui_vorbis_decode(bytes, size, PLAYER_UI_MUSIC_MAX_PCM_BYTES, NULL, NULL, &pcm,
                                  detail, sizeof(detail)) == PLAYER_UI_VORBIS_OK, detail);
    CHECK(pcm.frames * 2u * sizeof(int16_t) < PLAYER_UI_MUSIC_MAX_PCM_BYTES,
          "the shipped track is inside the limit");
    /* What decoding the track again on the launcher's return costs. */
    printf("decoded the whole track in %llu ms\n",
           (unsigned long long)(SDL_GetTicks() - decode_started));
    CHECK(pcm.channels == 2 && pcm.sample_rate == 44100, "the track is 44.1 kHz stereo");
    /* 2:05.15 at 44.1 kHz. */
    CHECK(pcm.frames > 44100u * 125u && pcm.frames < 44100u * 126u, "the whole track decodes");

    player_ui_music_mix_init(&mix, &pcm);
    CHECK(player_ui_music_mix_has_sound(&mix), "the track has sound to loop");
    CHECK(mix.loop_start > 0u && mix.loop_end < pcm.frames,
          "the silence at both ends of the recording is outside the loop");
    CHECK(!frame_silent(&pcm, mix.loop_start) && frame_silent(&pcm, mix.loop_start - 1u),
          "the loop starts on the first frame with sound");
    CHECK(!frame_silent(&pcm, mix.loop_end - 1u) && frame_silent(&pcm, mix.loop_end),
          "the loop ends after the last frame with sound");
    CHECK(mix.cursor == mix.loop_start && mix.gain == 0.0f, "it starts at the loop, silent");

    /* The seam: at full gain, the frame after the loop's last is its first, and
       the step between them is no larger than the quietest end of the recording
       allows (both sit next to digital silence). */
    player_ui_music_mix_ramp(&mix, 1.0f, 0u);
    mix.cursor = mix.loop_end - 4u;
    player_ui_music_mix_render(&mix, out, 8u);
    for (int c = 0; c < 2; ++c) {
        CHECK(out[3 * 2 + c] == pcm.samples[(mix.loop_end - 1u) * 2u + (uint64_t)c],
              "the loop's last frame plays as decoded");
        CHECK(out[4 * 2 + c] == pcm.samples[mix.loop_start * 2u + (uint64_t)c],
              "the loop wraps to its first frame");
        CHECK(abs_i(out[3 * 2 + c] - out[4 * 2 + c]) <= 256,
              "the step at the seam is a whisper, not a click");
    }
    CHECK(mix.cursor == mix.loop_start + 4u, "the cursor runs on past the seam");

    /* A level is applied as a gain, rounded. */
    player_ui_music_mix_ramp(&mix, 0.5f, 0u);
    mix.cursor = mix.loop_start + 44100u;
    player_ui_music_mix_render(&mix, out, 1u);
    for (int c = 0; c < 2; ++c) {
        float half = (float)pcm.samples[(mix.loop_start + 44100u) * 2u + (uint64_t)c] * 0.5f;
        int expect = (int)(half >= 0.0f ? half + 0.5f : half - 0.5f);
        CHECK(out[c] == expect, "half volume halves each sample");
    }

    /* Fade in over 1.5 s of frames, fade out over 0.5 s. */
    {
        int16_t *scratch = (int16_t *)malloc(66150u * 2u * sizeof(int16_t));
        CHECK(scratch != NULL, "scratch buffer");
        player_ui_music_mix_ramp(&mix, 0.0f, 0u);
        player_ui_music_mix_ramp(&mix, 0.8f, 66150u);
        player_ui_music_mix_render(&mix, scratch, 33075u);
        CHECK(mix.gain > 0.39f && mix.gain < 0.41f, "half way through the fade-in, half the level");
        player_ui_music_mix_render(&mix, scratch, 33075u);
        CHECK(mix.gain == 0.8f && mix.step == 0.0f, "the fade-in ends at the level");
        player_ui_music_mix_ramp(&mix, 0.0f, 22050u);
        player_ui_music_mix_render(&mix, scratch, 22049u);
        CHECK(!player_ui_music_mix_silent(&mix), "one frame before the fade-out ends, not silent");
        player_ui_music_mix_render(&mix, scratch, 1u);
        CHECK(player_ui_music_mix_silent(&mix), "the fade-out ends in silence");
        player_ui_music_mix_render(&mix, scratch, 64u);
        for (int i = 0; i < 128; ++i) CHECK(scratch[i] == 0, "silence after the fade-out");
        free(scratch);
    }

    /* Damage: bytes that are not Ogg, and a capture pattern broken. */
    {
        unsigned char junk[4096];
        PlayerUiPcm none;
        for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = (unsigned char)(i * 37u + 11u);
        CHECK(player_ui_vorbis_decode(junk, sizeof(junk), PLAYER_UI_MUSIC_MAX_PCM_BYTES, NULL,
                                      NULL, &none, detail,
                                      sizeof(detail)) == PLAYER_UI_VORBIS_NOT_VORBIS &&
                  none.samples == NULL && detail[0] != '\0',
              "bytes that are not Ogg Vorbis are refused, with a reason");
        bytes[0] = 'X';
        CHECK(player_ui_vorbis_decode(bytes, size, PLAYER_UI_MUSIC_MAX_PCM_BYTES, NULL, NULL,
                                      &none, detail, sizeof(detail)) ==
                  PLAYER_UI_VORBIS_NOT_VORBIS, "a broken first page is refused");
        CHECK(player_ui_vorbis_decode(NULL, 0u, PLAYER_UI_MUSIC_MAX_PCM_BYTES, NULL, NULL,
                                      &none, detail, sizeof(detail)) ==
                  PLAYER_UI_VORBIS_NOT_VORBIS, "nothing is refused");
    }
    /* A track that is all digital silence has nothing to loop. */
    {
        int16_t quiet[64] = {0};
        PlayerUiPcm silent_pcm = {quiet, 32u, 2, 44100};
        PlayerUiMusicMix silent_mix;
        player_ui_music_mix_init(&silent_mix, &silent_pcm);
        CHECK(!player_ui_music_mix_has_sound(&silent_mix), "a silent track has no loop");
        player_ui_music_mix_ramp(&silent_mix, 1.0f, 0u);
        player_ui_music_mix_render(&silent_mix, out, 8u);
        for (int i = 0; i < 16; ++i) CHECK(out[i] == 0, "and renders silence");
    }
    SDL_free(bytes);
    player_ui_pcm_free(&pcm);
    return 1;
}

/* ---- damaged headers ---------------------------------------------------------- */

/* The Ogg page checksum: CRC-32, polynomial 0x04c11db7, not reflected, start 0. */
static uint32_t ogg_crc(const unsigned char *data, size_t size) {
    uint32_t crc = 0u;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

/* The size of the page at `page` (header, segment table and body). */
static size_t ogg_page_size(const unsigned char *page) {
    size_t size = 27u + page[26];
    for (unsigned i = 0; i < page[26]; ++i) size += page[27 + i];
    return size;
}

static void ogg_page_seal(unsigned char *page) {
    uint32_t crc;
    memset(page + 22, 0, 4u);
    crc = ogg_crc(page, ogg_page_size(page));
    for (int i = 0; i < 4; ++i) page[22 + i] = (unsigned char)(crc >> (8 * i));
}

/* Appends a page holding whole packets of the given sizes (each under 255 bytes). */
static size_t ogg_page(unsigned char *out, unsigned char type, uint32_t sequence,
                       const unsigned char *body, const unsigned char *sizes, unsigned count) {
    size_t body_size = 0u;
    memset(out, 0, 27u);
    memcpy(out, "OggS", 4u);
    out[5] = type;
    out[14] = 0x4bu; /* serial number */
    for (int i = 0; i < 4; ++i) out[18 + i] = (unsigned char)(sequence >> (8 * i));
    out[26] = (unsigned char)count;
    for (unsigned i = 0; i < count; ++i) {
        out[27 + i] = sizes[i];
        body_size += sizes[i];
    }
    memcpy(out + 27 + count, body, body_size);
    ogg_page_seal(out);
    return 27u + count + body_size;
}

/* Vorbis packs header fields least significant bit first. */
static void put_bits(unsigned char *buf, size_t *bit, uint32_t value, int count) {
    for (int i = 0; i < count; ++i, ++*bit)
        if ((value >> i) & 1u) buf[*bit >> 3] |= (unsigned char)(1u << (*bit & 7u));
}

static int test_damaged_headers_are_refused(const char *track) {
    char detail[256];
    PlayerUiPcm none;
    /* The last page's granule set to 2^63 + 10000: a negative 64-bit length
       whose low 32 bits read as 10000 frames. Before the length was checked
       whole, the 32-bit length passed every check and the decode returned
       frames stb_vorbis never wrote. */
    {
        size_t size = 0u;
        unsigned char *bytes = read_file(track, &size);
        size_t last = 0u;
        bool found = false;
        CHECK(bytes != NULL && size > 27u, "the shipped track is readable");
        for (size_t at = size - 27u; at > 0u; --at) {
            if (memcmp(bytes + at, "OggS", 4u) == 0 && (bytes[at + 5] & 4u) != 0u) {
                last = at;
                found = true;
                break;
            }
        }
        CHECK(found && last + ogg_page_size(bytes + last) == size, "the track ends with its last page");
        for (int i = 0; i < 8; ++i)
            bytes[last + 6 + (size_t)i] =
                (unsigned char)((0x8000000000002710ull >> (8 * i)) & 0xffu);
        ogg_page_seal(bytes + last);
        CHECK(player_ui_vorbis_decode(bytes, size, PLAYER_UI_MUSIC_MAX_PCM_BYTES, NULL, NULL,
                                      &none, detail, sizeof(detail)) ==
                      PLAYER_UI_VORBIS_UNSUPPORTED &&
                  none.samples == NULL && strstr(detail, "declares -") != NULL,
              "a last-page granule of 2^63 or more is refused as a length");
        SDL_free(bytes);
    }
    /* A setup header of four codebooks of 2^18 entries each, about 3 MB of
       decoder memory apiece from a few bytes of file: refused once it passes
       the decoder's arena, before the file is found to be damaged further on.
       Without the arena stb_vorbis allocated all of it (up to about 0.9 GB for
       256 such codebooks). */
    {
        unsigned char file[512];
        unsigned char body[256];
        unsigned char sizes[2];
        size_t at = 0u;
        size_t bit;
        memset(body, 0, sizeof(body));
        /* Identification: 2 channels, 44.1 kHz, blocks of 256 and 2048. */
        memcpy(body, "\x01vorbis", 7u);
        body[11] = 2u;
        body[12] = 0x44u; body[13] = 0xacu;
        body[28] = 0xb8u;
        body[29] = 1u;
        sizes[0] = 30u;
        at += ogg_page(file + at, 2u, 0u, body, sizes, 1u);
        /* Comment (no vendor, no comments), then setup. */
        memset(body, 0, sizeof(body));
        memcpy(body, "\x03vorbis", 7u);
        body[15] = 1u;
        memcpy(body + 16, "\x05vorbis", 7u);
        bit = (16u + 7u) * 8u;
        put_bits(body, &bit, 3u, 8);                  /* four codebooks */
        for (int c = 0; c < 4; ++c) {
            put_bits(body, &bit, 0x564342u, 24);      /* sync */
            put_bits(body, &bit, 1u, 16);             /* dimensions */
            put_bits(body, &bit, 0x40000u, 24);       /* entries */
            put_bits(body, &bit, 1u, 1);              /* ordered */
            put_bits(body, &bit, 17u, 5);             /* every codeword 18 bits long */
            put_bits(body, &bit, 0x40000u, 19);       /* ... for all 2^18 entries */
            put_bits(body, &bit, 0u, 4);              /* no lookup */
        }
        sizes[0] = 16u;
        sizes[1] = (unsigned char)((bit + 7u) / 8u - 16u + 8u);
        at += ogg_page(file + at, 0u, 1u, body, sizes, 2u);
        CHECK(player_ui_vorbis_decode(file, at, PLAYER_UI_MUSIC_MAX_PCM_BYTES, NULL, NULL, &none,
                                      detail, sizeof(detail)) == PLAYER_UI_VORBIS_UNSUPPORTED &&
                  none.samples == NULL && strstr(detail, "decoder setup") != NULL,
              "a setup header that needs more than the decoder's arena is refused");
    }
    return 1;
}

/* Updates `music` until its state is `want` or WAIT_LIMIT_MS passes. */
static bool wait_for(PlayerUiMusic *music, const PlayerUiMusicRequest *request,
                     PlayerUiMusicState want) {
    uint64_t deadline = SDL_GetTicks() + WAIT_LIMIT_MS;
    for (;;) {
        player_ui_music_update(music, request, SDL_GetTicks());
        if (player_ui_music_state(music) == want) return true;
        if (SDL_GetTicks() > deadline) {
            fprintf(stderr, "waiting for %s, still %s\n", player_ui_music_state_name(want),
                    player_ui_music_state_name(player_ui_music_state(music)));
            return false;
        }
        SDL_Delay(5);
    }
}

/* Updates `music` until the device has pulled some of the fade-in. */
static bool wait_audible(PlayerUiMusic *music, const PlayerUiMusicRequest *request) {
    uint64_t deadline = SDL_GetTicks() + WAIT_LIMIT_MS;
    while (player_ui_music_gain(music) < 0.05f) {
        player_ui_music_update(music, request, SDL_GetTicks());
        if (SDL_GetTicks() > deadline) return false;
        SDL_Delay(5);
    }
    return true;
}

static int test_player_states(const char *track) {
    PlayerUiMusicRequest off = {false, true, true, 80};
    PlayerUiMusicRequest on = {true, true, true, 80};
    PlayerUiMusicRequest game = {true, false, true, 80};
    PlayerUiMusicRequest away = {true, true, false, 80};
    PlayerUiMusic *music;

    /* A missing file is named, never played. */
    music = player_ui_music_create("no-such-folder/ChillLofi.ogg", PLAYER_UI_MUSIC_TRACK);
    CHECK(music != NULL, "create");
    player_ui_music_update(music, &off, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_OFF && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "while off nothing is read and SDL audio is never started");
    CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_MISSING), "a missing track is MISSING");
    CHECK(strstr(player_ui_music_detail(music), PLAYER_UI_MUSIC_TRACK) != NULL &&
              strstr(player_ui_music_detail(music), "not there") != NULL,
          "the detail names the track, not the folder it was looked for in");
    CHECK(!player_ui_music_holds_audio(music) && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "a missing track opens no audio");
    player_ui_music_update(music, &off, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_OFF && player_ui_music_detail(music)[0] == '\0',
          "turning it off clears the condition");
    player_ui_music_destroy(music);

    /* A damaged file is named too. */
    {
        const char *damaged = SR_SELFTEST_BUILD_ROOT "/test_player_ui_music_damaged.ogg";
        SDL_IOStream *io = SDL_IOFromFile(damaged, "wb");
        CHECK(io != NULL && SDL_WriteIO(io, "OggS but not really", 19u) == 19u, "write junk");
        SDL_CloseIO(io);
        music = player_ui_music_create(damaged, PLAYER_UI_MUSIC_TRACK);
        CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_DAMAGED), "a damaged track is DAMAGED");
        CHECK(strstr(player_ui_music_detail(music), "Ogg Vorbis") != NULL,
              "the detail says what is wrong");
        CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0, "a damaged track opens no audio");
        player_ui_music_destroy(music);
        (void)SDL_RemovePath(damaged);
    }

    /* The real track: plays, stops for a game, resumes, pauses without focus. */
    music = player_ui_music_create(track, PLAYER_UI_MUSIC_TRACK);
    CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_PLAYING), "the track plays");
    CHECK(player_ui_music_holds_audio(music) && SDL_WasInit(SDL_INIT_AUDIO) != 0,
          "playing holds a stream on the (dummy) device");
    {
        /* The device pulls the fade-in: the level rises from silence. */
        uint64_t deadline = SDL_GetTicks() + WAIT_LIMIT_MS;
        while (player_ui_music_gain(music) < 0.05f) {
            player_ui_music_update(music, &on, SDL_GetTicks());
            CHECK(SDL_GetTicks() <= deadline, "the fade-in rises");
            SDL_Delay(5);
        }
        CHECK(player_ui_music_gain(music) < 0.8f, "the fade-in has not ended yet");
    }
    player_ui_music_update(music, &game, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_FADING_OUT &&
              player_ui_music_holds_audio(music),
          "a starting game first fades the music out");
    CHECK(wait_for(music, &game, PLAYER_UI_MUSIC_STOPPED), "then stops it");
    CHECK(!player_ui_music_holds_audio(music) && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "and lets go of the device and SDL's audio before the game runs");
    CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_PLAYING), "back in the launcher it plays again");
    CHECK(wait_for(music, &away, PLAYER_UI_MUSIC_PAUSED), "without focus it pauses");
    CHECK(!player_ui_music_holds_audio(music) && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "paused, it holds no audio");
    CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_PLAYING), "with focus back it plays");

    /* Another audio user (the menu sounds) keeps its own reference: the music
       letting go never stops SDL audio under it. */
    CHECK(wait_audible(music, &on), "audible");
    CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO), "a second audio user");
    player_ui_music_update(music, &off, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_FADING_OUT,
          "turning it off fades the music first");
    CHECK(wait_for(music, &off, PLAYER_UI_MUSIC_OFF) && !player_ui_music_holds_audio(music),
          "then stops it");
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0, "the other user's audio stays up");
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0, "and stops with that user");
    player_ui_music_destroy(music);
    player_ui_music_destroy(NULL);

    /* The Settings row says what stops the music while it is on. */
    CHECK(strcmp(player_ui_music_row_value(false, PLAYER_UI_MUSIC_MISSING), "Off") == 0, "off");
    CHECK(strcmp(player_ui_music_row_value(true, PLAYER_UI_MUSIC_PLAYING), "On") == 0, "on");
    CHECK(strcmp(player_ui_music_row_value(true, PLAYER_UI_MUSIC_STOPPED), "On") == 0,
          "stopped for a game is still on");
    CHECK(strcmp(player_ui_music_row_value(true, PLAYER_UI_MUSIC_MISSING), "On: file missing") == 0,
          "a missing file is named on the row");
    CHECK(strcmp(player_ui_music_row_value(true, PLAYER_UI_MUSIC_DAMAGED), "On: file damaged") == 0,
          "a damaged file is named on the row");
    CHECK(strcmp(player_ui_music_row_value(true, PLAYER_UI_MUSIC_NO_DEVICE),
                 "On: no sound device") == 0, "no device is named on the row");
    return 1;
}

/* Review round 1 of #2536: the decode, the release and the transitions. */
static int test_decode_release_and_transitions(const char *track) {
    PlayerUiMusicRequest on = {true, true, true, 80};
    PlayerUiMusicRequest softer = {true, true, true, 70};
    PlayerUiMusicRequest quiet = {true, true, true, 0};
    PlayerUiMusicRequest off = {false, true, true, 80};
    PlayerUiMusicRequest game = {true, false, true, 80};
    PlayerUiMusicRequest game_away = {true, false, false, 80};
    PlayerUiMusicRequest away = {true, true, false, 80};
    PlayerUiMusic *music;
    uint64_t t0;
    uint64_t stopped_at;

    /* A game that starts during the decode cancels it and joins its worker before
       the launch may go on. The held decode waits for its cancel, so without the
       cancel this would hang. */
    player_ui_music_test_hold_decode(true);
    music = player_ui_music_create(track, PLAYER_UI_MUSIC_TRACK);
    CHECK(music != NULL, "create");
    player_ui_music_update(music, &on, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_LOADING &&
              player_ui_music_decoding(music), "the decode runs on its worker");
    player_ui_music_update(music, &game, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_STOPPED, "a starting game stops it");
    CHECK(!player_ui_music_decoding(music) && player_ui_music_test_pcm_frames(music) == 0u,
          "and no decode worker is left, nor any decoded track");
    CHECK(!player_ui_music_holds_audio(music) && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "no audio is held");
    player_ui_music_update(music, &game, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_STOPPED &&
              !player_ui_music_decoding(music), "nothing is read while the game runs");

    /* Quitting during the decode cancels and joins it too. */
    player_ui_music_update(music, &on, SDL_GetTicks());
    CHECK(player_ui_music_decoding(music), "back in the launcher the decode starts again");
    player_ui_music_destroy(music);

    /* Turning the setting on and off quickly during the decode. */
    music = player_ui_music_create(track, PLAYER_UI_MUSIC_TRACK);
    for (int i = 0; i < 5; ++i) {
        player_ui_music_update(music, &on, SDL_GetTicks());
        CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_LOADING &&
                  player_ui_music_decoding(music), "on: the decode starts");
        player_ui_music_update(music, &off, SDL_GetTicks());
        CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_OFF &&
                  !player_ui_music_decoding(music), "off: the decode is cancelled and joined");
    }
    player_ui_music_test_hold_decode(false);

    /* A real decode, cancelled part way, stops within one chunk. */
    player_ui_music_update(music, &on, SDL_GetTicks());
    SDL_Delay(50);
    t0 = SDL_GetTicks();
    player_ui_music_update(music, &game, SDL_GetTicks());
    printf("a decode cancelled part way was joined in %llu ms\n",
           (unsigned long long)(SDL_GetTicks() - t0));
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_STOPPED &&
              !player_ui_music_decoding(music), "the cancelled decode is joined");
    CHECK(SDL_GetTicks() - t0 < 1000u, "promptly");

    CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_PLAYING), "after all that the track plays");
    /* A volume step during the 1.5 s fade-in keeps the fade's length. */
    player_ui_music_update(music, &softer, SDL_GetTicks());
    CHECK(player_ui_music_test_ramp_frames(music) > 44100u,
          "a volume step during the fade-in does not cut it short");
    /* Music volume 0 holds no device. */
    CHECK(wait_for(music, &quiet, PLAYER_UI_MUSIC_PAUSED), "at volume 0 it pauses");
    CHECK(!player_ui_music_holds_audio(music) && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "and holds no audio");
    CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_PLAYING), "a level above 0 plays again");

    /* Focus lost during the fade before a game: the game still wins. */
    CHECK(wait_audible(music, &on), "audible");
    player_ui_music_update(music, &game, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_FADING_OUT, "PLAY fades the music");
    CHECK(wait_for(music, &game_away, PLAYER_UI_MUSIC_STOPPED), "focus lost too: it stops");
    CHECK(!player_ui_music_holds_audio(music) && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "and lets go of the device");
    CHECK(player_ui_music_take_release_timeout(music) == 0u, "a device that took data: no timeout");
    /* While the game runs the decoded track is freed; it resumes where it stopped. */
    CHECK(player_ui_music_test_pcm_frames(music) == 0u, "no decoded track is kept for the game");
    stopped_at = player_ui_music_test_cursor(music);
    CHECK(stopped_at > 0u, "where the loop stood is kept");
    /* Back in the launcher but unheard (no focus, or volume 0): nothing is read. */
    player_ui_music_update(music, &away, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_PAUSED &&
              !player_ui_music_decoding(music) && player_ui_music_test_pcm_frames(music) == 0u,
          "without focus the track is not decoded again");
    player_ui_music_update(music, &quiet, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_PAUSED &&
              !player_ui_music_decoding(music) && player_ui_music_test_pcm_frames(music) == 0u,
          "at volume 0 the track is not decoded again");
    CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_PLAYING), "back in the launcher it plays");
    CHECK(player_ui_music_test_pcm_frames(music) > 0u, "decoded again");
    CHECK(player_ui_music_test_cursor(music) >= stopped_at &&
              player_ui_music_test_cursor(music) < stopped_at + 44100u * 10u,
          "from where it stopped");

    /* A device that stops taking data once the fade starts: the release waits the
       fade and the grace, no longer, then closes and reports it once. */
    CHECK(wait_audible(music, &on), "audible");
    player_ui_music_test_stall_release(true);
    t0 = SDL_GetTicks();
    player_ui_music_update(music, &game, t0);
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_FADING_OUT, "the fade starts");
    player_ui_music_update(music, &game, t0 + 700u);
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_FADING_OUT, "700 ms: still waiting");
    player_ui_music_update(music, &game, t0 + 749u);
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_FADING_OUT &&
              player_ui_music_take_release_timeout(music) == 0u, "749 ms: still waiting");
    player_ui_music_update(music, &game, t0 + 750u);
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_STOPPED &&
              !player_ui_music_holds_audio(music) && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "750 ms: the device is closed and SDL audio let go");
    CHECK(player_ui_music_take_release_timeout(music) == 750u, "the timeout is reported");
    CHECK(player_ui_music_take_release_timeout(music) == 0u, "once");
    /* The same limit when the setting turns off: 50 ms and the grace. */
    CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_PLAYING) && wait_audible(music, &on),
          "it plays again");
    t0 = SDL_GetTicks();
    player_ui_music_update(music, &off, t0);
    player_ui_music_update(music, &off, t0 + 299u);
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_FADING_OUT, "off: 299 ms, waiting");
    player_ui_music_update(music, &off, t0 + 300u);
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_OFF && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "off: 300 ms, closed");
    CHECK(player_ui_music_take_release_timeout(music) == 300u, "and reported");
    player_ui_music_test_stall_release(false);
    player_ui_music_destroy(music);

    /* No worker thread: the track decodes on this thread and plays at once. */
    player_ui_music_test_no_thread(true);
    music = player_ui_music_create(track, PLAYER_UI_MUSIC_TRACK);
    player_ui_music_update(music, &on, SDL_GetTicks());
    CHECK(!player_ui_music_decoding(music) &&
              player_ui_music_state(music) == PLAYER_UI_MUSIC_PLAYING &&
              player_ui_music_holds_audio(music), "decoded inline, it plays");
    player_ui_music_test_no_thread(false);
    CHECK(wait_for(music, &off, PLAYER_UI_MUSIC_OFF), "and stops");
    player_ui_music_destroy(music);

    /* A file too large to be the track, and a folder where the file should be. */
    {
        const char *big = SR_SELFTEST_BUILD_ROOT "/test_player_ui_music_big.ogg";
        const char *folder = SR_SELFTEST_BUILD_ROOT "/test_player_ui_music_folder.ogg";
        SDL_IOStream *io = SDL_IOFromFile(big, "wb");
        CHECK(io != NULL, "create the large file");
        CHECK(SDL_SeekIO(io, (Sint64)PLAYER_UI_MUSIC_MAX_FILE_BYTES, SDL_IO_SEEK_SET) >= 0 &&
                  SDL_WriteIO(io, "x", 1u) == 1u, "grow it past the cap");
        SDL_CloseIO(io);
        music = player_ui_music_create(big, PLAYER_UI_MUSIC_TRACK);
        CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_DAMAGED), "a file over the cap is DAMAGED");
        CHECK(strstr(player_ui_music_detail(music), "larger than 64 MB") != NULL,
              "the detail says why");
        player_ui_music_destroy(music);
        (void)SDL_RemovePath(big);
        CHECK(SDL_CreateDirectory(folder), "create the folder");
        music = player_ui_music_create(folder, PLAYER_UI_MUSIC_TRACK);
        CHECK(wait_for(music, &on, PLAYER_UI_MUSIC_DAMAGED), "a folder is DAMAGED");
        CHECK(strstr(player_ui_music_detail(music), "not a file") != NULL, "the detail says why");
        player_ui_music_destroy(music);
        (void)SDL_RemovePath(folder);
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_player_ui_music <ChillLofi.ogg>\n");
        return 2;
    }
    (void)SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!test_track_decodes_and_loops(argv[1]) || !test_damaged_headers_are_refused(argv[1]) ||
        !test_player_states(argv[1]) || !test_decode_release_and_transitions(argv[1])) {
        return 1;
    }
    puts("player UI launcher music passed");
    return 0;
}
