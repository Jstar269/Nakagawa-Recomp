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
 *   - Nothing moves while anything else may be using the old folders. Every process of this version
 *     that uses the per-user folders holds a shared lock on the plan's in-use file for its life (the
 *     player through nk_data_migrate_host, the Python tools through prereq_fetcher.default_data_root,
 *     which also refuse by name while a run holds the run lock). Before its first move a run takes
 *     that lock exclusively, without waiting, and also checks every lock the version before #324
 *     takes in its data folder: locks/library.lock and locks/build-package-*.lock (flock) and
 *     games/.staging_*.lock (fcntl). On Linux it also looks through /proc, among the processes of
 *     the same user, for one whose program, working folder or open file is inside an old folder, or
 *     that runs a program of this one's name (plan.instance_name). When any is found, nothing moves:
 *     the old folders stay in use for the run (KEPT_OLD, DATA_MIGRATION_IN_USE, told once) and the
 *     next start tries again. What cannot be seen: on macOS, an older Nakagawa window, game or tool
 *     that holds none of those locks at that moment; on Linux, one that runs under another file
 *     name and has nothing open in the old folders, or one of another user. For those, a completed
 *     run leaves a record (plan.done_file); an old folder that appears again after it can only hold
 *     what was written there after the move, and a start that finds it beside the new folders says
 *     so by name (DATA_MIGRATION_LATE_WRITES: which folder is in use, that the other holds later
 *     files), with both folders to open, and merges or deletes neither.
 *   - library.json is rewritten under the library's own lock (locks/library.lock, as every writer of
 *     it takes): the old one is taken before the first move and held until the rewrite is done.
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
 *     symbolic link (a rename moves the link, and a relative link would then point elsewhere), or
 *     holds a relative link that leads out of the folder that moves with it (GNU stow's links: they
 *     would point elsewhere after the move; absolute links, and relative ones that stay inside, are
 *     fine); two folders that matter, or one of them and a folder that stays (the Linux cache or
 *     logs), are the same folder or one is inside the other (XDG homes that point at each other); a
 *     folder that matters belongs to another user than the one running (sudo with HOME kept);
 *     library.json was written by a newer Nakagawa; another process may be using the old folders
 *     (above); or the lock cannot be taken. If the new folders already hold data too, that is
 *     REFUSED instead, since neither layout is then complete.
 *   - After a successful run the old folders are gone, so a second run finds nothing to do.
 *   - A run stopped part-way (power lost, the process killed) is finished by the next one: the folders
 *     not yet moved still are, Linux's data/saves still moves up when the data folder had already
 *     moved, and library.json paths into an old data folder that is gone are still rewritten. Once
 *     a run has finished (its record exists), neither of those finishing steps runs again: a
 *     data/saves folder that appears later (an explicit --user-data-root naming Nakagawa/data) stays
 *     where it is, and library.json is not read at every start.
 *   - An undo that fails leaves that folder in the new place, and then the folder it was moved into
 *     is not put back either (it holds it). The message says where every folder is now; the next
 *     start carries on from there.
 *   - Every JSON string in library.json that names the old data folder, or a path below it, however
 *     it is spelled (through a link, with "//", "." or "..", or a trailing '/': both sides are
 *     compared lexically cleaned and with links resolved), is rewritten to the same path below the
 *     new one (the original is kept, written whole by a temporary file and a rename, as
 *     library.json.pre-migration; an existing one that does not parse is replaced). Each string is
 *     decoded before it is compared and the new value is escaped as the library writes it. The
 *     rewritten file is read back: a run that moved the data never reports DONE while a stored path
 *     still names the old folder; it undoes its moves and says so by name. library.json.bak and the
 *     library.json.v<N>.bak files keep the old paths: nk_library_load falls back to .bak only when
 *     the primary is unreadable, and they are not rewritten.
 *   - A folder that cannot be flushed to the disk after a rename that succeeded (durability unknown,
 *     the rename done) is a named log line (DATA_MIGRATION_NOT_FLUSHED); the run goes on, and the
 *     next start finds the folders where the rename put them.
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
    /* Shared by every process of this version that uses the folders; taken exclusively, without
       waiting, before the first move. Empty for none. */
    char in_use_file[NK_MAX_PATH];
    /* Keep a shared lock on in_use_file for the rest of this process's life (the player). */
    bool hold_in_use;
    /* Linux: look through /proc for another process using an old folder (nk_data_migrate.h). */
    bool scan_processes;
    /* The file name of this program; another process running one of that name may be an older
       copy. Empty for no such check. */
    char instance_name[64];
    /* The record a completed run writes (when it was, and the old folders it retired). Empty for
       none. */
    char done_file[NK_MAX_PATH];
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
#define NK_MIGRATE_CODE_IN_USE "DATA_MIGRATION_IN_USE"
#define NK_MIGRATE_CODE_LATE_WRITES "DATA_MIGRATION_LATE_WRITES"
#define NK_MIGRATE_CODE_OTHER_OWNER "DATA_MIGRATION_OTHER_OWNER"
#define NK_MIGRATE_CODE_XDG_IGNORED "DATA_MIGRATION_XDG_IGNORED"
#define NK_MIGRATE_CODE_RECORD_NOT_WRITTEN "DATA_MIGRATION_RECORD_NOT_WRITTEN"

void nk_data_migrate_run(const NkDataMigrationPlan *plan, NkDataMigration *result);

/* Drops the shared in-use lock a run kept (plan.hold_in_use). A later run in this process does it
   first, so this process never counts as another user of the folders. */
void nk_data_migrate_release_in_use(void);

/* The host's plan and the run in one call; NOTHING when the host has no old layout. KEPT_OLD also
   switches this process to the old folders (nk_platform_set_old_user_data_layout). The player's
   run keeps the in-use lock for the life of the process and looks (on Linux) for other processes
   using the old folders or running a program of the player's own file name. */
void nk_data_migrate_host(NkDataMigration *result);

/* For a KEPT_OLD result: true the first time this message is seen, so the user is told once; it is
   remembered in the cache folder (a cache that cannot be written tells the user again next time). */
bool nk_data_migrate_notice_is_new(const NkDataMigration *result);

/* Test seam: replace the rename used for every move and undo (NULL restores nk_rename_utf8). A
   replacement that fails sets errno as rename does. */
typedef int (*NkMigrateRenameFn)(const char *from, const char *to);
void nk_data_migrate_set_rename_for_test(NkMigrateRenameFn rename_fn);

/* Test seam: replace the folder flush after every rename (NULL restores nk_platform_sync_dir). A
   replacement that fails returns false with errno set. */
typedef bool (*NkMigrateSyncDirFn)(const char *dir);
void nk_data_migrate_set_sync_dir_for_test(NkMigrateSyncDirFn sync_fn);

/* Test seam: the effective user ID the owner check compares with (-1 restores geteuid()). */
void nk_data_migrate_set_euid_for_test(long euid);

#ifdef __cplusplus
}
#endif

#endif /* NK_DATA_MIGRATE_H */
