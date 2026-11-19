// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// atrac3p_bridge_selftest.c — regression suite for the PSP ATRAC3+ HLE
// decode bridge (src/rt/atrac3p_bridge.c, PR-B).
//
// Public, source-owned tests (no game inputs, no copyrighted fixtures):
//   - create() validation (channel set {1,2,3,4,6,7,8}, block_align > 0,
//     NULL out),
//   - decode() NULL-argument rejection and the exact-frame contract
//     (frame_size must equal block_align; anything else is a contract
//     violation, not a decodable frame),
//   - the deterministic terminator frame (0x60) decodes through the bridge
//     with the nb_samples = ATRAC3P_FRAME_SAMPLES PSP contract and
//     all-zero PCM,
//   - the transform-path canary MONO unit frame decodes deterministically
//     through the bridge (reaches reconstruct_frame(); broken transform
//     configs crash here, mirroring the PR-A canary),
//   - a deterministic synthetic invalid-gain frame returns AVERROR_INVALIDDATA
//     once, stays refused on retry, and can be followed by a valid decode after
//     reset,
//   - determinism: byte-identical PCM across instances, after bridge reset,
//     and after destroy/recreate (the HLE seek/loop-rewind primitive),
//   - the plain-AT3 half (atrac3_bridge_*): config validation, create refusals,
//     the exact-frame contract with samples_out = 0, a silent frame's 1024 zero
//     samples, a decoder failure that latches without re-entering the decoder,
//     and reset clearing the latch,
//   - destroy(NULL) no-op.
//
// Exit code 0 = all checks passed.

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define SR_DUP _dup
#define SR_DUP2 _dup2
#define SR_CLOSE _close
#define SR_FILENO _fileno
#else
#include <unistd.h>
#define SR_DUP dup
#define SR_DUP2 dup2
#define SR_CLOSE close
#define SR_FILENO fileno
#endif

#include "atrac3p_bridge.h"
#include "atrac3p/atrac3p_api.h"
#include "libavutil/mem.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "vfpu_tables.h"
#include "perf.h"
#include "pcm_wav_dump.h"

/* White-box: the private title acceptance tool's container parser and entry point, compiled into
 * this TU so its refusals are checked on synthetic containers (no title input). */
#define main atrac3p_title_accept_main
#include "atrac3p_title_accept.c"
#undef main

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, name)                                                      \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);           \
        }                                                                      \
    } while (0)

/* Terminator-only frame: start bit 0, then channel-unit id 3
 * (CH_UNIT_TERMINATOR) -> 0b01100000 = 0x60. Decodes to 2048 all-zero
 * samples per channel. */
static const uint8_t TERMINATOR_FRAME[] = { 0x60 };

/* Deterministic MONO channel-unit frame; exercises the IMDCT/IPQF transform
 * pipeline (see atrac3p_selftest.c for the full rationale). */
static const uint8_t MONO_UNIT_FRAME[] = { 0x16, 0x0c, 0xf6, 0x86, 0x87, 0x35, 0xd3, 0x1c, 0xb8 };

/* Build the 196th nine-byte xorshift32 fixture from seed 0x61c88647. It is
 * synthetic bitstream data constructed for this regression; it reaches the
 * decoder's invalid gain-location refusal path. */
static void make_invalid_gain_frame(uint8_t frame[9])
{
    uint32_t state = 0x61c88647u;
    for (unsigned candidate = 0; candidate <= 195u; candidate++) {
        for (int i = 0; i < 9; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            frame[i] = (uint8_t)state;
        }
    }
}

static int count_captured_text(FILE *capture, const char *needle)
{
    long end;
    char *text;
    size_t size;
    int count = 0;

    if (fflush(stderr) != 0 || fflush(capture) != 0 ||
        fseek(capture, 0, SEEK_END) != 0)
        return -1;
    end = ftell(capture);
    if (end < 0 || fseek(capture, 0, SEEK_SET) != 0)
        return -1;
    size = (size_t)end;
    text = (char *)malloc(size + 1u);
    if (!text)
        return -1;
    size = fread(text, 1u, size, capture);
    text[size] = '\0';
    for (char *at = text; (at = strstr(at, needle)) != NULL; at += strlen(needle))
        count++;
    free(text);
    return count;
}

