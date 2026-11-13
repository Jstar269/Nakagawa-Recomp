// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// fbcap_selftest.c - selftest for the presenter-neutral frame capture service (fbcap.c).
//
// Drives the public API with synthetic frames and checks exactly what is published:
//   - a host-served capture (the offscreen and GDI presenters) publishes an exact P6 PPM:
//     header, size, R,G,B channel order and every pixel byte;
//   - a read-back capture (the sdl3vk presenter) publishes the same bytes for the same frame,
//     so a capture means the same thing whichever presenter showed the frame;
//   - a skipped present (cancel) and a refused re-arm never publish or report stale results;
//   - a failed publication or present resolves as failed (-1), never as success;
//   - the publisher creates the parent directory it needs;
//   - SR_FBSNAP_WINDOWS alone selects the FBSNAP slot with every present captured
//     (fbcap_policy.c), while an explicit SR_FBSNAP keeps its meaning;
//   - the texture dump's PNG encoder (fixed-Huffman deflate) inflates, through the project's
//     own inflater (nk_psp_inflate.c), to exactly the image, and shrinks a flat one;
//   - sr_file_publish_new never replaces a file that is already there, leaves no temp file,
//     and, like a capture, writes into a folder whose name is not ASCII; its tagged form (the
//     dump's workers) does the same with the caller's temp names;
//   - sr_png_encode_bound is never passed, and the dump folder check refuses what a texture pack
//     folder check refuses (ill-formed UTF-8, a relative path, 512 bytes or more, a device path).
// No game data, no GPU, no window: exit 0 = every check passed.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "fbcap.h"
#include "fbcap_policy.h" /* declares sr_fbcap_path with size_t: after <stdio.h> */
#include "nk_platform.h"    /* wide-path (UTF-8) file calls for the non-ASCII folder */
#include "nk_psp_inflate.h" /* the in-tree inflater the encoder is checked against */

/* Scratch root for the files this test writes: the checkout's build/ by default, or
 * the BUILD_ROOT the Makefile passes as -DSR_SELFTEST_BUILD_ROOT, so a scratch run never
 * touches the checkout. */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#define PSP_W 480u
#define PSP_H 272u

static int s_failures;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "fbcap_selftest FAIL: %s\n", (msg)); \
            s_failures++;                                        \
        }                                                        \
    } while (0)

static char s_dir[512];
static uint32_t s_frame[PSP_W * PSP_H];               /* host frame: 0x00RRGGBB words */
static unsigned char s_frame_rgb[PSP_W * PSP_H * 3];  /* the P6 body it must publish */

/* Path of a named file inside the selftest output directory. */
static void out_path(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "%s/%s", s_dir, name);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void make_dir(const char *p) {
#ifdef _WIN32
    (void)_mkdir(p);
#else
    (void)mkdir(p, 0777);
#endif
}

static void remove_dir(const char *p) {
#ifdef _WIN32
    (void)_rmdir(p);
#else
    (void)rmdir(p);
#endif
}

/* Fill the synthetic host frame, the layout gui.c's converted s_px uses, and the RGB
 * stream a correct publisher writes for it. Every channel varies on both axes. */
static void make_frame(void) {
    for (uint32_t y = 0; y < PSP_H; y++)
        for (uint32_t x = 0; x < PSP_W; x++) {
            uint32_t r = (x * 255u) / PSP_W;
            uint32_t g = (y * 255u) / PSP_H;
            uint32_t b = ((x + y) * 255u) / (PSP_W + PSP_H);
            size_t i = (size_t)y * PSP_W + x;
            s_frame[i] = (r << 16) | (g << 8) | b;
            s_frame_rgb[i * 3 + 0] = (unsigned char)r;
            s_frame_rgb[i * 3 + 1] = (unsigned char)g;
            s_frame_rgb[i * 3 + 2] = (unsigned char)b;
        }
}

/* Read a whole file and compare it with the expected P6 header and RGB body, byte for
 * byte, including that nothing trails the body. */
static int ppm_matches(const char *path, uint32_t w, uint32_t h, const unsigned char *rgb) {
    char header[64];
    int n = snprintf(header, sizeof header, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    size_t body = (size_t)w * h * 3u;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *got = (unsigned char *)malloc((size_t)n + body + 1u);
    if (!got) { fclose(f); return 0; }
    size_t len = fread(got, 1, (size_t)n + body + 1u, f);
    fclose(f);
    int ok = len == (size_t)n + body && memcmp(got, header, (size_t)n) == 0 &&
             memcmp(got + n, rgb, body) == 0;
    free(got);
    return ok;
}

/* Both files exist and hold identical bytes. */
static int files_identical(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    int ok = fa && fb;
    while (ok) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) ok = 0;
        if (ca == EOF || cb == EOF) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return ok;
}

/* Invalid requests are refused and arm nothing. */
static void test_refused_requests(void) {
    char long_path[1100];
    memset(long_path, 'a', sizeof long_path - 1u);
    long_path[sizeof long_path - 1u] = '\0';
    CHECK(sr_capture_arm(NULL) == 0, "refuse: NULL path");
    CHECK(sr_capture_arm("") == 0, "refuse: empty path");
    CHECK(sr_capture_arm(long_path) == 0, "refuse: path longer than the capture slot");
    CHECK(!sr_capture_is_armed(), "refuse: nothing armed after refused requests");
    CHECK(sr_capture_result() == 0, "refuse: refused requests report nothing attempted");
}

/* A host-served capture (offscreen/GDI) publishes exactly the presented frame. */
static void test_host_capture(void) {
    char path[600];
    out_path(path, sizeof path, "host.ppm");
    remove(path);
    /* Pixel (479,0) is r=254 g=0 b=162: pins the R,G,B order of the published body. */
    CHECK(s_frame_rgb[479 * 3 + 0] == 254 && s_frame_rgb[479 * 3 + 1] == 0 &&
          s_frame_rgb[479 * 3 + 2] == 162, "fixture: channel-order sentinel");
    CHECK(sr_capture_arm(path) == 1, "host: arm accepted");
    CHECK(sr_capture_is_armed(), "host: armed state visible to presenters");
    CHECK(sr_capture_result() == 0, "host: nothing attempted before service");
    CHECK(strcmp(sr_capture_source_label(), "") == 0, "host: no source before service");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "host: serve publishes");
    CHECK(sr_capture_result() == 1, "host: result is published");
    CHECK(strcmp(sr_capture_source_label(), "cpu-framebuffer") == 0, "host: source label");
    CHECK(ppm_matches(path, PSP_W, PSP_H, s_frame_rgb), "host: PPM header and pixels exact");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 0,
          "host: a present with nothing armed publishes nothing");
}

/* The sdl3vk presenter reads its B8G8R8A8 framebuffer image back instead of serving the
 * host frame. For the same frame the published file must be byte-identical to the
 * host-served one, so offscreen and windowed captures can be compared directly. */
static void test_readback_matches_host_capture(void) {
    char host[600], readback[600];
    out_path(host, sizeof host, "same_host.ppm");
    out_path(readback, sizeof readback, "same_readback.ppm");
    remove(host);
    remove(readback);
    CHECK(sr_capture_arm(host) == 1, "same: host arm");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "same: host publish");
    CHECK(sr_capture_arm(readback) == 1, "same: readback arm");
    sr_capture_mark_recorded(SR_CAP_SRC_CPU, PSP_W, PSP_H);
    CHECK(sr_capture_is_recorded() && !sr_capture_is_armed(), "same: recorded, not armed");
    CHECK(sr_capture_publish_recorded(s_frame, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "same: readback publish");
    CHECK(strcmp(sr_capture_source_label(), "cpu-framebuffer") == 0, "same: readback label");
    CHECK(files_identical(host, readback), "same: host-served and read-back files identical");
}

/* A GE render target is read back as R8G8B8A8 with a padded row pitch: channels publish
 * in order and the row padding never reaches the file. */
static void test_gpu_readback(void) {
    enum { GW = 8, GH = 4, GROW = GW * 4 + 8 };
    static unsigned char buf[GROW * GH];
    static unsigned char expect[GW * GH * 3];
    char path[600];
    out_path(path, sizeof path, "gpu.ppm");
    remove(path);
    memset(buf, 0xAB, sizeof buf);
    for (unsigned y = 0; y < GH; y++)
        for (unsigned x = 0; x < GW; x++) {
            unsigned char *p = buf + y * GROW + x * 4u;
            p[0] = (unsigned char)(x * 10u);       /* R */
            p[1] = (unsigned char)(y * 20u);       /* G */
            p[2] = (unsigned char)((x + y) * 7u);  /* B */
            p[3] = 0xEEu;
            size_t i = ((size_t)y * GW + x) * 3u;
            expect[i + 0] = p[0];
            expect[i + 1] = p[1];
            expect[i + 2] = p[2];
        }
    CHECK(sr_capture_arm(path) == 1, "gpu: arm accepted");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, GW, GH);
    CHECK(sr_capture_arm(path) == 0, "gpu: a re-arm while the readback is pending is refused");
    sr_capture_cancel();
    CHECK(sr_capture_is_recorded(), "gpu: cancel never drops a recorded readback");
    CHECK(sr_capture_serve_host(buf, GW, GH, GROW, SR_CAP_ORDER_RGBX) == 0,
          "gpu: a host present never services a recorded readback");
    CHECK(sr_capture_publish_recorded(buf, GROW, SR_CAP_ORDER_RGBX) == 1, "gpu: publish");
    CHECK(sr_capture_result() == 1, "gpu: result is published");
    CHECK(strcmp(sr_capture_source_label(), "gpu-render-target") == 0, "gpu: source label");
    CHECK(!sr_capture_is_recorded(), "gpu: publish resolves the recorded state");
    CHECK(ppm_matches(path, GW, GH, expect), "gpu: RGB order, pitch and pixels exact");
}

