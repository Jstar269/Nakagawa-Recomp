/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Which per-user folders a Linux or macOS install uses, and the move the user can ask for (#324).
 *
 * The folders that matter (the library and data, the saves, the settings) live in `Nakagawa` under the
 * host's own data base (docs/SETUP.md, "Where your data lives"). Windows always did. Linux used
 * `nakagawa-recomp` in the XDG data home (saves inside it) and the XDG config home; its cache and logs
 * stay where they were, in the XDG cache and state homes. macOS used `NakagawaRecomp`, whose whole
 * folder becomes `Nakagawa`, with Caches and Logs moving into it. Windows and Android have no older
 * layout: their plan is empty and nothing here runs.
 *
 * Nothing moves at start-up. The layout is decided once per process, by one rule that the player
 * (nk_data_layout_decide) and the Python tools (tools/nk_core/prereq_fetcher.py) share, in this order:
 *   1. The move's journal exists (a move was interrupted): the player finishes the move, or undoes it,
 *      before anything uses the folders; the tools refuse by name and leave that to the player.
 *   2. The marker exists in the new folder: the new layout.
 *   3. A folder of the old layout that matters holds files: the old layout, all of it.
 *   4. Otherwise the new layout, and the marker is written, so a stray old folder made later can never
 *      turn a new install back.
 * Data in both places never stops a start: the folder the rule chose is used, and a notice names both
 * (NK_LAYOUT_NOTICE_BOTH, or NK_LAYOUT_NOTICE_RETURNED for an old folder that holds files again after
 * the marker, which only an older version of Nakagawa that was still running can make). Nothing is
 * merged, moved or deleted to resolve it. The RETURNED notice skips an old folder that the new layout
 * writes itself (Linux XDG homes that point at each other, so the old data folder is also the cache):
 * its files are this version's own.
 *
 * The rule's cases, which tests/native/test_data_migrate.c and tools/test_prereq_fetcher.py both read
 * from here, so the two cannot drift apart. Linux layout; "files" is a folder with a file in it,
 * "empty" a folder without one; the player column is the layout it uses, the tools column the folder
 * a tool uses or "refuse".
 * BEGIN DATA FOLDER RULE CASES
 * case                      journal marker old_data old_config new_data player tools  notice
 * fresh                     no      no     none     none       none     new    new    none
 * fresh_with_empty_folders  no      no     empty    empty      empty    new    new    none
 * new_folder_without_marker no      no     none     none       files    new    new    none
 * old_data                  no      no     files    none       none     old    old    none
 * old_settings_only         no      no     none     files      none     old    old    none
 * old_layout                no      no     files    files      empty    old    old    none
 * both_without_marker       no      no     files    files      files    old    old    both
 * marker                    no      yes    none     none       files    new    new    none
 * marker_old_data_returned  no      yes    files    none       files    new    new    returned
 * marker_old_settings_back  no      yes    none     files      files    new    new    returned
 * marker_empty_old_folders  no      yes    empty    empty      files    new    new    none
 * interrupted_move          yes     no     files    files      none     new    refuse none
 * END DATA FOLDER RULE CASES
 *
 * The move (nk_data_migrate_move) runs only when the user asks for it in Settings, with no game
 * running and no other player open. It refuses, by name and before touching anything, when another
 * player holds the player lock, when a library, package-build or staging lock of this or the previous
 * version is held, when the new folder already holds files, when a folder that would move is a link or
 * holds a relative link that would point elsewhere afterwards, belongs to another user, overlaps a
 * folder that stays (Linux's cache and logs), or is on another file system than its destination, or
 * when the library was written by a newer version. Then it is one journalled transaction:
 *   - The run lock (`.nakagawa-migration.lock` in the data base) is held throughout, and the library's
 *     own lock from before the first rename to after the rewrite.
 *   - The journal (`.nakagawa-move-in-progress`, in the data base: it exists before the move, lies on
 *     the new folder's file system, and no rename carries it) lists every step in order: each rename
 *     with its source and destination, then the library rewrite. It is synced, and its folder too,
 *     before the first rename.
 *   - Each step's state is read from the disk, never from a counter: source gone and destination
 *     present is done; source present and destination absent (or only empty folders) is to do;
 *     anything else is an error. Only empty folders are ever removed (one rmdir at a time).
 *   - The library rewrite matches each stored path lexically cleaned or with links resolved (the
 *     resolved old folder is taken before the first rename and kept in the journal), keeps the
 *     original as library.json.pre-migration, and reads the result back: a path that still names the
 *     old folder fails the step.
 *   - Commit is one rename: the journal becomes the marker (`.nakagawa-layout` in the new folder),
 *     after everything it vouches for is synced.
 *   - A step that fails undoes the steps before it, newest first (the one that failed did not run),
 *     and removes the journal. If the undo fails too, the journal stays, and the next start goes
 *     through rule 1.
 * Recovery at start (rule 1) takes the same locks and the same lock checks as the move, rolls the
 * remaining steps forward, and undoes them if one cannot complete. Only when neither finishes, or
 * the locks cannot be taken (another copy is running), does the start stop, with a message that names
 * both folders and what each holds, and Quit. That is the one case that blocks a start, and it can
 * only follow a move the user started. After a move, or a failed one it could not put back, the player
 * quits without saving and starts again: it never switches folders mid-run.
 *
 * Not detectable: a window of an older version (it takes none of this version's locks and holds its
 * own only while it writes). The confirmation asks the user to close every Nakagawa window first;
 * anything such a copy writes into an old folder after the move is named by the notice and never used
 * or deleted.
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
    /* Its `from` lies inside the move before it's `to` (Linux's old data/saves, which becomes the
       root's saves); it is part of the move only when that folder exists. */
    bool after_previous;
    /* A cache or logs folder: a failure to move it is a log line, never a failed move. */
    bool disposable;
} NkMigrateMove;

typedef struct NkDataMigrationPlan {
    char base[NK_MAX_PATH];         /* the data base: the new folder's parent, which holds the locks */
    char new_root[NK_MAX_PATH];
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
    char lock_file[NK_MAX_PATH];        /* the run lock; empty for none (a test's own plan) */
    char player_lock_file[NK_MAX_PATH]; /* every player holds it shared; a move or recovery alone */
    char journal_file[NK_MAX_PATH];     /* the move's journal, in the data base */
    char marker_file[NK_MAX_PATH];      /* in the new folder: the new layout is the one in use */
    /* Named conditions met while the plan was built (a folder whose path could not be formed, an
       XDG variable that is ignored), one line each; copied to the log. */
    char notes[1024];
} NkDataMigrationPlan;

/* ---- The rule ---- */

typedef enum {
    NK_LAYOUT_NEW = 0,
    NK_LAYOUT_OLD,
    NK_LAYOUT_BLOCKED /* an interrupted move could be neither finished nor undone: stop the start */
} NkLayoutChoice;

typedef enum {
    NK_LAYOUT_NOTICE_NONE = 0,
    NK_LAYOUT_NOTICE_BOTH,     /* the old layout is in use; the new folder holds files too */
    NK_LAYOUT_NOTICE_RETURNED  /* the new layout is in use; an old folder holds files again */
} NkLayoutNotice;

typedef struct {
    NkLayoutChoice layout;
    NkLayoutNotice notice_kind;
    /* The case of the rule that decided: "journal", "marker", "old", "fresh", or "none" (a host
       without an older layout, or no plan). */
    const char *rule;
    bool recovered;        /* an interrupted move was finished (NEW) or undone (OLD) at this start */
    bool can_move;         /* the old layout is in use and the user can move it (Settings) */
    char notice[2048];     /* the non-blocking notice, or "" */
    char message[4096];    /* BLOCKED: what to tell the user, with the code first */
    char log[2048];        /* named lines for the log, each ending in '\n' */
    char folder_a[NK_MAX_PATH]; /* the folder in use (or, BLOCKED, the old one) */
    char folder_b[NK_MAX_PATH]; /* the other one */
} NkLayoutDecision;

/* Decides the layout for this process under the run lock, recovering an interrupted move first. On
   a plan with a player lock, the caller holds it shared from here on (released with
   nk_data_migrate_release_player_lock). Does not switch the platform's layout. */
void nk_data_layout_decide(const NkDataMigrationPlan *plan, NkLayoutDecision *out);

/* The host's plan and the rule in one call, and the platform switched to the chosen layout
   (nk_platform_set_old_user_data_layout). A host without an older layout gets NEW and nothing else. */
void nk_data_layout_host(NkLayoutDecision *out);

void nk_data_migrate_release_player_lock(void);

/* ---- The move ---- */

typedef enum {
    NK_MIGRATE_NOTHING = 0, /* nothing to move: no old folder holds files, or already moved */
    NK_MIGRATE_DONE,        /* moved and committed: restart to use the new folders */
    NK_MIGRATE_REFUSED,     /* a check failed: nothing was touched */
    NK_MIGRATE_FAILED       /* a step failed: undone, or (message) left for the next start */
} NkMigrateStatus;

typedef struct {
    NkMigrateStatus status;
    size_t moved;
    bool library_rewritten;
    /* The folders are no longer the ones this process started on (DONE, or a failed move that could
       not be put back and waits for the next start): quit and start again. */
    bool must_restart;
    char message[4096];     /* for the user: starts with the code, then what to do */
    char log[2048];         /* named lines for the log, each ending in '\n' */
    char folder_a[NK_MAX_PATH]; /* the old folder */
    char folder_b[NK_MAX_PATH]; /* the new one */
} NkDataMigration;

/* Codes at the start of messages and log lines. */
#define NK_MIGRATE_CODE_BOTH_PRESENT "DATA_MIGRATION_BOTH_PRESENT"
#define NK_MIGRATE_CODE_FAILED "DATA_MIGRATION_FAILED"
#define NK_MIGRATE_CODE_OTHER_DISK "DATA_MIGRATION_OTHER_DISK"
#define NK_MIGRATE_CODE_LINKED "DATA_MIGRATION_LINKED"
#define NK_MIGRATE_CODE_OVERLAP "DATA_MIGRATION_OVERLAP"
#define NK_MIGRATE_CODE_NEWER_LIBRARY "DATA_MIGRATION_NEWER_LIBRARY"
#define NK_MIGRATE_CODE_LOCK "DATA_MIGRATION_LOCK"
#define NK_MIGRATE_CODE_IN_USE "DATA_MIGRATION_IN_USE"
#define NK_MIGRATE_CODE_GAME_RUNNING "DATA_MIGRATION_GAME_RUNNING"
#define NK_MIGRATE_CODE_OTHER_OWNER "DATA_MIGRATION_OTHER_OWNER"
#define NK_MIGRATE_CODE_UNSAFE_NAME "DATA_MIGRATION_UNSAFE_NAME"
#define NK_MIGRATE_CODE_INTERRUPTED "DATA_MIGRATION_INTERRUPTED"
#define NK_MIGRATE_CODE_LEFT_BEHIND "DATA_MIGRATION_LEFT_BEHIND"
#define NK_MIGRATE_CODE_PATH_UNAVAILABLE "DATA_MIGRATION_PATH_UNAVAILABLE"
#define NK_MIGRATE_CODE_NOT_FLUSHED "DATA_MIGRATION_NOT_FLUSHED"
#define NK_MIGRATE_CODE_XDG_IGNORED "DATA_MIGRATION_XDG_IGNORED"
#define NK_MIGRATE_CODE_MARKER_NOT_WRITTEN "DATA_MIGRATION_MARKER_NOT_WRITTEN"
#define NK_LAYOUT_CODE_BOTH "DATA_FOLDERS_BOTH_HOLD_FILES"
#define NK_LAYOUT_CODE_RETURNED "DATA_FOLDERS_OLD_FOLDER_RETURNED"

/* The move the user asked for. `game_running`: a game this player launched is still running. The
   caller holds the player lock shared (nk_data_layout_decide); it is taken exclusively for the move
   and given back shared. After DONE the process must not use its folders again: restart it. */
void nk_data_migrate_move(const NkDataMigrationPlan *plan, bool game_running, NkDataMigration *result);

/* ---- Test seams ---- */

/* Replace the rename used for every step, undo and the commit (NULL restores nk_rename_utf8). A
   replacement that fails sets errno as rename does. */
typedef int (*NkMigrateRenameFn)(const char *from, const char *to);
void nk_data_migrate_set_rename_for_test(NkMigrateRenameFn rename_fn);

/* Replace the folder flush after every rename (NULL restores nk_platform_sync_dir). A replacement
   that fails returns false with errno set. */
typedef bool (*NkMigrateSyncDirFn)(const char *dir);
void nk_data_migrate_set_sync_dir_for_test(NkMigrateSyncDirFn sync_fn);

/* The effective user ID the owner check compares with (-1 restores geteuid()). */
void nk_data_migrate_set_euid_for_test(long euid);

/* Called at each named point of a move or recovery: "begin" (every check passed and every lock
   taken, before the journal is written), "journal" (synced, before the first rename),
   "step:<n>" (after step n, from 1), "undo:<n>" (after step n was undone), "commit" (after the
   commit rename). A test kills its process there. NULL for none. */
typedef void (*NkMigratePointFn)(const char *point);
void nk_data_migrate_set_point_hook_for_test(NkMigratePointFn hook);

#ifdef __cplusplus
}
#endif

#endif