static int pcm_sha256_hex(const int16_t *pcm, size_t samples, char out[65])
{
    uint8_t digest[32];
    size_t i;

    sr_vfpu_sha256((const uint8_t *)pcm, samples * sizeof(int16_t), digest);
    for (i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = '\0';
    return 0;
}

static void test_create_validation(void)
{
    Atrac3pBridge *b = NULL;

    CHECK(atrac3p_bridge_create(0, 1536, &b) < 0, "reject 0 channels");
    CHECK(atrac3p_bridge_create(5, 1536, &b) < 0, "reject 5 channels");
    CHECK(atrac3p_bridge_create(9, 1536, &b) < 0, "reject 9 channels");
    CHECK(atrac3p_bridge_create(2, 0, &b) < 0, "reject block_align 0");
    CHECK(atrac3p_bridge_create(2, -1, &b) < 0, "reject negative block_align");
    CHECK(atrac3p_bridge_create(2, 1536, NULL) < 0, "reject NULL out");
    CHECK(b == NULL, "no instance left behind on failure");
    /* valid configs across the whole accepted channel set */
    {
        static const int chans[] = { 1, 2, 3, 4, 6, 7, 8 };
        for (size_t i = 0; i < sizeof(chans) / sizeof(chans[0]); i++) {
            Atrac3pBridge *ok = NULL;
            CHECK(atrac3p_bridge_create(chans[i], 1536, &ok) == 0,
                  "create valid channel count");
            atrac3p_bridge_destroy(ok);
        }
    }
}

static void test_decode_contract(void)
{
    Atrac3pBridge *b = NULL;
    int16_t pcm[ATRAC3P_MAX_CHANNELS * ATRAC3P_FRAME_SAMPLES];
    int samples = -1;

    CHECK(atrac3p_bridge_create(2, 1536, &b) == 0, "create stereo bridge");
    if (!b) return;

    /* NULL argument rejection: every required pointer is validated before any
     * decoder state is touched. */
    CHECK(atrac3p_bridge_decode(NULL, TERMINATOR_FRAME, 1, pcm, &samples) < 0,
          "reject NULL bridge");
    CHECK(atrac3p_bridge_decode(b, NULL, 1, pcm, &samples) < 0,
          "reject NULL frame");
    CHECK(atrac3p_bridge_decode(b, TERMINATOR_FRAME, 1, NULL, &samples) < 0,
          "reject NULL pcm_out");
    CHECK(atrac3p_bridge_decode(b, TERMINATOR_FRAME, 1, pcm, NULL) < 0,
          "reject NULL samples_out");

    /* Exact-frame contract: the HLE feeds one blockAlign-sized frame. A
     * different size is a caller contract violation and must be rejected
     * with samples_out left at 0, not partially decoded. */
    samples = -1;
    CHECK(atrac3p_bridge_decode(b, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm, &samples) < 0,
          "reject frame_size != block_align (too small)");
    CHECK(samples == 0, "samples_out zeroed on contract violation");

    atrac3p_bridge_destroy(b);
}

static void test_terminator_decode(void)
{
    Atrac3pBridge *b1 = NULL, *b2 = NULL;
    int16_t pcm1[2 * ATRAC3P_FRAME_SAMPLES];
    int16_t pcm2[2 * ATRAC3P_FRAME_SAMPLES];
    char sha1[65], sha2[65];
    int ret, samples = -1, i;
    const size_t n = 2 * (size_t)ATRAC3P_FRAME_SAMPLES;

    /* The terminator canary uses block_align 1 in the PR-A suite; here the
     * bridge enforces the exact-frame contract, so the frame must fill the
     * configured block_align. Create with block_align 1 (a legal PSP-adjacent
     * config for a single-byte frame) and stereo output. */
    CHECK(atrac3p_bridge_create(2, 1, &b1) == 0, "create h1 (terminator)");
    CHECK(atrac3p_bridge_create(2, 1, &b2) == 0, "create h2 (terminator)");
    if (!b1 || !b2) return;

    ret = atrac3p_bridge_decode(b1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm1, &samples);
    CHECK(ret == 0, "terminator decode succeeds through bridge");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "bridge nb_samples = 2048");
    for (i = 0; i < (int)n; i++)
        CHECK(pcm1[i] == 0, "terminator PCM all-zero through bridge");

    /* determinism across instances */
    CHECK(atrac3p_bridge_decode(b2, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm2, &samples) == 0, "h2 terminator decode");
    pcm_sha256_hex(pcm1, n, sha1);
    pcm_sha256_hex(pcm2, n, sha2);
    CHECK(strcmp(sha1, sha2) == 0, "deterministic PCM across bridge instances");

    /* reset restores post-create state (HLE seek / loop-rewind primitive) */
    CHECK(atrac3p_bridge_reset(b1) == 0, "bridge reset");
    CHECK(atrac3p_bridge_decode(b1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm1, &samples) == 0, "decode after reset");
    pcm_sha256_hex(pcm1, n, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "PCM after bridge reset identical");

    /* destroy/recreate produces the same output (fresh-instance determinism) */
    atrac3p_bridge_destroy(b1);
    b1 = NULL;
    CHECK(atrac3p_bridge_create(2, 1, &b1) == 0, "recreate bridge");
    CHECK(atrac3p_bridge_decode(b1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                                pcm1, &samples) == 0, "decode after recreate");
    pcm_sha256_hex(pcm1, n, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "PCM after recreate identical");
    CHECK(nk_pcm_wav_dump("atrac3p_bridge_terminator", pcm1, ATRAC3P_FRAME_SAMPLES, 2u,
                          44100u) >= 0,
          "terminator WAV dump");

    atrac3p_bridge_destroy(b1);
    atrac3p_bridge_destroy(b2);
}

