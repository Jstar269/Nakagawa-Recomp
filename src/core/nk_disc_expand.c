/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Compressed disc images expanded once at import (see nk_disc_expand.h).
 *
 * Project-authored from the public descriptions of the formats: the CISO
 * header and block index (CSO v1, and the v2 revision that adds LZ4 blocks and
 * marks a stored block by its size), ZISO (the same layout with LZ4 blocks)
 * and the LZ4 block format. No third-party CSO, ZSO or LZ4 source was copied.
 * DEFLATE blocks go through the project's own nk_psp_inflate_raw; CHD goes
 * through the registered backend (nk_disc_chd.c).
 */

#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "nk_disc_expand.h"

#include "nk_iso.h"
#include "nk_platform.h"
#include "nk_shared_helpers.h"
#include "nk_psp_inflate.h"
#include "nk_psp_sha1.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define NK_CISO_HEADER_BYTES 24u
#define NK_CISO_MIN_BLOCK 2048u
#define NK_CISO_MAX_BLOCK (1024u * 1024u)
#define NK_CISO_MAX_ALIGN 16u
#define NK_CISO_PLAIN_FLAG 0x80000000u
#define NK_DISC_FINGERPRINT_HEAD (64u * 1024u)
#define NK_DISC_PROGRESS_STEP (1024u * 1024u)
#define NK_DISC_IO_BUFFER (1024u * 1024u)
#define NK_DISC_EXPANDED_INVALID "DISC_IMAGE_EXPANDED_INVALID"
#define NK_DISC_IDENTITY_TAG "sha256:"
#define NK_DISC_CANCELLED_UNWRITTEN "Adding the disc image was cancelled before anything was written."

static const NkDiscChdBackend *s_chd_backend = NULL;

void nk_disc_set_chd_backend(const NkDiscChdBackend *backend) {
    s_chd_backend = backend;
}

static bool disc_fail(NkDiscExpandStatus *status, const char *code, const char *fmt, ...) {
    if (status) {
        va_list args;
        snprintf(status->code, sizeof(status->code), "%s", code);
        va_start(args, fmt);
        vsnprintf(status->message, sizeof(status->message), fmt, args);
        va_end(args);
    }
    return false;
}

static void disc_status_clear(NkDiscExpandStatus *status) {
    if (status) {
        status->code[0] = '\0';
        status->message[0] = '\0';
    }
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

NkDiscFormat nk_disc_format_from_magic(const uint8_t *bytes, size_t length) {
    if (!bytes) return NK_DISC_FORMAT_NONE;
    if (length >= 4u && memcmp(bytes, "CISO", 4) == 0) return NK_DISC_FORMAT_CSO;
    if (length >= 4u && memcmp(bytes, "ZISO", 4) == 0) return NK_DISC_FORMAT_ZSO;
    if (length >= 8u && memcmp(bytes, "MComprHD", 8) == 0) return NK_DISC_FORMAT_CHD;
    return NK_DISC_FORMAT_NONE;
}

NkDiscFormat nk_disc_image_format(const char *path) {
    uint8_t magic[8];
    size_t got;
    FILE *f;
    if (!path || !path[0]) return NK_DISC_FORMAT_NONE;
    f = nk_fopen_utf8(path, "rb");
    if (!f) return NK_DISC_FORMAT_NONE;
    got = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    return nk_disc_format_from_magic(magic, got);
}

const char *nk_disc_format_extension(NkDiscFormat format) {
    switch (format) {
    case NK_DISC_FORMAT_CSO: return "cso";
    case NK_DISC_FORMAT_ZSO: return "zso";
    case NK_DISC_FORMAT_CHD: return "chd";
    default: return "";
    }
}

/* ---- source file ------------------------------------------------------- */

typedef struct {
    FILE *f;
    uint64_t size;
    uint64_t pos; /* where the next fread starts; UINT64_MAX when unknown */
} NkDiscSource;

static bool source_open(NkDiscSource *source, const char *path, NkDiscExpandStatus *status) {
    int64_t end;
    memset(source, 0, sizeof(*source));
    source->f = nk_fopen_utf8(path, "rb");
    if (!source->f) {
        return disc_fail(status, NK_DISC_READ_FAILED,
                         "The disc image could not be opened for reading.");
    }
    /* Before any other use of the stream, as setvbuf requires. */
    setvbuf(source->f, NULL, _IOFBF, NK_DISC_IO_BUFFER);
    if (nk_fseek64(source->f, 0, SEEK_END) != 0 || (end = nk_ftell64(source->f)) < 0 ||
        nk_fseek64(source->f, 0, SEEK_SET) != 0) {
        fclose(source->f);
        source->f = NULL;
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image's size could not be read.");
    }
    source->size = (uint64_t)end;
    source->pos = 0;
    return true;
}

static void source_close(NkDiscSource *source) {
    if (source->f) fclose(source->f);
    source->f = NULL;
}

/* Read exactly `bytes` at `offset`; a span past the end of the file is a
   truncated image. Blocks are read in file order, so the stream seeks only
   when the next span does not start where the last one ended (a seek would
   throw away the read buffer). */
static bool source_read_at(NkDiscSource *source, uint64_t offset, void *dst, size_t bytes,
                           NkDiscExpandStatus *status) {
    if (offset > source->size || (uint64_t)bytes > source->size - offset) {
        return disc_fail(status, NK_DISC_TRUNCATED,
                         "The disc image ends before the data its header describes.");
    }
    if (offset != source->pos &&
        (offset > (uint64_t)INT64_MAX || nk_fseek64(source->f, (int64_t)offset, SEEK_SET) != 0)) {
        source->pos = UINT64_MAX;
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image could not be read.");
    }
    if (fread(dst, 1, bytes, source->f) != bytes) {
        source->pos = UINT64_MAX;
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image could not be read.");
    }
    source->pos = offset + bytes;
    return true;
}

/* ---- output ------------------------------------------------------------ */

/* Writes the expansion to f, or, when compare is set instead, checks it
   against the image already there: the first difference stops the expansion
   with differs set (and a status disc_decode clears: it never leaves this
   file). */
typedef struct {
    FILE *f;
    FILE *compare;
    bool differs;
    uint64_t written;
    uint64_t total;
    uint64_t next_report;
    const NkDiscExpandCallbacks *callbacks;
} NkDiscSink;

#define NK_DISC_COMPARE_CHUNK (64u * 1024u)

static bool sink_compare(NkDiscSink *sink, const uint8_t *data, size_t bytes,
                         NkDiscExpandStatus *status) {
    uint8_t stored[NK_DISC_COMPARE_CHUNK];
    while (bytes > 0) {
        size_t piece = bytes < sizeof(stored) ? bytes : sizeof(stored);
        if (fread(stored, 1, piece, sink->compare) != piece || memcmp(stored, data, piece) != 0) {
            sink->differs = true;
            return disc_fail(status, NK_DISC_INTERNAL, "The disc images differ.");
        }
        data += piece;
        bytes -= piece;
    }
    return true;
}

static bool sink_write(NkDiscSink *sink, const uint8_t *data, size_t bytes,
                       NkDiscExpandStatus *status) {
    if (bytes > sink->total - sink->written) {
        return disc_fail(status, NK_DISC_INTERNAL, "The expansion produced more data than expected.");
    }
    if (sink->compare) {
        if (!sink_compare(sink, data, bytes, status)) return false;
    } else if (bytes && fwrite(data, 1, bytes, sink->f) != bytes) {
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "The expanded disc image could not be written (%s). Check that the "
                         "drive has enough free space.", strerror(errno));
    }
    sink->written += bytes;
    if (sink->written >= sink->next_report || sink->written == sink->total) {
        sink->next_report = sink->written + NK_DISC_PROGRESS_STEP;
        if (sink->callbacks && sink->callbacks->on_progress) {
            sink->callbacks->on_progress(sink->written, sink->total, sink->callbacks->user);
        }
        if (sink->callbacks && sink->callbacks->is_cancelled &&
            sink->callbacks->is_cancelled(sink->callbacks->user)) {
            return disc_fail(status, NK_DISC_CANCELLED,
                             sink->compare ? NK_DISC_CANCELLED_UNWRITTEN
                                           : "The expansion was cancelled, and the partial disc "
                                             "image was removed.");
        }
    }
    return true;
}

