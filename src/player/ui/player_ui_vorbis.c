/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The one translation unit that builds stb_vorbis (third_party/stb/stb_vorbis.c,
   imported byte-identical; see third_party/stb/PROVENANCE.md). It decodes from
   memory only: no stdio, no push-data API. The warnings the project's flags
   raise in upstream code are silenced here, around the include, rather than by
   editing the vendored file. */

#include "player_ui_vorbis.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-value"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wpedantic"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wcomma"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#include "../../../third_party/stb/stb_vorbis.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* Frames decoded per call: about a tenth of a second at 44.1 kHz. */
#define PLAYER_UI_VORBIS_CHUNK_FRAMES 4096u

static void player_ui_vorbis_detail(char *detail, size_t detail_size, const char *text) {
    if (detail != NULL && detail_size > 0u) (void)snprintf(detail, detail_size, "%s", text);
}

void player_ui_pcm_free(PlayerUiPcm *pcm) {
    if (pcm == NULL) return;
    free(pcm->samples);
    memset(pcm, 0, sizeof(*pcm));
}

PlayerUiVorbisResult player_ui_vorbis_decode(const unsigned char *bytes, size_t size,
                                             PlayerUiVorbisCancel cancel, void *cancel_user,
                                             PlayerUiPcm *out, char *detail,
                                             size_t detail_size) {
    stb_vorbis *vorbis;
    stb_vorbis_info info;
    unsigned int declared;
    uint64_t decoded = 0u;
    int error = 0;
    int16_t *samples;
    if (out == NULL) return PLAYER_UI_VORBIS_NOT_VORBIS;
    memset(out, 0, sizeof(*out));
    if (detail != NULL && detail_size > 0u) detail[0] = '\0';
    if (bytes == NULL || size == 0u || size > (size_t)INT_MAX) {
        player_ui_vorbis_detail(detail, detail_size, "the file is empty or too large to read");
        return PLAYER_UI_VORBIS_NOT_VORBIS;
    }
    vorbis = stb_vorbis_open_memory(bytes, (int)size, &error, NULL);
    if (vorbis == NULL) {
        char text[96];
        (void)snprintf(text, sizeof(text), "not an Ogg Vorbis stream (stb_vorbis error %d)",
                       error);
        player_ui_vorbis_detail(detail, detail_size, text);
        return error == VORBIS_outofmem ? PLAYER_UI_VORBIS_NO_MEMORY
                                        : PLAYER_UI_VORBIS_NOT_VORBIS;
    }
    info = stb_vorbis_get_info(vorbis);
    declared = stb_vorbis_stream_length_in_samples(vorbis);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate < 8000u ||
        info.sample_rate > 192000u) {
        char text[96];
        (void)snprintf(text, sizeof(text), "%d channels at %u Hz (the launcher plays mono or "
                       "stereo, 8 to 192 kHz)", info.channels, info.sample_rate);
        player_ui_vorbis_detail(detail, detail_size, text);
        stb_vorbis_close(vorbis);
        return PLAYER_UI_VORBIS_UNSUPPORTED;
    }
    if (declared == 0u ||
        (uint64_t)declared > (uint64_t)info.sample_rate * PLAYER_UI_VORBIS_MAX_SECONDS) {
        char text[128];
        (void)snprintf(text, sizeof(text), "the stream declares %u frames (the launcher plays "
                       "1 frame to %u seconds)", declared, PLAYER_UI_VORBIS_MAX_SECONDS);
        player_ui_vorbis_detail(detail, detail_size, text);
        stb_vorbis_close(vorbis);
        return PLAYER_UI_VORBIS_UNSUPPORTED;
    }
    /* At most 600 s x 192 kHz x 2 channels x 2 bytes: well inside size_t. */
    samples = (int16_t *)malloc((size_t)declared * (size_t)info.channels * sizeof(int16_t));
    if (samples == NULL) {
        player_ui_vorbis_detail(detail, detail_size, "out of memory for the decoded track");
        stb_vorbis_close(vorbis);
        return PLAYER_UI_VORBIS_NO_MEMORY;
    }
    while (decoded < declared) {
        uint64_t left = declared - decoded;
        int want = (int)(left < PLAYER_UI_VORBIS_CHUNK_FRAMES ? left
                                                              : PLAYER_UI_VORBIS_CHUNK_FRAMES);
        int got;
        if (cancel != NULL && cancel(cancel_user)) {
            free(samples);
            stb_vorbis_close(vorbis);
            player_ui_vorbis_detail(detail, detail_size, "the decode was cancelled");
            return PLAYER_UI_VORBIS_CANCELLED;
        }
        got = stb_vorbis_get_samples_short_interleaved(
            vorbis, info.channels, samples + decoded * (uint64_t)info.channels,
            want * info.channels);
        if (got <= 0) break;
        decoded += (uint64_t)got;
    }
    stb_vorbis_close(vorbis);
    if (decoded != declared) {
        char text[128];
        (void)snprintf(text, sizeof(text), "decoded %llu of the %u frames the stream declares",
                       (unsigned long long)decoded, declared);
        player_ui_vorbis_detail(detail, detail_size, text);
        free(samples);
        return PLAYER_UI_VORBIS_INCOMPLETE;
    }
    out->samples = samples;
    out->frames = decoded;
    out->channels = info.channels;
    out->sample_rate = (int)info.sample_rate;
    return PLAYER_UI_VORBIS_OK;
}
