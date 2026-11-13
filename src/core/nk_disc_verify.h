/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Disc verification: a whole-image hash compared with Redump's PSP list.
 *
 * A disc image is hashed whole, with SHA-1 and CRC-32 (the hashes Redump
 * lists), on the user's machine. A compressed image (CSO, ZSO, CHD) is hashed
 * after its one-time expansion (nk_disc_expand.h), so it hashes the same as the
 * .iso it holds. The hashes are compared with one Redump datafile: the list the
 * user imported (nk_disc_dat_import) when there is one, which takes precedence,
 * else the list bundled with Nakagawa (assets/redump/redump-psp.zip in the
 * player's folder, Redump's own download kept byte for byte). Nothing is
 * uploaded or downloaded.
 *
 * Tiers (the same rules as tools/nk_core/disc_verify.py):
 *   VERIFIED_RETAIL  the SHA-1 matches a disc in the list, and the CRC-32 and
 *                    size agree;
 *   MODIFIED         no match, but the disc ID is a known retail disc ID: one
 *                    the list names in a <serial>, or one a verified image in
 *                    the same library carries. The image's hashes differ from
 *                    every listed dump of its disc: a damaged dump does that
 *                    as well as a changed copy (a fan translation, a patch, a
 *                    trimmed image), and the check can't tell which;
 *   UNKNOWN          neither: not in the list, which means "not shown to be
 *                    retail", never "bad";
 *   NOT_CHECKED      no list found, or the image was never hashed.
 *
 * The datafile format (Logiqx XML as Redump publishes it, or the zip Redump
 * serves) is written down once, byte for byte, in the module documentation of
 * tools/nk_core/disc_verify.py ("The datafile format", rules 1-10), which the
 * command-line tools read with. This reader accepts exactly that format and
 * refuses everything else with the same code and message, so the player and
 * the tools never give two verdicts on one disc;
 * tests/native/disc_dat_conformance.txt holds both readers to it. In short:
 * a strict, ASCII-markup subset of XML in UTF-8, read in one forward pass (its
 * cost linear in its size); no entity expanded and no DTD read; one zip entry,
 * a .dat, exactly as Redump's zip lays it out, inflated under zlib's rules into
 * at most its declared size and checked against its CRC-32.
 */

#ifndef NK_DISC_VERIFY_H
#define NK_DISC_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nk_disc_expand.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The imported list, under the user's data folder ("verification/redump.dat",
   the path tools/nk_core/disc_verify.py uses too). */
#define NK_DISC_DAT_DIR "verification"
#define NK_DISC_DAT_FILE "redump.dat"
/* The bundled list, under the player's folder (and the checkout):
   "assets/redump/redump-psp.zip", BUNDLED_DAT_RELATIVE_PATH in disc_verify.py. */
#define NK_DISC_BUNDLED_DAT_DIR1 "assets"
#define NK_DISC_BUNDLED_DAT_DIR2 "redump"
#define NK_DISC_BUNDLED_DAT_FILE "redump-psp.zip"
/* The largest datafile read, a file or the .dat a zip holds (checked against
   the zip's own size field before inflating, and on the bytes inflated).
   Redump's PSP list of 2026-11-12 is 1,475,779 bytes for 3,546 discs, about
   416 bytes a disc, so 16 MiB holds about 40,000 discs: ten times the list,
   more than the PSP library will ever add. The reader holds at most the file
   (16 MiB), the inflated .dat (16 MiB), NK_DISC_DAT_MAX_ROMS roms and
   NK_DISC_DAT_MAX_SERIALS disc IDs. Each limit is the MAX_* of the same name in
   disc_verify.py (the format's rules 1, 5-9). */
#define NK_DISC_DAT_MAX_BYTES (16u * 1024u * 1024u)
/* Roms read (ten times the list's 3,546), tags (every '<'; the list has about
   eleven a disc), disc IDs over all games (the list names 3,907). */
#define NK_DISC_DAT_MAX_ROMS 40000u
#define NK_DISC_DAT_MAX_TAGS 480000u
#define NK_DISC_DAT_MAX_SERIALS 80000u
/* Elements nested (the list nests three deep). */
#define NK_DISC_DAT_MAX_DEPTH 8u
/* A <serial>'s raw bytes, and the disc IDs one names (the list's longest is 74
   bytes; the most IDs one names is five). */
