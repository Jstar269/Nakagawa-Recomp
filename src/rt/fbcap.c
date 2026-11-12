// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-10-08.
// See NOTICE.md for upstream lineage and modification provenance.
//
// fbcap.c - presenter-neutral frame capture service (issue #57)
//
// The capture state machine, the parent-directory helper and the atomic P6 publisher were
// moved here from src/rt/gpu_sdl3vk/sdl3vk.c, so this file keeps that file's notices.
//
// The single owner of capture state and of the P6 PPM publisher. Presenters only supply
// the pixels they presented (see fbcap.h for the arm/service/resolve protocol); they never
// hold capture state of their own. Host-only C: no SDL, Vulkan or guest-memory dependency,
// so fbcap-selftest drives it directly with synthetic frames.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "fbcap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#endif

enum {
    CAP_IDLE = 0,  /* nothing armed */
    CAP_ARMED,     /* armed; the presenter will service the next presented frame */
    CAP_RECORDED,  /* GPU readback recorded in a submitted present; waiting on its fence */
    CAP_DONE,      /* file published */
    CAP_FAILED     /* attempted and failed */
};

static int s_state = CAP_IDLE;
static int s_result;                 /* 1 published, 0 nothing attempted, -1 failed */
static char s_path[1024];
static sr_cap_source s_src = SR_CAP_SRC_NONE;
static uint32_t s_w, s_h;
static int s_probe;                  /* the pending request is a probe: no file */
static int s_probe_ready;            /* a probe result is waiting to be taken */
static uint32_t s_probe_brightest;
static int s_probe_in_flight;        /* a deferred probe's readback is still on the GPU */
static uint32_t s_probe_w, s_probe_h;
static SrCaptureProbeResolver s_probe_resolver;

static int cap_is_sep(char c) { return c == '/' || c == '\\'; }

/* 1 when `dir` is a directory after this call, created now or already there. A failed
 * mkdir is judged by what is on disk, not by its errno: Windows reports an existing drive
 * root ("C:/") as EACCES, and an existing directory must never fail a publish. An existing
 * non-directory is not a directory, so the publish then fails cleanly. */
