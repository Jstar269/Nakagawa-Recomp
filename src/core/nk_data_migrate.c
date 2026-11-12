/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* One-time move of an older per-user folder into the unified one (#324). Rules: nk_data_migrate.h. */

#include "nk_data_migrate.h"

#include "nk_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIGRATE_LIBRARY_MAX_BYTES (16u * 1024u * 1024u)
#define MIGRATE_WALK_DEPTH 24

static NkMigrateRenameFn s_rename = NULL;

void nk_data_migrate_set_rename_for_test(NkMigrateRenameFn rename_fn) { s_rename = rename_fn; }

static int do_rename(const char *from, const char *to) {
    return s_rename ? s_rename(from, to) : nk_rename_utf8(from, to);
}

/* ---- What a folder holds ---- */

typedef enum { TREE_ABSENT = 0, TREE_EMPTY, TREE_CONTENT } TreeState;

typedef struct {
    const char *dir;
    int depth;
    bool content;
} Walk;

static TreeState tree_state(const char *path, int depth);

/* A folder holds data when it has any file, any entry the listing skips (a link or a file kept off
   this PC), or a folder that does. A folder that cannot be read counts as holding data, so the safe
   answer is the one that moves nothing. */
static bool walk_entry(const char *name, bool is_dir, void *context) {
    Walk *walk = (Walk *)context;
    if (!is_dir) {
        walk->content = true;
        return false;
    }
    char child[NK_MAX_PATH];
    if (snprintf(child, sizeof(child), "%s%c%s", walk->dir, nk_platform_path_separator(), name) >=
        (int)sizeof(child)) {
        walk->content = true;
        return false;
    }
    if (tree_state(child, walk->depth + 1) != TREE_EMPTY) {
        walk->content = true;
        return false;
    }
    return true;
}

static TreeState tree_state(const char *path, int depth) {
    if (nk_platform_path_missing(path)) return TREE_ABSENT;
    if (depth > MIGRATE_WALK_DEPTH || !nk_platform_dir_exists(path)) return TREE_CONTENT;
    Walk walk = {path, depth, false};
    size_t skipped = 0;
    bool listed = nk_platform_list_dir_counted(path, walk_entry, &walk, &skipped);
    if (walk.content) return TREE_CONTENT;
    return (listed && skipped == 0u) ? TREE_EMPTY : TREE_CONTENT;
}

static bool parent_dir(const char *path, char *dir, size_t cap) {
    size_t length = strlen(path);
    while (length > 0u && !nk_platform_is_path_separator(path[length - 1u])) length--;
    if (length <= 1u || length >= cap) return false;
    memcpy(dir, path, length - 1u);
    dir[length - 1u] = '\0';
    return true;
}

/* ---- Messages ---- */

static void say(NkDataMigration *r, const char *format, ...) {
    size_t used = strlen(r->message);
    if (used >= sizeof(r->message) - 1u) return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(r->message + used, sizeof(r->message) - used, format, args);
    va_end(args);
}

/* ---- library.json ---- */

static bool json_plain(const char *text) {
    for (const char *c = text; *c; c++) {
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20u) return false;
    }
    return true;
}

static bool read_whole(const char *path, char **data, size_t *size) {
    *data = NULL;
    *size = 0;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return false;
    size_t cap = 4096u, length = 0u;
    char *buffer = (char *)malloc(cap);
    while (buffer) {
        if (length == cap) {
            cap *= 2u;
            char *grown = cap <= MIGRATE_LIBRARY_MAX_BYTES + 1u ? (char *)realloc(buffer, cap) : NULL;
            if (!grown) {
                free(buffer);
                buffer = NULL;
                break;
            }
            buffer = grown;
        }
        size_t got = fread(buffer + length, 1u, cap - length, f);
        if (got == 0u) break;
        length += got;
    }
    bool failed = !buffer || ferror(f) != 0 || length > MIGRATE_LIBRARY_MAX_BYTES;
    fclose(f);
    if (failed) {
        free(buffer);
        return false;
    }
    *data = buffer;
    *size = length;
    return true;
}

static bool write_whole(const char *path, const char *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) return false;
    bool ok = (size == 0u || fwrite(data, 1u, size, f) == size) && fflush(f) == 0 &&
              nk_platform_sync_file(f);
    return (fclose(f) == 0) && ok;
}

/* Rewrites `"<from>/` to `"<to>/` inside library.json's strings, and a path that is exactly `<from>`.
   Returns false only on a failure that should undo the migration; a library with nothing to rewrite
   is fine. */
