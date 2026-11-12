/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* One-time move of an older per-user folder into the unified one (#324).
 *
 * The folders that matter (the library and data, the saves, the settings) live in `Nakagawa` under the
 * host's own data base (docs/SETUP.md, "Where your data lives"). Windows always did. Linux used
 * `nakagawa-recomp` in the XDG data home (saves inside it) and the XDG config home; its cache and logs
 * stay where they were, in the XDG cache and state homes, and are never moved. macOS used
 * `NakagawaRecomp`, whose whole folder becomes `Nakagawa`, with Caches and Logs moving into it.
 * nk_data_migrate_run() moves the older folders into place, once. It runs on Linux and macOS only:
 * Windows and Android have nothing to move, and their plan is empty.
 *
 * The rules, each with a test in tests/native/test_data_migrate.c:
 *   - One run at a time. The whole run, its checks included, holds an exclusive lock on the plan's
 *     lock file (a second start waits for the first, then finds the work done), so the state each
 *     check sees is the state the moves act on.
 *   - A move is a rename of a whole folder. Nothing is copied and nothing is deleted. The only
 *     removal is of an empty folder below a destination, one rmdir at a time, which the system
 *     refuses for a folder that holds anything; the rename then replaces the empty destination, and
 *     fails if anything appeared in it.
 *   - Only the folders that matter can stop the start. A destination of one that holds data is not
 *     replaced: every move is checked before the first one happens; if one holds data, nothing moves
 *     and the result is REFUSED, with both folders named (folder_a, folder_b) and what to do. A move
 *     of one that fails (another disk, a locked or read-only folder) undoes the moves already made,
 *     newest first, and the result is FAILED; another disk is named as such, and nothing is copied
 *     between disks. A cache or logs move that cannot be made is skipped: that folder stays where it
 *     is, a named line goes to `log`, and the run goes on.
 *   - The old folders stay in use for the run (KEPT_OLD, nothing moved or written, the player starts
 *     and the user is told by name) when moving would not be safe: a folder that matters is a
 *     symbolic link (a rename moves the link, and a relative link would then point elsewhere); two
 *     folders that matter, or one of them and a folder that stays (the Linux cache or logs), are the
 *     same folder or one is inside the other (XDG homes that point at each other); library.json was
 *     written by a newer Nakagawa; or the lock cannot be taken. If the new folders already hold data
 *     too, that is REFUSED instead, since neither layout is then complete.
 *   - After a successful run the old folders are gone, so a second run finds nothing to do.
 *   - A run stopped part-way (power lost, the process killed) is finished by the next one: the folders
 *     not yet moved still are, Linux's data/saves still moves up when the data folder had already
 *     moved, and library.json paths into an old data folder that is gone are still rewritten. This
 *     data/saves rule never retires: content that later appears in the new data/saves moves up too,
 *     or is REFUSED when the root's saves folder holds data.
 *   - An undo that fails leaves that folder in the new place, and then the folder it was moved into
 *     is not put back either (it holds it). The message says where every folder is now; the next
 *     start carries on from there.
 *   - Every JSON string in library.json that is the old data folder, or a path below it, is
 *     rewritten to the new one (the original is kept, written whole by a temporary file and a
 *     rename, as library.json.pre-migration; an existing one that does not parse is replaced). Each
 *     string is decoded before it is compared and the new value is escaped as the library writes it.
 *     library.json.bak and the library.json.v<N>.bak files keep the old paths: nk_library_load
 *     falls back to .bak only when the primary is unreadable, and the next start's rewrite then
 *     repairs the paths again while the old data folder stays absent.
 *
 * The plan comes from the host (nk_platform_user_data_plan). */

#ifndef NK_DATA_MIGRATE_H
#define NK_DATA_MIGRATE_H

#include "nk_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_MIGRATE_MAX_MOVES 8
#define NK_MIGRATE_MAX_KEPT 2

typedef struct {
    char from[NK_MAX_PATH];
    char to[NK_MAX_PATH];
    char label[32];       /* what the user calls it: "data", "saves", "settings", "cache", "logs" */
    /* Runs only when the move before it ran, or to finish an earlier run: its `from` lies inside
       that move's `to` (Linux's old data/saves, which becomes the root's saves). */
    bool after_previous;
    /* A cache or logs folder: never stops the start. */
    bool disposable;
} NkMigrateMove;

typedef struct NkDataMigrationPlan {
    char new_root[NK_MAX_PATH];     /* made before the first move */
    NkMigrateMove moves[NK_MIGRATE_MAX_MOVES];
    size_t move_count;
    /* Folders this host keeps where they are (Linux's cache and logs). A folder that matters which
       is one of them, or holds or lies in one, cannot be moved apart from it. */
    char kept[NK_MIGRATE_MAX_KEPT][NK_MAX_PATH];
    char kept_label[NK_MIGRATE_MAX_KEPT][32];
    size_t kept_count;
    /* library.json's stored paths under remap_from are rewritten to remap_to; both empty for none. */
    char remap_from[NK_MAX_PATH];       /* the old data folder, no trailing separator */
    char remap_to[NK_MAX_PATH];         /* the new data folder */
    char library_file[NK_MAX_PATH];     /* library.json in its new place */
    char old_library_file[NK_MAX_PATH]; /* library.json in the old data folder */
    /* Held for the whole run; empty for none (a plan a test builds by hand). */
    char lock_file[NK_MAX_PATH];
    /* Named conditions met while the plan was built (a folder whose path could not be formed),
       one line each; copied to the result's log. */
    char notes[1024];
} NkDataMigrationPlan;

typedef enum {
    NK_MIGRATE_NOTHING = 0, /* no old folder holds data: a fresh install, or already migrated */
    NK_MIGRATE_DONE,
    NK_MIGRATE_REFUSED,     /* a folder that matters cannot move: nothing was moved; stop the start */
    NK_MIGRATE_FAILED,      /* a move of a folder that matters failed: undone as far as message says */
    NK_MIGRATE_KEPT_OLD     /* nothing moved; the old folders stay in use for this run */
} NkMigrateStatus;

typedef struct {
    NkMigrateStatus status;
    size_t moved;
    bool library_rewritten;
    char message[4096];     /* for the user: starts with the code, then what to do */
    char log[2048];         /* named lines for the log (a folder left behind), each ending in '\n' */
    /* REFUSED and FAILED: the two folders the message is about, for "Open both folders". */
    char folder_a[NK_MAX_PATH];
    char folder_b[NK_MAX_PATH];
} NkDataMigration;

/* Codes at the start of NkDataMigration.message, and of the lines in log. */
#define NK_MIGRATE_CODE_BOTH_PRESENT "DATA_MIGRATION_BOTH_PRESENT"
#define NK_MIGRATE_CODE_FAILED "DATA_MIGRATION_FAILED"
#define NK_MIGRATE_CODE_OTHER_DISK "DATA_MIGRATION_OTHER_DISK"
#define NK_MIGRATE_CODE_LINKED "DATA_MIGRATION_LINKED"
#define NK_MIGRATE_CODE_OVERLAP "DATA_MIGRATION_OVERLAP"
#define NK_MIGRATE_CODE_NEWER_LIBRARY "DATA_MIGRATION_NEWER_LIBRARY"
#define NK_MIGRATE_CODE_LOCK "DATA_MIGRATION_LOCK"
#define NK_MIGRATE_CODE_LEFT_BEHIND "DATA_MIGRATION_LEFT_BEHIND"
#define NK_MIGRATE_CODE_PATH_UNAVAILABLE "DATA_MIGRATION_PATH_UNAVAILABLE"
#define NK_MIGRATE_CODE_LIBRARY_NOT_UPDATED "DATA_MIGRATION_LIBRARY_NOT_UPDATED"
#define NK_MIGRATE_CODE_NOT_FLUSHED "DATA_MIGRATION_NOT_FLUSHED"

void nk_data_migrate_run(const NkDataMigrationPlan *plan, NkDataMigration *result);

/* The host's plan and the run in one call; NOTHING when the host has no old layout. KEPT_OLD also
   switches this process to the old folders (nk_platform_set_old_user_data_layout). */
void nk_data_migrate_host(NkDataMigration *result);

/* For a KEPT_OLD result: true the first time this message is seen, so the user is told once; it is
   remembered in the cache folder (a cache that cannot be written tells the user again next time). */
bool nk_data_migrate_notice_is_new(const NkDataMigration *result);

/* Test seam: replace the rename used for every move and undo (NULL restores nk_rename_utf8). A
   replacement that fails sets errno as rename does. */
typedef int (*NkMigrateRenameFn)(const char *from, const char *to);
void nk_data_migrate_set_rename_for_test(NkMigrateRenameFn rename_fn);

#ifdef __cplusplus
}
#endif

#endif /* NK_DATA_MIGRATE_H */