/* A skipped present cancels the arm: nothing is published for it, a later present cannot
 * service it, and the next arm publishes only its own frame. */
static void test_skipped_present_cancels(void) {
    char skipped[600], next[600];
    out_path(skipped, sizeof skipped, "skipped.ppm");
    out_path(next, sizeof next, "after_skip.ppm");
    remove(skipped);
    remove(next);
    CHECK(sr_capture_arm(skipped) == 1, "skip: arm accepted");
    sr_capture_cancel();
    CHECK(sr_capture_result() == 0, "skip: cancel resolves as nothing attempted");
    CHECK(!sr_capture_is_armed(), "skip: cancel clears the armed state");
    CHECK(strcmp(sr_capture_source_label(), "") == 0, "skip: no source for a cancelled arm");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 0,
          "skip: a later present must not service the cancelled arm");
    CHECK(!file_exists(skipped), "skip: no file for the cancelled arm");
    CHECK(sr_capture_arm(next) == 1, "skip: a fresh arm after cancel");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "skip: fresh arm publishes");
    CHECK(file_exists(next) && !file_exists(skipped), "skip: only the fresh path is written");
}

/* A refused re-arm keeps the pending capture and clears only the reportable result. */
static void test_refused_rearm_keeps_pending(void) {
    char first[600], second[600];
    out_path(first, sizeof first, "first.ppm");
    out_path(second, sizeof second, "second.ppm");
    remove(first);
    remove(second);
    CHECK(sr_capture_arm(first) == 1, "rearm: first arm accepted");
    CHECK(sr_capture_arm(second) == 0, "rearm: second arm refused while pending");
    CHECK(sr_capture_result() == 0, "rearm: refused arm exposes no stale result");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "rearm: pending capture still serviced");
    CHECK(file_exists(first) && !file_exists(second), "rearm: the pending path is written");
}

/* A failed publication resolves as failed (-1), never as success, and does not wedge the
 * service: the next arm works. */
static void test_failed_publication(void) {
    char blocked[600], next[600];
    out_path(blocked, sizeof blocked, "blocked.ppm");
    out_path(next, sizeof next, "next.ppm");
    make_dir(blocked);                     /* a directory cannot be replaced by the file */
    remove(next);
    CHECK(sr_capture_arm(blocked) == 1, "fail: arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == -1,
          "fail: an unwritable destination is attempted-and-failed");
    CHECK(sr_capture_result() == -1, "fail: result is -1, never a fabricated success");
    CHECK(sr_capture_arm(next) == 1, "fail: the service is not wedged after a failure");
    CHECK(sr_capture_result() == 0, "fail: a new arm clears the previous failure");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "fail: the next capture publishes");
    CHECK(ppm_matches(next, PSP_W, PSP_H, s_frame_rgb), "fail: the next file is exact");
}

/* Failures before publication (a failed present, an invalid pixel description) resolve
 * as failed and publish nothing. */
static void test_explicit_failures(void) {
    char present_failed[600], bad_pitch[600];
    out_path(present_failed, sizeof present_failed, "present_failed.ppm");
    out_path(bad_pitch, sizeof bad_pitch, "bad_pitch.ppm");
    remove(present_failed);
    remove(bad_pitch);
    CHECK(sr_capture_arm(present_failed) == 1, "explicit: arm accepted");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, 8, 4);
    sr_capture_fail("synthetic present failure");
    CHECK(sr_capture_result() == -1, "explicit: fail resolves a recorded capture as -1");
    CHECK(!sr_capture_is_recorded(), "explicit: fail clears the recorded state");
    CHECK(sr_capture_publish_recorded(s_frame, PSP_W * 4u, SR_CAP_ORDER_RGBX) == 0,
          "explicit: a failed capture cannot be published later");
    CHECK(sr_capture_result() == -1 && !file_exists(present_failed),
          "explicit: the failure stands and no file exists");

    CHECK(sr_capture_arm(bad_pitch) == 1, "explicit: second arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u - 1u, SR_CAP_ORDER_BGRX) == -1,
          "explicit: a row pitch shorter than the pixels is refused");
    CHECK(sr_capture_result() == -1 && !file_exists(bad_pitch),
          "explicit: invalid pixels publish nothing");
}

/* The publisher creates the parent directories it needs (FBSNAP writes under
 * build/snapshots/, which no build step creates). */