static bool rewrite_library(const NkDataMigrationPlan *plan, bool *rewritten, char *why,
                            size_t why_size) {
    *rewritten = false;
    if (!plan->remap_from[0] || !plan->remap_to[0] || !plan->library_file[0]) return true;
    if (nk_platform_path_missing(plan->library_file)) return true;
    if (!json_plain(plan->remap_from) || !json_plain(plan->remap_to)) return true;
    char *data = NULL;
    size_t size = 0;
    if (!read_whole(plan->library_file, &data, &size)) {
        snprintf(why, why_size, "the library file %s could not be read", plan->library_file);
        return false;
    }
    char needle[NK_MAX_PATH + 2], replacement[NK_MAX_PATH + 2];
    snprintf(needle, sizeof(needle), "\"%s", plan->remap_from);
    snprintf(replacement, sizeof(replacement), "\"%s", plan->remap_to);
    size_t needle_len = strlen(needle), replacement_len = strlen(replacement);
    size_t hits = 0;
    for (size_t i = 0; i + needle_len <= size; i++) {
        /* The old folder itself, or a path below it: the next character ends the string or is a
           separator, so "/data-old" is not mistaken for "/data". */
        if (memcmp(data + i, needle, needle_len) == 0 &&
            (i + needle_len == size || data[i + needle_len] == '"' || data[i + needle_len] == '/')) {
            hits++;
            i += needle_len - 1u;
        }
    }
    if (hits == 0u) {
        free(data);
        return true;
    }
    size_t out_cap = size + hits * (replacement_len > needle_len ? replacement_len - needle_len : 0u) + 1u;
    char *out = (char *)malloc(out_cap);
    if (!out) {
        free(data);
        snprintf(why, why_size, "out of memory while rewriting the library");
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; i < size; i++) {
        if (i + needle_len <= size && memcmp(data + i, needle, needle_len) == 0 &&
            (i + needle_len == size || data[i + needle_len] == '"' || data[i + needle_len] == '/')) {
            memcpy(out + o, replacement, replacement_len);
            o += replacement_len;
            i += needle_len - 1u;
        } else {
            out[o++] = data[i];
        }
    }
    char backup[NK_MAX_PATH], temp[NK_MAX_PATH];
    bool ok = snprintf(backup, sizeof(backup), "%s.pre-migration", plan->library_file) <
                  (int)sizeof(backup) &&
              snprintf(temp, sizeof(temp), "%s.migrating", plan->library_file) < (int)sizeof(temp);
    /* The original is kept once: a backup already there is the older, truer original. */
    if (ok && nk_platform_path_missing(backup)) ok = write_whole(backup, data, size);
    ok = ok && write_whole(temp, out, o) && nk_rename_utf8(temp, plan->library_file) == 0;
    if (!ok) {
        (void)nk_remove_utf8(temp);
        snprintf(why, why_size, "the library file %s could not be rewritten", plan->library_file);
    } else {
        *rewritten = true;
    }
    free(data);
    free(out);
    return ok;
}

/* ---- The run ---- */

typedef struct {
    bool active;
    bool clear_to; /* the destination is a tree of empty folders: remove it first */
} MoveState;