static void test_imdct_path_decode(void)
{
    Atrac3pBridge *b1 = NULL, *b2 = NULL;
    int16_t pcm1[ATRAC3P_FRAME_SAMPLES];
    int16_t pcm2[ATRAC3P_FRAME_SAMPLES];
    char sha1[65], sha2[65];
    int ret, samples = -1;

    /* The bridge enforces the exact-frame contract (frame_size == block_align),
     * so the canary is configured with block_align == its own size; the decoder
     * behaviour is identical to the PR-A canary (which used a wider align). */
    CHECK(atrac3p_bridge_create(1, (int)sizeof(MONO_UNIT_FRAME), &b1) == 0,
          "create bridge h1");
    CHECK(atrac3p_bridge_create(1, (int)sizeof(MONO_UNIT_FRAME), &b2) == 0,
          "create bridge h2");
    if (!b1 || !b2) return;

    ret = atrac3p_bridge_decode(b1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                                pcm1, &samples);
    CHECK(ret == 0, "mono unit decode through bridge");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "mono unit bridge nb_samples");

    CHECK(atrac3p_bridge_decode(b2, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                                pcm2, &samples) == 0, "h2 mono unit decode");
    pcm_sha256_hex(pcm1, ATRAC3P_FRAME_SAMPLES, sha1);
    pcm_sha256_hex(pcm2, ATRAC3P_FRAME_SAMPLES, sha2);
    CHECK(strcmp(sha1, sha2) == 0, "mono unit PCM deterministic across bridges");

    CHECK(atrac3p_bridge_reset(b1) == 0, "reset bridge h1");
    CHECK(atrac3p_bridge_decode(b1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                                pcm1, &samples) == 0, "mono unit after reset");
    pcm_sha256_hex(pcm1, ATRAC3P_FRAME_SAMPLES, sha1);
    CHECK(strcmp(sha1, sha2) == 0, "mono unit PCM deterministic after reset");
    /* NK_PCM_WAV_DIR=<dir> keeps the canary PCM as a WAV file for listening. */
    CHECK(nk_pcm_wav_dump("atrac3p_bridge_mono_unit", pcm1, ATRAC3P_FRAME_SAMPLES, 1u,
                          44100u) >= 0,
          "mono unit WAV dump");

    atrac3p_bridge_destroy(b1);
    atrac3p_bridge_destroy(b2);
}

