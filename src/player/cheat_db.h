/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The cheat database (#2639; maintainer decision 2026-11-21): "Download cheat database"
 * fetches a community CWCheat database into cheats/cheat.db in the user data folder, and "Use
 * my own file" copies one the user already has there. The project never ships or commits a
 * database.
 *
 * What is downloaded comes from a list of sources the project keeps in its repository,
 * assets/cheat_db_sources.json, read from the repository's main branch on the host below, so
 * the project can change or remove a source without a program release. Each source is pinned
 * to one commit of its repository, with the file's exact size and SHA-256: a download that
 * differs from them is refused by name and nothing is written.
 *
 * Nothing here opens a connection on its own and nothing here names a network API: the caller
 * passes the transport (the player passes the native WinHTTP one, from a click), and both
 * requests go to NK_CHEAT_DB_HOST only. The list and the database are untrusted input: each is
 * read under a size cap, the list by a bounded JSON reader, the database by the project's
 * CWCheat reader (src/core/nk_cheats.h) before it may replace anything. */

#ifndef NK_PLAYER_CHEAT_DB_H
#define NK_PLAYER_CHEAT_DB_H

#include "package_builder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one host both requests reach (docs/SETUP.md "Network use"). */
#define NK_CHEAT_DB_HOST "raw.githubusercontent.com"
/* Every URL either request may name starts with this. */
#define NK_CHEAT_DB_URL_PREFIX "https://" NK_CHEAT_DB_HOST "/"
/* The list of sources: this repository's own file, from its main branch. */
#define NK_CHEAT_DB_SOURCES_URL \
    "https://" NK_CHEAT_DB_HOST "/Jstar269/nakagawa-recomp/main/assets/cheat_db_sources.json"
#define NK_CHEAT_DB_USER_AGENT "NakagawaRecomp/CheatDatabase"
/* The list's "format" value, and the one schema_version this reader understands. A list of
 * another version is refused by name (NK_CHEAT_DB_LIST_VERSION): a newer player reads it. */
#define NK_CHEAT_DB_SOURCES_FORMAT "nakagawa-cheat-db-sources"
#define NK_CHEAT_DB_SOURCES_SCHEMA 1
#define NK_CHEAT_DB_SOURCES_MAX_BYTES (64u * 1024u)
#define NK_CHEAT_DB_MAX_SOURCES 8u
/* The file the download and the import write, in the cheats folder of the user data folder.
 * A launch with cheats on reads it when the title has no cheats/<DISC-ID>.ini of its own. */
#define NK_CHEAT_DB_FILE_NAME "cheat.db"

#define NK_CHEAT_DB_ID_MAX 48u
#define NK_CHEAT_DB_TEXT_MAX 96u
#define NK_CHEAT_DB_URL_MAX 384u

/* One source of the list. Every field is checked when the list is read. */
typedef struct {
    char id[NK_CHEAT_DB_ID_MAX];           /* [a-z0-9-], for logs */
    char name[NK_CHEAT_DB_TEXT_MAX];       /* shown: the database's name */
    char maintainer[NK_CHEAT_DB_TEXT_MAX]; /* shown: who keeps it */
    char licence[NK_CHEAT_DB_TEXT_MAX];    /* shown: its licence as stated, "No licence stated" */
    char homepage[NK_CHEAT_DB_URL_MAX];    /* shown as text, never opened by the player */
    char commit[41];                       /* the pinned commit, 40 lowercase hex digits */
    char url[NK_CHEAT_DB_URL_MAX];         /* https://NK_CHEAT_DB_HOST/<owner>/<repo>/<commit>/<file> */
    uint64_t size;                         /* exact bytes, at most NK_CHEATS_MAX_FILE_BYTES */
    uint8_t sha256[32];
} NkCheatDbSource;

typedef struct {
    size_t count;
    NkCheatDbSource sources[NK_CHEAT_DB_MAX_SOURCES];
} NkCheatDbSources;

/* What a checked database holds: games (its _S lines) and cheats (its _C lines). */
typedef struct {
    uint32_t games;
    uint32_t cheats;
} NkCheatDbCounts;

typedef enum {
    NK_CHEAT_DB_OK = 0,
    NK_CHEAT_DB_BAD_ARGUMENT,
    NK_CHEAT_DB_NOT_REACHED,       /* no answer from the host */
    NK_CHEAT_DB_HTTP_STATUS,       /* an answer other than 200 */
    NK_CHEAT_DB_REDIRECT_REFUSED,  /* the bytes would come from another host */
    NK_CHEAT_DB_TOO_LARGE,
    NK_CHEAT_DB_TRUNCATED,         /* the body ended before its declared length */
    NK_CHEAT_DB_SIZE_MISMATCH,     /* not the pinned size */
    NK_CHEAT_DB_HASH_MISMATCH,     /* not the pinned SHA-256 */
    NK_CHEAT_DB_LIST_INVALID,
    NK_CHEAT_DB_LIST_VERSION,
    NK_CHEAT_DB_NOT_A_DATABASE,    /* the CWCheat reader does not accept it as a database */
    NK_CHEAT_DB_EXISTS,            /* cheat.db is there and overwriting was not confirmed */
    NK_CHEAT_DB_IO_ERROR,
    NK_CHEAT_DB_CANCELLED,
    NK_CHEAT_DB_OUT_OF_MEMORY
} NkCheatDbStatus;

/* The status's stable name, "CHEAT_DB_HASH_MISMATCH" and so on, for logs and the UI. */
const char *nk_cheat_db_status_name(NkCheatDbStatus status);

/* True, with false returned, to stop a download; called between reads. May be NULL. */
typedef bool (*NkCheatDbCancelFn)(void *context);

/* <data_root>/cheats/cheat.db. False when it does not fit. */
bool nk_cheat_db_path(const char *data_root, char *out, size_t out_size);

/* Reads the list's text. NK_CHEAT_DB_OK with at least one source, or the reason in error. */
NkCheatDbStatus nk_cheat_db_parse_sources(const char *text, size_t size, NkCheatDbSources *out,
                                          char *error, size_t error_size);

/* Fetches NK_CHEAT_DB_SOURCES_URL through transport and reads it. */
NkCheatDbStatus nk_cheat_db_fetch_sources(PackageHttpOpen transport, void *transport_context,
                                          NkCheatDbSources *out, char *error, size_t error_size);

/* Whether text is a cheat database the player may use as cheats/cheat.db: within
 * NK_CHEATS_MAX_FILE_BYTES, accepted by the CWCheat reader, with at least one game and every
 * cheat under a game's _S line (a cheat outside one would apply to every game).
 * Every database entry must start off (_C0). The user's per-disc files are separate. */
NkCheatDbStatus nk_cheat_db_check_text(const char *text, size_t size, NkCheatDbCounts *counts,
                                       char *error, size_t error_size);

/* Downloads source, checks its size, SHA-256 and text, and only then writes it to dest_path
 * (through a temporary file, renamed into place). An existing dest_path is replaced only with
 * overwrite; without it the result is NK_CHEAT_DB_EXISTS and nothing is requested. */
NkCheatDbStatus nk_cheat_db_download(PackageHttpOpen transport, void *transport_context,
                                     const NkCheatDbSource *source, const char *dest_path,
                                     bool overwrite, NkCheatDbCancelFn should_cancel,
                                     void *cancel_context, NkCheatDbCounts *counts,
                                     char *error, size_t error_size);

/* "Use my own file": reads source_path under the same cap, checks it as above and writes it to
 * dest_path the same way. */
NkCheatDbStatus nk_cheat_db_import_file(const char *source_path, const char *dest_path,
                                        bool overwrite, NkCheatDbCounts *counts,
                                        char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* NK_PLAYER_CHEAT_DB_H */