/* ---- LZ4 block decoder ------------------------------------------------- */

/* Decode one LZ4 block into dst (capacity dst_cap) until at least `want`
   bytes are out. Bytes after that point (index alignment padding) are not
   read. Every length, offset and copy is checked against both buffers. */
static bool lz4_block_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap,
                             size_t want, size_t *out_len, const char **why) {
    size_t ip = 0;
    size_t op = 0;
    while (op < want) {
        uint8_t token;
        size_t literals;
        size_t offset;
        size_t match;
        if (ip >= src_len) {
            *why = "the block ends before its data is complete";
            return false;
        }
        token = src[ip++];
        literals = (size_t)(token >> 4);
        if (literals == 15u) {
            uint8_t more;
            do {
                if (ip >= src_len) {
                    *why = "a literal length runs past the block";
                    return false;
                }
                more = src[ip++];
                literals += more;
                if (literals > dst_cap) {
                    *why = "a literal run is longer than the block";
                    return false;
                }
            } while (more == 255u);
        }
        if (literals > src_len - ip) {
            *why = "a literal run runs past the block";
            return false;
        }
        if (literals > dst_cap - op) {
            *why = "a literal run is longer than the block";
            return false;
        }
        memcpy(dst + op, src + ip, literals);
        ip += literals;
        op += literals;
        if (op >= want) break;
        if (src_len - ip < 2u) {
            *why = "a match offset runs past the block";
            return false;
        }
        offset = (size_t)src[ip] | ((size_t)src[ip + 1u] << 8);
        ip += 2u;
        if (offset == 0u || offset > op) {
            *why = "a match points before the start of the block";
            return false;
        }
        match = (size_t)(token & 15u);
        if (match == 15u) {
            uint8_t more;
            do {
                if (ip >= src_len) {
                    *why = "a match length runs past the block";
                    return false;
                }
                more = src[ip++];
                match += more;
                if (match > dst_cap) {
                    *why = "a match is longer than the block";
                    return false;
                }
            } while (more == 255u);
        }
        match += 4u;
        if (match > dst_cap - op) {
            *why = "a match is longer than the block";
            return false;
        }
        /* Byte by byte: a match may overlap the bytes it is copying. */
        for (size_t i = 0; i < match; i++) dst[op + i] = dst[op - offset + i];
        op += match;
    }
    *out_len = op;
    return true;
}

/* ---- CSO / ZSO ----------------------------------------------------------- */

typedef struct {
    NkDiscFormat format;
    uint32_t version;     /* 1 or 2 for CSO, 1 for ZSO */
    uint64_t total_bytes;
    uint32_t block_size;
    uint32_t align;
    uint32_t block_count;
    uint32_t *index;      /* block_count + 1 entries */
} NkCisoImage;

static uint64_t ciso_offset(const NkCisoImage *image, uint32_t entry) {
    return (uint64_t)(image->index[entry] & ~NK_CISO_PLAIN_FLAG) << image->align;
}

static uint64_t ciso_max_block_span(const NkCisoImage *image) {
    return (uint64_t)image->block_size * 2u + ((uint64_t)1u << image->align);
}