static void test_invalid_gain_frame_refuses_once(void)
{
    Atrac3pBridge *bridge = NULL;
    int16_t pcm[ATRAC3P_FRAME_SAMPLES];
    uint8_t invalid_frame[9];
    FILE *capture = NULL;
    int stderr_fd = SR_FILENO(stderr);
    int saved_stderr = -1;
    int ret1 = 0, ret2 = 0, samples1 = -1, samples2 = -1;
    int error_count = -1;
    int redirected = 0;

    CHECK(atrac3p_bridge_create(1, 9, &bridge) == 0,
          "create bridge for synthetic invalid gain frame");
    if (!bridge) return;
    capture = tmpfile();
    CHECK(capture != NULL, "create synthetic decoder log capture");
    if (!capture) {
        atrac3p_bridge_destroy(bridge);
        return;
    }
    /* POSIX dup2 returns the new descriptor on success, MSVCRT _dup2 returns 0;
     * both return -1 on failure, so only that value means the redirect did not
     * happen. Testing for 0 left Linux with stderr already pointing at the
     * capture file while the test believed it had failed, so every later line,
     * the summary included, vanished into the deleted temporary file. */
    saved_stderr = SR_DUP(stderr_fd);
    if (saved_stderr >= 0 && SR_DUP2(SR_FILENO(capture), stderr_fd) != -1)
        redirected = 1;
    CHECK(redirected, "capture decoder error without exposing fixture output");
    if (!redirected) {
        if (saved_stderr >= 0) SR_CLOSE(saved_stderr);
        fclose(capture);
        atrac3p_bridge_destroy(bridge);
        return;
    }

    make_invalid_gain_frame(invalid_frame);
    ret1 = atrac3p_bridge_decode(bridge, invalid_frame, sizeof(invalid_frame),
                                 pcm, &samples1);
    ret2 = atrac3p_bridge_decode(bridge, invalid_frame, sizeof(invalid_frame),
                                 pcm, &samples2);
    fflush(stderr);
    SR_DUP2(saved_stderr, stderr_fd);
    SR_CLOSE(saved_stderr);
    error_count = count_captured_text(capture, "Invalid gain location");

    CHECK(ret1 == AVERROR_INVALIDDATA,
          "invalid gain frame returns the named invalid-data result");
    CHECK(ret2 == AVERROR_INVALIDDATA,
          "repeated decode request keeps the named failure result");
    CHECK(samples1 == 0 && samples2 == 0,
          "invalid gain frame never reports decoded samples");
    if (error_count != 1)
        fprintf(stderr, "invalid gain error calls=%d\n", error_count);
    CHECK(error_count == 1,
          "invalid gain path is entered once across repeated decode requests");

    CHECK(atrac3p_bridge_reset(bridge) == 0,
          "reset clears the failed-frame refusal");
    int valid_samples = -1;
    int valid_ret = atrac3p_bridge_decode(bridge, MONO_UNIT_FRAME,
                                         sizeof(MONO_UNIT_FRAME), pcm,
                                         &valid_samples);
    CHECK(valid_ret == 0 && valid_samples == ATRAC3P_FRAME_SAMPLES,
          "valid synthetic frame decodes after reset");

    fclose(capture);
    atrac3p_bridge_destroy(bridge);
}

/* Plain ATRAC3 (RIFF format 0x0270): the 14-byte WAV fmt extension.  Bytes 6-7 are the
 * coding mode (0 = one sound unit per channel, nonzero = joint stereo) and bytes 10-11 the
 * frame factor; the others are not gates (atrac3_config_valid). */
static void at3_extradata(uint8_t ext[14], unsigned coding_mode, unsigned frame_factor)
{
    memset(ext, 0, 14);
    ext[0] = 1;
    ext[6] = (uint8_t)coding_mode;
    ext[8] = (uint8_t)coding_mode;
    ext[10] = (uint8_t)frame_factor;
}

/* A mono 96-byte plain AT3 frame that decodes to silence: sound unit id 0x28 (101000b),
 * then zero coded bands, gain points, tonal components and subband VLC indices.  An
 * all-zero frame carries unit id 0 and is refused as invalid data. */
#define AT3_MONO_ALIGN 96
static void make_at3_silent_frame(uint8_t frame[AT3_MONO_ALIGN])
{
    memset(frame, 0, AT3_MONO_ALIGN);
    frame[0] = 0xA0;
}

