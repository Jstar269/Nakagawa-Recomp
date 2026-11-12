/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Store-only ZIP archives for save backups (#1883), and a reader for archives
 * other programs made (texture packs, #1880; NkZipReadOptions below).
 *
 * The writer makes ordinary ZIP files (PKWARE APPNOTE 6.3.x): every entry is
 * stored (method 0) with its CRC-32 and sizes in the local header, no data
 * descriptors, no ZIP64, no encryption. Names are relative, with '/' between
 * folders; a folder entry ends in '/'. Any ZIP tool opens them.
 *
 * The reader treats every archive as untrusted: it may come from another PC or
 * have been edited by hand. It accepts only what the writer makes and refuses
 * everything else with its own status code, before a byte is written:
 *   - a name that is absolute, has a drive letter, a backslash, a "." or ".."
 *     folder, an empty folder name, a NUL byte, a control character or one a
 *     Windows folder can't hold, a reserved device name (CON, NUL, COM1...),
 *     or is not UTF-8; and two names that are the same when case is ignored,
 *     or a file whose name another entry uses as a folder;
 *   - a method other than stored, an encrypted entry, a data descriptor, any
 *     other flag, ZIP64 (its 0xFFFF / 0xFFFFFFFF markers, its records or its
 *     extra field), and a multi-disk archive;
 *   - an end record it can't find exactly once, a central directory that is not
 *     where the end record says, a local header that disagrees with its central
 *     entry (name, flags, method, CRC, sizes), data that runs past the central
 *     directory or overlaps another entry, and a CRC-32 that does not match.
 * All size and offset arithmetic is checked, and the caps below bound what an
 * archive can ask for. Extraction writes only inside the folder it is given.
 *
 * Built with NK_ZIP_NO_HOST_IO (the fuzzer), only the memory reader and the
 * name check are compiled: nothing reads or writes host files.
 *
 * Opened with NkZipReadOptions, the reader also takes what common ZIP tools
 * write and the save format never holds: DEFLATE entries (method 8, unpacked
 * by the project's own inflater, src/core/nk_psp_inflate.h, then CRC-checked),
 * the DEFLATE level bits (flag bits 1 and 2) on them, data descriptors (flag
 * bit 3: the local header's CRC and sizes may be zero, and the central
 * directory's are used), and caps of the caller's choosing. Every other rule
 * above still holds; ZIP64 is still refused. Without options nothing changes. */

#ifndef NK_ZIP_H
#define NK_ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caps, generous for PSP save data (a save folder holds a handful of files of
   a few hundred KiB; a whole Memory Stick tree with game-data installs a few
   hundred MiB). The reader refuses an archive past them, and the writer
   refuses to make one. */
#define NK_ZIP_MAX_ENTRIES 4096u                               /* files and folders */
#define NK_ZIP_MAX_TOTAL_BYTES (512ull * 1024ull * 1024ull)    /* all entries' data */
#define NK_ZIP_MAX_NAME_BYTES 512u                             /* one entry's name */
#define NK_ZIP_MAX_DEPTH 32u                                   /* folders in a name */
/* The central directory is read whole; 16 MiB holds 4096 entries with long
   names and extra fields many times over. */
#define NK_ZIP_MAX_CENTRAL_BYTES (16u * 1024u * 1024u)

typedef enum {
    NK_ZIP_OK = 0,
    NK_ZIP_ERR_ARGUMENT,          /* a NULL or empty argument */
    NK_ZIP_ERR_IO,                /* the host could not read or write a file */
    NK_ZIP_ERR_NO_MEMORY,
    NK_ZIP_ERR_NOT_ZIP,           /* no end-of-central-directory record */
    NK_ZIP_ERR_END_RECORD,        /* more than one end record, or a wrong comment length */
    NK_ZIP_ERR_TRUNCATED,         /* the archive ends inside a record */
    NK_ZIP_ERR_MULTI_DISK,
    NK_ZIP_ERR_ZIP64,
    NK_ZIP_ERR_CENTRAL_DIRECTORY, /* not where the end record says, or malformed */
    NK_ZIP_ERR_TOO_MANY_ENTRIES,  /* more than NK_ZIP_MAX_ENTRIES */
    NK_ZIP_ERR_TOO_LARGE,         /* more than NK_ZIP_MAX_TOTAL_BYTES */
    NK_ZIP_ERR_OFFSET,            /* a header or data past the end of its area */
    NK_ZIP_ERR_OVERLAP,           /* two entries share bytes */
    NK_ZIP_ERR_METHOD,            /* compressed: only stored (method 0) is read */
    NK_ZIP_ERR_ENCRYPTED,
    NK_ZIP_ERR_DATA_DESCRIPTOR,   /* general-purpose flag bit 3 */
    NK_ZIP_ERR_FLAGS,             /* any other flag but UTF-8 names (bit 11) */
    NK_ZIP_ERR_SIZE_MISMATCH,     /* stored sizes differ, or a folder holds data */
    NK_ZIP_ERR_LOCAL_MISMATCH,    /* a local header disagrees with the central entry */
    NK_ZIP_ERR_CRC,
    NK_ZIP_ERR_NAME_EMPTY,
    NK_ZIP_ERR_NAME_TOO_LONG,
    NK_ZIP_ERR_NAME_NUL,
    NK_ZIP_ERR_NAME_ABSOLUTE,
    NK_ZIP_ERR_NAME_BACKSLASH,
    NK_ZIP_ERR_NAME_DRIVE,
    NK_ZIP_ERR_NAME_PARENT,       /* a ".." folder */
    NK_ZIP_ERR_NAME_DOT,          /* a "." folder */
    NK_ZIP_ERR_NAME_EMPTY_SEGMENT,
    NK_ZIP_ERR_NAME_CHARACTER,    /* a control character, one of <>:"|?*, or a trailing dot or space */
    NK_ZIP_ERR_NAME_RESERVED,     /* CON, PRN, AUX, NUL, COM1-9, LPT1-9 */
    NK_ZIP_ERR_NAME_ENCODING,     /* not UTF-8 */
    NK_ZIP_ERR_NAME_TOO_DEEP,     /* more than NK_ZIP_MAX_DEPTH folders */
    NK_ZIP_ERR_NAME_DUPLICATE,    /* the same name as another entry, case ignored */
    NK_ZIP_ERR_NAME_CONFLICT,     /* a file whose name another entry uses as a folder */
    NK_ZIP_ERR_EXISTS,            /* extraction: the file is in the folder already */
    NK_ZIP_ERR_LEFT_OUT,          /* writing a folder: an entry in it could not be archived */
    NK_ZIP_ERR_INFLATE,           /* a DEFLATE entry's data is damaged (NkZipReadOptions only) */
    NK_ZIP_STATUS_COUNT
} NkZipStatus;

/* What went wrong and, when an entry did, its name (printable bytes only,
   cut to fit), so a message can say which file was refused. */
typedef struct {
    NkZipStatus status;
    char entry[160];
} NkZipError;

/* "ZIP_NAME_PARENT": a stable code for logs and error cards. */
const char *nk_zip_status_code(NkZipStatus status);
/* One plain sentence: "A file name in it climbs out of its folder (\"..\")." */
const char *nk_zip_status_message(NkZipStatus status);

/* CRC-32 (ISO 3309 / ITU-T V.42, the ZIP checksum); start with crc 0. */
uint32_t nk_zip_crc32(uint32_t crc, const void *data, size_t size);

/* Sorts `count` items of `size` bytes with at most about 2 n log2 n comparisons
   whatever their order (a heapsort, not stable), for keys whose order an archive or
   a pack chooses. The ZIP reader sorts with it, and texture packs do. */
void nk_zip_sort(void *items, size_t count, size_t size, int (*compare)(const void *, const void *));

/* Built with NK_SORT_COUNT_COMPARES (the texture-pack selftest only), every comparison function the ZIP
   reader and texture packs sort with counts its calls here, whatever sort calls it, so a test can bound
   what opening a pack in a hostile order costs. Without it the count compiles to nothing. */
#ifdef NK_SORT_COUNT_COMPARES
extern unsigned long long nk_sort_compares;
#define NK_SORT_COMPARE_COUNT() ((void)nk_sort_compares++)
#else
#define NK_SORT_COMPARE_COUNT() ((void)0)
#endif

/* Checks one entry name by the reader's rules. `size` is its byte count (it
   need not be NUL-terminated). A trailing '/' marks a folder: *is_folder says
   so when given. */
NkZipStatus nk_zip_check_name(const char *name, size_t size, bool *is_folder);

/* ---- Reader ---- */

typedef struct NkZipReader NkZipReader;

typedef struct {
    const char *name;    /* as stored; a folder ends in '/'; owned by the reader */
    uint32_t size;       /* bytes; 0 for a folder */
    uint32_t crc32;
    bool is_folder;
    bool deflated;       /* stored with DEFLATE (method 8); else stored as is */
    uint64_t packed_size;/* bytes the entry takes in the archive */
} NkZipEntryInfo;

/* Opens an archive held in memory, which must outlive the reader. Every
   structural check above runs here; CRCs are checked by verify, read and
   extract. On failure *out is NULL and err (when given) says why. */
NkZipStatus nk_zip_reader_open_memory(NkZipReader **out, const void *data, size_t size,
                                      NkZipError *err);
/* Reading archives other programs made (texture packs, #1880). A zero field
   takes the default cap above. */
typedef struct {
    size_t max_entries;        /* files and folders; at most 65534 (more needs ZIP64) */
    uint64_t max_total_bytes;  /* all entries' unpacked data */
    bool deflate;              /* also read method 8 entries, with flag bits 1 and 2 */
    bool data_descriptor;      /* also read entries with flag bit 3 */
} NkZipReadOptions;
#define NK_ZIP_READ_MAX_ENTRIES 65534u
/* nk_zip_reader_open_memory with options (NULL: none). A DEFLATE entry is
   unpacked whole in memory by read and verify; its declared size bounds the
   output, and a stream that ends short of it, runs past it or fails its CRC is
   refused. */
NkZipStatus nk_zip_reader_open_memory_with(NkZipReader **out, const void *data, size_t size,
                                           const NkZipReadOptions *options, NkZipError *err);
size_t nk_zip_reader_count(const NkZipReader *reader);
uint64_t nk_zip_reader_total_bytes(const NkZipReader *reader);
bool nk_zip_reader_entry(const NkZipReader *reader, size_t index, NkZipEntryInfo *out);
/* Reads every entry's data and checks its CRC-32. */
NkZipStatus nk_zip_reader_verify(NkZipReader *reader, NkZipError *err);
/* Copies one entry's data into buf (cap bytes at least its size) and checks
   its CRC-32. A DEFLATE entry is unpacked straight into buf, so the read holds
   buf and the entry's packed bytes, nothing more. On failure buf holds
   whatever was written, not the entry. */
NkZipStatus nk_zip_reader_read(NkZipReader *reader, size_t index, void *buf, size_t cap,
                               NkZipError *err);
void nk_zip_reader_close(NkZipReader *reader);

#ifndef NK_ZIP_NO_HOST_IO
/* Opens an archive file (UTF-8 path). The file stays open until close. */
NkZipStatus nk_zip_reader_open_file(NkZipReader **out, const char *path, NkZipError *err);
/* The same with options (NULL: none), as nk_zip_reader_open_memory_with. */
NkZipStatus nk_zip_reader_open_file_with(NkZipReader **out, const char *path,
                                         const NkZipReadOptions *options, NkZipError *err);
/* Checks every CRC first, then writes each entry inside `folder`, which must
   exist. A file already there is refused (NK_ZIP_ERR_EXISTS), never
   overwritten. On failure part of the archive may have been written: the
   caller extracts into a fresh folder and deletes it on failure. */
NkZipStatus nk_zip_reader_extract(NkZipReader *reader, const char *folder, NkZipError *err);

/* ---- Writer ---- */

typedef struct NkZipWriter NkZipWriter;

/* Creates (or replaces) the archive at `path`. Every entry gets the one DOS
   date and time given, in MS-DOS form (date 0x0021 is 1980-01-01). Fixed
   values make the bytes depend on the names and contents alone. */
NkZipStatus nk_zip_writer_open(NkZipWriter **out, const char *path, uint16_t dos_date,
                               uint16_t dos_time, NkZipError *err);
/* A folder entry; the trailing '/' is added when missing. */
NkZipStatus nk_zip_writer_add_folder(NkZipWriter *writer, const char *name, NkZipError *err);
NkZipStatus nk_zip_writer_add_bytes(NkZipWriter *writer, const char *name, const void *data,
                                    size_t size, NkZipError *err);
/* Streams the host file at host_path into the archive under `name`. */
NkZipStatus nk_zip_writer_add_file(NkZipWriter *writer, const char *name, const char *host_path,
                                   NkZipError *err);
/* Writes the central directory and closes the file; frees the writer either
   way. On failure the partial file is deleted. Names that would be refused as
   duplicates or conflicts on read are refused here too. */
NkZipStatus nk_zip_writer_finish(NkZipWriter *writer, NkZipError *err);
/* Closes, deletes the partial file and frees. */
void nk_zip_writer_discard(NkZipWriter *writer);

/* Archives everything under `folder` (files and folders, in name order) with
   names relative to it, into a new archive at zip_path, and puts the archive
   on the disk (nk_platform_sync_file) before it reports success. An entry it
   cannot archive fails the whole archive, never leaves a gap: a link, a
   junction or other reparse point (a cloud placeholder among them), a file a
   cloud service keeps off this computer, an entry that cannot be read, and a
   folder that cannot be listed whole (NK_ZIP_ERR_LEFT_OUT or NK_ZIP_ERR_IO,
   with the folder it was in). On failure no file is left at zip_path. When
   given, *out_entries and *out_bytes say what went in. */
NkZipStatus nk_zip_write_folder(const char *folder, const char *zip_path, uint16_t dos_date,
                                uint16_t dos_time, size_t *out_entries, uint64_t *out_bytes,
                                NkZipError *err);

/* What a folder holds, by nk_zip_write_folder's rules: EMPTY only when it
   lists whole with nothing in it, not even an entry the archive would leave
   out; UNREADABLE when it cannot be listed whole. Only EMPTY is safe to
   delete without a backup. */
typedef enum {
    NK_ZIP_FOLDER_EMPTY = 0,
    NK_ZIP_FOLDER_HAS_ENTRIES,
    NK_ZIP_FOLDER_UNREADABLE
} NkZipFolderState;
NkZipFolderState nk_zip_folder_state(const char *folder);

#if defined(NK_SAVES_TEST_SEAMS)
/* White-box seams for the saves tests (#1883), declared and defined only when
   the test build defines NK_SAVES_TEST_SEAMS; production builds do not carry
   them. Each seam counts its calls; nk_zip_test_fail makes the nth call from
   now fail (NK_ZIP_TEST_EVERY: every call, 0: none) and resets the count. */
typedef enum {
    NK_ZIP_SEAM_LIST_FAIL = 0,  /* a folder listing fails */
    NK_ZIP_SEAM_LIST_SKIP,      /* a folder listing leaves one entry out */
    NK_ZIP_SEAM_SYNC_FAIL,      /* putting a written file on the disk fails */
    NK_ZIP_SEAM_EXTRACT_FAIL,   /* writing an extracted file fails */
    NK_ZIP_SEAM_COUNT
} NkZipSeam;
#define NK_ZIP_TEST_EVERY 0xFFFFFFFFu
void nk_zip_test_fail(NkZipSeam seam, unsigned nth);
unsigned nk_zip_test_calls(NkZipSeam seam);
void nk_zip_test_reset(void);
#endif
#endif /* NK_ZIP_NO_HOST_IO */

#ifdef __cplusplus
}
#endif

#endif /* NK_ZIP_H */