static bool ciso_open(NkDiscSource *source, NkDiscFormat format, NkCisoImage *image,
                      NkDiscExpandStatus *status) {
    uint8_t header[NK_CISO_HEADER_BYTES];
    uint32_t header_size;
    uint32_t raw_version;
    uint64_t index_bytes;
    uint64_t data_start;
    const char *name = format == NK_DISC_FORMAT_CSO ? "CSO" : "ZSO";
    memset(image, 0, sizeof(*image));
    image->format = format;
    if (source->size < NK_CISO_HEADER_BYTES) {
        return disc_fail(status, NK_DISC_TRUNCATED,
                         "This %s disc image is shorter than its header.", name);
    }
    if (!source_read_at(source, 0, header, sizeof(header), status)) return false;
    if (nk_disc_format_from_magic(header, sizeof(header)) != format) {
        return disc_fail(status, NK_DISC_HEADER_INVALID, "This is not a %s disc image.", name);
    }
    header_size = read_le32(header + 4);
    image->total_bytes = read_le64(header + 8);
    image->block_size = read_le32(header + 16);
    raw_version = header[20];
    image->align = header[21];

    if (format == NK_DISC_FORMAT_CSO && raw_version <= 1u) {
        image->version = 1u;
    } else if (format == NK_DISC_FORMAT_CSO && raw_version == 2u) {
        image->version = 2u;
    } else if (format == NK_DISC_FORMAT_ZSO && raw_version <= 1u) {
        image->version = 1u;
    } else {
        return disc_fail(status, NK_DISC_VERSION_UNSUPPORTED,
                         "This %s disc image is format version %u, which Nakagawa can't read.",
                         name, (unsigned)raw_version);
    }
    /* v2 fixes the header size; older writers left it 0 or wrote 24. */
    if (header_size != NK_CISO_HEADER_BYTES &&
        !(image->version == 1u && header_size == 0u)) {
        return disc_fail(status, NK_DISC_HEADER_INVALID,
                         "This %s disc image's header names a size of %u bytes, not %u.",
                         name, (unsigned)header_size, (unsigned)NK_CISO_HEADER_BYTES);
    }
    if (image->block_size < NK_CISO_MIN_BLOCK || image->block_size > NK_CISO_MAX_BLOCK ||
        (image->block_size & (image->block_size - 1u)) != 0u) {
        return disc_fail(status, NK_DISC_HEADER_INVALID,
                         "This %s disc image's block size (%u bytes) is not a power of two "
                         "between 2 KiB and 1 MiB.", name, (unsigned)image->block_size);
    }
    if (image->align > NK_CISO_MAX_ALIGN) {
        return disc_fail(status, NK_DISC_HEADER_INVALID,
                         "This %s disc image's index alignment (%u) is out of range.", name,
                         (unsigned)image->align);
    }
    if (image->total_bytes == 0u || image->total_bytes > NK_DISC_MAX_EXPANDED_BYTES ||
        image->total_bytes % NK_DISC_SECTOR_BYTES != 0u) {
        return disc_fail(status, NK_DISC_SIZE_INVALID,
                         "This %s disc image's header names an expanded size that no PSP disc "
                         "has.", name);
    }
    /* Bounded by the size check: at most 2^21 blocks of 2 KiB. */
    image->block_count = (uint32_t)((image->total_bytes + image->block_size - 1u) /
                                    image->block_size);
    index_bytes = ((uint64_t)image->block_count + 1u) * 4u;
    data_start = NK_CISO_HEADER_BYTES + index_bytes;
    if (data_start > source->size) {
        return disc_fail(status, NK_DISC_TRUNCATED,
                         "This %s disc image ends inside its block index.", name);
    }
    image->index = (uint32_t *)malloc((size_t)index_bytes);
    if (!image->index) return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    {
        uint8_t *raw = (uint8_t *)image->index;
        if (!source_read_at(source, NK_CISO_HEADER_BYTES, raw, (size_t)index_bytes, status)) {
            free(image->index);
            image->index = NULL;
            return false;
        }
        for (uint32_t i = 0; i <= image->block_count; i++) {
            image->index[i] = read_le32(raw + (size_t)i * 4u);
        }
    }
    for (uint32_t i = 0; i < image->block_count; i++) {
        uint64_t start = ciso_offset(image, i);
        uint64_t end = ciso_offset(image, i + 1u);
        if ((i == 0u && start < data_start) || end < start || end > source->size ||
            end - start > ciso_max_block_span(image)) {
            free(image->index);
            image->index = NULL;
            return disc_fail(status, NK_DISC_INDEX_INVALID,
                             "Block %u of this %s disc image points outside the file.",
                             (unsigned)i, name);
        }
    }
    return true;
}

static void ciso_close(NkCisoImage *image) {
    free(image->index);
    image->index = NULL;
}

static bool ciso_expand(NkDiscSource *source, NkCisoImage *image, NkDiscSink *sink,
                        NkDiscExpandStatus *status) {
    size_t span_cap = (size_t)ciso_max_block_span(image);
    uint8_t *packed = (uint8_t *)malloc(span_cap);
    uint8_t *plain = (uint8_t *)malloc(image->block_size);
    bool ok = true;
    if (!packed || !plain) {
        free(packed);
        free(plain);
        return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    }
    for (uint32_t i = 0; ok && i < image->block_count; i++) {
        uint64_t start = ciso_offset(image, i);
        size_t span = (size_t)(ciso_offset(image, i + 1u) - start);
        uint64_t remaining = image->total_bytes - (uint64_t)i * image->block_size;
        size_t want = remaining < image->block_size ? (size_t)remaining : image->block_size;
        bool flag = (image->index[i] & NK_CISO_PLAIN_FLAG) != 0u;
        bool stored;
        bool lz4;
        if (image->format == NK_DISC_FORMAT_CSO && image->version == 2u) {
            /* v2: a block as large as a whole block is stored; the flag picks
               LZ4 over DEFLATE for the others. */
            stored = span >= image->block_size;
            lz4 = !stored && flag;
        } else {
            stored = flag;
            lz4 = image->format == NK_DISC_FORMAT_ZSO;
        }
        if (!source_read_at(source, start, packed, span, status)) {
            ok = false;
            break;
        }
        if (stored) {
            if (span < want) {
                ok = disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                               "Stored block %u of this disc image is shorter than a block.",
                               (unsigned)i);
                break;
            }
            ok = sink_write(sink, packed, want, status);
        } else if (lz4) {
            size_t produced = 0;
            const char *why = "";
            if (!lz4_block_decode(packed, span, plain, image->block_size, want, &produced, &why)) {
                ok = disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                               "Block %u of this disc image is damaged: %s.", (unsigned)i, why);
                break;
            }
            ok = sink_write(sink, plain, want, status);
        } else {
            size_t inflated_len = 0;
            char why[128] = "";
            /* Into the block buffer: a block that would inflate past a whole
               block fails as an overflow, as the cap did. */
            int rc = nk_psp_inflate_raw_into(packed, span, plain, image->block_size,
                                             &inflated_len, NULL, why, sizeof(why));
            /* A short last block may be padded to a whole block. */
            if (rc != NK_INFLATE_OK || inflated_len < want ||
                (inflated_len != want && inflated_len != image->block_size)) {
                ok = disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                               "Block %u of this disc image is damaged: %s.", (unsigned)i,
                               rc != NK_INFLATE_OK ? why : "it holds the wrong amount of data");
                break;
            }
            ok = sink_write(sink, plain, want, status);
        }
    }
    free(packed);
    free(plain);
    return ok;
}

/* ---- CHD (through the registered backend) ------------------------------ */

static bool chd_open_checked(const char *path, void **handle, uint64_t *logical_bytes,
                             uint32_t *hunk_bytes, uint8_t raw_sha1[NK_DISC_SHA1_BYTES],
                             NkDiscExpandStatus *status) {
    memset(raw_sha1, 0, NK_DISC_SHA1_BYTES);
    if (!s_chd_backend) {
        return disc_fail(status, NK_DISC_CHD_UNAVAILABLE,
                         "This build of Nakagawa can't read CHD disc images.");
    }
    if (!s_chd_backend->open(path, handle, logical_bytes, hunk_bytes, raw_sha1, status)) {
        if (status && !status->code[0]) {
            disc_fail(status, NK_DISC_CHD_INVALID, "This CHD disc image could not be read.");
        }
        return false;
    }
    if (*logical_bytes == 0u || *logical_bytes > NK_DISC_MAX_EXPANDED_BYTES ||
        *logical_bytes % NK_DISC_SECTOR_BYTES != 0u) {
        s_chd_backend->close(*handle);
        *handle = NULL;
        return disc_fail(status, NK_DISC_SIZE_INVALID,
                         "This CHD disc image names an expanded size that no PSP disc has.");
    }
    if (*hunk_bytes < NK_DISC_SECTOR_BYTES || *hunk_bytes > NK_DISC_CHD_MAX_HUNK_BYTES ||
        *hunk_bytes % NK_DISC_SECTOR_BYTES != 0u) {
        s_chd_backend->close(*handle);
        *handle = NULL;
        return disc_fail(status, NK_DISC_CHD_UNSUPPORTED,
                         "This CHD disc image uses a block size (%u bytes) that is not a "
                         "whole number of 2048-byte sectors.", (unsigned)*hunk_bytes);
    }
    return true;
}