static void test_atrac3_bridge(void)
{
    uint8_t ext[14], good[AT3_MONO_ALIGN], bad[AT3_MONO_ALIGN];
    int16_t pcm[2 * ATRAC3_FRAME_SAMPLES];
    Atrac3Bridge *b = NULL;
    int samples = -1;

    /* Configuration: the format tag, mono or stereo, the 14-byte extension, a frame factor,
     * joint stereo only with two channels, and a block size of 96, 152 or 192 bytes per
     * channel per frame factor. */
    at3_extradata(ext, 0, 1);
    CHECK(atrac3_config_valid(0x0270, 1, 96, ext, 14), "at3: a mono 96-byte config is valid");
    CHECK(atrac3_config_valid(0x0270, 2, 304, ext, 14), "at3: a stereo 304-byte config is valid");
    CHECK(!atrac3_config_valid(0xFFFE, 1, 96, ext, 14), "at3: another format tag is refused");
    CHECK(!atrac3_config_valid(0x0270, 3, 288, ext, 14), "at3: three channels are refused");
    CHECK(!atrac3_config_valid(0x0270, 1, 100, ext, 14), "at3: an off-table block size is refused");
    CHECK(!atrac3_config_valid(0x0270, 1, 96, ext, 10), "at3: a 10-byte extension is refused");
    CHECK(!atrac3_config_valid(0x0270, 1, 96, NULL, 14), "at3: a missing extension is refused");
    at3_extradata(ext, 0, 0);
    CHECK(!atrac3_config_valid(0x0270, 1, 96, ext, 14), "at3: frame factor 0 is refused");
    at3_extradata(ext, 1, 1);
    CHECK(!atrac3_config_valid(0x0270, 1, 96, ext, 14), "at3: joint stereo on one channel is refused");
    CHECK(atrac3_config_valid(0x0270, 2, 192, ext, 14), "at3: joint stereo on two channels is valid");

    /* create() refuses what the config check refuses and leaves no instance behind. */
    at3_extradata(ext, 0, 1);
    b = (Atrac3Bridge *)&samples;   /* not NULL, so the failure must clear it */
    CHECK(atrac3_bridge_create(1, 100, ext, &b) < 0 && b == NULL,
          "at3: create refuses an off-table block size and clears its output");
    CHECK(atrac3_bridge_create(1, 96, NULL, &b) < 0 && b == NULL,
          "at3: create refuses a missing extension");
    CHECK(atrac3_bridge_create(1, 96, ext, NULL) < 0, "at3: create refuses a NULL output");
    CHECK(atrac3_bridge_create(1, 96, ext, &b) == 0 && b != NULL, "at3: create a mono bridge");
    if (!b) return;

    make_at3_silent_frame(good);
    memset(bad, 0, sizeof(bad));

    /* The exact-frame contract: NULL arguments and a frame of another size are refused with
     * samples_out at 0, and such a refusal is not a decoder failure that latches. */
    CHECK(atrac3_bridge_decode(NULL, good, AT3_MONO_ALIGN, pcm, &samples) < 0 &&
              atrac3_bridge_decode(b, NULL, AT3_MONO_ALIGN, pcm, &samples) < 0 &&
              atrac3_bridge_decode(b, good, AT3_MONO_ALIGN, NULL, &samples) < 0 &&
              atrac3_bridge_decode(b, good, AT3_MONO_ALIGN, pcm, NULL) < 0,
          "at3: decode refuses each NULL argument");
    samples = -1;
    CHECK(atrac3_bridge_decode(b, good, AT3_MONO_ALIGN - 1, pcm, &samples) == AVERROR(EINVAL) &&
              samples == 0,
          "at3: a frame shorter than the block is refused with no samples");
    memset(pcm, 0x55, sizeof(pcm));
    samples = -1;
    int ret = atrac3_bridge_decode(b, good, AT3_MONO_ALIGN, pcm, &samples);
    int silent = 1;
    for (int i = 0; i < ATRAC3_FRAME_SAMPLES; i++)
        if (pcm[i] != 0) silent = 0;
    CHECK(ret == 0 && samples == ATRAC3_FRAME_SAMPLES && silent,
          "at3: a silent frame decodes to 1024 zero samples after a size refusal");

    /* A decoder failure latches: the bad frame enters the decoder once, a later good frame
     * gets the same error without decoding, and only reset() clears it. */
    FILE *capture = tmpfile();
    int stderr_fd = SR_FILENO(stderr);
    int saved_stderr = capture ? SR_DUP(stderr_fd) : -1;
    int redirected = saved_stderr >= 0 && SR_DUP2(SR_FILENO(capture), stderr_fd) != -1;
    CHECK(redirected, "at3: capture the decoder's error line");
    int samples1 = -1, samples2 = -1;
    int ret1 = atrac3_bridge_decode(b, bad, AT3_MONO_ALIGN, pcm, &samples1);
    int ret2 = atrac3_bridge_decode(b, good, AT3_MONO_ALIGN, pcm, &samples2);
    int entered = -1;
    if (redirected) {
        fflush(stderr);
        SR_DUP2(saved_stderr, stderr_fd);
        entered = count_captured_text(capture, "Sound Unit id != 0x28");
    }
    if (saved_stderr >= 0) SR_CLOSE(saved_stderr);
    if (capture) fclose(capture);
    CHECK(ret1 == AVERROR_INVALIDDATA && samples1 == 0,
          "at3: a frame with a bad sound unit id is refused as invalid data, with no samples");
    CHECK(ret2 == AVERROR_INVALIDDATA && samples2 == 0,
          "at3: the refusal latches, so a good frame after it gets the same error");
    CHECK(entered == 1, "at3: the latched refusal does not re-enter the decoder");
    CHECK(atrac3_bridge_reset(b) == 0, "at3: reset succeeds");
    samples = -1;
    CHECK(atrac3_bridge_decode(b, good, AT3_MONO_ALIGN, pcm, &samples) == 0 &&
              samples == ATRAC3_FRAME_SAMPLES,
          "at3: reset clears the latch and the good frame decodes");
    CHECK(atrac3_bridge_reset(NULL) < 0, "at3: reset refuses a NULL bridge");

    atrac3_bridge_destroy(b);
    atrac3_bridge_destroy(NULL);   /* no-op */
}