#define NK_DISC_DAT_MAX_SERIAL_BYTES 256u
#define NK_DISC_DAT_MAX_GAME_SERIALS 16u
/* A header <name>, <version> or <date>, in printable ASCII. */
#define NK_DISC_DAT_MAX_HEADER_FIELD 63u

#define NK_DISC_SHA1_HEX_CHARS 40u
#define NK_DISC_CRC32_HEX_CHARS 8u

/* Named boundary codes. */
#define NK_DISC_DAT_READ_FAILED    "DISC_DAT_READ_FAILED"
#define NK_DISC_DAT_WRITE_FAILED   "DISC_DAT_WRITE_FAILED"
#define NK_DISC_DAT_NOT_DATAFILE   "DISC_DAT_NOT_DATAFILE"
#define NK_DISC_DAT_EMPTY          "DISC_DAT_EMPTY"
#define NK_DISC_DAT_TOO_LARGE      "DISC_DAT_TOO_LARGE"
#define NK_DISC_DAT_ENTITY_REFUSED "DISC_DAT_ENTITY_REFUSED"
#define NK_DISC_DAT_ZIP_INVALID    "DISC_DAT_ZIP_INVALID"

typedef enum {
    NK_DISC_TIER_NOT_CHECKED = 0,
    NK_DISC_TIER_VERIFIED_RETAIL,
    NK_DISC_TIER_MODIFIED,
    NK_DISC_TIER_UNKNOWN
} NkDiscTier;

/* "NOT_CHECKED", "VERIFIED_RETAIL", "MODIFIED", "UNKNOWN": the names
   library.json stores. Inline, so nk_library.c needs no other object. */
static inline const char *nk_disc_tier_name(NkDiscTier tier) {
    switch (tier) {
    case NK_DISC_TIER_VERIFIED_RETAIL: return "VERIFIED_RETAIL";
    case NK_DISC_TIER_MODIFIED: return "MODIFIED";
    case NK_DISC_TIER_UNKNOWN: return "UNKNOWN";
    case NK_DISC_TIER_NOT_CHECKED:
    default: return "NOT_CHECKED";
    }
}

/* The tier a stored name means; false (and NOT_CHECKED) for any other text. */
static inline bool nk_disc_tier_from_name(const char *name, NkDiscTier *out_tier) {
    static const NkDiscTier tiers[] = {
        NK_DISC_TIER_NOT_CHECKED, NK_DISC_TIER_VERIFIED_RETAIL, NK_DISC_TIER_MODIFIED,
        NK_DISC_TIER_UNKNOWN,
    };
    if (out_tier) *out_tier = NK_DISC_TIER_NOT_CHECKED;
    for (size_t i = 0; name && i < sizeof(tiers) / sizeof(tiers[0]); i++) {
        if (strcmp(name, nk_disc_tier_name(tiers[i])) == 0) {
            if (out_tier) *out_tier = tiers[i];
            return true;
        }
    }
    return false;
}

/* SHA-1 (FIPS 180-4). */
typedef struct {
    uint32_t state[5];
    uint64_t byte_count;
    uint8_t block[64];
    size_t block_len;
} NkSha1;
void nk_sha1_init(NkSha1 *ctx);
void nk_sha1_update(NkSha1 *ctx, const uint8_t *data, size_t len);
void nk_sha1_finish(NkSha1 *ctx, uint8_t digest[20]);

/* CRC-32 as Redump and zlib compute it (reflected 0xEDB88320, ~0 in and out).
   Start from 0 and pass each result back in. */
uint32_t nk_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* The whole-image hashes of one disc image: lowercase hex, and its size. */
typedef struct {
    char sha1[NK_DISC_SHA1_HEX_CHARS + 1];
    char crc32[NK_DISC_CRC32_HEX_CHARS + 1];
    uint64_t size_bytes;
} NkDiscDigest;

/* Hash a whole plain image, reading it once. callbacks (optional) report
   progress and can cancel; a cancel or read failure fills status (CANCELLED,
   DISC_IMAGE_READ_FAILED) and leaves out cleared. Run it on a worker: a disc
   is up to 1.8 GB. */