static bool sha1_is_zero(const uint8_t sha1[NK_DISC_SHA1_BYTES]) {
    for (unsigned i = 0; i < NK_DISC_SHA1_BYTES; i++) {
        if (sha1[i] != 0u) return false;
    }
    return true;
}

/* libchdr checks a compressed hunk against its CRC16, but a hunk an
   uncompressed CHD stores as is has no check of its own. The SHA-1 the header
   records for the whole of the data covers both, so a header that records one
   (every chdman image does) is held to it. */
static bool chd_expand(void *handle, uint64_t logical_bytes, uint32_t hunk_bytes,
                       const uint8_t raw_sha1[NK_DISC_SHA1_BYTES], NkDiscSink *sink,
                       NkDiscExpandStatus *status) {
    uint8_t *hunk = (uint8_t *)malloc(hunk_bytes);
    uint64_t hunks = (logical_bytes + hunk_bytes - 1u) / hunk_bytes;
    bool check = !sha1_is_zero(raw_sha1);
    SHA_CTX ctx;
    bool ok = true;
    if (!hunk) return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    if (check) SHAInit(&ctx);
    for (uint64_t i = 0; ok && i < hunks; i++) {
        uint64_t remaining = logical_bytes - i * hunk_bytes;
        size_t want = remaining < hunk_bytes ? (size_t)remaining : hunk_bytes;
        if (!s_chd_backend->read_hunk(handle, (uint32_t)i, hunk, status)) {
            if (status && !status->code[0]) {
                disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                          "Block %u of this CHD disc image is damaged.", (unsigned)i);
            }
            ok = false;
            break;
        }
        /* want <= hunk_bytes <= NK_DISC_CHD_MAX_HUNK_BYTES, so it fits an int. */
        if (check) SHAUpdate(&ctx, hunk, (int)want);
        ok = sink_write(sink, hunk, want, status);
    }
    free(hunk);
    if (ok && check) {
        uint8_t digest[NK_DISC_SHA1_BYTES];
        SHAFinal(digest, &ctx);
        if (memcmp(digest, raw_sha1, NK_DISC_SHA1_BYTES) != 0) {
            ok = disc_fail(status, NK_DISC_BLOCK_CORRUPT,
                           "This CHD disc image is damaged: its data does not match the SHA-1 "
                           "its header records.");
        }
    }
    return ok;
}

/* ---- expansion --------------------------------------------------------- */

bool nk_disc_expanded_size(const char *source_path, NkDiscFormat *out_format,
                           uint64_t *out_expanded_bytes, NkDiscExpandStatus *status) {
    NkDiscFormat format;
    disc_status_clear(status);
    if (out_format) *out_format = NK_DISC_FORMAT_NONE;
    if (out_expanded_bytes) *out_expanded_bytes = 0;
    if (!source_path || !source_path[0]) {
        return disc_fail(status, NK_DISC_INTERNAL, "No disc image was named.");
    }
    format = nk_disc_image_format(source_path);
    if (out_format) *out_format = format;
    if (format == NK_DISC_FORMAT_CSO || format == NK_DISC_FORMAT_ZSO) {
        NkDiscSource source;
        NkCisoImage image;
        bool ok;
        if (!source_open(&source, source_path, status)) return false;
        ok = ciso_open(&source, format, &image, status);
        source_close(&source);
        if (!ok) return false;
        if (out_expanded_bytes) *out_expanded_bytes = image.total_bytes;
        ciso_close(&image);
        return true;
    }
    if (format == NK_DISC_FORMAT_CHD) {
        void *handle = NULL;
        uint64_t logical = 0;
        uint32_t hunk = 0;
        uint8_t raw_sha1[NK_DISC_SHA1_BYTES];
        if (!chd_open_checked(source_path, &handle, &logical, &hunk, raw_sha1, status)) {
            return false;
        }
        s_chd_backend->close(handle);
        if (out_expanded_bytes) *out_expanded_bytes = logical;
        return true;
    }
    return disc_fail(status, NK_DISC_NOT_COMPRESSED,
                     "This file is not a compressed disc image (.cso, .zso or .chd).");
}

static bool path_join(char *out, size_t out_size, const char *dir, const char *name) {
    return nk_shared_path_join(out, out_size, dir, name,
                               nk_platform_path_separator(), NK_SHARED_PATH_JOIN_ALWAYS,
                               NK_SHARED_PATH_REQUIRE_POSITIVE_LENGTH);
}

static bool path_suffix(char *out, size_t out_size, const char *path, const char *suffix) {
    int written = snprintf(out, out_size, "%s%s", path, suffix);
    return written > 0 && (size_t)written < out_size;
}

/* Expand source_path into out_path (a new file), or, with compare, check the
   expansion against the image already at out_path: *differs is then set when
   they are not the same bytes (a different size included) and the call still
   succeeds. Every other failure, a cancel included, fills status. */
static bool disc_decode(const char *source_path, const char *out_path, bool compare,
                        const NkDiscExpandCallbacks *callbacks, uint64_t *out_total,
                        bool *differs, NkDiscExpandStatus *status) {
    NkDiscFormat format;
    NkDiscSink sink;
    NkDiscSource source;
    NkCisoImage image;
    void *chd = NULL;
    uint64_t total = 0;
    uint32_t hunk = 0;
    uint8_t raw_sha1[NK_DISC_SHA1_BYTES];
    bool ok;
    *out_total = 0;
    *differs = false;
    memset(raw_sha1, 0, sizeof(raw_sha1));
    memset(&source, 0, sizeof(source));
    memset(&image, 0, sizeof(image));
    format = nk_disc_image_format(source_path);
    if (format == NK_DISC_FORMAT_CSO || format == NK_DISC_FORMAT_ZSO) {
        if (!source_open(&source, source_path, status)) return false;
        if (!ciso_open(&source, format, &image, status)) {
            source_close(&source);
            return false;
        }
        total = image.total_bytes;
    } else if (format == NK_DISC_FORMAT_CHD) {
        if (!chd_open_checked(source_path, &chd, &total, &hunk, raw_sha1, status)) return false;
    } else {
        return disc_fail(status, NK_DISC_NOT_COMPRESSED,
                         "This file is not a compressed disc image (.cso, .zso or .chd).");
    }

    memset(&sink, 0, sizeof(sink));
    sink.total = total;
    sink.callbacks = callbacks;
    if (compare) {
        /* An image of another size is another image: nothing to expand. */
        if (nk_platform_get_file_size(out_path) != (int64_t)total) {
            *differs = true;
            ok = true;
            goto close_source;
        }
        sink.compare = nk_fopen_utf8(out_path, "rb");
        if (!sink.compare) {
            ok = disc_fail(status, NK_DISC_READ_FAILED,
                           "The disc image Nakagawa expanded earlier could not be read.");
            goto close_source;
        }
        setvbuf(sink.compare, NULL, _IOFBF, NK_DISC_IO_BUFFER);
    } else {
        sink.f = nk_platform_fopen_private(out_path, "wb");
        if (!sink.f) {
            ok = disc_fail(status, NK_DISC_WRITE_FAILED,
                           "The expanded disc image could not be created in Nakagawa's data "
                           "folder.");
            goto close_source;
        }
        setvbuf(sink.f, NULL, _IOFBF, NK_DISC_IO_BUFFER);
    }
    if (callbacks && callbacks->on_progress) callbacks->on_progress(0, total, callbacks->user);
    ok = format == NK_DISC_FORMAT_CHD ? chd_expand(chd, total, hunk, raw_sha1, &sink, status)
                                      : ciso_expand(&source, &image, &sink, status);
    if (sink.compare) {
        fclose(sink.compare);
        if (sink.differs) {
            *differs = true;
            disc_status_clear(status);
            ok = true;
        }
    } else if (fclose(sink.f) != 0 && ok) {
        ok = disc_fail(status, NK_DISC_WRITE_FAILED,
                       "The expanded disc image could not be written. Check that the drive "
                       "has enough free space.");
    }
    if (ok && !*differs && sink.written != total) {
        ok = disc_fail(status, NK_DISC_INTERNAL, "The expansion ended early.");
    }
close_source:
    if (format == NK_DISC_FORMAT_CHD) {
        s_chd_backend->close(chd);
    } else {
        ciso_close(&image);
        source_close(&source);
    }
    *out_total = total;
    return ok;
}