void nk_data_migrate_run(const NkDataMigrationPlan *plan, NkDataMigration *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    if (!plan || plan->move_count > NK_MIGRATE_MAX_MOVES) {
        result->status = NK_MIGRATE_FAILED;
        snprintf(result->message, sizeof(result->message), "%s: the migration plan is invalid.",
                 NK_MIGRATE_CODE_FAILED);
        return;
    }

    /* Check every move before the first one happens. */
    MoveState state[NK_MIGRATE_MAX_MOVES];
    memset(state, 0, sizeof(state));
    bool any = false;
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        bool previous_active = i > 0u && state[i - 1u].active;
        if (move->after_previous) {
            /* Its folder is normally made by the move before it. A run stopped between the two
               (power lost, the process killed) leaves that folder holding data and the earlier
               old folder gone, so it still moves then; else no later run would find it. */
            state[i].active = previous_active || tree_state(move->from, 0) == TREE_CONTENT;
        } else {
            state[i].active = tree_state(move->from, 0) == TREE_CONTENT;
        }
        if (!state[i].active) continue;
        any = true;
        TreeState to = tree_state(move->to, 0);
        if (to == TREE_CONTENT) {
            result->status = NK_MIGRATE_REFUSED;
            snprintf(result->message, sizeof(result->message),
                     "%s: Nakagawa found data in both %s and %s. It did not move or delete "
                     "anything. Keep the data you want in %s: move the other folder somewhere "
                     "outside Nakagawa's folders (or merge the two by hand), then start Nakagawa "
                     "again.",
                     NK_MIGRATE_CODE_BOTH_PRESENT, move->from, move->to, move->to);
            return;
        }
        state[i].clear_to = to == TREE_EMPTY;
    }
    if (!any) {
        /* A run stopped after its last move and before the library rewrite leaves stored paths
           into an old data folder that no longer exists; finish the rewrite. Nothing moved in
           this run, so a library that cannot be read or written is left to the library loader. */
        char unused[NK_MAX_PATH + 96];
        if (plan->remap_from[0] && nk_platform_path_missing(plan->remap_from) &&
            rewrite_library(plan, &result->library_rewritten, unused, sizeof(unused)) &&
            result->library_rewritten) {
            result->status = NK_MIGRATE_DONE;
            snprintf(result->message, sizeof(result->message),
                     "Nakagawa finished an earlier move of your data to %s: your library's stored "
                     "paths were updated (a copy of the old file is library.json.pre-migration).",
                     plan->new_root[0] ? plan->new_root : "its new folder");
            return;
        }
        result->library_rewritten = false;
        result->status = NK_MIGRATE_NOTHING;
        return;
    }

    if (plan->new_root[0] && !nk_platform_mkdir_p(plan->new_root)) {
        result->status = NK_MIGRATE_FAILED;
        snprintf(result->message, sizeof(result->message),
                 "%s: Nakagawa could not create %s, so it did not move anything. Check that the "
                 "folder can be written, then start Nakagawa again.",
                 NK_MIGRATE_CODE_FAILED, plan->new_root);
        return;
    }

    size_t done[NK_MIGRATE_MAX_MOVES];
    size_t done_count = 0;
    const NkMigrateMove *failed_move = NULL;
    for (size_t i = 0; i < plan->move_count && !failed_move; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (!state[i].active) continue;
        /* A move that depends on the one before it has nothing to do when that move made no such
           folder (an old data folder without saves). */
        if (move->after_previous && tree_state(move->from, 0) != TREE_CONTENT) {
            state[i].active = false;
            continue;
        }
        char parent[NK_MAX_PATH];
        bool ok = true;
        if (state[i].clear_to) ok = nk_platform_remove_tree(move->to, NULL);
        if (ok && parent_dir(move->to, parent, sizeof(parent))) ok = nk_platform_mkdir_p(parent);
        if (ok) ok = do_rename(move->from, move->to) == 0;
        if (!ok) {
            failed_move = move;
            break;
        }
        done[done_count++] = i;
    }

    char why[NK_MAX_PATH + 96] = "";
    bool library_ok = true;
    if (!failed_move) {
        library_ok = rewrite_library(plan, &result->library_rewritten, why, sizeof(why));
    }
    if (!failed_move && library_ok) {
        result->status = NK_MIGRATE_DONE;
        result->moved = done_count;
        snprintf(result->message, sizeof(result->message),
                 "Nakagawa moved your data to %s. The old folders are empty or gone; nothing was "
                 "deleted.%s",
                 plan->new_root[0] ? plan->new_root : "its new folder",
                 result->library_rewritten
                     ? " Your library's stored paths were updated (a copy of the old file is "
                       "library.json.pre-migration)."
                     : "");
        return;
    }

    /* Undo, newest first. */
    result->status = NK_MIGRATE_FAILED;
    size_t stuck = 0;
    char stuck_list[768] = "";
    for (size_t k = done_count; k > 0u; k--) {
        const NkMigrateMove *move = &plan->moves[done[k - 1u]];
        if (do_rename(move->to, move->from) != 0) {
            stuck++;
            size_t used = strlen(stuck_list);
            snprintf(stuck_list + used, sizeof(stuck_list) - used, " %s (should be %s);", move->to,
                     move->from);
        }
    }
    if (failed_move) {
        snprintf(result->message, sizeof(result->message),
                 "%s: Nakagawa could not move %s to %s (another disk, a folder in use, or a "
                 "folder that cannot be written). ",
                 NK_MIGRATE_CODE_FAILED, failed_move->from, failed_move->to);
    } else {
        snprintf(result->message, sizeof(result->message), "%s: %s. ", NK_MIGRATE_CODE_FAILED, why);
    }
    if (stuck == 0u) {
        say(result, "Everything it had already moved was put back, so your data is where it was. "
                    "Close other programs that use these folders and start Nakagawa again, or move "
                    "the folders by hand.");
    } else {
        say(result, "Putting some folders back failed, so they are still in the new place:%s "
                    "Nothing was deleted; move them back by hand, then start Nakagawa again.",
            stuck_list);
    }
}

void nk_data_migrate_host(NkDataMigration *result) {
    NkDataMigrationPlan plan;
    memset(&plan, 0, sizeof(plan));
    if (!nk_platform_user_data_plan(&plan)) {
        if (result) memset(result, 0, sizeof(*result));
        return;
    }
    nk_data_migrate_run(&plan, result);
}