static int cap_dir_ready(const char *dir) {
#ifdef _WIN32
    struct _stat st;
    if (_mkdir(dir) == 0) return 1;
    return _stat(dir, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (mkdir(dir, 0777) == 0) return 1;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Create the parent directory of `path` (mkdir -p on the directory component only). FBSNAP
 * publishes under build/snapshots/, which no build step creates. Every prefix is checked
 * without a trailing separator; a root ("/", or a drive root such as "C:/") always exists
 * and is never created. Relative and absolute paths, with either separator, behave alike. */
static int cap_ensure_parent_dir(const char *path) {
    char dir[1024];
    size_t n = strlen(path);
    if (!n || n >= sizeof dir) return 0;
    memcpy(dir, path, n + 1);
    char *sep = NULL;
    for (char *q = dir; *q; q++)
        if (cap_is_sep(*q)) sep = q;
    if (!sep) return 1;                          /* bare file name: the current directory */
    while (sep > dir && cap_is_sep(sep[-1])) sep--;  /* "a//file" names directory "a" */
    *sep = '\0';
    n = (size_t)(sep - dir);
    if (n == 0 || (n == 2 && dir[1] == ':')) return 1;   /* a file directly under "/" or a drive root */
    for (char *q = dir + 1; *q; q++) {
        if (!cap_is_sep(*q) || cap_is_sep(q[-1])) continue;
        size_t len = (size_t)(q - dir);
        if (len == 2 && dir[1] == ':') continue;          /* drive root */
        char save = *q;
        *q = '\0';
        int ready = cap_dir_ready(dir);
        *q = save;
        if (!ready) return 0;
    }
    return cap_dir_ready(dir);
}

/* The single terminal transition out of the armed/recorded states. Every failure funnels
 * through here, so sr_capture_result() can never report a stale or invented outcome. */
static void cap_finish(int ok, const char *why) {
    if (s_state != CAP_ARMED && s_state != CAP_RECORDED) return;
    if (ok) {
        s_state = CAP_DONE;
        s_result = 1;
    } else {
        s_state = CAP_FAILED;
        s_result = -1;
        fprintf(stderr, "fbcap: present capture failed: %s\n", why ? why : "unknown reason");
    }
}

/* ---- PNG publication (screenshots) --------------------------------------------------------
 * A path ending in ".png" (any case) is published as a PNG: 8-bit RGB, no interlace, filter
 * type 0 on every row, and a zlib stream of stored (uncompressed) deflate blocks with its
 * Adler-32. Stored blocks keep the writer small and exact; the file is about as large as the
 * PPM would be. Chunk CRCs are the PNG CRC-32 (polynomial 0xEDB88320). */
static int cap_path_is_png(const char *path) {
    size_t n = strlen(path);
    if (n < 4u) return 0;
    const char *e = path + n - 4u;
    return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') && (e[2] == 'n' || e[2] == 'N') &&
           (e[3] == 'g' || e[3] == 'G');
}

static uint32_t png_crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    static int table_ready;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        table_ready = 1;
    }
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

typedef struct {
    FILE *f;
    uint32_t crc;         /* running CRC of the IDAT chunk (type and data), pre-inverted */
    uint32_t adler_a;
    uint32_t adler_b;
    uint32_t left_total;  /* image bytes (filter bytes included) still to store */
    uint32_t left_block;  /* bytes still to store in the open deflate block */
    int ok;
} PngOut;

static void png_be32(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

static void png_put(PngOut *z, const uint8_t *p, size_t n) {
    if (!z->ok) return;
    z->crc = png_crc_update(z->crc, p, n);
    if (fwrite(p, 1, n, z->f) != n) z->ok = 0;
}

/* One whole chunk with its length and CRC (IHDR, IEND). */
static int png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t head[8];
    png_be32(head, len);
    memcpy(head + 4, type, 4u);
    uint32_t crc = png_crc_update(0xFFFFFFFFu, head + 4, 4u);
    if (len) crc = png_crc_update(crc, data, len);
    uint8_t tail[4];
    png_be32(tail, crc ^ 0xFFFFFFFFu);
    return fwrite(head, 1, 8u, f) == 8u && (len == 0u || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4u, f) == 4u;
}

/* Signature, IHDR, and the head of the one IDAT chunk. cap_pixels_valid bounds w*4*h by
 * UINT32_MAX, so the stored image (h * (1 + 3w) bytes) and the IDAT length fit in 32 bits. */
static int png_begin(PngOut *z, FILE *f, uint32_t w, uint32_t h) {
    static const uint8_t sig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    uint8_t ihdr[13];
    png_be32(ihdr, w);
    png_be32(ihdr + 4, h);
    ihdr[8] = 8u;    /* bit depth */
    ihdr[9] = 2u;    /* colour type: RGB */
    ihdr[10] = 0u;   /* deflate */
    ihdr[11] = 0u;   /* filter method 0 */
    ihdr[12] = 0u;   /* no interlace */
    uint64_t raw = (uint64_t)h * (1u + 3u * (uint64_t)w);
    uint64_t blocks = (raw + 65534u) / 65535u;
    uint64_t idat = 2u + 5u * blocks + raw + 4u;
    if (raw == 0u || idat > 0x7FFFFFFFu) return 0;   /* PNG chunk lengths are at most 2^31 - 1 */
    if (fwrite(sig, 1, 8u, f) != 8u || !png_chunk(f, "IHDR", ihdr, 13u)) return 0;
    uint8_t len[4];
    png_be32(len, (uint32_t)idat);
    if (fwrite(len, 1, 4u, f) != 4u) return 0;
    z->f = f;
    z->crc = 0xFFFFFFFFu;
    z->adler_a = 1u;
    z->adler_b = 0u;
    z->left_total = (uint32_t)raw;
    z->left_block = 0u;
    z->ok = 1;
    static const uint8_t zhead[2] = {0x78u, 0x01u};   /* deflate, 32K window, no dictionary */
    png_put(z, (const uint8_t *)"IDAT", 4u);
    png_put(z, zhead, 2u);
    return z->ok;
}

/* Store n image bytes, opening a stored block (at most 65535 bytes) whenever one is full. */
static void png_z_data(PngOut *z, const uint8_t *p, size_t n) {
    while (z->ok && n) {
        if (n > z->left_total) { z->ok = 0; return; }
        if (z->left_block == 0u) {
            uint32_t blk = z->left_total < 65535u ? z->left_total : 65535u;
            uint8_t hdr[5];
            hdr[0] = blk == z->left_total ? 1u : 0u;   /* BFINAL on the last block, BTYPE 00 */
            hdr[1] = (uint8_t)blk;
            hdr[2] = (uint8_t)(blk >> 8);
            hdr[3] = (uint8_t)~blk;
            hdr[4] = (uint8_t)(~blk >> 8);
            png_put(z, hdr, 5u);
            z->left_block = blk;
        }
        size_t take = n < z->left_block ? n : z->left_block;
        for (size_t i = 0; i < take; i++) {
            z->adler_a = (z->adler_a + p[i]) % 65521u;
            z->adler_b = (z->adler_b + z->adler_a) % 65521u;
        }
        png_put(z, p, take);
        z->left_block -= (uint32_t)take;
        z->left_total -= (uint32_t)take;
        p += take;
        n -= take;
    }
}

/* The Adler-32, the IDAT CRC and IEND. Every image byte must have been stored. */
static int png_end(PngOut *z) {
    if (!z->ok || z->left_total != 0u) return 0;
    uint8_t adler[4];
    png_be32(adler, (z->adler_b << 16) | z->adler_a);
    png_put(z, adler, 4u);
    if (!z->ok) return 0;
    uint8_t crc[4];
    png_be32(crc, z->crc ^ 0xFFFFFFFFu);
    return fwrite(crc, 1, 4u, z->f) == 4u && png_chunk(z->f, "IEND", NULL, 0u);
}

/* Publish w x h pixels as a P6 PPM whose name ends in .ppm, or as the PNG above for a name
 * ending in .png: the header matches the extension and exactly w*h*3 colour bytes follow, in
 * R,G,B order whatever the source byte order. Publication is atomic: a unique temp sibling is
 * written completely and then renamed over the destination, so a reader never observes a
 * half-written file. */
static int cap_write_file(const uint8_t *px, uint32_t row_bytes, sr_cap_order order) {
    if (!cap_ensure_parent_dir(s_path)) return 0;
    static unsigned long s_tmp_seq;
    char tmp[1024 + 64];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%lu", s_path, ++s_tmp_seq) >= (int)sizeof tmp)
        return 0;
    unsigned char *rgb = (unsigned char *)malloc((size_t)s_w * 3u);
    if (!rgb) return 0;
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(rgb); return 0; }
    const int png = cap_path_is_png(s_path);
    PngOut z;
    int ok = png ? png_begin(&z, f, s_w, s_h) : fprintf(f, "P6\n%u %u\n255\n", s_w, s_h) > 0;
    /* Byte offsets of R, G, B inside one pixel for the source byte order. */
    const int ir = order == SR_CAP_ORDER_RGBX ? 0 : 2;
    const int ib = order == SR_CAP_ORDER_RGBX ? 2 : 0;
    for (uint32_t y = 0; ok && y < s_h; y++) {
        const uint8_t *row = px + (size_t)y * row_bytes;
        for (uint32_t x = 0; x < s_w; x++) {
            const uint8_t *p = row + (size_t)x * 4u;
            rgb[x * 3u + 0u] = p[ir];
            rgb[x * 3u + 1u] = p[1];
            rgb[x * 3u + 2u] = p[ib];
        }
        if (png) {
            static const uint8_t filter_none = 0u;   /* every PNG row starts with its filter type */
            png_z_data(&z, &filter_none, 1u);
            png_z_data(&z, rgb, (size_t)s_w * 3u);
            ok = z.ok;
        } else {
            ok = fwrite(rgb, 1, (size_t)s_w * 3u, f) == (size_t)s_w * 3u;
        }
    }
    if (ok && png) ok = png_end(&z);
    if (ok && fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    free(rgb);
    if (!ok) { remove(tmp); return 0; }
#ifdef _WIN32
    if (!MoveFileExA(tmp, s_path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp);
        fprintf(stderr, "fbcap: publish failed (MoveFileExA: %lu)\n", (unsigned long)GetLastError());
        return 0;
    }
#else
    if (rename(tmp, s_path) != 0) { remove(tmp); return 0; }
#endif
    return 1;
}

/* A w x h frame of row_bytes-long rows is a valid pixel description: the frame size is
 * checked in 64-bit arithmetic and bounded like the readback buffer (at most UINT32_MAX
 * source bytes), so no size computation that follows can wrap. */
static int cap_pixels_valid(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                            sr_cap_order order) {
    return px && w != 0u && h != 0u && (uint64_t)row_bytes >= (uint64_t)w * 4u &&
           (uint64_t)row_bytes * h <= UINT32_MAX &&
           (order == SR_CAP_ORDER_BGRX || order == SR_CAP_ORDER_RGBX);
}

/* The probe's result: the brightest of the three colour bytes of any pixel. The ignored byte
 * sits at offset 3 in both orders. Rows are scanned in order and the scan stops after the
 * first row that holds a 255, the largest possible result. */
static uint32_t probe_brightest(const uint8_t *px, uint32_t w, uint32_t h, uint32_t row_bytes) {
    uint32_t brightest = 0;
    for (uint32_t y = 0; y < h && brightest < 255u; y++) {
        const uint8_t *row = px + (size_t)y * row_bytes;
        uint32_t m = 0;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = row + (size_t)x * 4u;
            if (p[0] > m) m = p[0];
            if (p[1] > m) m = p[1];
            if (p[2] > m) m = p[2];
        }
        if (m > brightest) brightest = m;
    }
    return brightest;
}