bool nk_disc_expand_file(const char *source_path, const char *dest_iso,
                         const NkDiscExpandCallbacks *callbacks,
                         uint64_t *out_expanded_bytes, NkDiscExpandStatus *status) {
    char part[NK_MAX_PATH + 8];
    uint64_t total = 0;
    bool differs = false;
    bool ok;
    disc_status_clear(status);
    if (out_expanded_bytes) *out_expanded_bytes = 0;
    if (!source_path || !dest_iso || !dest_iso[0]) {
        return disc_fail(status, NK_DISC_INTERNAL, "No disc image was named.");
    }
    if (!path_suffix(part, sizeof(part), dest_iso, ".part")) {
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "The path for the expanded disc image is too long.");
    }
    ok = disc_decode(source_path, part, false, callbacks, &total, &differs, status);
    if (ok && nk_platform_get_file_size(part) != (int64_t)total) {
        ok = disc_fail(status, NK_DISC_WRITE_FAILED,
                       "The expanded disc image on the drive is not the expected size.");
    }
    if (ok && nk_rename_utf8(part, dest_iso) != 0) {
        ok = disc_fail(status, NK_DISC_WRITE_FAILED,
                       "The expanded disc image could not be moved into place.");
    }
    if (!ok) {
        (void)nk_remove_utf8(part);
        return false;
    }
    if (out_expanded_bytes) *out_expanded_bytes = total;
    return true;
}

/* ---- import: identity, reuse and the per-title folder ------------------- */

static bool file_modified_time(const char *path, uint64_t *out) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[32768];
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768) <= 0 ||
        !GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        return false;
    }
    *out = ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) |
           data.ftLastWriteTime.dwLowDateTime;
    return true;
#else
    struct stat data;
    if (stat(path, &data) != 0) return false;
    *out = (uint64_t)data.st_mtim.tv_sec * 1000000000ull + (uint64_t)data.st_mtim.tv_nsec;
    return true;
#endif
}

static void put_le64(uint8_t *p, uint64_t value) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static void hex_encode(const uint8_t *bytes, size_t count, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; i++) {
        out[i * 2u] = hex[bytes[i] >> 4];
        out[i * 2u + 1u] = hex[bytes[i] & 15u];
    }
    out[count * 2u] = '\0';
}

#define NK_DISC_UNREADABLE_SOURCE                                                          \
    "The file could not be read to its end, so Nakagawa could not tell whether it was "  \
    "added before. Check that the drive it is on is connected, then add it again. "      \
    "Nothing was kept."

bool nk_disc_file_identity(const char *path, const NkDiscExpandCallbacks *callbacks,
                           char out[NK_DISC_IDENTITY_CHARS + 1], uint64_t *out_bytes,
                           NkDiscExpandStatus *status) {
    NkDiscSource source;
    NkSha256 ctx;
    uint8_t digest[32];
    uint8_t *buffer;
    uint8_t extra;
    uint64_t done = 0;
    bool ok = true;
    disc_status_clear(status);
    if (out_bytes) *out_bytes = 0;
    if (!out) return disc_fail(status, NK_DISC_INTERNAL, "No result was given.");
    out[0] = '\0';
    if (!path || !path[0]) return disc_fail(status, NK_DISC_INTERNAL, "No file was named.");
    if (!source_open(&source, path, status)) return false;
    buffer = (uint8_t *)malloc(NK_DISC_IO_BUFFER);
    if (!buffer) {
        source_close(&source);
        return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    }
    nk_sha256_init(&ctx);
    while (ok && done < source.size) {
        uint64_t left = source.size - done;
        size_t piece = left < NK_DISC_IO_BUFFER ? (size_t)left : (size_t)NK_DISC_IO_BUFFER;
        if (fread(buffer, 1, piece, source.f) != piece) {
            ok = disc_fail(status, NK_DISC_READ_FAILED, NK_DISC_UNREADABLE_SOURCE);
            break;
        }
        nk_sha256_update(&ctx, buffer, piece);
        done += piece;
        if (callbacks && callbacks->on_progress) {
            callbacks->on_progress(done, source.size, callbacks->user);
        }
        if (callbacks && callbacks->is_cancelled && callbacks->is_cancelled(callbacks->user)) {
            ok = disc_fail(status, NK_DISC_CANCELLED, NK_DISC_CANCELLED_UNWRITTEN);
        }
    }
    /* The file ends where its size said it would: one that grew while it was
       read is not the file that was measured. */
    if (ok && fread(&extra, 1, 1, source.f) != 0) {
        ok = disc_fail(status, NK_DISC_READ_FAILED, NK_DISC_UNREADABLE_SOURCE);
    }
    free(buffer);
    source_close(&source);
    if (!ok) return false;
    nk_sha256_finish(&ctx, digest);
    hex_encode(digest, sizeof(digest), out);
    if (out_bytes) *out_bytes = done;
    return true;
}