bool nk_disc_hash_file(const char *path, const NkDiscExpandCallbacks *callbacks,
                       NkDiscDigest *out, NkDiscExpandStatus *status);

typedef struct {
    uint8_t sha1[20];
    uint32_t crc32;
    uint64_t size_bytes;
    uint32_t listed; /* its place in the datafile, so a repeated SHA-1 keeps the first */
} NkDiscDatRom;

/* The parts of one datafile verification uses. roms is sorted by SHA-1 and
   serials (nine-character disc IDs, "TEST71002") are sorted, both for binary
   search. Free with nk_disc_dat_free. */
typedef struct {
    NkDiscDatRom *roms;
    size_t rom_count;
    char (*serials)[10];
    size_t serial_count;
    size_t games;
    size_t rejected_roms;
    char name[128];
    char version[64];
    char date[64];
    /* Set by nk_disc_dat_load_active on an import older than the bundled list:
       the bundled list's date (printable ASCII). Empty otherwise. */
    char newer_bundled_date[64];
} NkDiscDat;

void nk_disc_dat_free(NkDiscDat *dat);
/* Read a datafile, or the zip Redump serves, from memory: the whole format
   (rules 1-10), disc_verify.py's dat_from_bytes. Every refusal fills status
   with a DISC_DAT_* code and the message disc_verify.py gives. */
bool nk_disc_dat_read(const uint8_t *bytes, size_t length, NkDiscDat *out,
                      NkDiscExpandStatus *status);
/* Read a datafile's text from memory (rules 3-10, no zip and no size check:
   disc_verify.py's parse_dat_bytes). One UTF-8 byte-order mark is skipped. */
bool nk_disc_dat_parse(const char *text, size_t length, NkDiscDat *out,
                       NkDiscExpandStatus *status);
/* nk_disc_dat_parse, also giving the bytes the reader looked at, each counted
   every time it is looked at (NULL for none), whether it succeeds or not: the
   tests hold it to a fixed multiple of length, for any input. */
bool nk_disc_dat_parse_counted(const char *text, size_t length, NkDiscDat *out,
                               NkDiscExpandStatus *status, uint64_t *bytes_examined);
/* Whether a list's date sorts before another's, both in Redump's
   "YYYY-MM-DD HH-MM-SS" form (the time is optional; on the same day it counts
   only when both have one). Any other form is never called older. */
bool nk_disc_list_date_is_older(const char *date, const char *other);
/* The same, read from a file of at most NK_DISC_DAT_MAX_BYTES: a .dat, or the
   zip Redump serves with one .dat inside (DISC_DAT_ZIP_INVALID when it is not
   one). */
bool nk_disc_dat_load(const char *path, NkDiscDat *out, NkDiscExpandStatus *status);
/* <data_root>/verification/redump.dat. */
bool nk_disc_dat_path(const char *data_root, char *out, size_t out_size);
/* <base_dir>assets/redump/redump-psp.zip, with the platform's separators.
   base_dir is the player's folder; it may end in a separator (as
   SDL_GetBasePath's does) or not. */
bool nk_disc_bundled_dat_path(const char *base_dir, char *out, size_t out_size);
/* Check a datafile the user chose (a .dat, or Redump's zip) and keep a copy
   of the .dat in their data folder, replacing an earlier import whole
   (written beside it, then renamed). From then on it takes precedence over the
   bundled list. out (optional) receives the parsed list. */
bool nk_disc_dat_import(const char *source, const char *data_root, NkDiscDat *out,
                        NkDiscExpandStatus *status);

/* Which list a check used. */
typedef enum {
    NK_DISC_LIST_NONE = 0,  /* neither list was found */
    NK_DISC_LIST_IMPORTED,  /* <data_root>/verification/redump.dat */
    NK_DISC_LIST_BUNDLED    /* the list bundled with Nakagawa */
} NkDiscListSource;

/* The list a check uses: the import in data_root when there is one, else the
   bundled list at bundled_path (NULL or "" for none). True with *source set
   when a list was read; false with *source NONE and an empty status code when
   neither exists; false with *source naming the list and status saying why
   when the chosen list can't be read. A damaged import is reported, never
   replaced by the bundled list. An import older than the bundled list is still
   used (the user chose it), with out->newer_bundled_date set. */