/* Validate the pixel description, publish the capture, then resolve it. */
static int cap_publish(const void *px, uint32_t row_bytes, sr_cap_order order) {
    if (!cap_pixels_valid(px, s_w, s_h, row_bytes, order)) {
        cap_finish(0, "invalid pixel description");
        return -1;
    }
    if (s_probe) {
        s_probe_brightest = probe_brightest((const uint8_t *)px, s_w, s_h, row_bytes);
        s_probe_ready = 1;
        cap_finish(1, NULL);
        return 1;
    }
    if (!cap_write_file((const uint8_t *)px, row_bytes, order)) {
        cap_finish(0, "write or publish failed");
        return -1;
    }
    cap_finish(1, NULL);
    return 1;
}

int sr_capture_arm(const char *path) {
    /* A rejected request is a new capture attempt, not permission to reuse the last
     * completed result. In-flight state is preserved; only the reportable outcome clears. */
    s_result = 0;
    if (s_state == CAP_ARMED || s_state == CAP_RECORDED) return 0;
    if (!path || !path[0] || strlen(path) >= sizeof s_path) return 0;
    snprintf(s_path, sizeof s_path, "%s", path);
    s_probe = 0;
    s_src = SR_CAP_SRC_NONE;
    s_w = s_h = 0;
    s_state = CAP_ARMED;
    return 1;
}