static bool disc_fingerprint(const char *path, NkDiscFormat format, uint64_t size,
                             char out[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1],
                             NkDiscExpandStatus *status) {
    static const char domain[] = "nk-disc-fingerprint/v1";
    uint8_t fields[17];
    uint8_t digest[20];
    uint64_t modified = 0;
    size_t head_len = size < NK_DISC_FINGERPRINT_HEAD ? (size_t)size : NK_DISC_FINGERPRINT_HEAD;
    uint8_t *head;
    SHA_CTX ctx;
    FILE *f;
    if (!file_modified_time(path, &modified)) {
        return disc_fail(status, NK_DISC_READ_FAILED,
                         "The disc image's modification time could not be read.");
    }
    head = (uint8_t *)malloc(head_len ? head_len : 1u);
    if (!head) return disc_fail(status, NK_DISC_INTERNAL, "Out of memory.");
    f = nk_fopen_utf8(path, "rb");
    if (!f || fread(head, 1, head_len, f) != head_len) {
        if (f) fclose(f);
        free(head);
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image could not be read.");
    }
    fclose(f);
    fields[0] = (uint8_t)format;
    put_le64(fields + 1, size);
    put_le64(fields + 9, modified);
    SHAInit(&ctx);
    SHAUpdate(&ctx, (BYTE *)domain, (int)(sizeof(domain) - 1u));
    SHAUpdate(&ctx, fields, (int)sizeof(fields));
    SHAUpdate(&ctx, head, (int)head_len);
    SHAFinal(digest, &ctx);
    free(head);
    hex_encode(digest, NK_DISC_LEGACY_FINGERPRINT_CHARS / 2u, out);
    return true;
}

bool nk_disc_legacy_fingerprint(const char *source, NkDiscFormat format,
                                char out[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1],
                                NkDiscExpandStatus *status) {
    int64_t size;
    disc_status_clear(status);
    if (!out) return disc_fail(status, NK_DISC_INTERNAL, "No result was given.");
    out[0] = '\0';
    if (!source || !source[0]) return disc_fail(status, NK_DISC_INTERNAL, "No file was named.");
    size = nk_platform_get_file_size(source);
    if (size < 0) return disc_fail(status, NK_DISC_READ_FAILED, "The file could not be opened.");
    return disc_fingerprint(source, format, (uint64_t)size, out, status);
}