bool nk_disc_dat_load_active(const char *data_root, const char *bundled_path, NkDiscDat *out,
                             NkDiscListSource *source, NkDiscExpandStatus *status);

/* Whether the list names this disc ID in a <serial>, compared after ASCII
   upper-casing (rule 9). */
bool nk_disc_dat_has_serial(const NkDiscDat *dat, const char *disc_id);

/* The tier of one image. dat or digest NULL (or an empty digest) is
   NOT_CHECKED. disc_id_verified_elsewhere says a verified image in the same
   library carries this disc ID. */
NkDiscTier nk_disc_classify(const NkDiscDat *dat, const char *disc_id,
                            const NkDiscDigest *digest, bool disc_id_verified_elsewhere);

/* Disc IDs in a <serial> value ("TEST-71002, TEST-71002GA") as PARAM.SFO
   spells them: up to max_ids nine-character IDs, without repeats. Returns how
   many were written. */
size_t nk_disc_normalize_serials(const char *text, size_t length, char (*out_ids)[10],
                                 size_t max_ids);

/* ---- one call for an add worker ----------------------------------------- */

/* How one disc check ended. HASHED and later fill digest; tier says how the
   image compares with the list used (NOT_CHECKED when none was found). */
typedef enum {
    NK_DISC_VERIFY_HASHED = 0,     /* hashed; tier is set */
    NK_DISC_VERIFY_CANCELLED,      /* callbacks->is_cancelled said stop; nothing kept */
    NK_DISC_VERIFY_READ_FAILED,    /* the image could not be read whole; nothing kept */
    NK_DISC_VERIFY_LIST_UNUSABLE   /* hashed, but the list is damaged: tier NOT_CHECKED */
} NkDiscVerifyResult;

typedef struct {
    NkDiscVerifyResult result;
    NkDiscTier tier;
    NkDiscDigest digest;
    NkDiscListSource list;  /* which list the image was compared with */
    char list_date[64];     /* that list's own date (its header's <date>, else <version>) */
    char code[48];     /* empty, or the NK_DISC_* / DISC_DAT_* code behind a failure */
    char reason[640];  /* plain sentences for the card and the log; names the list */
} NkDiscVerification;

/* Hash the image at iso_path (for a compressed disc, the expansion the import
   made) and compare it with the list nk_disc_dat_load_active picks: the import
   in data_root, else the bundled list at bundled_dat_path (NULL for none;
   nk_disc_bundled_dat_path makes it). Reads the whole image once, so call it
   from a worker, never the UI thread. callbacks may be NULL; its on_progress
   gets bytes done of the total and is_cancelled is polled between 1 MiB reads.
   disc_id_verified_elsewhere: a verified image in the same library has this
   disc ID (nk_library_has_verified_disc_id), which makes a modified image
   MODIFIED rather than UNKNOWN. Returns true for HASHED and LIST_UNUSABLE.
   check.list and check.reason say which list was used and its date. The image
   is never changed and nothing leaves the machine.

       NkDiscVerification check;
       NkDiscExpandCallbacks cb = { on_progress, is_cancelled, job };
       if (nk_disc_verify_image(game.iso_path, data_root, bundled_dat_path, game.disc_id,
                                nk_library_has_verified_disc_id(&app->library, game.disc_id),
                                &cb, &check)) {
           nk_disc_verification_to_entry(&check, &game);   (hashes and tier)
       }
       log or show check.reason either way, then add the game as today.

   Redump's current PSP list names disc IDs in <serial> for almost every disc,
   so a changed image of a listed disc is MODIFIED. A list without serials (an
   older export) makes a modified image MODIFIED only when its retail disc is
   verified in the same library; otherwise it is UNKNOWN, which means "not
   shown to be retail", not "modified" and not "bad". */
bool nk_disc_verify_image(const char *iso_path, const char *data_root,
                          const char *bundled_dat_path, const char *disc_id,
                          bool disc_id_verified_elsewhere,
                          const NkDiscExpandCallbacks *callbacks, NkDiscVerification *out);

/* Copy a hashed check's hashes and tier into a library entry; anything else
   leaves the entry unhashed. */
void nk_disc_verification_to_entry(const NkDiscVerification *check, NkGameEntry *entry);

#ifdef __cplusplus
}
#endif

#endif /* NK_DISC_VERIFY_H */
