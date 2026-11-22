/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_at3.h"

#include "atrac3p_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_UI_AT3_TAG_ATRAC3 0x0270u
#define PLAYER_UI_AT3_TAG_EXTENSIBLE 0xFFFEu
/* The ATRAC3 fmt extension: cbSize, then 14 bytes the decoder reads. */
#define PLAYER_UI_AT3_EXTRADATA_BYTES 14u
/* The largest frame accepted, as the runtime's sceAtrac track analysis. */
#define PLAYER_UI_AT3_MAX_ALIGN 0x10000u
/* The cancel callback is asked before every this many frames (about 0.75 s of
   ATRAC3, 1.5 s of ATRAC3+, a few milliseconds of decoding). */
#define PLAYER_UI_AT3_CANCEL_EVERY 32u

static uint32_t at3_le16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t at3_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static PlayerUiAt3Result at3_fail(PlayerUiAt3Result result, char *detail, size_t detail_size,
                                  const char *reason) {
    if (detail != NULL && detail_size > 0u) (void)snprintf(detail, detail_size, "%s", reason);
    return result;
}

const char *player_ui_at3_result_name(PlayerUiAt3Result result) {
    switch (result) {
    case PLAYER_UI_AT3_OK: return "ok";
    case PLAYER_UI_AT3_NOT_RIFF: return "not_riff";
    case PLAYER_UI_AT3_UNSUPPORTED: return "unsupported";
    case PLAYER_UI_AT3_DAMAGED: return "damaged";
    case PLAYER_UI_AT3_NO_MEMORY: return "no_memory";
    case PLAYER_UI_AT3_CANCELLED: return "cancelled";
    case PLAYER_UI_AT3_TOO_LARGE: return "too_large";
    default: return "unknown";
    }
}

typedef struct {
    uint32_t tag;
    int channels;
    int sample_rate;
    uint32_t align;
    uint8_t extradata[PLAYER_UI_AT3_EXTRADATA_BYTES];
    const uint8_t *data;
    size_t data_bytes;
} At3Track;

/* Reads "fmt " and "data". Every offset is checked against the bytes there
   (and the RIFF size), so a hostile size cannot wrap the cursor. */
static PlayerUiAt3Result at3_parse(const uint8_t *bytes, size_t size, At3Track *track,
                                   char *detail, size_t detail_size) {
    size_t end;
    size_t off = 12u;
    bool have_fmt = false;
    bool have_data = false;
    memset(track, 0, sizeof(*track));
    if (size < 12u || memcmp(bytes, "RIFF", 4u) != 0 || memcmp(bytes + 8, "WAVE", 4u) != 0) {
        return at3_fail(PLAYER_UI_AT3_NOT_RIFF, detail, detail_size, "not a RIFF WAVE file");
    }
    end = size;
    if ((uint64_t)at3_le32(bytes + 4) + 8u < (uint64_t)end) {
        end = (size_t)at3_le32(bytes + 4) + 8u;
    }
    /* A RIFF size of 0 to 3 would end the file inside its own header; the
       cursor below must never pass the end, so refuse that here. */
    if (end < off) {
        return at3_fail(PLAYER_UI_AT3_DAMAGED, detail, detail_size,
                        "the RIFF size ends inside the file header");
    }
    while (off <= end && end - off >= 8u) {
        const uint8_t *chunk = bytes + off;
        size_t payload = (size_t)at3_le32(chunk + 4);
        size_t avail = end - off - 8u;
        if (memcmp(chunk, "fmt ", 4u) == 0 && !have_fmt) {
            const uint8_t *p = chunk + 8;
            if (payload < 16u || payload > avail) {
                return at3_fail(PLAYER_UI_AT3_DAMAGED, detail, detail_size,
                                "the fmt chunk is short or runs past the file");
            }
            track->tag = at3_le16(p);
            track->channels = (int)at3_le16(p + 2);
            track->sample_rate = (int)at3_le32(p + 4);
            track->align = at3_le16(p + 12);
            if (track->tag == PLAYER_UI_AT3_TAG_ATRAC3) {
                if (payload < 18u + PLAYER_UI_AT3_EXTRADATA_BYTES ||
                    at3_le16(p + 16) != PLAYER_UI_AT3_EXTRADATA_BYTES) {
                    return at3_fail(PLAYER_UI_AT3_UNSUPPORTED, detail, detail_size,
                                    "the ATRAC3 fmt extension is not 14 bytes");
                }
                memcpy(track->extradata, p + 18, PLAYER_UI_AT3_EXTRADATA_BYTES);
            }
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4u) == 0 && !have_data) {
            track->data = chunk + 8;
            /* A data chunk that says it is longer than the file keeps the
               whole frames there are; the frame count below refuses none. */
            track->data_bytes = payload < avail ? payload : avail;
            have_data = true;
        }
        if (have_fmt && have_data) break;
        if (payload > avail) break;
        /* The chunk and its pad byte. */
        if (avail - payload < (payload & 1u)) break;
        off += 8u + payload + (payload & 1u);
    }
    if (!have_fmt) {
        return at3_fail(PLAYER_UI_AT3_DAMAGED, detail, detail_size, "the file has no fmt chunk");
    }
    if (!have_data) {
        return at3_fail(PLAYER_UI_AT3_DAMAGED, detail, detail_size, "the file has no data chunk");
    }
    if (track->tag != PLAYER_UI_AT3_TAG_ATRAC3 && track->tag != PLAYER_UI_AT3_TAG_EXTENSIBLE) {
        return at3_fail(PLAYER_UI_AT3_UNSUPPORTED, detail, detail_size,
                        "the format is neither ATRAC3 nor ATRAC3+");
    }
    /* The launcher plays mono and stereo clips. */
    if (track->channels < 1 || track->channels > 2) {
        return at3_fail(PLAYER_UI_AT3_UNSUPPORTED, detail, detail_size,
                        "the clip is neither mono nor stereo");
    }
    if (track->sample_rate < 8000 || track->sample_rate > 48000) {
        return at3_fail(PLAYER_UI_AT3_UNSUPPORTED, detail, detail_size,
                        "the sample rate is outside 8000 to 48000 Hz");
    }
    if (track->align < 1u || track->align > PLAYER_UI_AT3_MAX_ALIGN) {
        return at3_fail(PLAYER_UI_AT3_UNSUPPORTED, detail, detail_size,
                        "the frame size is outside 1 to 65536 bytes");
    }
    if (track->tag == PLAYER_UI_AT3_TAG_ATRAC3 &&
        !atrac3_config_valid((int)track->tag, track->channels, (int)track->align,
                             track->extradata, (int)PLAYER_UI_AT3_EXTRADATA_BYTES)) {
        return at3_fail(PLAYER_UI_AT3_UNSUPPORTED, detail, detail_size,
                        "the ATRAC3 frame configuration is not one the decoder takes");
    }
    if (track->data_bytes < track->align) {
        return at3_fail(PLAYER_UI_AT3_DAMAGED, detail, detail_size,
                        "the data chunk holds no whole frame");
    }
    return PLAYER_UI_AT3_OK;
}

