/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored minimal stand-in for FFmpeg n4.4 libavcodec/avcodec.h
 * (PR-A).
 *
 * Only the declarations the imported decoder subset actually references are
 * kept. This is NOT the FFmpeg public API header; it is a closed standalone
 * subset. The AVCodecContext below carries exactly the fields the imported
 * atrac3plus and atrac3 decoder code touches (priv_data, channels,
 * channel_layout, block_align, flags, sample_fmt, codec_id, frame_number,
 * extradata, extradata_size). See
 * src/rt/atrac3p/PROVENANCE.md.
 */

#ifndef AT3P_LIBAVCODEC_AVCODEC_H
#define AT3P_LIBAVCODEC_AVCODEC_H

#include <stdint.h>
#include <stddef.h>

/* Upstream avcodec.h also includes libavutil/attributes.h (for the
 * av_* attribute macros used by imported decoder code, e.g. av_cold in
 * libavcodec/atrac.c). */
#include "libavutil/attributes.h"

/* Bitstream readers must never read past this padding at the end of a frame
 * buffer. The wrapper guarantees it via its internal scratch buffer. */
#define AV_INPUT_BUFFER_PADDING_SIZE 32

/* Value used only as a flag bit tested by the imported decoder init
 * (avctx->flags & AV_CODEC_FLAG_BITEXACT). The upstream enum value is
 * irrelevant inside this standalone subset; the wrapper always sets it. */
#define AV_CODEC_FLAG_BITEXACT (1 << 0)

/* Sample format tag written by the imported init. The value is not read
 * anywhere in the subset; it is kept only to preserve the upstream
 * assignment (avctx->sample_fmt = AV_SAMPLE_FMT_FLTP). */
#define AV_SAMPLE_FMT_FLTP 8

/* The AV_CH_* / AV_CH_LAYOUT_* channel mask constants are provided by the
 * imported upstream libavutil/channel_layout.h. */

/* Codec tags. The numeric values are not meaningful in this subset. The only
 * codec_id comparison left in the imported code is atrac3.c's
 * AV_CODEC_ID_ATRAC3AL check in atrac3_decode_init(); the atrac3plusdec.c
 * LOCAL DELTA entry point dropped upstream's AV_CODEC_ID_ATRAC3P comparison.
 * The wrapper (atrac3p_api.c) sets AV_CODEC_ID_ATRAC3P or AV_CODEC_ID_ATRAC3
 * on each context it creates. */
#define AV_CODEC_ID_ATRAC3P 0x17008
#define AV_CODEC_ID_ATRAC3  0x17003
#define AV_CODEC_ID_ATRAC3AL 0x17004

/* Minimal codec context carrying only the fields the imported decoder subset
 * uses. The wrapper allocates one per decoder instance and treats it as the
 * instance handle (AVCodecContext.priv_data owns the ATRAC3PContext). */
typedef struct AVCodecContext {
    void *priv_data;      /* ATRAC3PContext */
    int channels;
    uint64_t channel_layout;
    int block_align;
    int flags;
    int sample_fmt;
    int codec_id;
    int frame_number;     /* read by one imported av_log call site */
    const uint8_t *extradata;
    int extradata_size;
} AVCodecContext;

#endif /* AT3P_LIBAVCODEC_AVCODEC_H */