/* Four capital letters and five digits: the only form used as a folder name. */
static bool disc_id_valid(const char *id) {
    if (!id || strlen(id) != 9u) return false;
    for (size_t i = 0; i < 9u; i++) {
        unsigned char c = (unsigned char)id[i];
        if (i < 4u ? !(c >= 'A' && c <= 'Z') : !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

/* One short line, written beside its final name and renamed into place. */
static bool write_line_file(const char *path, const char *line) {
    char tmp[NK_MAX_PATH + 8];
    FILE *f;
    bool ok;
    if (!path_suffix(tmp, sizeof(tmp), path, ".tmp")) return false;
    f = nk_platform_fopen_private(tmp, "wb");
    if (!f) return false;
    ok = fprintf(f, "%s\n", line) > 0;
    ok = (fclose(f) == 0) && ok;
    if (ok) ok = nk_rename_utf8(tmp, path) == 0;
    if (!ok) (void)nk_remove_utf8(tmp);
    return ok;
}

static bool read_line_file(const char *path, char *out, size_t out_size) {
    FILE *f = nk_fopen_utf8(path, "rb");
    size_t got;
    if (!f) return false;
    got = fread(out, 1, out_size - 1u, f);
    fclose(f);
    out[got] = '\0';
    while (got > 0 && (out[got - 1u] == '\n' || out[got - 1u] == '\r')) out[--got] = '\0';
    return got > 0;
}

typedef struct {
    char expanded_dir[NK_MAX_PATH];
    char ref[NK_MAX_PATH];
    char staging[NK_MAX_PATH];
    char lock[NK_MAX_PATH];
} NkDiscImportPaths;

/* <data>/discs/expanded/<key>.ref, .lock and .iso; key is an identity, or a
   sampled fingerprint when an earlier record is looked up (its .ref only). */
static bool import_paths(const char *data_root, const char *key, NkDiscImportPaths *paths) {
    char discs[NK_MAX_PATH];
    char name[NK_DISC_IDENTITY_CHARS + 8];
    if (!path_join(discs, sizeof(discs), data_root, "discs") ||
        !path_join(paths->expanded_dir, sizeof(paths->expanded_dir), discs, "expanded")) {
        return false;
    }
    snprintf(name, sizeof(name), "%s.ref", key);
    if (!path_join(paths->ref, sizeof(paths->ref), paths->expanded_dir, name)) return false;
    snprintf(name, sizeof(name), "%s.lock", key);
    if (!path_join(paths->lock, sizeof(paths->lock), paths->expanded_dir, name)) return false;
    snprintf(name, sizeof(name), "%s.iso", key);
    return path_join(paths->staging, sizeof(paths->staging), paths->expanded_dir, name);
}

/* <data>/discs/<ID>/<ID>.<key>.<ext>.iso and its directory, <key> being the
   start of the source's identity (or, for an earlier record, of its sampled
   fingerprint), so two different images of one disc ID never share a file. A
   NULL key names the <ID>.<ext>.iso that earlier builds wrote. */
static bool title_iso_path(const char *data_root, const char *disc_id, const char *key,
                           NkDiscFormat format, char *dir, size_t dir_size, char *iso,
                           size_t iso_size) {
    char discs[NK_MAX_PATH];
    char name[64];
    if (!path_join(discs, sizeof(discs), data_root, "discs") ||
        !path_join(dir, dir_size, discs, disc_id)) {
        return false;
    }
    if (key) {
        snprintf(name, sizeof(name), "%s.%.*s.%s.iso", disc_id,
                 (int)NK_DISC_TITLE_FINGERPRINT_CHARS, key, nk_disc_format_extension(format));
    } else {
        snprintf(name, sizeof(name), "%s.%s.iso", disc_id, nk_disc_format_extension(format));
    }
    return path_join(iso, iso_size, dir, name);
}

/* A per-title file name a ref may hold: the disc ID, a dot, then letters,
   digits and single dots, ending in ".iso". Never a path. */
static bool title_file_name_valid(const char *disc_id, const char *name) {
    size_t len = strlen(name);
    if (len < 14u || len > 63u || strncmp(name, disc_id, 9u) != 0 || name[9] != '.' ||
        strcmp(name + len - 4u, ".iso") != 0 || strstr(name, "..") != NULL) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.')) {
            return false;
        }
    }
    return true;
}

/* The image an identity's ref names ("<DISC_ID> <file name>"). */
static bool read_identity_ref(const char *ref, const char *data_root, char disc_id[16],
                              char *iso, size_t iso_size) {
    char line[96];
    char discs[NK_MAX_PATH];
    char dir[NK_MAX_PATH];
    if (!read_line_file(ref, line, sizeof(line)) || strlen(line) < 11u || line[9] != ' ') {
        return false;
    }
    line[9] = '\0';
    if (!disc_id_valid(line) || !title_file_name_valid(line, line + 10) ||
        !path_join(discs, sizeof(discs), data_root, "discs") ||
        !path_join(dir, sizeof(dir), discs, line) || !path_join(iso, iso_size, dir, line + 10)) {
        return false;
    }
    snprintf(disc_id, 16, "%.9s", line);
    return true;
}

/* Whether the image at iso is this source's: its .source records the
   source's identity and it has the expanded size. Only the size of the image
   on the drive is checked; a change that keeps the size is not seen here. */
static bool identity_recorded(const char *iso, const NkDiscImport *out) {
    static const char tag[] = NK_DISC_IDENTITY_TAG;
    char sidecar[NK_MAX_PATH + 8];
    char recorded[sizeof(tag) + NK_DISC_IDENTITY_CHARS + 8];
    return path_suffix(sidecar, sizeof(sidecar), iso, ".source") &&
           read_line_file(sidecar, recorded, sizeof(recorded)) &&
           strncmp(recorded, tag, sizeof(tag) - 1u) == 0 &&
           strcmp(recorded + sizeof(tag) - 1u, out->identity) == 0 &&
           nk_platform_get_file_size(iso) == (int64_t)out->expanded_bytes;
}

/* Whether the image at iso carries an earlier, sampled record of this
   fingerprint and the expanded size: a candidate only, never a match. */
static bool legacy_recorded(const char *iso, const char *fingerprint, uint64_t expanded_bytes) {
    char sidecar[NK_MAX_PATH + 8];
    char recorded[NK_DISC_IDENTITY_CHARS + 16];
    return path_suffix(sidecar, sizeof(sidecar), iso, ".source") &&
           read_line_file(sidecar, recorded, sizeof(recorded)) &&
           strcmp(recorded, fingerprint) == 0 &&
           nk_platform_get_file_size(iso) == (int64_t)expanded_bytes;
}

/* The image an earlier import recorded under this file's sampled fingerprint,
   under its fingerprint's name or the older unnamed one. */
static bool find_legacy_import(const char *source, const char *data_root, const NkDiscImport *out,
                               char disc_id_out[16], char *iso, size_t iso_size) {
    char fingerprint[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1];
    char disc_id[32];
    char dir[NK_MAX_PATH];
    NkDiscImportPaths legacy;
    NkDiscExpandStatus ignored;
    const char *keys[2];
    if (!nk_disc_legacy_fingerprint(source, out->format, fingerprint, &ignored) ||
        !import_paths(data_root, fingerprint, &legacy) ||
        !read_line_file(legacy.ref, disc_id, sizeof(disc_id)) || !disc_id_valid(disc_id)) {
        return false;
    }
    keys[0] = fingerprint;
    keys[1] = NULL;
    for (int i = 0; i < 2; i++) {
        if (title_iso_path(data_root, disc_id, keys[i], out->format, dir, sizeof(dir), iso,
                           iso_size) &&
            legacy_recorded(iso, fingerprint, out->expanded_bytes)) {
            snprintf(disc_id_out, 16, "%.9s", disc_id);
            return true;
        }
    }
    return false;
}

/* Record out->iso_path as the expansion of out->identity: its ref, then its
   .source. Written in this order, an interrupted upgrade of an earlier record
   leaves a ref whose image still carries the sampled record, which the next
   add compares again. */
static bool write_identity_records(const char *ref, const NkDiscImport *out) {
    char line[sizeof(NK_DISC_IDENTITY_TAG) + NK_DISC_IDENTITY_CHARS + 8];
    char sidecar[NK_MAX_PATH + 8];
    const char *name = strrchr(out->iso_path, nk_platform_path_separator());
    name = name ? name + 1 : out->iso_path;
    if (!title_file_name_valid(out->disc_id, name) ||
        !path_suffix(sidecar, sizeof(sidecar), out->iso_path, ".source")) {
        return false;
    }
    snprintf(line, sizeof(line), "%.9s %.63s", out->disc_id, name);
    if (!write_line_file(ref, line)) return false;
    snprintf(line, sizeof(line), "%s%s", NK_DISC_IDENTITY_TAG, out->identity);
    return write_line_file(sidecar, line);
}

/* The source's size and write time, taken before its identity is computed and
   checked again before anything is recorded for it. */
typedef struct {
    int64_t size;
    uint64_t modified;
} NkDiscStamp;

static bool source_stamp(const char *path, NkDiscStamp *out) {
    out->size = nk_platform_get_file_size(path);
    return out->size >= 0 && file_modified_time(path, &out->modified);
}

static bool source_unchanged(const char *path, const NkDiscStamp *before,
                             NkDiscExpandStatus *status) {
    NkDiscStamp now;
    if (source_stamp(path, &now) && now.size == before->size &&
        now.modified == before->modified) {
        return true;
    }
    return disc_fail(status, NK_DISC_READ_FAILED,
                     "The file changed while Nakagawa was adding it. Add it again once nothing "
                     "is writing to it. Nothing was kept.");
}

/* One progress bar over the import: the identity first (the source's size),
   then the comparison or the expansion (the expanded size). */
typedef struct {
    const NkDiscExpandCallbacks *outer;
    uint64_t offset;
    uint64_t total;
} NkDiscPhase;

static void phase_progress(uint64_t done, uint64_t total, void *user) {
    const NkDiscPhase *phase = (const NkDiscPhase *)user;
    uint64_t at;
    (void)total;
    if (!phase->outer || !phase->outer->on_progress) return;
    at = done > phase->total - phase->offset ? phase->total : phase->offset + done;
    phase->outer->on_progress(at, phase->total, phase->outer->user);
}

static bool phase_cancelled(void *user) {
    const NkDiscPhase *phase = (const NkDiscPhase *)user;
    return phase->outer && phase->outer->is_cancelled &&
           phase->outer->is_cancelled(phase->outer->user);
}

bool nk_disc_import_probe(const char *source, const char *data_root, NkDiscImport *out,
                          NkDiscExpandStatus *status) {
    int64_t size;
    disc_status_clear(status);
    if (!out) return disc_fail(status, NK_DISC_INTERNAL, "No result was given.");
    memset(out, 0, sizeof(*out));
    if (!source || !source[0] || !data_root || !data_root[0]) {
        return disc_fail(status, NK_DISC_INTERNAL, "No disc image or data folder was named.");
    }
    size = nk_platform_get_file_size(source);
    if (size < 0) {
        return disc_fail(status, NK_DISC_READ_FAILED, "The disc image could not be opened.");
    }
    out->source_bytes = (uint64_t)size;
    return nk_disc_expanded_size(source, &out->format, &out->expanded_bytes, status);
}

/* Reuse, upgrade or expand while the caller holds paths->lock. */
static bool disc_import_locked(const char *source, const char *data_root,
                               const NkDiscImportPaths *paths, const NkDiscStamp *stamp,
                               NkDiscPhase *phase, NkDiscImport *out,
                               NkDiscExpandStatus *status) {
    NkDiscExpandCallbacks hooks;
    NkIsoMetadata meta;
    char dir[NK_MAX_PATH];
    char sidecar[NK_MAX_PATH + 8];
    char legacy_iso[NK_MAX_PATH];
    char legacy_id[16];
    char found_id[16];
    uint64_t written = 0;
    hooks.on_progress = phase_progress;
    hooks.is_cancelled = phase_cancelled;
    hooks.user = phase;
    phase->offset = out->source_bytes;

    /* The expansion recorded for this identity. Looked up under the lock: an
       import that held it a moment ago may have placed it. */
    if (read_identity_ref(paths->ref, data_root, found_id, out->iso_path, sizeof(out->iso_path)) &&
        identity_recorded(out->iso_path, out)) {
        snprintf(out->disc_id, sizeof(out->disc_id), "%s", found_id);
        out->reused = true;
        return true;
    }
    out->iso_path[0] = '\0';

    /* An earlier import recorded under this file's sampled fingerprint is
       this file's only when its image is this file's expansion, byte for
       byte; then its record is upgraded and the image keeps its name. */
    if (find_legacy_import(source, data_root, out, legacy_id, legacy_iso, sizeof(legacy_iso))) {
        uint64_t compared = 0;
        bool differs = false;
        if (!disc_decode(source, legacy_iso, true, &hooks, &compared, &differs, status)) {
            return false;
        }
        if (!differs) {
            if (!source_unchanged(source, stamp, status)) return false;
            snprintf(out->iso_path, sizeof(out->iso_path), "%s", legacy_iso);
            snprintf(out->disc_id, sizeof(out->disc_id), "%s", legacy_id);
            if (!write_identity_records(paths->ref, out)) {
                out->iso_path[0] = '\0';
                return disc_fail(status, NK_DISC_WRITE_FAILED,
                                 "The record of the disc image could not be updated in "
                                 "Nakagawa's data folder.");
            }
            out->reused = true;
            return true;
        }
    }

    if (!nk_disc_expand_file(source, paths->staging, &hooks, &written, status)) return false;
    if (!source_unchanged(source, stamp, status)) {
        (void)nk_remove_utf8(paths->staging);
        return false;
    }

    /* The expansion is a disc image now: name its disc before it gets a
       per-title folder, and refuse anything that is not a PSP game. */
    memset(&meta, 0, sizeof(meta));
    if (nk_iso_inspect(paths->staging, &meta) != NK_OK) {
        (void)nk_remove_utf8(paths->staging);
        return disc_fail(status, NK_DISC_EXPANDED_INVALID,
                         "The compressed file expanded, but what it holds is not a readable "
                         "disc image: %.200s", meta.error_message);
    }
    if (!meta.has_psp_game_folder) {
        (void)nk_remove_utf8(paths->staging);
        return disc_fail(status, NK_DISC_NOT_PSP,
                         "This disc image isn't a PSP game: it has no PSP_GAME folder. "
                         "Choose the disc image of a PSP game disc.");
    }
    if (!disc_id_valid(meta.disc_id)) {
        (void)nk_remove_utf8(paths->staging);
        return disc_fail(status, NK_DISC_ID_MISSING,
                         "This PSP disc image does not name a valid disc ID, so Nakagawa can't "
                         "keep it apart from other games.");
    }
    snprintf(out->disc_id, sizeof(out->disc_id), "%.9s", meta.disc_id);
    if (!title_iso_path(data_root, out->disc_id, out->identity, out->format, dir, sizeof(dir),
                        out->iso_path, sizeof(out->iso_path)) ||
        !path_suffix(sidecar, sizeof(sidecar), out->iso_path, ".source") ||
        !nk_platform_mkdir_p_private(dir)) {
        (void)nk_remove_utf8(paths->staging);
        out->iso_path[0] = '\0';
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "The disc image's folder could not be made in Nakagawa's data folder.");
    }
    /* The old sidecar goes first, so an interrupted replace never leaves an
       earlier expansion's record beside this image. */
    (void)nk_remove_utf8(sidecar);
    if (nk_rename_utf8(paths->staging, out->iso_path) != 0 ||
        !write_identity_records(paths->ref, out)) {
        (void)nk_remove_utf8(paths->staging);
        out->iso_path[0] = '\0';
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "The expanded disc image could not be moved into its folder.");
    }
    out->expanded_bytes = written;
    return true;
}