int sr_capture_arm_probe(void) {
    s_result = 0;
    if (s_state == CAP_ARMED || s_state == CAP_RECORDED || s_probe_in_flight) return 0;
    s_path[0] = '\0';
    s_probe = 1;
    s_src = SR_CAP_SRC_NONE;
    s_w = s_h = 0;
    s_state = CAP_ARMED;
    return 1;
}

int sr_capture_probe_take(uint32_t *brightest) {
    if (!s_probe_ready) return 0;
    s_probe_ready = 0;
    if (brightest) *brightest = s_probe_brightest;
    return 1;
}

int sr_capture_pending_is_probe(void) {
    return (s_state == CAP_ARMED || s_state == CAP_RECORDED) && s_probe;
}

void sr_capture_probe_defer(void) {
    if (s_state != CAP_RECORDED || !s_probe) return;
    s_probe_in_flight = 1;
    s_probe_w = s_w;
    s_probe_h = s_h;
    /* The capture slot is free again; the probe has no reportable file result. */
    s_probe = 0;
    s_state = CAP_IDLE;
    s_result = 0;
}

int sr_capture_probe_in_flight(void) { return s_probe_in_flight; }

int sr_capture_publish_probe(const void *px, uint32_t row_bytes, sr_cap_order order) {
    if (!s_probe_in_flight) return 0;
    s_probe_in_flight = 0;
    if (!cap_pixels_valid(px, s_probe_w, s_probe_h, row_bytes, order)) {
        fprintf(stderr, "fbcap: picture probe failed: invalid pixel description\n");
        return -1;
    }
    s_probe_brightest = probe_brightest((const uint8_t *)px, s_probe_w, s_probe_h, row_bytes);
    s_probe_ready = 1;
    return 1;
}

void sr_capture_probe_drop(const char *why) {
    if (!s_probe_in_flight) return;
    s_probe_in_flight = 0;
    fprintf(stderr, "fbcap: picture probe dropped: %s\n", why ? why : "unknown reason");
}

void sr_capture_set_probe_resolver(SrCaptureProbeResolver resolver) { s_probe_resolver = resolver; }

int sr_capture_probe_poll(int block) {
    if (!s_probe_in_flight || !s_probe_resolver) return 0;
    return s_probe_resolver(block);
}

