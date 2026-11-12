/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* One-time move of an older per-user folder into the unified one (#324).
 *
 * Every host keeps its per-user files in `Nakagawa/{config,data,cache,logs,saves}` under the host's own
 * data base (docs/SETUP.md, "Where your data lives"). Windows always did. Linux used `nakagawa-recomp`
 * with the XDG config, cache and state folders beside it, macOS `NakagawaRecomp` with Caches and Logs
 * beside it. nk_data_migrate_run() moves those into the new place, once.
 *
 * The rules, each with a test in tests/native/test_data_migrate.c:
 *   - A move is a rename of a whole folder. Nothing is copied, and nothing is ever deleted except an
 *     empty folder tree that stood where a folder was to go.
 *   - A destination that holds a file is not replaced. Every move is checked before the first one
 *     happens; if any destination holds data, nothing moves and the result is REFUSED, with both paths
 *     named and what to do.
 *   - If a rename fails (another disk, a locked or read-only folder), the moves already made are undone
 *     in reverse order and the result is FAILED. If an undo fails too, the message says where each
 *     folder is.
 *   - After a successful run the old folders are gone, so a second run finds nothing to do.
 *   - A run stopped part-way (power lost, the process killed) is finished by the next one: the folders
 *     not yet moved still are, Linux's data/saves still moves up when the data folder had already
 *     moved, and library.json paths into an old data folder that is gone are still rewritten.
 *   - The stored absolute paths in library.json that pointed into the old data folder are rewritten to
 *     the new one (the original is kept as library.json.pre-migration).
 *
 * The plan comes from the host (nk_platform_user_data_plan): Windows has nothing to move. */

#ifndef NK_DATA_MIGRATE_H
#define NK_DATA_MIGRATE_H

#include "nk_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_MIGRATE_MAX_MOVES 8

typedef struct {
    char from[NK_MAX_PATH];
    char to[NK_MAX_PATH];
    /* Runs only when the move before it ran: its `from` is made by that move (Linux's old
       data/saves, which becomes the root's saves). */
    bool after_previous;
} NkMigrateMove;

typedef struct NkDataMigrationPlan {
    char new_root[NK_MAX_PATH];     /* made before the first move */
    NkMigrateMove moves[NK_MIGRATE_MAX_MOVES];
    size_t move_count;
    /* library.json's stored paths under remap_from are rewritten to remap_to; both empty for none. */
    char remap_from[NK_MAX_PATH];   /* the old data folder, no trailing separator */
    char remap_to[NK_MAX_PATH];     /* the new data folder */
    char library_file[NK_MAX_PATH]; /* library.json in its new place */
} NkDataMigrationPlan;

typedef enum {
    NK_MIGRATE_NOTHING = 0, /* no old folder holds data: a fresh install, or already migrated */
    NK_MIGRATE_DONE,
    NK_MIGRATE_REFUSED,     /* old and new both hold data: nothing was moved */
    NK_MIGRATE_FAILED       /* a move failed: undone as far as message says */
} NkMigrateStatus;

typedef struct {
    NkMigrateStatus status;
    size_t moved;
    bool library_rewritten;
    char message[2560];     /* for the user: starts with the code, then what to do */
} NkDataMigration;

/* Codes at the start of NkDataMigration.message for REFUSED and FAILED. */
#define NK_MIGRATE_CODE_BOTH_PRESENT "DATA_MIGRATION_BOTH_PRESENT"
#define NK_MIGRATE_CODE_FAILED "DATA_MIGRATION_FAILED"

void nk_data_migrate_run(const NkDataMigrationPlan *plan, NkDataMigration *result);

/* The host's plan and the run in one call; NOTHING when the host has no old layout. */
void nk_data_migrate_host(NkDataMigration *result);

/* Test seam: replace the rename used for every move (NULL restores nk_rename_utf8). */
typedef int (*NkMigrateRenameFn)(const char *from, const char *to);
void nk_data_migrate_set_rename_for_test(NkMigrateRenameFn rename_fn);

#ifdef __cplusplus
}
#endif

#endif /* NK_DATA_MIGRATE_H */