static void test_parent_directory(void) {
    char path[600], sub[600], nested[600];
    out_path(path, sizeof path, "nested/sub/frame_v1.ppm");
    out_path(sub, sizeof sub, "nested/sub");
    out_path(nested, sizeof nested, "nested");
    remove(path);
    remove_dir(sub);
    remove_dir(nested);
    CHECK(!file_exists(path), "parent: no file left from an earlier run");
    CHECK(sr_capture_arm(path) == 1, "parent: arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "parent: publish creates nested/sub");
    CHECK(ppm_matches(path, PSP_W, PSP_H, s_frame_rgb), "parent: file exact in the new dir");
}

/* An absolute destination publishes too: on Windows its first prefix is a drive root such as
 * "C:/", which mkdir reports as an error (EACCES) even though it exists. */
static void test_absolute_path(void) {
    char cwd[512] = "", base[1100], path[1200], sub[1200], top[1200];
    int absolute = s_dir[0] == '/' || s_dir[0] == '\\' || (s_dir[0] && s_dir[1] == ':');
    if (!absolute) {
#ifdef _WIN32
        CHECK(_getcwd(cwd, (int)sizeof cwd) != NULL, "absolute: current directory");
#else
        CHECK(getcwd(cwd, sizeof cwd) != NULL, "absolute: current directory");
#endif
    }
    int n = absolute ? snprintf(base, sizeof base, "%s", s_dir)
                     : snprintf(base, sizeof base, "%s/%s", cwd, s_dir);
    CHECK(n > 0 && (size_t)n < sizeof base, "absolute: base path fits");
    n = snprintf(top, sizeof top, "%s/abs", base);
    CHECK(n > 0 && (size_t)n < sizeof top, "absolute: directory path fits");
    n = snprintf(sub, sizeof sub, "%s/abs/sub", base);
    CHECK(n > 0 && (size_t)n < sizeof sub, "absolute: subdirectory path fits");
    n = snprintf(path, sizeof path, "%s/abs/sub/frame_v2.ppm", base);
    CHECK(n > 0 && (size_t)n < sizeof path, "absolute: file path fits");
    remove(path);
    remove_dir(sub);
    remove_dir(top);
    CHECK(sr_capture_arm(path) == 1, "absolute: arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "absolute: publish creates the missing directories under an absolute root");
    CHECK(ppm_matches(path, PSP_W, PSP_H, s_frame_rgb), "absolute: file exact");
}

/* SR_FBSNAP_WINDOWS is self-sufficient: with SR_FBSNAP unset, configured windows select the
 * FBSNAP slot and capture every present inside them. An explicit SR_FBSNAP keeps its
 * meaning, and SR_FBDUMP keeps priority over windows. */
static void test_windows_imply_every_present(void) {
    CHECK(sr_fbcap_snap_every(NULL, 0) == 0, "policy: nothing configured is off");
    CHECK(sr_fbcap_snap_every("", 0) == 0, "policy: an empty switch without windows is off");
    CHECK(sr_fbcap_snap_every(NULL, 1) == 1, "policy: windows alone capture every present");
    CHECK(sr_fbcap_snap_every("", 1) == 1, "policy: an empty switch counts as unset");
    CHECK(sr_fbcap_snap_every("0", 1) == 0, "policy: an explicit SR_FBSNAP=0 stays off");
    CHECK(sr_fbcap_snap_every("-2", 1) == 0, "policy: a negative cadence is off");
    CHECK(sr_fbcap_snap_every("every", 1) == 0, "policy: a non-numeric cadence is off");
    CHECK(sr_fbcap_snap_every("3", 0) == 3, "policy: SR_FBSNAP=3 alone keeps its cadence");
    CHECK(sr_fbcap_snap_every("3", 1) == 3, "policy: windows keep an explicit cadence");
    CHECK(sr_fbcap_snap_every("99999999999", 0) == INT_MAX, "policy: a huge cadence saturates");
    CHECK(sr_fbcap_owner(0, sr_fbcap_snap_every(NULL, 1) > 0) == SR_FBCAP_FBSNAP,
          "policy: windows without SR_FBSNAP own the FBSNAP slot");
    CHECK(sr_fbcap_owner(0, sr_fbcap_snap_every("0", 1) > 0) == SR_FBCAP_NONE,
          "policy: SR_FBSNAP=0 with windows owns nothing");
    CHECK(sr_fbcap_owner(1, sr_fbcap_snap_every(NULL, 1) > 0) == SR_FBCAP_FBDUMP,
          "policy: SR_FBDUMP keeps priority over windows");
}

/* A picture probe is serviced like a capture but publishes no file: it reports the brightest
 * colour byte, once, through either presenter route, and never blocks a later file capture. */
static void test_picture_probe(void) {
    static uint32_t dark[PSP_W * PSP_H];
    uint32_t b = 99u;
    char blocked[600], after[600];
    out_path(blocked, sizeof blocked, "probe_blocked.ppm");
    out_path(after, sizeof after, "after_probe.ppm");
    remove(blocked);
    remove(after);
    for (uint32_t i = 0; i < PSP_W * PSP_H; i++)
        dark[i] = (i % 3u == 0u) ? 0x00080000u : (i % 3u == 1u ? 0x00000800u : 0u);
    dark[17] |= 0xff000000u;                     /* the ignored byte never counts */
    CHECK(sr_capture_probe_take(&b) == 0, "probe: no result before any probe");
    CHECK(sr_capture_arm_probe() == 1, "probe: arm accepted");
    CHECK(sr_capture_arm(blocked) == 0, "probe: a pending probe refuses a file arm");
    CHECK(sr_capture_serve_host(dark, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "probe: host service resolves the probe");
    CHECK(!file_exists(blocked), "probe: no file is written");
    CHECK(sr_capture_probe_take(&b) == 1 && b == 8u, "probe: brightest byte of a dark frame is 8");
    CHECK(sr_capture_probe_take(&b) == 0, "probe: a result is taken once");

    /* GPU readback route, RGBX order, with one bright pixel in the last row. */
    static uint32_t gpu[PSP_W * PSP_H];
    memset(gpu, 0, sizeof gpu);
    gpu[PSP_W * (PSP_H - 1u) + 5u] = 0x00c80000u;  /* byte 2 (blue in RGBX) = 200 */
    CHECK(sr_capture_arm_probe() == 1, "probe: gpu arm accepted");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, PSP_W, PSP_H);
    CHECK(sr_capture_publish_recorded(gpu, PSP_W * 4u, SR_CAP_ORDER_RGBX) == 1, "probe: gpu publish");
    CHECK(sr_capture_probe_take(&b) == 1 && b == 200u, "probe: gpu brightest byte");

    /* A cancelled probe reports nothing, and a file capture works after a probe. */
    CHECK(sr_capture_arm_probe() == 1, "probe: arm before a skipped present");
    sr_capture_cancel();
    CHECK(sr_capture_probe_take(&b) == 0, "probe: a cancelled probe has no result");
    CHECK(sr_capture_arm(after) == 1, "probe: file arm after a probe");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "probe: file capture after a probe publishes");
    CHECK(ppm_matches(after, PSP_W, PSP_H, s_frame_rgb),
          "probe: file capture after a probe is exact");
    CHECK(sr_capture_probe_take(&b) == 0, "probe: a file capture leaves no probe result");
}

/* The dark-picture watch: a stretch opens at the first dark probe, reports once per report
 * period while probes keep arriving, stays quiet once probes stop, and a visible frame ends
 * it with RESUMED only when it had been reported. */
static void test_picture_watch(void) {
    SrPictureWatch w;
    uint32_t dark = 0u;
    memset(&w, 0, sizeof w);
    CHECK(sr_picture_watch_tick(&w, 5000u, &dark) == SR_PICTURE_WATCH_NONE, "watch: nothing open");
    CHECK(sr_picture_watch_probe(&w, 1000u, 40u) == SR_PICTURE_WATCH_NONE, "watch: visible frame");
    CHECK(sr_picture_watch_tick(&w, 1700u, &dark) == SR_PICTURE_WATCH_NONE, "watch: visible stays quiet");
    CHECK(sr_picture_watch_probe(&w, 1300u, SR_PICTURE_DARK_MAX) == SR_PICTURE_WATCH_NONE,
          "watch: the threshold value is dark");
    CHECK(sr_picture_watch_probe(&w, 1600u, 0u) == SR_PICTURE_WATCH_NONE, "watch: still dark");
    CHECK(sr_picture_watch_tick(&w, 1899u, &dark) == SR_PICTURE_WATCH_NONE, "watch: under one period");
    CHECK(sr_picture_watch_tick(&w, 1900u, &dark) == SR_PICTURE_WATCH_REPORT && dark == 600u,
          "watch: first report after one period from the first dark probe");
    CHECK(sr_picture_watch_tick(&w, 1901u, &dark) == SR_PICTURE_WATCH_NONE, "watch: once per period");
    CHECK(sr_picture_watch_probe(&w, 1902u, 1u) == SR_PICTURE_WATCH_NONE, "watch: dark probe");
    /* A VBLANK jump across two periods reports once, with the true elapsed count. */
    CHECK(sr_picture_watch_tick(&w, 2502u, &dark) == SR_PICTURE_WATCH_REPORT && dark == 1202u,
          "watch: a jump across a period reports once");
    /* Probes stop (the game stopped flipping): the no-frame watchdog's case, not this one's. */
    CHECK(sr_picture_watch_tick(&w, 1902u + 2u * SR_PICTURE_PROBE_EVERY + 1u + 600u, &dark) ==
          SR_PICTURE_WATCH_NONE, "watch: quiet once probes stop");
    CHECK(sr_picture_watch_probe(&w, 4000u, SR_PICTURE_DARK_MAX + 1u) == SR_PICTURE_WATCH_RESUMED,
          "watch: a visible frame ends a reported stretch");
    CHECK(sr_picture_watch_tick(&w, 9000u, &dark) == SR_PICTURE_WATCH_NONE, "watch: closed");
    CHECK(sr_picture_watch_probe(&w, 9100u, 0u) == SR_PICTURE_WATCH_NONE, "watch: new stretch");
    CHECK(sr_picture_watch_probe(&w, 9200u, 90u) == SR_PICTURE_WATCH_NONE,
          "watch: an unreported stretch ends without RESUMED");
}

/* The probe's result through the host service (synchronous) and through the deferred GPU
 * route (recorded, deferred, then published); both must give the same brightest byte. */
static uint32_t probe_via_host(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                               sr_cap_order order) {
    uint32_t b = 999u;
    CHECK(sr_capture_arm_probe() == 1, "pinned: host arm");
    CHECK(sr_capture_serve_host(px, w, h, row_bytes, order) == 1, "pinned: host service");
    CHECK(sr_capture_probe_take(&b) == 1, "pinned: host result");
    return b;
}

static uint32_t probe_via_deferred(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                                   sr_cap_order order) {
    uint32_t b = 999u;
    CHECK(sr_capture_arm_probe() == 1, "pinned: deferred arm");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, w, h);
    sr_capture_probe_defer();
    CHECK(sr_capture_probe_in_flight() == 1, "pinned: deferred in flight");
    CHECK(sr_capture_publish_probe(px, row_bytes, order) == 1, "pinned: deferred publish");
    CHECK(sr_capture_probe_take(&b) == 1, "pinned: deferred result");
    return b;
}

/* The probe's result is pinned on a table of frames: the brightest of the three colour bytes,
 * never the ignored byte or row padding, in either byte order, at the 4x frame size. */
static void test_probe_result_pinned(void) {
    static uint8_t big[2048u * 1088u * 4u];
    static uint8_t small[16u * 4u * 6u];   /* 6 rows of 16 bytes: 3 pixels + 4 bytes padding */
    const uint32_t row = 16u;
    struct { const char *name; uint32_t px, byte; uint8_t value; uint32_t want; } cases[] = {
        { "all zero", 0u, 0u, 0u, 0u },
        { "dark maximum in the last pixel", 5u * 3u + 2u, 0u, 8u, 8u },
        { "byte 1", 7u, 1u, 77u, 77u },
        { "byte 2", 4u, 2u, 200u, 200u },
        { "first-row 255", 1u, 2u, 255u, 255u },
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        memset(small, 0, sizeof small);
        for (uint32_t y = 0; y < 6u; y++) {
            for (uint32_t x = 0; x < 3u; x++) small[y * row + x * 4u + 3u] = 0xffu;  /* ignored */
            memset(small + y * row + 12u, 0xff, 4u);                                  /* padding */
        }
        uint32_t p = cases[c].px;
        small[(p / 3u) * row + (p % 3u) * 4u + cases[c].byte] = cases[c].value;
        for (int o = 0; o < 2; o++) {
            sr_cap_order order = o ? SR_CAP_ORDER_RGBX : SR_CAP_ORDER_BGRX;
            uint32_t h = probe_via_host(small, 3u, 6u, row, order);
            uint32_t d = probe_via_deferred(small, 3u, 6u, row, order);
            if (h != cases[c].want || d != cases[c].want)
                fprintf(stderr, "  pinned case '%s': host %u deferred %u, want %u\n",
                        cases[c].name, h, d, cases[c].want);
            CHECK(h == cases[c].want && d == cases[c].want, "pinned: brightest byte");
        }
    }
    /* A 4x dark frame whose only non-zero colour byte is the last pixel's: a full scan. */
    memset(big, 0, sizeof big);
    for (size_t i = 0; i < 2048u * 1088u; i++) big[i * 4u + 3u] = 0xffu;
    big[sizeof big - 4u] = 8u;
    CHECK(probe_via_host(big, 2048u, 1088u, 2048u * 4u, SR_CAP_ORDER_RGBX) == 8u,
          "pinned: 4x dark frame, host");
    CHECK(probe_via_deferred(big, 2048u, 1088u, 2048u * 4u, SR_CAP_ORDER_RGBX) == 8u,
          "pinned: 4x dark frame, deferred");
}

