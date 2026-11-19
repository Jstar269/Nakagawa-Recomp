/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * The CHD backend for nk_disc_expand.c, over the vendored libchdr
 * (third_party/libchdr, BSD-3-Clause; see its PROVENANCE.md).
 *
 * A PSP disc image in CHD form is a DVD-layout CHD (chdman createdvd): a run
 * of 2048-byte sectors whose hunks expand straight into the plain .iso. A
 * CD-layout CHD (chdman createcd) holds CD frames with their own sector
 * format and track list; it is refused by name instead of being guessed at,
 * and so are hard-disk, laserdisc and parent-linked CHDs.
 *
 * Linked into the player and the nk_disc_expand command-line wrapper only;
 * the runtime never reads a CHD.
 */

#include "nk_disc_chd.h"

#include "nk_platform.h"

#include <libchdr/chd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libchdr reads through these callbacks, so the file is opened with the
   project's UTF-8 open and 64-bit seek on every host. */
static uint64_t chd_file_size(void *user) {
    FILE *f = (FILE *)user;
    int64_t end;
    if (nk_fseek64(f, 0, SEEK_END) != 0) return (uint64_t)-1;
    end = nk_ftell64(f);
    return end < 0 ? (uint64_t)-1 : (uint64_t)end;
}

static size_t chd_file_read(void *dst, size_t size, size_t count, void *user) {
    return fread(dst, size, count, (FILE *)user);
}

static int chd_file_close(void *user) {
    return fclose((FILE *)user);
}

static int chd_file_seek(void *user, int64_t offset, int whence) {
    return nk_fseek64((FILE *)user, offset, whence);
}

static const core_file_callbacks k_chd_file_callbacks = {
    chd_file_size, chd_file_read, chd_file_close, chd_file_seek
};

static bool chd_status(NkDiscExpandStatus *status, const char *code, const char *message,
                       const char *detail) {
    if (status) {
        snprintf(status->code, sizeof(status->code), "%s", code);
        if (detail && detail[0]) {
            snprintf(status->message, sizeof(status->message), "%s (%s)", message, detail);
        } else {
            snprintf(status->message, sizeof(status->message), "%s", message);
        }
    }
    return false;
}

/* CHDERR_NONE when the image holds metadata `tag`, CHDERR_METADATA_NOT_FOUND
   when it does not, and any other error when the metadata chain itself could
   not be read (a cycle, an entry past the end of the file). */
static chd_error chd_find_metadata(chd_file *chd, uint32_t tag) {
    uint8_t scratch[256];
    uint32_t length = 0;
    uint32_t found_tag = 0;
    uint8_t flags = 0;
    return chd_get_metadata(chd, tag, 0, scratch, sizeof(scratch), &length, &found_tag, &flags);
}

static bool chd_metadata_damaged(NkDiscExpandStatus *status, chd_error err) {
    return chd_status(status, NK_DISC_CHD_INVALID,
                      "This CHD disc image is damaged or incomplete: its metadata could not "
                      "be read.", chd_error_string(err));
}

/* An error from libchdr's open or header read, named by what it means for the
   user: a parent-linked image, a version or compression this build can't read,
   a host short of memory, or a damaged file. */
static bool chd_open_failed(NkDiscExpandStatus *status, chd_error err) {
    switch (err) {
    case CHDERR_REQUIRES_PARENT:
        return chd_status(status, NK_DISC_CHD_UNSUPPORTED,
                          "This CHD disc image only stores its differences from a parent "
                          "image. Nakagawa reads complete CHD images only.", NULL);
    case CHDERR_UNSUPPORTED_VERSION:
        return chd_status(status, NK_DISC_CHD_UNSUPPORTED,
                          "This CHD disc image is a format version Nakagawa can't read.",
                          chd_error_string(err));
    case CHDERR_UNSUPPORTED_FORMAT:
        return chd_status(status, NK_DISC_CHD_UNSUPPORTED,
                          "This CHD disc image uses a compression Nakagawa can't read.",
                          chd_error_string(err));
    case CHDERR_OUT_OF_MEMORY:
        return chd_status(status, NK_DISC_INTERNAL,
                          "There is not enough memory to read this CHD disc image.",
                          chd_error_string(err));
    default:
        return chd_status(status, NK_DISC_CHD_INVALID,
                          "This CHD disc image is damaged or incomplete.", chd_error_string(err));
    }
}