int sr_picture_watch_probe(SrPictureWatch *w, uint32_t vcount, uint32_t brightest) {
    if (!w) return SR_PICTURE_WATCH_NONE;
    w->pending = 0;
    w->last_vcount = vcount;
    if (brightest <= SR_PICTURE_DARK_MAX) {
        if (!w->dark) {
            w->dark = 1;
            w->first_vcount = vcount;
            w->reported = 0;
        }
        return SR_PICTURE_WATCH_NONE;
    }
    int resumed = w->dark && w->reported > 0u;
    w->dark = 0;
    w->reported = 0;
    return resumed ? SR_PICTURE_WATCH_RESUMED : SR_PICTURE_WATCH_NONE;
}

int sr_picture_watch_tick(SrPictureWatch *w, uint32_t vcount, uint32_t *dark_vblanks) {
    if (!w || !w->dark) return SR_PICTURE_WATCH_NONE;
    /* VCOUNT is monotonic for the process, so both differences are true elapsed counts. */
    if (vcount - w->last_vcount > 2u * SR_PICTURE_PROBE_EVERY) return SR_PICTURE_WATCH_NONE;
    uint32_t elapsed = vcount - w->first_vcount;
    uint32_t bucket = elapsed / SR_PICTURE_REPORT_EVERY;
    if (bucket <= w->reported) return SR_PICTURE_WATCH_NONE;
    w->reported = bucket;
    if (dark_vblanks) *dark_vblanks = elapsed;
    return SR_PICTURE_WATCH_REPORT;
}

void sr_picture_watch_probe_sent(SrPictureWatch *w, uint32_t vcount) {
    if (!w) return;
    w->pending = 1;
    w->pending_vcount = vcount;
}

int sr_picture_watch_tick_needs_probe(const SrPictureWatch *w, uint32_t vcount) {
    if (!w || !w->pending) return 0;
    /* A dark result opens a stretch at the probe, due a report once a period has passed. */
    if (vcount - w->pending_vcount >= SR_PICTURE_REPORT_EVERY) return 1;
    /* Fed, the result either keeps the stretch (last_vcount becomes the probe's, so the tick
     * is not quiet) or ends it; it matters only when a new report bucket is due. */
    return w->dark && (vcount - w->first_vcount) / SR_PICTURE_REPORT_EVERY > w->reported;
}

int sr_picture_watch_collect(SrPictureWatch *w, int block, uint32_t *brightest) {
    if (!w || !w->pending) return SR_PICTURE_WATCH_NONE;
    uint32_t b = 0u;
    (void)sr_capture_probe_poll(block);
    if (sr_capture_probe_take(&b)) {
        if (brightest) *brightest = b;
        return sr_picture_watch_probe(w, w->pending_vcount, b);
    }
    if (block) sr_capture_probe_drop("no presenter finished the readback");
    if (!sr_capture_probe_in_flight()) w->pending = 0;
    return SR_PICTURE_WATCH_NONE;
}

int sr_capture_result(void) { return s_result; }

void sr_capture_cancel(void) {
    if (s_state != CAP_ARMED) return;
    s_state = CAP_IDLE;
    s_path[0] = '\0';
    s_src = SR_CAP_SRC_NONE;
    s_w = s_h = 0;
    s_result = 0;
}

void sr_capture_fail(const char *why) {
    if (s_state != CAP_ARMED && s_state != CAP_RECORDED) return;
    cap_finish(0, why);
}

const char *sr_capture_source_label(void) {
    switch (s_src) {
    case SR_CAP_SRC_CPU: return "cpu-framebuffer";
    case SR_CAP_SRC_GPU: return "gpu-render-target";
    default:             return "";
    }
}

int sr_capture_is_armed(void) { return s_state == CAP_ARMED; }

int sr_capture_is_recorded(void) { return s_state == CAP_RECORDED; }

int sr_capture_serve_host(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                          sr_cap_order order) {
    if (s_state != CAP_ARMED) return 0;
    s_src = SR_CAP_SRC_CPU;
    s_w = w;
    s_h = h;
    return cap_publish(px, row_bytes, order);
}

void sr_capture_mark_recorded(sr_cap_source src, uint32_t w, uint32_t h) {
    if (s_state != CAP_ARMED) return;
    s_src = src;
    s_w = w;
    s_h = h;
    s_state = CAP_RECORDED;
}

int sr_capture_publish_recorded(const void *px, uint32_t row_bytes, sr_cap_order order) {
    if (s_state != CAP_RECORDED) return 0;
    return cap_publish(px, row_bytes, order);
}