static int s_resolver_calls, s_resolver_blocking;
static int s_resolver_finish;  /* what the test resolver does: 1 publish, 0 still running */
static int test_resolver(int block) {
    static const uint8_t frame[8] = { 0u, 0u, 9u, 0xffu, 0u, 0u, 0u, 0u };
    s_resolver_calls++;
    if (block) s_resolver_blocking++;
    if (!block && !s_resolver_finish) return 0;
    (void)sr_capture_publish_probe(frame, 8u, SR_CAP_ORDER_BGRX);
    return 1;
}

/* The deferred probe protocol: the capture slot is free while a probe is in flight, a second
 * probe is refused, the result arrives once, and a dropped or invalid probe has none. */
static void test_deferred_probe(void) {
    char path[600];
    uint32_t b = 0u;
    out_path(path, sizeof path, "while_probe_in_flight.ppm");
    remove(path);
    CHECK(sr_capture_pending_is_probe() == 0, "deferred: nothing pending");
    CHECK(sr_capture_arm_probe() == 1, "deferred: arm");
    CHECK(sr_capture_pending_is_probe() == 1, "deferred: the armed request is a probe");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, 2u, 1u);
    CHECK(sr_capture_is_recorded() && sr_capture_pending_is_probe(), "deferred: recorded probe");
    sr_capture_probe_defer();
    CHECK(sr_capture_probe_in_flight() == 1, "deferred: in flight");
    CHECK(!sr_capture_is_armed() && !sr_capture_is_recorded(), "deferred: the slot is free");
    CHECK(sr_capture_probe_take(&b) == 0, "deferred: no result while in flight");
    CHECK(sr_capture_arm_probe() == 0, "deferred: a second probe is refused while one is in flight");
    CHECK(sr_capture_arm(path) == 1, "deferred: a file capture is accepted while a probe is in flight");
    CHECK(sr_capture_pending_is_probe() == 0, "deferred: the armed request is a file capture");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1 &&
          ppm_matches(path, PSP_W, PSP_H, s_frame_rgb),
          "deferred: the file capture publishes exactly while the probe is in flight");
    CHECK(sr_capture_probe_take(&b) == 0, "deferred: the file capture gave the probe no result");

    /* Polling: nothing happens without a resolver; with one, a running readback stays in
     * flight on a non-blocking poll and a blocking poll finishes it. */
    CHECK(sr_capture_probe_poll(1) == 0 && sr_capture_probe_in_flight(), "deferred: no resolver");
    sr_capture_set_probe_resolver(test_resolver);
    s_resolver_finish = 0;
    CHECK(sr_capture_probe_poll(0) == 0 && sr_capture_probe_in_flight(),
          "deferred: a running readback stays in flight");
    CHECK(sr_capture_probe_poll(1) == 1 && !sr_capture_probe_in_flight(),
          "deferred: a blocking poll finishes it");
    CHECK(sr_capture_probe_take(&b) == 1 && b == 9u, "deferred: the published result");
    CHECK(sr_capture_probe_take(&b) == 0, "deferred: taken once");
    CHECK(sr_capture_probe_poll(1) == 0, "deferred: nothing left to poll");
    sr_capture_set_probe_resolver(NULL);

    /* A dropped probe has no result; an invalid pixel description fails it without one. */
    CHECK(sr_capture_arm_probe() == 1, "deferred: arm again");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, 2u, 1u);
    sr_capture_probe_defer();
    sr_capture_probe_drop("test drop");
    CHECK(!sr_capture_probe_in_flight() && sr_capture_probe_take(&b) == 0,
          "deferred: a dropped probe has no result");
    CHECK(sr_capture_arm_probe() == 1, "deferred: arm after a drop");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, 2u, 1u);
    sr_capture_probe_defer();
    CHECK(sr_capture_publish_probe(s_frame, 4u, SR_CAP_ORDER_BGRX) == -1 &&
          !sr_capture_probe_in_flight() && sr_capture_probe_take(&b) == 0,
          "deferred: a short row fails the probe with no result");
    CHECK(sr_capture_publish_probe(s_frame, 8u, SR_CAP_ORDER_BGRX) == 0,
          "deferred: publishing with nothing in flight does nothing");

    /* Deferring applies only to a recorded probe: a recorded file capture keeps its state. */
    CHECK(sr_capture_arm(path) == 1, "deferred: file arm");
    sr_capture_mark_recorded(SR_CAP_SRC_GPU, PSP_W, PSP_H);
    sr_capture_probe_defer();
    CHECK(sr_capture_is_recorded() && !sr_capture_probe_in_flight(),
          "deferred: a recorded file capture is not deferred");
    CHECK(sr_capture_publish_recorded(s_frame, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "deferred: the file capture still publishes");
    remove(path);
}

/* ---- deferred watch against the synchronous one -------------------------------------
 * The reference feeds every probe result at its present, as display_present_active did when
 * the presenter waited for the readback. The deferred side follows hle.c's present and
 * VBLANK steps exactly: collect without waiting, collect with waiting only when the next
 * probe is due or sr_picture_watch_tick_needs_probe() says so, then tick. Readbacks finish
 * after random delays, mostly a few VBLANKs and sometimes more than a report period. Both
 * must emit the same probes, RESUMED verdicts and reports, with the same values, in the same
 * order. */
typedef struct { char kind; uint32_t key, value; } WatchEvent;
#define WATCH_EVENTS_MAX 8192
static WatchEvent s_ref_ev[WATCH_EVENTS_MAX], s_def_ev[WATCH_EVENTS_MAX];
static int s_ref_n, s_def_n;
static uint32_t s_sim_now, s_sim_ready, s_sim_brightest;
static int s_sim_blocking;

static void watch_log(WatchEvent *ev, int *n, char kind, uint32_t key, uint32_t value) {
    if (*n < WATCH_EVENTS_MAX) {
        ev[*n].kind = kind;
        ev[*n].key = key;
        ev[*n].value = value;
    }
    (*n)++;
}