/* A synthetic RIFF/WAVE ATRAC3+ container of the layout the title tool parses: fmt (16 bytes),
 * fact (4 bytes), then `frames` copies of `frame` (and the pad byte an odd chunk takes).  Returns its
 * size, or 0 when it does not fit in `cap`. */
static size_t title_riff(uint8_t *out, size_t cap, uint16_t channels, uint32_t rate, uint16_t align,
                         uint32_t fact, uint32_t frames, const uint8_t *frame)
{
    const size_t data = (size_t)frames * align;
    const size_t total = 12u + 8u + 16u + 8u + 4u + 8u + data + (data & 1u);   /* RIFF pad byte */
    if (total > cap) return 0;
    memset(out, 0, total);
    memcpy(out, "RIFF", 4u);
    const uint32_t riff = (uint32_t)(total - 8u);
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(riff >> (8 * i));
    memcpy(out + 8, "WAVEfmt ", 8u);
    out[16] = 16u;
    out[20] = 0xfeu; out[21] = 0xffu;   /* WAVE_FORMAT_EXTENSIBLE, as the title stream */
    out[22] = (uint8_t)channels;
    for (int i = 0; i < 4; i++) out[24 + i] = (uint8_t)(rate >> (8 * i));
    out[32] = (uint8_t)align; out[33] = (uint8_t)(align >> 8);
    memcpy(out + 36, "fact", 4u);
    out[40] = 4u;
    for (int i = 0; i < 4; i++) out[44 + i] = (uint8_t)(fact >> (8 * i));
    memcpy(out + 48, "data", 4u);
    for (int i = 0; i < 4; i++) out[52 + i] = (uint8_t)((uint32_t)data >> (8 * i));
    for (uint32_t f = 0; f < frames; f++) memcpy(out + 56 + (size_t)f * align, frame, align);
    return total;
}

static void test_title_accept_refusals(void)
{
    uint8_t buf[256];
    TitleStream ts;
    size_t n = title_riff(buf, sizeof(buf), 1u, 44100u, sizeof(MONO_UNIT_FRAME), 2048u, 1u,
                          MONO_UNIT_FRAME);
    CHECK(n && accept_parse(buf, n, &ts) && ts.channels == 1u && ts.fact_samples == 2048u,
          "title-accept: a well-formed synthetic container parses");
    /* A fact count past 0x7fffffff, as atrac_parse_track refuses: its frame rounding once wrapped
     * in 32 bits, so a stream far shorter than it declared passed the truncation check. */
    n = title_riff(buf, sizeof(buf), 1u, 44100u, sizeof(MONO_UNIT_FRAME), 0xfffff900u, 1u,
                   MONO_UNIT_FRAME);
    CHECK(n && !accept_parse(buf, n, &ts), "title-accept: a fact count above 0x7fffffff is refused");
    CHECK(accept_fact_frames(0xffffffffu) == 0x200000u && accept_fact_frames(2049u) == 2u,
          "title-accept: the declared frame count rounds up without wrapping");
    /* A zero sample rate has no duration: refused by name, not printed as inf. */
    n = title_riff(buf, sizeof(buf), 1u, 0u, sizeof(MONO_UNIT_FRAME), 2048u, 1u, MONO_UNIT_FRAME);
    CHECK(n && !accept_parse(buf, n, &ts), "title-accept: a zero sample rate is refused");
    /* Only a missing input is SKIP (77); one that exists but cannot be opened as a file (here a
     * directory) is a FAIL, never "not run". */
    char missing[] = "atrac3p-title-accept-input-that-does-not-exist.sgb";
    char dir[] = ".";
    char tool[] = "atrac3p_title_accept";
    char *argv_missing[] = { tool, missing, NULL };
    char *argv_dir[] = { tool, dir, NULL };
    CHECK(atrac3p_title_accept_main(2, argv_missing) == 77,
          "title-accept: a missing input is SKIP (77)");
    CHECK(atrac3p_title_accept_main(2, argv_dir) == 1,
          "title-accept: an input that exists but cannot be read is FAIL, not SKIP");
}

