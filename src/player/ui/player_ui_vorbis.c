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

/* Closes the decoder, then frees the arena it was built in (stb_vorbis_close
   does not free a caller's arena). */
static void player_ui_vorbis_close(stb_vorbis *vorbis, char *arena) {
    stb_vorbis_close(vorbis);
    free(arena);
}

void player_ui_pcm_free(PlayerUiPcm *pcm) {
    if (pcm == NULL) return;
    free(pcm->samples);
    memset(pcm, 0, sizeof(*pcm));
}

PlayerUiVorbisResult player_ui_vorbis_decode(const unsigned char *bytes, size_t size,
                                             uint64_t max_pcm_bytes,
                                             PlayerUiVorbisCancel cancel, void *cancel_user,
                                             PlayerUiPcm *out, char *detail,
                                             size_t detail_size) {
    stb_vorbis *vorbis;
    stb_vorbis_info info;
    stb_vorbis_alloc arena;
    stb_vorbis_int64 length;
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
    /* Every allocation stb_vorbis makes comes from this arena, so a damaged
       setup header that declares huge codebooks runs out of it instead of
       taking hundreds of MB before the checks below run. */
    arena.alloc_buffer_length_in_bytes = (int)PLAYER_UI_VORBIS_ARENA_BYTES;
    arena.alloc_buffer = (char *)malloc(PLAYER_UI_VORBIS_ARENA_BYTES);
    if (arena.alloc_buffer == NULL) {
        player_ui_vorbis_detail(detail, detail_size, "out of memory for the decoder");
        return PLAYER_UI_VORBIS_NO_MEMORY;
    }
    vorbis = stb_vorbis_open_memory(bytes, (int)size, &error, &arena);
    if (vorbis == NULL) {
        char text[128];
        free(arena.alloc_buffer);
        if (error == VORBIS_outofmem) {
            (void)snprintf(text, sizeof(text), "the stream's decoder setup needs more than "
                           "%u MB", (unsigned)(PLAYER_UI_VORBIS_ARENA_BYTES >> 20));
            player_ui_vorbis_detail(detail, detail_size, text);
            return PLAYER_UI_VORBIS_UNSUPPORTED;
        }
        (void)snprintf(text, sizeof(text), "not an Ogg Vorbis stream (stb_vorbis error %d)",
                       error);
        player_ui_vorbis_detail(detail, detail_size, text);
        return PLAYER_UI_VORBIS_NOT_VORBIS;
    }
    info = stb_vorbis_get_info(vorbis);
    /* The 64-bit length: the last page's granule, which a damaged file can set
       to 2^63 or more (a negative length) or to a value whose low 32 bits look
       plausible. It is checked whole before it is narrowed. */
    length = stb_vorbis_stream_length_in_samples64(vorbis);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate < 8000u ||
        info.sample_rate > 192000u) {
        char text[96];
        (void)snprintf(text, sizeof(text), "%d channels at %u Hz (the launcher plays mono or "
                       "stereo, 8 to 192 kHz)", info.channels, info.sample_rate);
        player_ui_vorbis_detail(detail, detail_size, text);
        player_ui_vorbis_close(vorbis, arena.alloc_buffer);
        return PLAYER_UI_VORBIS_UNSUPPORTED;
    }
    if (length <= 0 ||
        (uint64_t)length > (uint64_t)info.sample_rate * PLAYER_UI_VORBIS_MAX_SECONDS) {
        char text[128];
        (void)snprintf(text, sizeof(text), "the stream declares %lld frames (the launcher "
                       "plays 1 frame to %u seconds)", (long long)length,
                       PLAYER_UI_VORBIS_MAX_SECONDS);
        player_ui_vorbis_detail(detail, detail_size, text);
        player_ui_vorbis_close(vorbis, arena.alloc_buffer);
        return PLAYER_UI_VORBIS_UNSUPPORTED;
    }
    /* At most 192000 x 600 frames, so it fits. */
    declared = (unsigned int)length;
    {
        /* frames x channels x 2 bytes, checked against the limit by division,
           so the product is never formed when it would be too large. */
        uint64_t frame_bytes = (uint64_t)info.channels * (uint64_t)sizeof(int16_t);
        if ((uint64_t)declared > max_pcm_bytes / frame_bytes ||
            (uint64_t)declared * frame_bytes > (uint64_t)SIZE_MAX) {
            char text[160];
            (void)snprintf(text, sizeof(text), "the stream decodes to more than %llu MB "
                           "(%u frames, %d channels)",
                           (unsigned long long)(max_pcm_bytes >> 20), declared, info.channels);
            player_ui_vorbis_detail(detail, detail_size, text);
            player_ui_vorbis_close(vorbis, arena.alloc_buffer);
            return PLAYER_UI_VORBIS_TOO_LARGE;
        }
    }
    samples = (int16_t *)malloc((size_t)declared * (size_t)info.channels * sizeof(int16_t));
    if (samples == NULL) {
        player_ui_vorbis_detail(detail, detail_size, "out of memory for the decoded track");
        player_ui_vorbis_close(vorbis, arena.alloc_buffer);
        return PLAYER_UI_VORBIS_NO_MEMORY;
    }
    while (decoded < declared) {
        uint64_t left = declared - decoded;
        int want = (int)(left < PLAYER_UI_VORBIS_CHUNK_FRAMES ? left
                                                              : PLAYER_UI_VORBIS_CHUNK_FRAMES);
        int got;
        if (cancel != NULL && cancel(cancel_user)) {
            free(samples);
            player_ui_vorbis_close(vorbis, arena.alloc_buffer);
            player_ui_vorbis_detail(detail, detail_size, "the decode was cancelled");
            return PLAYER_UI_VORBIS_CANCELLED;
        }
        got = stb_vorbis_get_samples_short_interleaved(
            vorbis, info.channels, samples + decoded * (uint64_t)info.channels,
            want * info.channels);
        if (got <= 0) break;
        decoded += (uint64_t)got;
    }
    player_ui_vorbis_close(vorbis, arena.alloc_buffer);
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
