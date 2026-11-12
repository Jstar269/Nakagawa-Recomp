/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The launcher music (#2207): the shipped track decodes through the vendored
   stb_vorbis, loops between its first and last non-silent frames with no step
   at the seam, fades in and out over the frames it is asked to, and the player
   object reports a missing or damaged file by name, opens no audio while off,
   and lets go of the audio device (and its reference on SDL's audio subsystem)
   when a game starts or the window loses focus. Runs on SDL's dummy audio
   driver. Usage: test_player_ui_music <path to ChillLofi.ogg> */

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
    CHECK(bytes != NULL, "the shipped track is readable");
    CHECK(player_ui_vorbis_decode(bytes, size, NULL, NULL, &pcm, detail, sizeof(detail)) ==
              PLAYER_UI_VORBIS_OK, detail);
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
        CHECK(player_ui_vorbis_decode(junk, sizeof(junk), NULL, NULL, &none, detail,
                                      sizeof(detail)) == PLAYER_UI_VORBIS_NOT_VORBIS &&
                  none.samples == NULL && detail[0] != '\0',
              "bytes that are not Ogg Vorbis are refused, with a reason");
        bytes[0] = 'X';
        CHECK(player_ui_vorbis_decode(bytes, size, NULL, NULL, &none, detail, sizeof(detail)) ==
                  PLAYER_UI_VORBIS_NOT_VORBIS, "a broken first page is refused");
        CHECK(player_ui_vorbis_decode(NULL, 0u, NULL, NULL, &none, detail, sizeof(detail)) ==
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
    CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO), "a second audio user");
    player_ui_music_update(music, &off, SDL_GetTicks());
    CHECK(player_ui_music_state(music) == PLAYER_UI_MUSIC_OFF && !player_ui_music_holds_audio(music),
          "turning it off stops it at once");
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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_player_ui_music <ChillLofi.ogg>\n");
        return 2;
    }
    (void)SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!test_track_decodes_and_loops(argv[1]) || !test_player_states(argv[1])) return 1;
    puts("player UI launcher music passed");
    return 0;
}
