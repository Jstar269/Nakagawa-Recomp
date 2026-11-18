/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored minimal stand-in for FFmpeg n4.4 libavcodec/internal.h
 * (PR-A).
 *
 * Upstream internal.h is a large private-header aggregation that also pulls
 * in libavutil/internal.h. The imported decoder subset uses three symbols
 * through it, all declared upstream in libavutil/internal.h:
 *  - avpriv_report_missing_feature() (libavutil/internal.h:166, implemented
 *    in libavutil/log.c:483), called on the CH_UNIT_EXTENSION path and the
 *    GHA amplitude-mode-0 path;
 *  - avpriv_request_sample() (libavutil/internal.h:177, implemented in
 *    libavutil/log.c:474), called by atrac3.c decode_bytes() and by the
 *    bitstream.c VLC table builder;
 *  - ff_dlog() (libavutil/internal.h:196-200), a debug-gated av_log.
 * This header re-implements them as static inlines so the standalone subset
 * needs neither libavutil/internal.h nor log.c. See src/rt/atrac3p/PROVENANCE.md.
 */

#ifndef AT3P_LIBAVCODEC_INTERNAL_H
#define AT3P_LIBAVCODEC_INTERNAL_H

#include <stdarg.h>
#include <stdio.h>
#include "libavutil/log.h"
#include "libavutil/attributes.h"

/* Upstream missing_feature_sample() (libavutil/log.c) writes the caller's
 * message followed by " is not implemented. ...\n" at AV_LOG_WARNING. These
 * stand-ins report the message at AV_LOG_ERROR as one complete line in a
 * single av_log call; the callers' format strings carry no line ending. */
static inline void avpriv_report_missing_feature(void *avcl, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    av_log(avcl, AV_LOG_ERROR, "Missing feature: %s is not implemented.\n", msg);
}

/* Upstream defines ff_dlog in libavutil/internal.h:196-200 (debug-gated
 * av_log). The imported bitstream.c VLC table builder uses it;
 * AV_DEBUG/DEBUG are not defined in this subset, so the default no-op form
 * is reproduced exactly. */
#ifdef DEBUG
#define ff_dlog(ctx, ...) av_log(ctx, AV_LOG_DEBUG, __VA_ARGS__)
#else
#define ff_dlog(ctx, ...) do { if (0) av_log(ctx, AV_LOG_DEBUG, __VA_ARGS__); } while (0)
#endif

/* Upstream implements avpriv_request_sample() in libavutil/log.c:474 via
 * missing_feature_sample(1, ...). atrac3.c decode_bytes() calls it when a
 * scrambled (RealMedia) frame's input buffer is not 4-byte aligned; the
 * project API configures only WAV-format streams, which are not scrambled,
 * and passes an aligned scratch copy. bitstream.c:263 calls it when a VLC
 * subtable index does not fit the table entry while the static tables are
 * built. */
static inline void avpriv_request_sample(void *avcl, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    av_log(avcl, AV_LOG_ERROR,
           "Missing feature (sample requested): %s is not implemented.\n", msg);
}

#endif /* AT3P_LIBAVCODEC_INTERNAL_H */
