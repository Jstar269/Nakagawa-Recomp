/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Compressed disc images (CSO, ZSO, CHD), expanded once at import.
 *
 * A compressed disc image is turned into a plain .iso in the private per-user
 * data folder the first time it is added, and everything downstream (disc
 * inspection, decryption, code generation, the runtime's PSP_ISO reads) reads
 * that .iso exactly as it reads a user's own .iso. Nothing is decompressed
 * into guest or runtime memory: this is an import step only.
 *
 *   <data>/discs/<DISC_ID>/<DISC_ID>.<id>.<cso|zso|chd>.iso   the expanded image
 *   <data>/discs/<DISC_ID>/<DISC_ID>.<id>.<ext>.iso.source    "sha256:<identity>"
 *   <data>/discs/expanded/<identity>.ref                      "<DISC_ID> <file name>"
 *   <data>/discs/expanded/<identity>.lock                     held while it is imported
 *   <data>/discs/expanded/<DISC_ID>.<id>.<ext>.lock           held while that name is placed
 *
 * The identity of a compressed image is the SHA-256 of every byte of the file
 * the user added, as it is stored (what sha256sum prints for it), computed on
 * every add (nk_disc_file_identity). Adding a file with the same bytes again,
 * under any name or in any folder, reuses its expansion; a file that differs
 * anywhere is its own import. <id> is the first NK_DISC_TITLE_FINGERPRINT_CHARS
 * characters of the identity, so two images of one disc ID (a retail dump and
 * a fan translation) keep two expansions. When that name already holds an
 * image recorded for another identity (the first 64 bits of two hashes alike,
 * which takes a crafted file), it is never overwritten: <id> is then the whole
 * identity. The same disc in another container (a CSO and a ZSO of one .iso)
 * has other bytes, so it is expanded again.
 *
 * The bytes recorded are the bytes expanded. The source is held open for the
 * whole import with writes denied where the platform has share modes
 * (nk_platform_open_read_deny_writes: Windows, on a local volume or a server
 * that enforces them; a file another program already has open for writing is
 * read shared instead). Its stamp (NkDiscSourceStamp: size, write time, change
 * time and file identity) is taken before anything reads it and checked again
 * before anything is recorded, and before a record is written for a new
 * expansion or an upgrade the identity is computed a second time and must be
 * the same.
 *
 * Imports made before the full identity (F-f6233fe0) were recorded under a
 * sampled fingerprint of the source's size, write time and first 64 KiB
 * (nk_disc_legacy_fingerprint): a ref <fingerprint>.ref holding the disc ID,
 * the image as <DISC_ID>.<fp>.<ext>.iso or <DISC_ID>.<ext>.iso, and the
 * fingerprint as its .source. Such a record is never a match by itself: a file
 * whose sampled fingerprint names it is expanded in memory and compared byte
 * for byte with that image. Equal, the record is upgraded where it is (the
 * image keeps its name); different, the file gets its own expansion.
 *
 * CSO (CISO v1 and v2) and ZSO (ZISO) are read here: the block index, raw
 * DEFLATE blocks through nk_psp_inflate_raw and LZ4 blocks through a small
 * block decoder in nk_disc_expand.c. CHD is read through a backend the program
 * registers (nk_disc_chd.c, over the vendored libchdr); a program built without
 * it refuses CHD by name. Every header field is checked before it is used and
 * every refusal carries a named boundary code (NK_DISC_* below).
 */

#ifndef NK_DISC_EXPAND_H
#define NK_DISC_EXPAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "nk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NK_DISC_SECTOR_BYTES 2048u
/* No UMD holds more than 1.8 GB; anything past this bound is a damaged or
   hostile header, refused before any allocation or loop uses it. */
#define NK_DISC_MAX_EXPANDED_BYTES (4ull * 1024ull * 1024ull * 1024ull)
/* A file's identity: the lowercase hex SHA-256 of all of its bytes. */
#define NK_DISC_IDENTITY_CHARS 64u
/* The sampled fingerprint imports were recorded under before the identity. */
#define NK_DISC_LEGACY_FINGERPRINT_CHARS 32u
/* The part of the identity in an expansion's per-title file name. */
#define NK_DISC_TITLE_FINGERPRINT_CHARS 16u
/* The largest CHD hunk read; a header naming more is refused before libchdr
   allocates anything for it. */
#define NK_DISC_CHD_MAX_HUNK_BYTES (16u * 1024u * 1024u)
#define NK_DISC_SHA1_BYTES 20u

/* Named boundary codes. */
#define NK_DISC_TRUNCATED              "DISC_IMAGE_TRUNCATED"
#define NK_DISC_HEADER_INVALID         "DISC_IMAGE_HEADER_INVALID"
#define NK_DISC_VERSION_UNSUPPORTED    "DISC_IMAGE_VERSION_UNSUPPORTED"
#define NK_DISC_SIZE_INVALID           "DISC_IMAGE_SIZE_INVALID"
#define NK_DISC_INDEX_INVALID          "DISC_IMAGE_INDEX_INVALID"
#define NK_DISC_BLOCK_CORRUPT          "DISC_IMAGE_BLOCK_CORRUPT"
#define NK_DISC_READ_FAILED            "DISC_IMAGE_READ_FAILED"
#define NK_DISC_WRITE_FAILED           "DISC_IMAGE_WRITE_FAILED"
#define NK_DISC_NOT_COMPRESSED         "DISC_IMAGE_NOT_COMPRESSED"
#define NK_DISC_ID_MISSING             "DISC_IMAGE_ID_MISSING"
#define NK_DISC_NOT_PSP                "DISC_NOT_PSP"
#define NK_DISC_CHD_UNAVAILABLE        "DISC_IMAGE_CHD_UNAVAILABLE"
#define NK_DISC_CHD_INVALID            "DISC_IMAGE_CHD_INVALID"
#define NK_DISC_CHD_CD_UNSUPPORTED     "DISC_IMAGE_CHD_CD_UNSUPPORTED"
#define NK_DISC_CHD_UNSUPPORTED        "DISC_IMAGE_CHD_UNSUPPORTED"
#define NK_DISC_IMPORT_BUSY            "DISC_IMAGE_IMPORT_BUSY"
/* The library an import is keyed by cannot be read (the player's code for it). */
#define NK_DISC_LIBRARY_UNREADABLE     "IO_FAILURE"
#define NK_DISC_CANCELLED              "CANCELLED"
#define NK_DISC_INTERNAL               "INTERNAL_ERROR"

typedef enum {
    NK_DISC_FORMAT_NONE = 0, /* not a compressed disc image (a plain .iso or anything else) */
    NK_DISC_FORMAT_CSO,
    NK_DISC_FORMAT_ZSO,
    NK_DISC_FORMAT_CHD
} NkDiscFormat;

typedef struct {
    void (*on_progress)(uint64_t done_bytes, uint64_t total_bytes, void *user);
    bool (*is_cancelled)(void *user);
    void *user;
} NkDiscExpandCallbacks;

typedef struct {
    char code[48];     /* a NK_DISC_* name; empty on success */
    char message[384]; /* one plain sentence for the user */
} NkDiscExpandStatus;

/* The CHD reader the program links. open checks that the image is a DVD-layout
   CHD of 2048-byte sectors and reports its size and the SHA-1 its header
   records for the logical_bytes of data (all zero when it records none);
   read_hunk reads hunk `index` (hunk_bytes bytes) into dst. Every failure
   fills status. */
typedef struct {
    bool (*open)(const char *path, void **handle, uint64_t *logical_bytes,
                 uint32_t *hunk_bytes, uint8_t raw_sha1[NK_DISC_SHA1_BYTES],
                 NkDiscExpandStatus *status);
    bool (*read_hunk)(void *handle, uint32_t index, uint8_t *dst, NkDiscExpandStatus *status);
    void (*close)(void *handle);
} NkDiscChdBackend;

/* Install the CHD backend (NULL removes it). Call once at start-up, before
   any expansion runs. */
void nk_disc_set_chd_backend(const NkDiscChdBackend *backend);

/* The format named by a file's leading bytes ("CISO", "ZISO", "MComprHD"). */
NkDiscFormat nk_disc_format_from_magic(const uint8_t *bytes, size_t length);
/* The same, read from a file. NONE when the file cannot be read. */
NkDiscFormat nk_disc_image_format(const char *path);
/* "cso", "zso", "chd", or "" for NONE. */
const char *nk_disc_format_extension(NkDiscFormat format);

/* Check the header of a compressed image and report the size of the plain
   image it expands to, without expanding it. */
bool nk_disc_expanded_size(const char *source, NkDiscFormat *out_format,
                           uint64_t *out_expanded_bytes, NkDiscExpandStatus *status);

/* Expand source into dest_iso: written to dest_iso + ".part", its size checked
   (and, for a CHD whose header records one, the SHA-1 of its data), then
   renamed into place. A failure or a cancel removes the partial file and
   leaves any existing dest_iso untouched. */
bool nk_disc_expand_file(const char *source, const char *dest_iso,
                         const NkDiscExpandCallbacks *callbacks,
                         uint64_t *out_expanded_bytes, NkDiscExpandStatus *status);

typedef struct {
    NkDiscFormat format;
    uint64_t source_bytes;
    uint64_t expanded_bytes;
    /* The source's identity (nk_disc_file_identity); empty after a probe. */
    char identity[NK_DISC_IDENTITY_CHARS + 1];
    char disc_id[16];
    char iso_path[NK_MAX_PATH];
    /* A matching expansion was already in place and nothing was written. */
    bool reused;
} NkDiscImport;

/* The identity of the file at path: the SHA-256 of all of its bytes, read
   once in 1 MiB pieces. Progress reports (done, file size) after each piece,
   and is_cancelled is asked after each report. A file that cannot be read to
   the end, or that grows or shrinks while it is read, is NK_DISC_READ_FAILED;
   a cancel is NK_DISC_CANCELLED. out_bytes (optional) is the size hashed. */
bool nk_disc_file_identity(const char *path, const NkDiscExpandCallbacks *callbacks,
                           char out[NK_DISC_IDENTITY_CHARS + 1], uint64_t *out_bytes,
                           NkDiscExpandStatus *status);

/* A source file's stamp, taken before it is read and compared before anything
   is recorded for it: its size and write time, which any writer can set back,
   the time of its last change of any kind (POSIX st_ctim, which no ordinary
   program can set; the NTFS change time, which a program that may write the
   file can also set), and which file it is (POSIX st_dev and st_ino; the volume
   serial number and file index). A file system that does not keep one of the
   last three gives 0 for it in both stamps, and the field then never differs;
   0 in one stamp only is a change. What this catches on each platform:
   nk_platform_file_change_identity in nk_platform.h. */
typedef struct {
    int64_t size;
    uint64_t modified;
    uint64_t changed;
    uint64_t volume;
    uint64_t file_id;
} NkDiscSourceStamp;

/* False when the file's size or write time cannot be read. */
bool nk_disc_source_stamp(const char *path, NkDiscSourceStamp *out);

/* True when the file still has the stamp `before`, every field equal (a field
   0 in both is one the file system does not keep); otherwise
   NK_DISC_READ_FAILED, saying that the file changed while it was added. */
bool nk_disc_source_unchanged(const char *path, const NkDiscSourceStamp *before,
                              NkDiscExpandStatus *status);

/* Compute the file's identity again (progress and cancel as for
   nk_disc_file_identity) and require it to be `identity`: a file rewritten
   since the first read, even with the same size and write time, is
   NK_DISC_READ_FAILED, saying that it changed while it was added. */
bool nk_disc_identity_unchanged(const char *path, const NkDiscExpandCallbacks *callbacks,
                                const char *identity, NkDiscExpandStatus *status);

/* The sampled fingerprint earlier imports were recorded under: a hash of the
   format, the file's size, its write time and its first 64 KiB. Only used to
   find such a record, never to decide that two files are one. */
bool nk_disc_legacy_fingerprint(const char *source, NkDiscFormat format,
                                char out[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1],
                                NkDiscExpandStatus *status);

/* Identify a compressed image and check its header: out->format,
   out->source_bytes and out->expanded_bytes. Reads no more than the header,
   so it can run where the user is waiting. Whether the file was imported
   before needs its identity, which only nk_disc_import computes: a probe never
   reports reused. */
bool nk_disc_import_probe(const char *source, const char *data_root, NkDiscImport *out,
                          NkDiscExpandStatus *status);

/* Probe, compute the source's identity, and reuse the expansion recorded for
   it; otherwise upgrade an earlier sampled record whose image is this file's
   expansion byte for byte, or expand: the image is expanded into
   <data_root>/discs/expanded/, checked to be a PSP disc with a disc ID, and
   moved into its per-title folder. Progress runs once over the identity (the
   source's size), the expansion or comparison (the expanded size) and the
   second identity (the source's size again; a reuse needs none and ends the
   bar at once). The import holds <identity>.lock from the lookup to the move,
   and the lock of the per-title file name while it places the image, so a
   second import of the same file, or of another file that would take the same
   name, that starts meanwhile (another program, or another worker) is refused
   as NK_DISC_IMPORT_BUSY instead of writing over the first one's image. A
   source that cannot be opened with writes denied, or whose size, write time
   or bytes change during the import, is refused as NK_DISC_READ_FAILED, and
   nothing of it is kept. */
bool nk_disc_import(const char *source, const char *data_root,
                    const NkDiscExpandCallbacks *callbacks, NkDiscImport *out,
                    NkDiscExpandStatus *status);

/* nk_disc_import for a library holding entries[0..entry_count): a new
   expansion goes into the folder of the key the disc is added under
   (nk_iso_library_key_for_image), so a prototype disc whose library entry
   holds its v1 key is expanded into discs/<v1 key>, the folder that entry's
   checks and removal use, and any other disc into discs/<its own key>. An
   expansion recorded earlier under the other key moves to that folder (an
   emptied old folder is removed; one that holds anything else stays). A move
   that fails is named and leaves that expansion where its record names it.
   nk_disc_import is this with no library
   (entries NULL): a new expansion goes under the disc's own key and a recorded
   one stays where it is. */
bool nk_disc_import_for_library(const char *source, const char *data_root,
                                const NkGameEntry *entries, int entry_count,
                                const NkDiscExpandCallbacks *callbacks, NkDiscImport *out,
                                NkDiscExpandStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* NK_DISC_EXPAND_H */