// av_size_mult() is the overflow-checked multiply the decoder's array helpers
// call before any allocation. The vendored n4.4 form carried a glibc-inspired
// bit trick that skipped the division below sqrt(SIZE_MAX); this project's
// LOCAL DELTA (src/rt/atrac3p/libavutil/mem.h) decides every operand pair. The
// checks below pin the contract the callers depend on: a product that fits is
// returned unchanged, a product that does not is refused with AVERROR(EINVAL)
// and leaves *r alone, and zero never divides.
static void test_av_size_mult(void)
{
    size_t res = 0;

    /* Products that fit are returned as they are. */
    CHECK(av_size_mult(5, 10, &res) == 0 && res == 50,
          "av_size_mult multiplies in range");
    CHECK(av_size_mult(1, 100, &res) == 0 && res == 100,
          "av_size_mult by one");
    CHECK(av_size_mult(1, SIZE_MAX, &res) == 0 && res == SIZE_MAX,
          "av_size_mult 1 * SIZE_MAX");
    CHECK(av_size_mult(SIZE_MAX, 1, &res) == 0 && res == SIZE_MAX,
          "av_size_mult SIZE_MAX * 1");
    CHECK(av_size_mult(SIZE_MAX / 2, 2, &res) == 0 && res == (SIZE_MAX / 2) * 2,
          "av_size_mult (SIZE_MAX / 2) * 2");

    /* A zero operand is a valid product, not a division by zero. */
    CHECK(av_size_mult(0, 12345, &res) == 0 && res == 0,
          "av_size_mult zero left operand");
    CHECK(av_size_mult(12345, 0, &res) == 0 && res == 0,
          "av_size_mult zero right operand");
    CHECK(av_size_mult(0, 0, &res) == 0 && res == 0,
          "av_size_mult zero both operands");

    /* A product that does not fit is refused, and *r is not written. */
    res = 7;
    CHECK(av_size_mult(SIZE_MAX, 2, &res) == AVERROR(EINVAL) && res == 7,
          "av_size_mult SIZE_MAX * 2 overflows and leaves *r");
    res = 7;
    CHECK(av_size_mult(2, SIZE_MAX, &res) == AVERROR(EINVAL) && res == 7,
          "av_size_mult 2 * SIZE_MAX overflows and leaves *r");
    CHECK(av_size_mult(SIZE_MAX, SIZE_MAX, &res) == AVERROR(EINVAL),
          "av_size_mult SIZE_MAX * SIZE_MAX overflows");
    CHECK(av_size_mult((SIZE_MAX / 2) + 1, 2, &res) == AVERROR(EINVAL),
          "av_size_mult first product above half overflows");
}

int main(void)
{
    sr_perf_init();
    test_create_validation();
    test_decode_contract();
    test_terminator_decode();
    test_imdct_path_decode();
    test_invalid_gain_frame_refuses_once();
    test_atrac3_bridge();
    test_title_accept_refusals();
    test_av_size_mult();

    atrac3p_bridge_destroy(NULL);   /* no-op */

    if (g_failures == 0)
        fprintf(stderr, "ATRAC3P-BRIDGE-SELFTEST: all %d checks passed\n", g_checks);
    else
        fprintf(stderr, "ATRAC3P-BRIDGE-SELFTEST: %d/%d checks FAILED\n",
                g_failures, g_checks);
    return g_failures ? 1 : 0;
}