static uint32_t sim_rand(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static int sim_resolver(int block) {
    uint8_t frame[8] = { 0u, 0u, 0u, 0xffu, 0u, 0u, 0u, 0xffu };
    if (block) s_sim_blocking++;
    if (!block && s_sim_now - s_sim_ready > 0x80000000u) return 0;   /* now < ready */
    frame[5] = (uint8_t)s_sim_brightest;
    (void)sr_capture_publish_probe(frame, 8u, SR_CAP_ORDER_BGRX);
    return 1;
}

static void sim_collect(SrPictureWatch *w, int block) {
    uint32_t key = w->pending_vcount, b = 0u;
    int v = sr_picture_watch_collect(w, block, &b);
    if (v == SR_PICTURE_WATCH_RESUMED) watch_log(s_def_ev, &s_def_n, 'R', key, b);
}

static void test_watch_deferred_matches_synchronous(void) {
    enum { VBLANKS = 24000 };
    static uint8_t present[VBLANKS], level[VBLANKS];
    int reports = 0, resumed = 0, mismatched_trials = 0;
    s_sim_blocking = 0;
    sr_capture_set_probe_resolver(sim_resolver);
    for (uint32_t trial = 1u; trial <= 48u; trial++) {
        uint32_t seed = trial * 2654435761u;
        /* Every fourth trial starts just below the VCOUNT wrap. */
        uint32_t base = (trial % 4u == 0u) ? 0xffffffffu - 9000u : 1000u + trial;
        /* Brightness in runs: dark runs long enough to be reported, bright runs between. */
        int dark = 0;
        uint32_t left = 0u;
        for (uint32_t i = 0; i < VBLANKS; i++) {
            if (left == 0u) {
                dark = !dark;
                left = dark ? 200u + sim_rand(&seed) % 4000u : 50u + sim_rand(&seed) % 1500u;
            }
            left--;
            level[i] = dark ? (uint8_t)(sim_rand(&seed) % (SR_PICTURE_DARK_MAX + 1u))
                            : (uint8_t)(SR_PICTURE_DARK_MAX + 1u + sim_rand(&seed) % 247u);
        }
        /* Presents every one or two VBLANKs, with occasional long gaps. */
        memset(present, 0, sizeof present);
        for (uint32_t i = 0; i < VBLANKS;) {
            present[i] = 1u;
            i += (sim_rand(&seed) % 100u < 3u) ? 300u + sim_rand(&seed) % 700u
                                                : 1u + sim_rand(&seed) % 2u;
        }
        SrPictureWatch ref, def;
        memset(&ref, 0, sizeof ref);
        memset(&def, 0, sizeof def);
        uint32_t ref_last = 0u, def_last = 0u, dd = 0u;
        s_ref_n = s_def_n = 0;
        for (uint32_t i = 0; i < VBLANKS; i++) {
            uint32_t v = base + i;
            s_sim_now = v;
            if (present[i]) {
                /* Reference: the result is fed inside the present. */
                if (v - ref_last >= SR_PICTURE_PROBE_EVERY) {
                    ref_last = v;
                    watch_log(s_ref_ev, &s_ref_n, 'P', v, 0u);
                    if (sr_picture_watch_probe(&ref, v, level[i]) == SR_PICTURE_WATCH_RESUMED)
                        watch_log(s_ref_ev, &s_ref_n, 'R', v, level[i]);
                }
                /* Deferred: hle.c's display_present_active. */
                sim_collect(&def, 0);
                if (def.pending && v - def_last >= SR_PICTURE_PROBE_EVERY) sim_collect(&def, 1);
                if (!def.pending && v - def_last >= SR_PICTURE_PROBE_EVERY &&
                    sr_capture_arm_probe()) {
                    sr_capture_mark_recorded(SR_CAP_SRC_GPU, 2u, 1u);
                    sr_capture_probe_defer();
                    def_last = v;
                    sr_picture_watch_probe_sent(&def, v);
                    watch_log(s_def_ev, &s_def_n, 'P', v, 0u);
                    uint32_t r = sim_rand(&seed) % 100u;
                    dd = r < 6u ? sim_rand(&seed) % 1300u : sim_rand(&seed) % 4u;
                    s_sim_ready = v + dd;
                    s_sim_brightest = level[i];
                }
            }
            /* Reference tick. */
            uint32_t d = 0u;
            if (sr_picture_watch_tick(&ref, v, &d) == SR_PICTURE_WATCH_REPORT)
                watch_log(s_ref_ev, &s_ref_n, 'T', v, d);
            /* Deferred: hle.c's sr_vblank_tick. */
            sim_collect(&def, 0);
            if (sr_picture_watch_tick_needs_probe(&def, v)) sim_collect(&def, 1);
            d = 0u;
            if (sr_picture_watch_tick(&def, v, &d) == SR_PICTURE_WATCH_REPORT)
                watch_log(s_def_ev, &s_def_n, 'T', v, d);
        }
        sr_capture_probe_drop("end of trial");
        int same = s_ref_n == s_def_n && s_ref_n <= WATCH_EVENTS_MAX;
        for (int k = 0; same && k < s_ref_n; k++)
            same = s_ref_ev[k].kind == s_def_ev[k].kind && s_ref_ev[k].key == s_def_ev[k].key &&
                   s_ref_ev[k].value == s_def_ev[k].value;
        if (!same) {
            mismatched_trials++;
            int k = 0;
            while (k < s_ref_n && k < s_def_n && s_ref_ev[k].kind == s_def_ev[k].kind &&
                   s_ref_ev[k].key == s_def_ev[k].key && s_ref_ev[k].value == s_def_ev[k].value)
                k++;
            fprintf(stderr, "  watch trial %u: first difference at event %d of %d/%d\n",
                    trial, k, s_ref_n, s_def_n);
        }
        for (int k = 0; k < s_ref_n && k < WATCH_EVENTS_MAX; k++) {
            if (s_ref_ev[k].kind == 'T') reports++;
            if (s_ref_ev[k].kind == 'R') resumed++;
        }
    }
    sr_capture_set_probe_resolver(NULL);
    CHECK(mismatched_trials == 0, "watch: deferred results give the synchronous verdicts and reports");
    /* The trials must reach the cases that matter: reports, resumed stretches, and ticks that
     * had to wait for a pending result. */
    CHECK(reports >= 50 && resumed >= 10 && s_sim_blocking >= 10,
          "watch: the trials reach reports, resumed stretches and blocking collects");
    if (reports < 50 || resumed < 10 || s_sim_blocking < 10)
        fprintf(stderr, "  watch coverage: reports %d resumed %d blocking %d\n",
                reports, resumed, s_sim_blocking);
}

/* ---- PNG publication and the screenshot key -------------------------------------------- */

static uint32_t rd_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* PNG CRC-32, bit by bit: an implementation independent of the publisher's table. */
static uint32_t ref_crc32(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

/* Parse a published PNG: the signature; IHDR (w x h, 8-bit RGB, no interlace); one IDAT whose
 * zlib stream is stored deflate blocks with a correct Adler-32; IEND last with nothing after it;
 * every chunk CRC right. Each decoded row must start with filter 0 and equal rgb. */
static int png_matches(const char *path, uint32_t w, uint32_t h, const unsigned char *rgb) {
    static const unsigned char sig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t raw_len = (size_t)h * (1u + 3u * (size_t)w);
    size_t cap = raw_len + 4096u;
    unsigned char *b = (unsigned char *)malloc(cap);
    unsigned char *raw = (unsigned char *)malloc(raw_len);
    size_t len = b ? fread(b, 1, cap, f) : 0u;
    fclose(f);
    int ok = b && raw && len < cap && len >= 8u + 25u + 12u + 12u && memcmp(b, sig, 8u) == 0;
    size_t pos = 8u, got = 0u;
    int seen_ihdr = 0, seen_idat = 0, seen_iend = 0;
    while (ok && !seen_iend) {
        if (pos + 12u > len) { ok = 0; break; }
        uint32_t clen = rd_be32(b + pos);
        if (clen > len - pos - 12u) { ok = 0; break; }
        const unsigned char *type = b + pos + 4u;
        const unsigned char *data = b + pos + 8u;
        if (ref_crc32(type, (size_t)clen + 4u) != rd_be32(data + clen)) { ok = 0; break; }
        if (memcmp(type, "IHDR", 4u) == 0) {
            ok = !seen_ihdr && clen == 13u && rd_be32(data) == w && rd_be32(data + 4) == h &&
                 data[8] == 8u && data[9] == 2u && data[10] == 0u && data[11] == 0u &&
                 data[12] == 0u;
            seen_ihdr = 1;
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            ok = seen_ihdr && !seen_idat && clen >= 6u && data[0] == 0x78u &&
                 (((uint32_t)data[0] << 8) | data[1]) % 31u == 0u;
            size_t zp = 2u;
            int final = 0;
            while (ok && !final) {
                if (zp + 5u > clen - 4u) { ok = 0; break; }
                final = data[zp] & 1u;
                ok = (data[zp] >> 1) == 0u;   /* BTYPE 00: a stored block */
                uint32_t bl = data[zp + 1] | ((uint32_t)data[zp + 2] << 8);
                uint32_t nl = data[zp + 3] | ((uint32_t)data[zp + 4] << 8);
                ok = ok && (bl ^ 0xFFFFu) == nl && zp + 5u + bl <= clen - 4u &&
                     got + bl <= raw_len;
                if (ok) memcpy(raw + got, data + zp + 5u, bl);
                got += bl;
                zp += 5u + bl;
            }
            uint32_t a = 1u, bsum = 0u;
            for (size_t i = 0; ok && i < got; i++) {
                a = (a + raw[i]) % 65521u;
                bsum = (bsum + a) % 65521u;
            }
            ok = ok && zp + 4u == clen && rd_be32(data + zp) == ((bsum << 16) | a);
            seen_idat = 1;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            ok = seen_idat && clen == 0u && pos + 12u == len;
            seen_iend = 1;
        } else {
            ok = 0;
        }
        pos += (size_t)clen + 12u;
    }
    ok = ok && seen_iend && got == raw_len;
    for (uint32_t y = 0; ok && y < h; y++) {
        const unsigned char *row = raw + (size_t)y * (1u + 3u * (size_t)w);
        ok = row[0] == 0u && memcmp(row + 1, rgb + (size_t)y * w * 3u, (size_t)w * 3u) == 0;
    }
    free(b);
    free(raw);
    return ok;
}

#define PNG_W2 1000u
#define PNG_H2 70u
static unsigned char s_rgbx[PNG_W2 * PNG_H2 * 4u];
static unsigned char s_rgbx_want[PNG_W2 * PNG_H2 * 3u];

/* A capture armed with a .png path publishes a PNG of exactly the frame, in both byte orders and
 * across several stored blocks, an upper-case extension included; a .ppm path keeps P6. */
static void test_png_capture(void) {
    char path[600];
    out_path(path, sizeof path, "shot.png");
    remove(path);
    CHECK(sr_capture_arm(path) == 1, "png: arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "png: host service publishes");
    CHECK(sr_capture_result() == 1, "png: result is published");
    CHECK(png_matches(path, PSP_W, PSP_H, s_frame_rgb), "png: chunks, CRCs, zlib and pixels exact");

    for (size_t i = 0; i < (size_t)PNG_W2 * PNG_H2; i++) {
        unsigned char r = (unsigned char)(i * 7u);
        unsigned char g = (unsigned char)(i * 13u + 1u);
        unsigned char b = (unsigned char)(i * 29u + 2u);
        s_rgbx[i * 4u + 0u] = r;
        s_rgbx[i * 4u + 1u] = g;
        s_rgbx[i * 4u + 2u] = b;
        s_rgbx[i * 4u + 3u] = 0xA5u;
        s_rgbx_want[i * 3u + 0u] = r;
        s_rgbx_want[i * 3u + 1u] = g;
        s_rgbx_want[i * 3u + 2u] = b;
    }
    out_path(path, sizeof path, "SHOT_RGBX.PNG");
    remove(path);
    CHECK(sr_capture_arm(path) == 1, "png: second arm accepted");
    CHECK(sr_capture_serve_host(s_rgbx, PNG_W2, PNG_H2, PNG_W2 * 4u, SR_CAP_ORDER_RGBX) == 1,
          "png: RGBX service publishes");
    CHECK(png_matches(path, PNG_W2, PNG_H2, s_rgbx_want), "png: multi-block RGBX image exact");

    out_path(path, sizeof path, "still_ppm.ppm");
    remove(path);
    CHECK(sr_capture_arm(path) == 1, "png: ppm arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1,
          "png: ppm service publishes");
    CHECK(ppm_matches(path, PSP_W, PSP_H, s_frame_rgb), "png: a .ppm path keeps the P6 format");
}

static void test_screenshot_name(void) {
    char out[96];
    CHECK(sr_screenshot_name("UCUS98701", 2026, 11, 9, 7, 5, 3, 1, out, sizeof out) == 1 &&
              strcmp(out, "UCUS98701_20261109-070503.png") == 0,
          "name: disc, date and time");
    CHECK(sr_screenshot_name("UCUS98744", 2026, 1, 31, 23, 59, 59, 2, out, sizeof out) == 1 &&
              strcmp(out, "UCUS98744_20260131-235959-2.png") == 0,
          "name: second shot in the same second");
    CHECK(sr_screenshot_name("../a b\\c:d", 2026, 11, 9, 0, 0, 0, 1, out, sizeof out) == 1 &&
              strcmp(out, "abcd_20261109-000000.png") == 0,
          "name: separators and other characters dropped from the disc ID");
    CHECK(sr_screenshot_name(NULL, 2026, 11, 9, 0, 0, 0, 1, out, sizeof out) == 1 &&
              strcmp(out, "game_20261109-000000.png") == 0,
          "name: no disc ID reads game");
    CHECK(sr_screenshot_name("/:.", 2026, 11, 9, 0, 0, 0, 1, out, sizeof out) == 1 &&
              strcmp(out, "game_20261109-000000.png") == 0,
          "name: a disc ID with no kept character reads game");
    CHECK(sr_screenshot_name("UCUS98701", 2026, 13, 9, 0, 0, 0, 1, out, sizeof out) == 0 &&
              out[0] == '\0',
          "name: month out of range refused");
    CHECK(sr_screenshot_name("UCUS98701", 2026, 11, 9, 0, 0, 0, 100, out, sizeof out) == 0,
          "name: sequence above 99 refused");
    CHECK(sr_screenshot_name("UCUS98701", 2026, 11, 9, 0, 0, 0, 1, out, 10u) == 0 &&
              out[0] == '\0',
          "name: a buffer too small is refused, not truncated");
}

/* The texture dump's folder: under the user data folder, one per disc ID, never shared. */
static void test_texdump_folder(void) {
    char out[64];
    CHECK(sr_texdump_folder("C:/data", "ULUS10000", out, sizeof out) == 1 &&
              strcmp(out, "C:/data/texdump/ULUS10000") == 0,
          "dump folder: user data, texdump, disc ID");
    CHECK(sr_texdump_folder("/d", "../a b\\c:d", out, sizeof out) == 1 &&
              strcmp(out, "/d/texdump/abcd") == 0,
          "dump folder: separators and other characters dropped from the disc ID");
    CHECK(sr_texdump_folder("/d", "/:.", out, sizeof out) == 0 && out[0] == '\0',
          "dump folder: a disc ID with no kept character is refused, not shared");
    CHECK(sr_texdump_folder("/d", NULL, out, sizeof out) == 0, "dump folder: no disc ID refused");
    CHECK(sr_texdump_folder("", "ULUS10000", out, sizeof out) == 0 && out[0] == '\0',
          "dump folder: no user data folder refused");
    CHECK(sr_texdump_folder("/d", "ULUS10000", out, 12u) == 0 && out[0] == '\0',
          "dump folder: a buffer too small is refused, not truncated");
    /* The runtime holds the user data root in Windows' extended form, where '/' does not separate. */
    CHECK(sr_texdump_folder("\\\\?\\C:\\data", "ULUS10000", out, sizeof out) == 1 &&
              strcmp(out, "C:\\data/texdump/ULUS10000") == 0,
          "dump folder: an extended drive root is written plain");
    CHECK(sr_texdump_folder("\\\\?\\UNC\\srv\\share\\d", "ULUS10000", out, sizeof out) == 1 &&
              strcmp(out, "\\\\srv\\share\\d/texdump/ULUS10000") == 0,
          "dump folder: an extended UNC root is written plain");
    CHECK(sr_texdump_folder("\\\\?\\", "ULUS10000", out, sizeof out) == 0 && out[0] == '\0',
          "dump folder: an extended prefix with no folder is refused");
}

/* The dump folder is checked as a texture pack folder is before anything is made or written in
 * it (sr_texdump_folder_problem): strict UTF-8, a full path on this host, shorter than a pack
 * folder may be, no device namespace; each refusal says why without naming the path. */
static void test_texdump_folder_problem(void) {
#ifdef _WIN32
#define DUMP_ABS "C:/data/texdump/"
#else
#define DUMP_ABS "/data/texdump/"
#endif
    char out[700];
    CHECK(sr_texdump_folder_problem(DUMP_ABS "ULUS10000") == NULL, "dump folder check: a full path is taken");
    CHECK(sr_texdump_folder_problem(DUMP_ABS "\xC3\xA9\xE6\x97\xA5 \xF0\x9F\x8E\xAE") == NULL,
          "dump folder check: a full path with 2-, 3- and 4-byte UTF-8 is taken");
    CHECK(sr_texdump_folder_problem(NULL) != NULL && sr_texdump_folder_problem("") != NULL,
          "dump folder check: no folder is refused");
    const char *why = sr_texdump_folder_problem("texdump/ULUS10000");
    CHECK(why && strstr(why, "full path") && !strstr(why, "texdump/"),
          "dump folder check: a relative path is refused, and the reason names no path");
#ifdef _WIN32
    CHECK(sr_texdump_folder_problem("C:texdump") != NULL, "dump folder check: a drive-relative path is refused");
    CHECK(sr_texdump_folder_problem("\\texdump") != NULL, "dump folder check: a path from the drive's root alone is refused");
    CHECK(sr_texdump_folder_problem("\\\\srv\\share\\texdump/X") == NULL, "dump folder check: a UNC path is taken");
#endif
    static const char *const bad_utf8[] = {
        DUMP_ABS "\xC0\xAF",           /* overlong '/' */
        DUMP_ABS "\xE0\x80\xAF",       /* overlong, three bytes */
        DUMP_ABS "\xED\xA0\x80",       /* a surrogate */
        DUMP_ABS "\xF4\x90\x80\x80",   /* past U+10FFFF */
        DUMP_ABS "\xE6\x97",           /* cut */
        DUMP_ABS "\x80",               /* a lone continuation byte */
        DUMP_ABS "\xFF",
    };
    for (size_t i = 0; i < sizeof bad_utf8 / sizeof bad_utf8[0]; i++) {
        why = sr_texdump_folder_problem(bad_utf8[i]);
        CHECK(why && strstr(why, "UTF-8"), "dump folder check: ill-formed UTF-8 is refused as such");
    }
    static const char *const devices[] = { "\\\\.\\COM1", "\\\\?\\C:\\data", "//./COM1", "//?/C:/data", "\\\\.\\C:\\data" };
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        why = sr_texdump_folder_problem(devices[i]);
#ifdef _WIN32
        CHECK(why && strstr(why, "device"), "dump folder check: a device or extended path is refused");
#else
        CHECK(why != NULL, "dump folder check: a device or extended path is refused");
#endif
    }
    /* Length: SR_TEXDUMP_DIR_MAX - 1 bytes is taken, SR_TEXDUMP_DIR_MAX refused. */
    const size_t base = strlen(DUMP_ABS);
    memcpy(out, DUMP_ABS, base);
    memset(out + base, 'a', SR_TEXDUMP_DIR_MAX - 1u - base);
    out[SR_TEXDUMP_DIR_MAX - 1u] = '\0';
    CHECK(sr_texdump_folder_problem(out) == NULL, "dump folder check: a path one byte under the limit is taken");
    out[SR_TEXDUMP_DIR_MAX - 1u] = 'a';
    out[SR_TEXDUMP_DIR_MAX] = '\0';
    why = sr_texdump_folder_problem(out);
    CHECK(why && strstr(why, "512"), "dump folder check: a path at the limit is refused");
    /* What sr_texdump_folder makes from a full root passes. */
    CHECK(sr_texdump_folder(DUMP_ABS "u", "ULUS10000", out, sizeof out) == 1 && sr_texdump_folder_problem(out) == NULL,
          "dump folder check: the folder made from a full root is taken");
#undef DUMP_ABS
}

static void test_screenshot_request(void) {
    sr_screenshot_clear();
    CHECK(!sr_screenshot_pending(), "request: none pending at start");
    sr_screenshot_request();
    sr_screenshot_request();
    CHECK(sr_screenshot_pending(), "request: pending after the key");
    sr_screenshot_clear();
    CHECK(!sr_screenshot_pending(), "request: taken once cleared");
}

/* ---- the texture dump's PNG encoder and its no-replace publish -------------------------------- */

static uint32_t enc_rand(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

/* Encode `px` and read the PNG back: signature, IHDR (w x h, 8-bit RGBA), one IDAT holding one
 * fixed-Huffman block, IEND last, every CRC right; the IDAT's zlib stream inflated by
 * nk_psp_inflate (which checks the Adler-32) is exactly the filter-0 rows. *png_len is the size. */
static int encode_round_trips(const uint32_t *px, uint32_t w, uint32_t h, size_t *png_len) {
    static const unsigned char sig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    uint8_t *png = NULL;
    size_t len = 0;
    *png_len = 0;
    if (!sr_png_encode_rgba(px, w, h, &png, &len) || !png) return 0;
    *png_len = len;
    const size_t raw_len = (size_t)h * (1u + 4u * (size_t)w);
    int ok = len >= 8u + 25u + 12u + 12u && memcmp(png, sig, 8u) == 0;
    size_t pos = 8u;
    int seen_ihdr = 0, seen_idat = 0, seen_iend = 0;
    while (ok && !seen_iend) {
        if (pos + 12u > len) { ok = 0; break; }
        const uint32_t clen = rd_be32(png + pos);
        if (clen > len - pos - 12u) { ok = 0; break; }
        const unsigned char *type = png + pos + 4u;
        const unsigned char *data = png + pos + 8u;
        if (ref_crc32(type, (size_t)clen + 4u) != rd_be32(data + clen)) { ok = 0; break; }
        if (memcmp(type, "IHDR", 4u) == 0) {
            ok = !seen_ihdr && clen == 13u && rd_be32(data) == w && rd_be32(data + 4) == h &&
                 data[8] == 8u && data[9] == 6u && data[10] == 0u && data[11] == 0u && data[12] == 0u;
            seen_ihdr = 1;
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            ok = seen_ihdr && !seen_idat && clen >= 7u && data[0] == 0x78u &&
                 (((uint32_t)data[0] << 8) | data[1]) % 31u == 0u &&
                 (data[2] & 1u) == 1u && ((data[2] >> 1) & 3u) == 1u;   /* BFINAL, BTYPE 01 */
            uint8_t *raw = NULL;
            size_t got = 0;
            char err[160];
            ok = ok && nk_psp_inflate(data, clen, &raw, &got, raw_len, raw_len + 1u, err, sizeof err) == 0;
            ok = ok && got == raw_len;
            for (uint32_t y = 0; ok && y < h; y++) {
                const uint8_t *row = raw + (size_t)y * (1u + 4u * (size_t)w);
                ok = row[0] == 0u;
                for (uint32_t x = 0; ok && x < w; x++) {
                    const uint32_t c = px[(size_t)y * w + x];
                    ok = row[1 + x * 4u] == (uint8_t)c && row[2 + x * 4u] == (uint8_t)(c >> 8) &&
                         row[3 + x * 4u] == (uint8_t)(c >> 16) && row[4 + x * 4u] == (uint8_t)(c >> 24);
                }
            }
            free(raw);
            seen_idat = 1;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            ok = seen_idat && clen == 0u && pos + 12u == len;
            seen_iend = 1;
        } else {
            ok = 0;
        }
        pos += (size_t)clen + 12u;
    }
    free(png);
    return ok && seen_iend;
}

static void test_png_encode(void) {
    enum { FW = 64, FH = 64, NW = 37, NH = 23, LW = 300, LH = 9 };
    static uint32_t flat[FW * FH], noise[NW * NH], longrow[LW * LH];
    uint32_t seed = 12345u;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++)
            flat[y * FW + x] = (x / 8 + y / 8) % 2 ? 0xFF2040E0u : 0x80C0A060u;
    for (int i = 0; i < NW * NH; i++) noise[i] = enc_rand(&seed);
    /* Rows that repeat a 7-texel run (matches of 258 bytes and short distances), and a row that
     * repeats one 1200 bytes back (a distance past 258 with extra bits). */
    for (int y = 0; y < LH; y++)
        for (int x = 0; x < LW; x++)
            longrow[y * LW + x] = y % 3 == 2 ? longrow[(y - 1) * LW + x] : 0xFF000000u | (uint32_t)(x % 7) * 0x111111u;
    size_t n = 0;
    const uint32_t one = 0x7F102030u;
    CHECK(encode_round_trips(flat, FW, FH, &n), "png encode: a flat texture inflates to exactly its rows");
    CHECK(n > 0 && n < (size_t)FH * (1u + 4u * FW) / 8u, "png encode: a flat texture is compressed to under an eighth");
    CHECK(encode_round_trips(noise, NW, NH, &n), "png encode: noise (no matches) inflates exactly");
    CHECK(encode_round_trips(longrow, LW, LH, &n), "png encode: long and distant matches inflate exactly");
    CHECK(encode_round_trips(&one, 1u, 1u, &n), "png encode: a 1x1 image inflates exactly");
    /* Past the 32 KiB window and the encoder's first output buffer: 512x512 noise (1 MiB of
     * filtered rows, no matches, so the output grows several times and every window slot is
     * reused); noise that repeats exactly 32768 bytes back (one 16384-texel row whose second half
     * is its first: the longest distance deflate has), and noise that repeats 32769 bytes back
     * (two equal 8192-texel rows of 1 + 32768 bytes: one past the window, so no match); and runs
     * of one texel longer than a 258-byte match after a change. */
    enum { BW = 512, BH = 512, PW = 256, PH = 256, DW = 16384, HALF = DW / 2 };
    static uint32_t big[BW * BH], far_px[DW], runs[PW * PH];
    for (int i = 0; i < BW * BH; i++) big[i] = enc_rand(&seed);
    CHECK(encode_round_trips(big, BW, BH, &n) && n > (size_t)BH * (1u + 4u * BW),
          "png encode: 512x512 noise (output growth, window reuse) inflates exactly");
    for (int x = 0; x < DW; x++) far_px[x] = x < HALF ? enc_rand(&seed) : far_px[x - HALF];
    CHECK(encode_round_trips(far_px, DW, 1u, &n), "png encode: a repeat exactly 32768 bytes back inflates exactly");
    CHECK(n < (1u + 4u * DW) * 5u / 8u, "png encode: a repeat exactly 32768 bytes back is matched");
    CHECK(encode_round_trips(far_px, HALF, 2u, &n), "png encode: a repeat 32769 bytes back inflates exactly");
    CHECK(n > 2u * (1u + 4u * HALF), "png encode: a repeat 32769 bytes back, past the window, is not matched");
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++)
            runs[y * PW + x] = x < 3 ? enc_rand(&seed) : (x < 200 ? 0xFF336699u : 0xFF996633u);
    CHECK(encode_round_trips(runs, PW, PH, &n), "png encode: runs past 258 bytes after a change inflate exactly");
    /* Transparent texels (bytes equal to the one before: matches one byte back) between noise. */
    for (int i = 0; i < PW * PH; i++) runs[i] = (i / 37) % 2 ? 0u : enc_rand(&seed);
    CHECK(encode_round_trips(runs, PW, PH, &n), "png encode: runs of one byte value inflate exactly");
    /* sr_png_encode_bound, the budget the texture dump holds for a file still being encoded: never
     * passed, even near its worst case, noise whose every byte is a 9-bit literal (144 and up), and
     * within a sixteenth of it there (only the filter bytes and the few chance matches cost less). */
    for (int i = 0; i < BW * BH; i++) big[i] = enc_rand(&seed) | 0x90909090u;
    CHECK(encode_round_trips(big, BW, BH, &n) && n <= sr_png_encode_bound(BW, BH) &&
              sr_png_encode_bound(BW, BH) - n <= sr_png_encode_bound(BW, BH) / 16u,
          "png encode: 9-bit-literal noise stays within the bound, and the bound is tight there");
    CHECK(encode_round_trips(flat, FW, FH, &n) && n <= sr_png_encode_bound(FW, FH), "png encode: the bound holds for a flat texture");
    CHECK(encode_round_trips(noise, NW, NH, &n) && n <= sr_png_encode_bound(NW, NH), "png encode: the bound holds for noise");
    CHECK(encode_round_trips(&one, 1u, 1u, &n) && n <= sr_png_encode_bound(1u, 1u) && sr_png_encode_bound(1u, 1u) == 70u,
          "png encode: a 1x1 image's bound is 63 + (9 * 5 + 17) / 8 = 70, and holds");
    CHECK(sr_png_encode_bound(0u, 4u) == 0u && sr_png_encode_bound(65536u, 65536u) == 0u,
          "png encode: no bound for a size the encoder refuses");
    uint8_t *out = (uint8_t *)1;
    size_t out_len = 1;
    CHECK(sr_png_encode_rgba(flat, 0u, 4u, &out, &out_len) == 0 && out == NULL && out_len == 0u,
          "png encode: an empty image is refused");
    CHECK(sr_png_encode_rgba(flat, 65536u, 65536u, &out, &out_len) == 0 && out == NULL,
          "png encode: an image past 4 GiB is refused");
}

static int read_back(const char *path, char *buf, size_t cap) {
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return -1;
    const size_t got = fread(buf, 1, cap - 1u, f);
    fclose(f);
    buf[got] = '\0';
    return (int)got;
}

static bool count_file(const char *name, void *ctx) {
    (void)name;
    ++*(int *)ctx;
    return true;
}

/* A user name with an accent or in Japanese puts the user data folder on such a path: the
 * texture dump's publish and a screenshot both write there, and a file that is already there is
 * never replaced. */
static void test_publish_new_unicode(void) {
    char dir[600], sub[700], path[800], shot[800], buf[64];
    /* "dump", e acute (U+00E9) and a CJK ideograph (U+65E5), in UTF-8 */
    snprintf(dir, sizeof dir, "%s/dump \xC3\xA9\xE6\x97\xA5", s_dir);
    snprintf(sub, sizeof sub, "%s/shots \xE6\x97\xA5", dir);
    snprintf(path, sizeof path, "%s/0000000000000000deadbeef.png", dir);
    snprintf(shot, sizeof shot, "%s/shot.png", sub);
    nk_remove_utf8(path);
    nk_remove_utf8(shot);
    CHECK(nk_platform_mkdir_p(dir), "publish: the non-ASCII folder is made");
    CHECK(sr_file_publish_new(path, "first", 5u) == SR_PUBLISH_WRITTEN,
          "publish: a new file is written in a non-ASCII folder");
    CHECK(nk_platform_file_exists(path) && read_back(path, buf, sizeof buf) == 5 && strcmp(buf, "first") == 0,
          "publish: the file holds its bytes");
    CHECK(sr_file_publish_new(path, "second!", 7u) == SR_PUBLISH_EXISTS,
          "publish: a file already there is reported, not replaced");
    CHECK(read_back(path, buf, sizeof buf) == 5 && strcmp(buf, "first") == 0,
          "publish: the file already there keeps its own bytes");
    int files = 0;
    CHECK(nk_platform_list_files(dir, count_file, &files) && files == 1,
          "publish: no temp file is left beside it");
    CHECK(sr_file_publish_new("", "x", 1u) == SR_PUBLISH_FAILED, "publish: an empty path is refused");
    /* Files already at temp names (another run's temp file, a name left behind) are never written
     * through: decoys at the old "<path>.tmp<n>" form and at this process's "<path>.tmp<pid>.<n>"
     * names keep their bytes, and the publish still lands. */
    {
        char second[800], decoy[900];
        snprintf(second, sizeof second, "%s/1111111111111111cafef00d.png", dir);
        nk_remove_utf8(second);
        enum { DECOYS = 12 };
        for (int k = 1; k <= DECOYS; k++) {
            for (int form = 0; form < 2; form++) {
                if (form) snprintf(decoy, sizeof decoy, "%s.tmp%lu.%d", second, nk_platform_process_id(), k);
                else snprintf(decoy, sizeof decoy, "%s.tmp%d", second, k);
                FILE *d = nk_fopen_utf8(decoy, "wb");
                if (d) { fputs("decoy", d); fclose(d); }
            }
        }
        CHECK(sr_file_publish_new(second, "third", 5u) == SR_PUBLISH_WRITTEN &&
                  read_back(second, buf, sizeof buf) == 5 && strcmp(buf, "third") == 0,
              "publish: a file is published past files already at temp names");
        int intact = 0;
        for (int k = 1; k <= DECOYS; k++) {
            for (int form = 0; form < 2; form++) {
                if (form) snprintf(decoy, sizeof decoy, "%s.tmp%lu.%d", second, nk_platform_process_id(), k);
                else snprintf(decoy, sizeof decoy, "%s.tmp%d", second, k);
                if (read_back(decoy, buf, sizeof buf) == 5 && strcmp(buf, "decoy") == 0) intact++;
                nk_remove_utf8(decoy);
            }
        }
        CHECK(intact == 2 * DECOYS, "publish: a file already at a temp name is never written through");
        nk_remove_utf8(second);
    }
    /* The tagged publish the dump's workers use: its temp names come from the caller's tag, so it
     * shares no counter between threads; decoys at those names are never written through either,
     * and a file already there is kept. */
    {
        char tagged[800], decoy[900];
        snprintf(tagged, sizeof tagged, "%s/2222222222222222feedf00d.png", dir);
        nk_remove_utf8(tagged);
        enum { DECOYS = 12 };
        const unsigned long tag = 4242ul;
        for (int k = 0; k < DECOYS; k++) {
            snprintf(decoy, sizeof decoy, "%s.tmp%lu.t%lu.%d", tagged, nk_platform_process_id(), tag, k);
            FILE *d = nk_fopen_utf8(decoy, "wb");
            if (d) { fputs("decoy", d); fclose(d); }
        }
        CHECK(sr_file_publish_new_tagged(tagged, "fourth", 6u, tag) == SR_PUBLISH_WRITTEN &&
                  read_back(tagged, buf, sizeof buf) == 6 && strcmp(buf, "fourth") == 0,
              "publish tagged: a file is published past files already at its temp names");
        CHECK(sr_file_publish_new_tagged(tagged, "fifth!", 6u, tag + 1u) == SR_PUBLISH_EXISTS &&
                  read_back(tagged, buf, sizeof buf) == 6 && strcmp(buf, "fourth") == 0,
              "publish tagged: a file already there is kept");
        int intact = 0;
        for (int k = 0; k < DECOYS; k++) {
            snprintf(decoy, sizeof decoy, "%s.tmp%lu.t%lu.%d", tagged, nk_platform_process_id(), tag, k);
            if (read_back(decoy, buf, sizeof buf) == 5 && strcmp(buf, "decoy") == 0) intact++;
            nk_remove_utf8(decoy);
        }
        CHECK(intact == DECOYS, "publish tagged: a file already at a temp name is never written through");
        nk_remove_utf8(tagged);
        CHECK(sr_file_publish_new_tagged("", "x", 1u, tag) == SR_PUBLISH_FAILED, "publish tagged: an empty path is refused");
    }
    /* A screenshot (cap_write_file) into a non-ASCII folder it has to make. */
    CHECK(sr_capture_arm(shot) == 1, "publish: screenshot arm accepted");
    CHECK(sr_capture_serve_host(s_frame, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX) == 1 &&
              sr_capture_result() == 1 && nk_platform_file_exists(shot),
          "publish: a screenshot is written into a non-ASCII folder");
}

int main(int argc, char **argv) {
    snprintf(s_dir, sizeof s_dir, "%s", argc > 1 ? argv[1] : SR_SELFTEST_BUILD_ROOT "/fbcap_selftest");
    make_dir(s_dir);
    make_frame();
    test_refused_requests();
    test_host_capture();
    test_readback_matches_host_capture();
    test_gpu_readback();
    test_skipped_present_cancels();
    test_refused_rearm_keeps_pending();
    test_failed_publication();
    test_explicit_failures();
    test_parent_directory();
    test_absolute_path();
    test_windows_imply_every_present();
    test_picture_probe();
    test_picture_watch();
    test_probe_result_pinned();
    test_deferred_probe();
    test_watch_deferred_matches_synchronous();
    test_png_capture();
    test_screenshot_name();
    test_texdump_folder();
    test_texdump_folder_problem();
    test_screenshot_request();
    test_png_encode();
    test_publish_new_unicode();
    if (s_failures) {
        fprintf(stderr, "fbcap_selftest: %d check(s) failed\n", s_failures);
        return 1;
    }
    puts("fbcap selftest: OK");
    return 0;
}