bool nk_disc_import(const char *source, const char *data_root,
                    const NkDiscExpandCallbacks *callbacks, NkDiscImport *out,
                    NkDiscExpandStatus *status) {
    NkDiscImportPaths paths;
    NkDiscExpandCallbacks hooks;
    NkDiscPhase phase;
    NkDiscStamp stamp;
    NkFileLock lock;
    bool ok;
    if (!nk_disc_import_probe(source, data_root, out, status)) return false;
    if (!source_stamp(source, &stamp) || stamp.size != (int64_t)out->source_bytes) {
        return disc_fail(status, NK_DISC_READ_FAILED, NK_DISC_UNREADABLE_SOURCE);
    }
    /* Both sizes are bounded (a file size, and NK_DISC_MAX_EXPANDED_BYTES), so
       the sum fits. */
    phase.outer = callbacks;
    phase.offset = 0;
    phase.total = out->source_bytes + out->expanded_bytes;
    hooks.on_progress = phase_progress;
    hooks.is_cancelled = phase_cancelled;
    hooks.user = &phase;
    /* Every add reads the whole file: nothing short of its bytes says that it
       is a file imported before. */
    if (!nk_disc_file_identity(source, &hooks, out->identity, NULL, status) ||
        !source_unchanged(source, &stamp, status)) {
        return false;
    }
    if (!import_paths(data_root, out->identity, &paths)) {
        return disc_fail(status, NK_DISC_WRITE_FAILED, "The path to Nakagawa's data folder is too long.");
    }
    if (!nk_platform_mkdir_p_private(paths.expanded_dir)) {
        return disc_fail(status, NK_DISC_WRITE_FAILED,
                         "Nakagawa's disc folder could not be made in its data folder.");
    }
    /* The staging name comes from the identity alone, so two imports of one
       file would write the same partial image, the second one's open cutting
       the first one's short under it. One holds the lock; the other is
       refused by name. */
    if (!nk_platform_file_lock_acquire(&lock, paths.lock, 0u)) {
        return disc_fail(status, NK_DISC_IMPORT_BUSY,
                         "This disc image is already being added by another Nakagawa window or "
                         "tool. Wait for that to finish, then add it again.");
    }
    ok = disc_import_locked(source, data_root, &paths, &stamp, &phase, out, status);
    nk_platform_file_lock_release(&lock);
    if (ok && callbacks && callbacks->on_progress) {
        callbacks->on_progress(phase.total, phase.total, callbacks->user);
    }
    return ok;
}