static bool chd_backend_open(const char *path, void **handle, uint64_t *logical_bytes,
                             uint32_t *hunk_bytes, uint8_t raw_sha1[NK_DISC_SHA1_BYTES],
                             NkDiscExpandStatus *status) {
    static const uint32_t cd_tags[] = {
        CDROM_OLD_METADATA_TAG, CDROM_TRACK_METADATA_TAG, CDROM_TRACK_METADATA2_TAG,
        GDROM_OLD_METADATA_TAG, GDROM_TRACK_METADATA_TAG
    };
    chd_file *chd = NULL;
    const chd_header *header;
    chd_header peek;
    chd_error err;
    FILE *f;
    *handle = NULL;
    f = nk_fopen_utf8(path, "rb");
    if (!f) {
        return chd_status(status, NK_DISC_READ_FAILED,
                          "The CHD disc image could not be opened for reading.", NULL);
    }
    /* The header alone first: libchdr's open allocates a hunk buffer, codec
       dictionaries and the map from these fields (and does not check a v5
       header), so sizes no PSP disc has are refused before any of that. The
       header read leaves the file open and with the caller. */
    memset(&peek, 0, sizeof(peek));
    err = chd_read_header_core_file_callbacks(&k_chd_file_callbacks, f, &peek);
    if (err != CHDERR_NONE) {
        fclose(f);
        return chd_open_failed(status, err);
    }
    if (peek.logicalbytes > NK_DISC_MAX_EXPANDED_BYTES) {
        fclose(f);
        return chd_status(status, NK_DISC_SIZE_INVALID,
                          "This CHD disc image names an expanded size that no PSP disc has.",
                          NULL);
    }
    if (peek.hunkbytes > NK_DISC_CHD_MAX_HUNK_BYTES) {
        char detail[64];
        fclose(f);
        snprintf(detail, sizeof(detail), "%u bytes", (unsigned)peek.hunkbytes);
        return chd_status(status, NK_DISC_CHD_UNSUPPORTED,
                          "This CHD disc image uses a block size larger than any PSP disc "
                          "image needs.", detail);
    }
    /* libchdr owns the file from here, on success and on failure alike. */
    err = chd_open_core_file_callbacks(&k_chd_file_callbacks, f, CHD_OPEN_READ, NULL, &chd);
    if (err != CHDERR_NONE) return chd_open_failed(status, err);
    header = chd_get_header(chd);
    if (!header || (header->flags & CHDFLAGS_HAS_PARENT) != 0u) {
        chd_close(chd);
        return chd_status(status, NK_DISC_CHD_UNSUPPORTED,
                          "This CHD disc image only stores its differences from a parent "
                          "image. Nakagawa reads complete CHD images only.", NULL);
    }
    err = chd_find_metadata(chd, DVD_METADATA_TAG);
    if (err != CHDERR_NONE && err != CHDERR_METADATA_NOT_FOUND) {
        chd_close(chd);
        return chd_metadata_damaged(status, err);
    }
    if (err == CHDERR_METADATA_NOT_FOUND) {
        bool cd = false;
        for (size_t i = 0; i < sizeof(cd_tags) / sizeof(cd_tags[0]); i++) {
            err = chd_find_metadata(chd, cd_tags[i]);
            if (err == CHDERR_NONE) {
                cd = true;
            } else if (err != CHDERR_METADATA_NOT_FOUND) {
                chd_close(chd);
                return chd_metadata_damaged(status, err);
            }
        }
        chd_close(chd);
        if (cd) {
            return chd_status(status, NK_DISC_CHD_CD_UNSUPPORTED,
                              "This CHD was made as a CD image (chdman createcd). A PSP disc "
                              "is stored as a DVD image: make the CHD again from the .iso "
                              "with chdman createdvd.", NULL);
        }
        return chd_status(status, NK_DISC_CHD_UNSUPPORTED,
                          "This CHD is not a DVD image (it is a hard disk, laserdisc or raw "
                          "image), so it can't hold a PSP disc.", NULL);
    }
    if (header->unitbytes != NK_DISC_SECTOR_BYTES) {
        chd_close(chd);
        return chd_status(status, NK_DISC_CHD_UNSUPPORTED,
                          "This CHD DVD image does not use 2048-byte sectors.", NULL);
    }
    if (header->hunkbytes == 0u || header->hunkcount == 0u ||
        (uint64_t)header->hunkcount * header->hunkbytes < header->logicalbytes) {
        chd_close(chd);
        return chd_status(status, NK_DISC_CHD_INVALID,
                          "This CHD disc image's header does not cover its own size.", NULL);
    }
    *logical_bytes = header->logicalbytes;
    *hunk_bytes = header->hunkbytes;
    memcpy(raw_sha1, header->rawsha1, NK_DISC_SHA1_BYTES);
    *handle = chd;
    return true;
}

static bool chd_backend_read_hunk(void *handle, uint32_t index, uint8_t *dst,
                                  NkDiscExpandStatus *status) {
    chd_error err = chd_read((chd_file *)handle, index, dst);
    if (err != CHDERR_NONE) {
        char detail[96];
        snprintf(detail, sizeof(detail), "block %u: %s", (unsigned)index, chd_error_string(err));
        return chd_status(status, NK_DISC_BLOCK_CORRUPT,
                          "A block of this CHD disc image is damaged.", detail);
    }
    return true;
}

static void chd_backend_close(void *handle) {
    if (handle) chd_close((chd_file *)handle);
}

static const NkDiscChdBackend k_libchdr_backend = {
    chd_backend_open, chd_backend_read_hunk, chd_backend_close
};

const NkDiscChdBackend *nk_disc_chd_libchdr_backend(void) {
    return &k_libchdr_backend;
}

void nk_disc_chd_register(void) {
    nk_disc_set_chd_backend(&k_libchdr_backend);
}