PlayerUiAt3Result player_ui_at3_decode(const uint8_t *bytes, size_t size, uint64_t max_pcm_bytes,
                                       PlayerUiAt3Cancel cancel, void *cancel_user,
                                       PlayerUiPcm *out, char *detail, size_t detail_size) {
    At3Track track;
    PlayerUiAt3Result result;
    bool plus;
    uint32_t frame_samples;
    uint64_t frames;
    uint64_t sample_count;
    int16_t *samples;
    Atrac3Handle *at3 = NULL;
    Atrac3pHandle *at3p = NULL;
    if (detail != NULL && detail_size > 0u) detail[0] = '\0';
    if (out == NULL) {
        return at3_fail(PLAYER_UI_AT3_DAMAGED, detail, detail_size, "no output");
    }
    memset(out, 0, sizeof(*out));
    if (bytes == NULL) {
        return at3_fail(PLAYER_UI_AT3_NOT_RIFF, detail, detail_size, "no bytes");
    }
    result = at3_parse(bytes, size, &track, detail, detail_size);
    if (result != PLAYER_UI_AT3_OK) return result;
    plus = track.tag == PLAYER_UI_AT3_TAG_EXTENSIBLE;
    frame_samples = plus ? (uint32_t)ATRAC3P_FRAME_SAMPLES : (uint32_t)ATRAC3_FRAME_SAMPLES;
    frames = (uint64_t)(track.data_bytes / track.align);
    /* frames <= 64 Mi and frame_samples * channels <= 4096: no product wraps. */
    sample_count = frames * frame_samples * (uint64_t)track.channels;
    if (sample_count > max_pcm_bytes / sizeof(int16_t) || sample_count > SIZE_MAX / sizeof(int16_t)) {
        return at3_fail(PLAYER_UI_AT3_TOO_LARGE, detail, detail_size,
                        "the clip would decode to more than the launcher keeps");
    }
    if ((plus ? atrac3p_create(track.channels, (int)track.align, &at3p)
              : atrac3_create(track.channels, (int)track.align, track.extradata, &at3)) != 0) {
        return at3_fail(PLAYER_UI_AT3_UNSUPPORTED, detail, detail_size,
                        "the decoder refused the clip's configuration");
    }
    samples = (int16_t *)malloc((size_t)sample_count * sizeof(int16_t));
    if (samples == NULL) {
        atrac3p_destroy(at3p);
        atrac3_destroy(at3);
        return at3_fail(PLAYER_UI_AT3_NO_MEMORY, detail, detail_size, "out of memory");
    }
    for (uint64_t f = 0u; f < frames; ++f) {
        const uint8_t *frame = track.data + (size_t)f * track.align;
        int16_t *pcm = samples + (size_t)(f * frame_samples * (uint64_t)track.channels);
        int decoded = 0;
        int rc;
        if (f % PLAYER_UI_AT3_CANCEL_EVERY == 0u && cancel != NULL && cancel(cancel_user)) {
            result = at3_fail(PLAYER_UI_AT3_CANCELLED, detail, detail_size, "cancelled");
            break;
        }
        rc = plus ? atrac3p_decode(at3p, frame, (int)track.align, pcm, &decoded)
                  : atrac3_decode(at3, frame, (int)track.align, pcm, &decoded);
        if (rc < 0 || decoded != (int)frame_samples) {
            char reason[96];
            (void)snprintf(reason, sizeof(reason), "frame %llu of %llu does not decode",
                           (unsigned long long)f + 1u, (unsigned long long)frames);
            result = at3_fail(PLAYER_UI_AT3_DAMAGED, detail, detail_size, reason);
            break;
        }
    }
    atrac3p_destroy(at3p);
    atrac3_destroy(at3);
    if (result != PLAYER_UI_AT3_OK) {
        free(samples);
        return result;
    }
    out->samples = samples;
    out->frames = frames * frame_samples;
    out->channels = track.channels;
    out->sample_rate = track.sample_rate;
    return PLAYER_UI_AT3_OK;
}
