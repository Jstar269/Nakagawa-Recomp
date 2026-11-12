/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* One-time move of an older per-user folder into the unified one (#324). Rules: nk_data_migrate.h. */

#include "nk_data_migrate.h"

#include "nk_json.h"
#include "nk_library.h"
#include "nk_platform.h"
#include "nk_user_data_schema.h"

#include <stdarg.h>
#include <stdint.h>
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

/* A growing output buffer; `failed` is sticky. */
typedef struct {
    char *data;
    size_t length;
    size_t cap;
    bool failed;
} OutBuf;

static void out_put(OutBuf *b, const char *bytes, size_t n) {
    if (b->failed || n == 0u) return;
    if (b->length + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096u;
        while (cap < b->length + n) {
            if (cap > ((size_t)-1) / 2u) {
                b->failed = true;
                return;
            }
            cap *= 2u;
        }
        char *grown = (char *)realloc(b->data, cap);
        if (!grown) {
            b->failed = true;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->length, bytes, n);
    b->length += n;
}

/* `text` as JSON string content, escaped as nk_library.c writes its strings. */
static void out_put_escaped(OutBuf *b, const char *text, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        char escape[8];
        if (c == '"' || c == '\\') {
            escape[0] = '\\';
            escape[1] = (char)c;
            out_put(b, escape, 2u);
        } else if (c == '\n') {
            out_put(b, "\\n", 2u);
        } else if (c == '\r') {
            out_put(b, "\\r", 2u);
        } else if (c == '\t') {
            out_put(b, "\\t", 2u);
        } else if (c < 0x20u) {
            snprintf(escape, sizeof(escape), "\\u%04x", (unsigned)c);
            out_put(b, escape, 6u);
        } else {
            out_put(b, (const char *)&text[i], 1u);
        }
    }
}

/* Decodes the body of a JSON string literal (the bytes between its quotes) into `out`. False for a
   malformed escape or a value longer than `cap`; such a string is left as it is. */
static bool json_unescape(const char *body, size_t n, char *out, size_t cap, size_t *out_length) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (body[i] != '\\') {
            if (o >= cap) return false;
            out[o++] = body[i++];
            continue;
        }
        uint32_t cp = 0;
        size_t used = 0;
        if (nk_json_decode_escape(body + i + 1u, n - i - 1u, &cp, &used) != NULL) return false;
        char utf8[4];
        size_t bytes = nk_json_utf8_encode(cp, utf8);
        if (o + bytes > cap) return false;
        memcpy(out + o, utf8, bytes);
        o += bytes;
        i += 1u + used;
    }
    *out_length = o;
    return true;
}

/* Rewrites every JSON string in library.json whose value is the old data folder, or a path below
   it, to the same path below the new one. Each string is decoded before it is compared and the new
   value is escaped as the library writes it, so a folder name with a quote or a backslash is found
   and stays valid JSON; every other byte is kept. A library a newer Nakagawa wrote
   (schema_version above NK_LIBRARY_SCHEMA_VERSION) is refused by name, as the library writer
   refuses to replace one. Returns false only on a failure that should undo the migration; a
   library with nothing to rewrite is fine. */
static bool rewrite_library(const NkDataMigrationPlan *plan, bool *rewritten, char *why,
                            size_t why_size) {
    *rewritten = false;
    if (!plan->remap_from[0] || !plan->remap_to[0] || !plan->library_file[0]) return true;
    if (nk_platform_path_missing(plan->library_file)) return true;
    long version = nk_user_data_file_version(plan->library_file, MIGRATE_LIBRARY_MAX_BYTES);
    if (version > (long)NK_LIBRARY_SCHEMA_VERSION) {
        snprintf(why, why_size,
                 "the library file %s was written by a newer version of Nakagawa (schema_version "
                 "%ld), which this version cannot update",
                 plan->library_file, version);
        return false;
    }
    char *data = NULL;
    size_t size = 0;
    if (!read_whole(plan->library_file, &data, &size)) {
        snprintf(why, why_size, "the library file %s could not be read", plan->library_file);
        return false;
    }
    const size_t from_length = strlen(plan->remap_from);
    const size_t to_length = strlen(plan->remap_to);
    const char sep = nk_platform_path_separator();
    static char value[NK_MAX_PATH * 2u];
    static char renamed[NK_MAX_PATH * 2u];
    OutBuf out = {NULL, 0u, 0u, false};
    size_t hits = 0;
    bool too_long = false;
    for (size_t i = 0; i < size && !out.failed;) {
        if (data[i] != '"') {
            out_put(&out, data + i, 1u);
            i++;
            continue;
        }
        size_t end = i + 1u;
        while (end < size && data[end] != '"') end += data[end] == '\\' ? 2u : 1u;
        if (end >= size) { /* an unterminated string: keep the rest as it is */
            out_put(&out, data + i, size - i);
            break;
        }
        size_t value_length = 0;
        bool match = json_unescape(data + i + 1u, end - i - 1u, value, sizeof(value), &value_length) &&
                     value_length >= from_length &&
                     memcmp(value, plan->remap_from, from_length) == 0 &&
                     (value_length == from_length || value[from_length] == '/' ||
                      value[from_length] == sep);
        size_t renamed_length = match ? to_length + (value_length - from_length) : 0u;
        if (match && renamed_length >= NK_MAX_PATH) {
            too_long = true;
            match = false;
        }
        if (!match) {
            out_put(&out, data + i, end + 1u - i);
        } else {
            memcpy(renamed, plan->remap_to, to_length);
            memcpy(renamed + to_length, value + from_length, value_length - from_length);
            out_put(&out, "\"", 1u);
            out_put_escaped(&out, renamed, renamed_length);
            out_put(&out, "\"", 1u);
            hits++;
        }
        i = end + 1u;
    }
    if (out.failed || too_long) {
        free(data);
        free(out.data);
        snprintf(why, why_size,
                 too_long ? "a path in the library file %s would be too long in the new folder"
                          : "out of memory while rewriting the library file %s",
                 plan->library_file);
        return false;
    }
    if (hits == 0u) {
        free(data);
        free(out.data);
        return true;
    }
    char backup[NK_MAX_PATH], temp[NK_MAX_PATH];
    bool ok = snprintf(backup, sizeof(backup), "%s.pre-migration", plan->library_file) <
                  (int)sizeof(backup) &&
              snprintf(temp, sizeof(temp), "%s.migrating", plan->library_file) < (int)sizeof(temp);
    /* The original is kept once: a backup already there is the older, truer original. */
    if (ok && nk_platform_path_missing(backup)) ok = write_whole(backup, data, size);
    ok = ok && write_whole(temp, out.data, out.length) && nk_rename_utf8(temp, plan->library_file) == 0;
    if (!ok) {
        (void)nk_remove_utf8(temp);
        snprintf(why, why_size, "the library file %s could not be rewritten", plan->library_file);
    } else {
        *rewritten = true;
    }
    free(data);
    free(out.data);
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
        char unused[NK_MAX_PATH + 192];
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

    char why[NK_MAX_PATH + 192] = "";
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
