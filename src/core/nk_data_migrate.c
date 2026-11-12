/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* One-time move of an older per-user folder into the unified one (#324). Rules: nk_data_migrate.h. */

#if !defined(_WIN32) && !defined(_WIN64)
/* As in nk_platform_posix.c: realpath and readlink are XSI, flock is BSD (glibc declares it with
   _DEFAULT_SOURCE; macOS hides it under _POSIX_C_SOURCE unless _DARWIN_C_SOURCE is set). */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#endif

#include "nk_data_migrate.h"

#include "nk_json.h"
#include "nk_library.h"
#include "nk_platform.h"
#include "nk_user_data_schema.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NkMigrateRenameFn s_rename = NULL;

void nk_data_migrate_set_rename_for_test(NkMigrateRenameFn rename_fn) { s_rename = rename_fn; }

/* ---- Messages ---- */

static void append(char *buffer, size_t cap, const char *format, va_list args) {
    size_t used = strlen(buffer);
    if (used >= cap - 1u) return;
    (void)vsnprintf(buffer + used, cap - used, format, args);
}

static void say(NkDataMigration *r, const char *format, ...) {
    va_list args;
    va_start(args, format);
    append(r->message, sizeof(r->message), format, args);
    va_end(args);
}

/* ---- The host's plan and the notice ---- */

void nk_data_migrate_host(NkDataMigration *result) {
    if (!result) return;
    NkDataMigrationPlan plan;
    memset(&plan, 0, sizeof(plan));
    if (!nk_platform_user_data_plan(&plan)) {
        memset(result, 0, sizeof(*result));
        return;
    }
    nk_data_migrate_run(&plan, result);
    if (result->status == NK_MIGRATE_KEPT_OLD && !nk_platform_set_old_user_data_layout(true)) {
        /* Not reached on a host that has a plan; said rather than started on the wrong folders. */
        result->status = NK_MIGRATE_FAILED;
        say(result, " (%s: this build cannot use the earlier folders on this host.)",
            NK_MIGRATE_CODE_FAILED);
    }
}

bool nk_data_migrate_notice_is_new(const NkDataMigration *result) {
    if (!result || !result->message[0]) return false;
    char cache[NK_MAX_PATH];
    char path[NK_MAX_PATH + 32];
    if (!nk_platform_get_path(NK_PATH_CACHE, cache, sizeof(cache))) return true;
    int written = snprintf(path, sizeof(path), "%s%cdata-migration-notice.txt", cache,
                           nk_platform_path_separator());
    if (written <= 0 || (size_t)written >= sizeof(path)) return true;
    const size_t length = strlen(result->message);
    FILE *f = nk_fopen_utf8(path, "rb");
    if (f) {
        char *seen = (char *)malloc(length + 2u);
        size_t got = seen ? fread(seen, 1u, length + 1u, f) : 0u;
        bool same = seen && got == length && memcmp(seen, result->message, length) == 0;
        free(seen);
        fclose(f);
        if (same) return false;
    }
    FILE *w = nk_fopen_utf8(path, "wb");
    if (w) {
        (void)fwrite(result->message, 1u, length, w);
        (void)fclose(w);
    }
    return true;
}

#if defined(_WIN32) || defined(_WIN64)

/* Windows always used the unified layout (nk_platform_user_data_plan is empty there), so the
   engine, which relies on POSIX rename, rmdir, lstat and flock, is not built. */
void nk_data_migrate_run(const NkDataMigrationPlan *plan, NkDataMigration *result) {
    (void)plan;
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->status = NK_MIGRATE_FAILED;
    snprintf(result->message, sizeof(result->message),
             "%s: Windows has no earlier folder layout to move.", NK_MIGRATE_CODE_FAILED);
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* One named line for the log; the line ends in '\n'. */
static void log_line(NkDataMigration *r, const char *format, ...) {
    va_list args;
    va_start(args, format);
    append(r->log, sizeof(r->log), format, args);
    va_end(args);
    size_t used = strlen(r->log);
    if (used > 0u && used < sizeof(r->log) - 1u && r->log[used - 1u] != '\n') {
        r->log[used] = '\n';
        r->log[used + 1u] = '\0';
    }
}

#define MIGRATE_LIBRARY_MAX_BYTES (16u * 1024u * 1024u)
#define MIGRATE_WALK_DEPTH 24

static int do_rename(const char *from, const char *to) {
    errno = 0;
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
   this PC), or a folder that does. A folder that cannot be read counts as holding data, and so does
   a tree deeper than MIGRATE_WALK_DEPTH: the safe answer is the one that replaces nothing. */
static bool walk_entry(const char *name, bool is_dir, void *context) {
    Walk *walk = (Walk *)context;
    if (!is_dir) {
        walk->content = true;
        return false;
    }
    char child[NK_MAX_PATH];
    int written = snprintf(child, sizeof(child), "%s/%s", walk->dir, name);
    if (written <= 0 || (size_t)written >= sizeof(child)) {
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

typedef enum { ENTRY_ABSENT = 0, ENTRY_LINK, ENTRY_PRESENT } EntryKind;

/* The entry itself, not what a link points at. One that cannot be looked at counts as present. */
static EntryKind entry_kind(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) return S_ISLNK(st.st_mode) ? ENTRY_LINK : ENTRY_PRESENT;
    return (errno == ENOENT || errno == ENOTDIR) ? ENTRY_ABSENT : ENTRY_PRESENT;
}

/* A destination that is a link was put there on purpose: it counts as holding data. */
static TreeState dest_state(const char *path) {
    if (entry_kind(path) == ENTRY_LINK) return TREE_CONTENT;
    return tree_state(path, 0);
}

static bool parent_dir(const char *path, char *dir, size_t cap) {
    size_t length = strlen(path);
    while (length > 0u && path[length - 1u] != '/') length--;
    if (length <= 1u || length > cap) return false;
    memcpy(dir, path, length - 1u);
    dir[length - 1u] = '\0';
    return true;
}

static bool copy_path(char *out, size_t cap, const char *path) {
    int written = snprintf(out, cap, "%s", path);
    return written >= 0 && (size_t)written < cap;
}

/* ---- Comparing folders ---- */

/* `path` with repeated and trailing '/' and "." parts removed and ".." applied as text. */
static bool lexical_clean(const char *path, char *out, size_t cap) {
    if (cap < 2u) return false;
    const bool absolute = path[0] == '/';
    const size_t keep = absolute ? 1u : 0u;
    size_t o = 0;
    if (absolute) out[o++] = '/';
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 1u && start[0] == '.') continue;
        if (n == 2u && start[0] == '.' && start[1] == '.' && o > keep) {
            while (o > keep && out[o - 1u] != '/') o--;
            if (o > keep) o--;
            continue;
        }
        if (o > keep) {
            if (o + 1u >= cap) return false;
            out[o++] = '/';
        }
        if (n >= cap - o) return false;
        memcpy(out + o, start, n);
        o += n;
    }
    out[o] = '\0';
    return true;
}

/* `path` with its longest existing part resolved by realpath (links followed) and the rest kept as
   written, so two spellings of one folder compare equal. False for a path too long to compare. */
static bool canonical_path(const char *path, char *out, size_t cap) {
    char clean[NK_MAX_PATH];
    if (!lexical_clean(path, clean, sizeof(clean))) return false;
    size_t cut = strlen(clean);
    while (cut > 0u) {
        char prefix[NK_MAX_PATH];
        memcpy(prefix, clean, cut);
        prefix[cut] = '\0';
        char resolved[PATH_MAX];
        if (realpath(prefix, resolved)) {
            const char *tail = clean + cut;
            int written = strcmp(resolved, "/") == 0 && tail[0]
                              ? snprintf(out, cap, "%s", tail)
                              : snprintf(out, cap, "%s%s", resolved, tail);
            return written > 0 && (size_t)written < cap;
        }
        if (errno != ENOENT && errno != ENOTDIR) break; /* compare the text as written */
        while (cut > 0u && clean[cut - 1u] != '/') cut--;
        if (cut > 0u) cut--;
    }
    return copy_path(out, cap, clean);
}

static bool same_or_inside(const char *a, const char *b) {
    size_t n = strlen(b);
    if (strncmp(a, b, n) != 0) return false;
    if (n == 1u && b[0] == '/') return true;
    return a[n] == '\0' || a[n] == '/';
}

/* The same folder, or one inside the other. Paths too long to compare count as overlapping: the
   answer that moves nothing. */
static bool folders_overlap(const char *x, const char *y) {
    char a[NK_MAX_PATH], b[NK_MAX_PATH];
    if (!canonical_path(x, a, sizeof(a)) || !canonical_path(y, b, sizeof(b))) return true;
    return same_or_inside(a, b) || same_or_inside(b, a);
}

/* ---- Removing only empty folders ---- */

/* Removes every folder below `dir`, deepest first, each with rmdir, which the system refuses for a
   folder that holds anything. Stops with false (errno set) at the first entry that is not a folder;
   nothing but an empty folder is ever removed. `dir` itself stays: the rename that follows replaces
   it, and fails if anything has appeared in it by then. */
static bool clear_empty_below(const char *dir) {
    for (;;) {
        DIR *handle = opendir(dir);
        if (!handle) return false;
        char child[NK_MAX_PATH];
        bool found = false;
        int error = 0;
        for (;;) {
            errno = 0;
            struct dirent *item = readdir(handle);
            if (!item) {
                error = errno;
                break;
            }
            if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
            int written = snprintf(child, sizeof(child), "%s/%s", dir, item->d_name);
            struct stat st;
            if (written <= 0 || (size_t)written >= sizeof(child)) {
                error = ENAMETOOLONG;
            } else if (lstat(child, &st) != 0) {
                error = errno;
            } else if (!S_ISDIR(st.st_mode)) {
                error = ENOTEMPTY;
            } else {
                found = true;
            }
            break;
        }
        closedir(handle);
        if (error != 0) {
            errno = error;
            return false;
        }
        if (!found) return true;
        if (!clear_empty_below(child) || rmdir(child) != 0) return false;
    }
}

/* ---- The lock ---- */

static bool take_lock(const char *path, int *fd_out, int *error) {
    char dir[NK_MAX_PATH];
    *fd_out = -1;
    if (parent_dir(path, dir, sizeof(dir)) && !nk_platform_mkdir_p(dir)) {
        *error = errno ? errno : EACCES;
        return false;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        *error = errno;
        return false;
    }
    /* Waits for as long as another start holds it; the system drops a holder's lock when it ends. */
    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            *error = errno;
            close(fd);
            return false;
        }
    }
    *fd_out = fd;
    return true;
}

static void flush_parent(NkDataMigration *r, const char *path) {
    char dir[NK_MAX_PATH];
    if (parent_dir(path, dir, sizeof(dir)) && !nk_platform_sync_dir(dir)) {
        log_line(r, "%s: the folder %s could not be flushed to the disk after a move (%s).",
                 NK_MIGRATE_CODE_NOT_FLUSHED, dir, strerror(errno));
    }
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

/* Writes `path` whole or not at all: a temporary file beside it, synced, then renamed over it. */
static bool write_atomic(const char *path, const char *suffix, const char *data, size_t size) {
    char temp[NK_MAX_PATH];
    int written = snprintf(temp, sizeof(temp), "%s%s", path, suffix);
    if (written <= 0 || (size_t)written >= sizeof(temp)) return false;
    bool ok = write_whole(temp, data, size) && nk_rename_utf8(temp, path) == 0;
    if (!ok) {
        (void)nk_remove_utf8(temp);
        return false;
    }
    char dir[NK_MAX_PATH];
    return !parent_dir(path, dir, sizeof(dir)) || nk_platform_sync_dir(dir);
}

/* A backup that reads and parses whole; a cut-off one does not. */
static bool backup_is_whole(const char *path) {
    char *data = NULL;
    size_t size = 0;
    if (!read_whole(path, &data, &size)) return false;
    char err[64];
    NkJsonNode *root = nk_json_parse(data, size, err, sizeof(err));
    free(data);
    if (!root) return false;
    nk_json_free(root);
    return true;
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
    if (n > ((size_t)-1) - b->length) {
        b->failed = true;
        return;
    }
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
        if (bytes > cap - o) return false;
        memcpy(out + o, utf8, bytes);
        o += bytes;
        i += 1u + used;
    }
    *out_length = o;
    return true;
}

typedef enum { LIB_UNCHANGED = 0, LIB_REWRITTEN, LIB_NEWER, LIB_FAILED } LibResult;

/* Rewrites every JSON string in library.json (keys and titles included, though only paths match in
   practice) whose value is the old data folder, or a path below it, to the same path below the new
   one. Each string is decoded before it is compared and the new value is escaped as the library
   writes it, so a folder name with a quote or a backslash is found and stays valid JSON; every
   other byte is kept. A library a newer Nakagawa wrote (schema_version above
   NK_LIBRARY_SCHEMA_VERSION) is not touched: LIB_NEWER. */
static LibResult rewrite_library(const NkDataMigrationPlan *plan, char *why, size_t why_size) {
    if (!plan->remap_from[0] || !plan->remap_to[0] || !plan->library_file[0]) return LIB_UNCHANGED;
    if (nk_platform_path_missing(plan->library_file)) return LIB_UNCHANGED;
    long version = nk_user_data_file_version(plan->library_file, MIGRATE_LIBRARY_MAX_BYTES);
    if (version > (long)NK_LIBRARY_SCHEMA_VERSION) {
        snprintf(why, why_size,
                 "the library file %s was written by a newer version of Nakagawa (schema_version "
                 "%ld), which this version does not change",
                 plan->library_file, version);
        return LIB_NEWER;
    }
    char *data = NULL;
    size_t size = 0;
    if (!read_whole(plan->library_file, &data, &size)) {
        snprintf(why, why_size, "the library file %s could not be read", plan->library_file);
        return LIB_FAILED;
    }
    const size_t from_length = strlen(plan->remap_from);
    const size_t to_length = strlen(plan->remap_to);
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
                     (value_length == from_length || value[from_length] == '/');
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
        return LIB_FAILED;
    }
    if (hits == 0u) {
        free(data);
        free(out.data);
        return LIB_UNCHANGED;
    }
    char backup[NK_MAX_PATH];
    int written = snprintf(backup, sizeof(backup), "%s.pre-migration", plan->library_file);
    bool ok = written > 0 && (size_t)written < sizeof(backup);
    /* The original is kept once, written whole. A backup already there is the older original when
       it parses (a run that stopped before its rewrite left it); one that does not is replaced. */
    if (ok && (nk_platform_path_missing(backup) || !backup_is_whole(backup))) {
        ok = write_atomic(backup, ".tmp", data, size);
    }
    ok = ok && write_atomic(plan->library_file, ".migrating", out.data, out.length);
    free(data);
    free(out.data);
    if (!ok) {
        snprintf(why, why_size, "the library file %s could not be rewritten", plan->library_file);
        return LIB_FAILED;
    }
    return LIB_REWRITTEN;
}

/* ---- The run ---- */

typedef struct {
    bool active;
    bool clear_to;  /* the destination is a tree of empty folders */
    bool skipped;   /* left where it is (a cache or logs folder, or one that is also kept) */
    char now[NK_MAX_PATH]; /* where its folder is before this run moves anything */
} MoveState;

typedef struct {
    const char *code;
    char text[2048];
    char folder[NK_MAX_PATH];
} KeepReason;

static void keep_reason(KeepReason *reason, const char *code, const char *folder, const char *format,
                        ...) {
    if (reason->code) return; /* the first reason found is the one given */
    reason->code = code;
    (void)copy_path(reason->folder, sizeof(reason->folder), folder);
    va_list args;
    va_start(args, format);
    (void)vsnprintf(reason->text, sizeof(reason->text), format, args);
    va_end(args);
}

/* A destination of a folder that matters already holds data: the user uses the new folders. */
static bool new_layout_holds_data(const NkDataMigrationPlan *plan, const char **which) {
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (move->disposable) continue;
        if (dest_state(move->to) == TREE_CONTENT) {
            *which = move->to;
            return true;
        }
    }
    return false;
}

static const char *first_old_folder(const NkDataMigrationPlan *plan) {
    for (size_t i = 0; i < plan->move_count; i++) {
        if (!plan->moves[i].disposable && !plan->moves[i].after_previous) return plan->moves[i].from;
    }
    return "";
}

/* Nothing moves. The old folders stay in use unless the new ones hold data too, which leaves no
   complete layout to start on. */
static void keep_old(const NkDataMigrationPlan *plan, NkDataMigration *result,
                     const KeepReason *reason) {
    const char *taken = NULL;
    if (new_layout_holds_data(plan, &taken)) {
        result->status = NK_MIGRATE_REFUSED;
        (void)copy_path(result->folder_a, sizeof(result->folder_a), reason->folder);
        (void)copy_path(result->folder_b, sizeof(result->folder_b), taken);
        snprintf(result->message, sizeof(result->message),
                 "%s: %s Nakagawa cannot keep using the earlier folders either, because %s holds "
                 "data too. It did not move or delete anything. Keep the folder you want, move the "
                 "other one somewhere outside Nakagawa's folders, then start Nakagawa again.",
                 reason->code, reason->text, taken);
        return;
    }
    result->status = NK_MIGRATE_KEPT_OLD;
    snprintf(result->message, sizeof(result->message),
             "%s: %s Nakagawa moved nothing and changed nothing, and keeps using your earlier "
             "folders (your library and saves stay in %s). Nothing needs doing.",
             reason->code, reason->text, first_old_folder(plan));
}

static bool plan_is_valid(const NkDataMigrationPlan *plan) {
    if (!plan || plan->move_count > NK_MIGRATE_MAX_MOVES || plan->kept_count > NK_MIGRATE_MAX_KEPT) {
        return false;
    }
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (!move->from[0] || !move->to[0]) return false;
        if (!move->after_previous) continue;
        /* Its folder must lie inside the previous move's destination. */
        if (i == 0u || move->disposable || plan->moves[i - 1u].disposable) return false;
        const char *outer = plan->moves[i - 1u].to;
        size_t n = strlen(outer);
        if (strncmp(move->from, outer, n) != 0 || move->from[n] != '/') return false;
    }
    return true;
}

/* No old folder holds data. A run stopped after its last move and before the library rewrite
   leaves stored paths into an old data folder that no longer exists; finish the rewrite. */
static void finish_earlier_run(const NkDataMigrationPlan *plan, NkDataMigration *result) {
    result->status = NK_MIGRATE_NOTHING;
    if (!plan->remap_from[0] || !nk_platform_path_missing(plan->remap_from)) return;
    char why[NK_MAX_PATH + 192] = "";
    switch (rewrite_library(plan, why, sizeof(why))) {
    case LIB_REWRITTEN:
        result->status = NK_MIGRATE_DONE;
        result->library_rewritten = true;
        snprintf(result->message, sizeof(result->message),
                 "Nakagawa finished an earlier move of your data to %s: your library's stored "
                 "paths were updated (a copy of the old file is library.json.pre-migration).",
                 plan->new_root[0] ? plan->new_root : "its new folder");
        return;
    case LIB_UNCHANGED:
        return;
    case LIB_NEWER:
    case LIB_FAILED:
        log_line(result, "%s: %s; its stored paths into %s were left as they are.",
                 NK_MIGRATE_CODE_LIBRARY_NOT_UPDATED, why, plan->remap_from);
        return;
    }
}

static void run_locked(const NkDataMigrationPlan *plan, NkDataMigration *result) {
    MoveState state[NK_MIGRATE_MAX_MOVES];
    memset(state, 0, sizeof(state));
    KeepReason reason;
    memset(&reason, 0, sizeof(reason));
    const char *taken = NULL;
    const bool new_has_data = new_layout_holds_data(plan, &taken);

    /* A folder that matters which is, holds or lies in a folder this host keeps where it is (XDG
       homes that point at each other). Its content cannot be told apart from that folder's. */
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (move->disposable || move->after_previous) continue;
        for (size_t k = 0; k < plan->kept_count; k++) {
            if (!folders_overlap(move->from, plan->kept[k])) continue;
            if (new_has_data) {
                state[i].skipped = true;
                log_line(result,
                         "%s: %s is also Nakagawa's %s folder (%s), so it is not moved: %s already "
                         "holds your data.",
                         NK_MIGRATE_CODE_LEFT_BEHIND, move->from, plan->kept_label[k], plan->kept[k],
                         taken);
            } else {
                keep_reason(&reason, NK_MIGRATE_CODE_OVERLAP, move->from,
                            "Your earlier %s folder %s and Nakagawa's %s folder %s are the same "
                            "folder, or one is inside the other (XDG_DATA_HOME, XDG_CONFIG_HOME, "
                            "XDG_CACHE_HOME or XDG_STATE_HOME point at each other), so one cannot "
                            "be moved without the other.",
                            move->label, move->from, plan->kept_label[k], plan->kept[k]);
            }
            break;
        }
    }

    /* Where each folder is now, whether it holds data, and whether it is a link. */
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (state[i].skipped) continue;
        bool copied;
        if (move->after_previous && state[i - 1u].active) {
            /* Still inside the previous move's folder, which has not moved yet. */
            const char *rest = move->from + strlen(plan->moves[i - 1u].to);
            int written = snprintf(state[i].now, sizeof(state[i].now), "%s%s", state[i - 1u].now, rest);
            copied = written > 0 && (size_t)written < sizeof(state[i].now);
        } else {
            copied = copy_path(state[i].now, sizeof(state[i].now), move->from);
        }
        if (!copied) {
            result->status = NK_MIGRATE_FAILED;
            snprintf(result->message, sizeof(result->message),
                     "%s: the path of your %s folder (%s) is too long where it is now, so Nakagawa "
                     "did not move anything.",
                     NK_MIGRATE_CODE_FAILED, move->label, move->from);
            return;
        }
        state[i].active = tree_state(state[i].now, 0) == TREE_CONTENT;
        if (!state[i].active || entry_kind(state[i].now) != ENTRY_LINK) continue;
        if (move->disposable) {
            state[i].active = false;
            state[i].skipped = true;
            log_line(result, "%s: your %s folder %s is a link, so it is not moved; it stays where it is.",
                     NK_MIGRATE_CODE_LEFT_BEHIND, move->label, state[i].now);
            continue;
        }
        char target[NK_MAX_PATH] = "";
        ssize_t n = readlink(state[i].now, target, sizeof(target) - 1u);
        if (n > 0 && (size_t)n < sizeof(target)) target[n] = '\0';
        else snprintf(target, sizeof(target), "a place that cannot be read");
        keep_reason(&reason, NK_MIGRATE_CODE_LINKED, state[i].now,
                    "Your %s folder %s is a link to %s. Nakagawa does not move a linked folder: "
                    "moving the link can change where it points.",
                    move->label, state[i].now, target);
    }

    /* Two folders being moved that are the same folder, or one inside the other. */
    for (size_t i = 0; i < plan->move_count; i++) {
        if (!state[i].active) continue;
        for (size_t j = i + 1u; j < plan->move_count; j++) {
            if (!state[j].active) continue;
            if (plan->moves[j].after_previous && j == i + 1u) continue; /* inside it by design */
            if (!folders_overlap(state[i].now, state[j].now)) continue;
            const NkMigrateMove *a = &plan->moves[i];
            const NkMigrateMove *b = &plan->moves[j];
            if (a->disposable || b->disposable) {
                size_t d = a->disposable ? i : j;
                state[d].active = false;
                state[d].skipped = true;
                log_line(result,
                         "%s: your %s folder %s is, holds or lies in another folder being moved, so "
                         "it is not moved on its own; it stays where it is.",
                         NK_MIGRATE_CODE_LEFT_BEHIND, plan->moves[d].label, state[d].now);
                continue;
            }
            keep_reason(&reason, NK_MIGRATE_CODE_OVERLAP, state[i].now,
                        "Your earlier %s folder %s and %s folder %s are the same folder, or one is "
                        "inside the other (XDG_DATA_HOME and XDG_CONFIG_HOME point at each other), "
                        "so Nakagawa cannot tell which files belong where.",
                        a->label, state[i].now, b->label, state[j].now);
        }
    }

    /* A library a newer Nakagawa wrote: neither moved nor rewritten. */
    bool essential_active = false;
    for (size_t i = 0; i < plan->move_count; i++) {
        if (state[i].active && !plan->moves[i].disposable) essential_active = true;
    }
    if (essential_active && plan->old_library_file[0]) {
        long version = nk_user_data_file_version(plan->old_library_file, MIGRATE_LIBRARY_MAX_BYTES);
        if (version > (long)NK_LIBRARY_SCHEMA_VERSION) {
            keep_reason(&reason, NK_MIGRATE_CODE_NEWER_LIBRARY, first_old_folder(plan),
                        "Your library %s was written by a newer version of Nakagawa "
                        "(schema_version %ld). This version leaves it as it is, so the move of your "
                        "data into %s waits for a version that understands that library.",
                        plan->old_library_file, version, plan->new_root);
        }
    }
    if (reason.code) {
        keep_old(plan, result, &reason);
        return;
    }

    /* Every destination is checked before the first move. */
    bool any = false;
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (!state[i].active) continue;
        TreeState to = dest_state(move->to);
        if (to == TREE_CONTENT) {
            if (move->disposable) {
                state[i].active = false;
                state[i].skipped = true;
                log_line(result,
                         "%s: your old %s folder %s was not moved, because %s already holds files; "
                         "it stays where it is and can be deleted.",
                         NK_MIGRATE_CODE_LEFT_BEHIND, move->label, state[i].now, move->to);
                continue;
            }
            result->status = NK_MIGRATE_REFUSED;
            (void)copy_path(result->folder_a, sizeof(result->folder_a), state[i].now);
            (void)copy_path(result->folder_b, sizeof(result->folder_b), move->to);
            snprintf(result->message, sizeof(result->message),
                     "%s: Nakagawa found your %s in both %s and %s. It did not move or delete "
                     "anything. Keep the data you want in %s: move the other folder somewhere "
                     "outside Nakagawa's folders (or merge the two by hand), then start Nakagawa "
                     "again.",
                     NK_MIGRATE_CODE_BOTH_PRESENT, move->label, state[i].now, move->to, move->to);
            return;
        }
        state[i].clear_to = to == TREE_EMPTY;
        any = true;
    }
    if (!any) {
        finish_earlier_run(plan, result);
        return;
    }

    if (plan->new_root[0] && !nk_platform_mkdir_p(plan->new_root)) {
        result->status = NK_MIGRATE_FAILED;
        (void)copy_path(result->folder_a, sizeof(result->folder_a), first_old_folder(plan));
        (void)copy_path(result->folder_b, sizeof(result->folder_b), plan->new_root);
        snprintf(result->message, sizeof(result->message),
                 "%s: Nakagawa could not create %s, so it did not move anything. Check that the "
                 "folder can be written, then start Nakagawa again.",
                 NK_MIGRATE_CODE_FAILED, plan->new_root);
        return;
    }

    size_t done[NK_MIGRATE_MAX_MOVES];
    size_t done_count = 0;
    const NkMigrateMove *failed_move = NULL;
    int failed_error = 0;
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (!state[i].active) continue;
        /* A move that depends on the one before it has nothing to do when that move brought no
           such folder (an old data folder without saves). */
        if (move->after_previous && tree_state(move->from, 0) != TREE_CONTENT) {
            state[i].active = false;
            continue;
        }
        char parent[NK_MAX_PATH];
        bool ok = true;
        int error = 0;
        if (state[i].clear_to && !clear_empty_below(move->to)) {
            ok = false;
            error = errno;
        }
        if (ok && parent_dir(move->to, parent, sizeof(parent)) && !nk_platform_mkdir_p(parent)) {
            ok = false;
            error = errno;
        }
        if (ok && do_rename(move->from, move->to) != 0) {
            ok = false;
            error = errno;
        }
        if (!ok) {
            if (move->disposable) {
                state[i].active = false;
                state[i].skipped = true;
                log_line(result,
                         "%s: your old %s folder %s could not be moved to %s (%s); it stays where it "
                         "is and can be deleted.",
                         NK_MIGRATE_CODE_LEFT_BEHIND, move->label, move->from, move->to,
                         error ? strerror(error) : "unknown error");
                continue;
            }
            failed_move = move;
            failed_error = error;
            break;
        }
        flush_parent(result, move->to);
        flush_parent(result, move->from);
        done[done_count++] = i;
    }

    char why[NK_MAX_PATH + 192] = "";
    LibResult library = LIB_UNCHANGED;
    if (!failed_move) library = rewrite_library(plan, why, sizeof(why));
    if (!failed_move && (library == LIB_UNCHANGED || library == LIB_REWRITTEN)) {
        result->status = NK_MIGRATE_DONE;
        result->moved = done_count;
        result->library_rewritten = library == LIB_REWRITTEN;
        snprintf(result->message, sizeof(result->message),
                 "Nakagawa moved your data to %s. Nothing was deleted.%s",
                 plan->new_root[0] ? plan->new_root : "its new folder",
                 result->library_rewritten
                     ? " Your library's stored paths were updated (a copy of the old file is "
                       "library.json.pre-migration)."
                     : "");
        return;
    }

    /* Undo, newest first. A dependent move whose undo fails stays inside the folder it was moved
       into, so that folder is not put back either. */
    bool back[NK_MIGRATE_MAX_MOVES];
    memset(back, 0, sizeof(back));
    bool all_back = true;
    bool hold = false;
    size_t hold_index = 0;
    for (size_t k = done_count; k > 0u; k--) {
        size_t index = done[k - 1u];
        const NkMigrateMove *move = &plan->moves[index];
        if (hold && index == hold_index) {
            hold = false;
            all_back = false;
            continue;
        }
        if (do_rename(move->to, move->from) == 0) {
            back[k - 1u] = true;
            flush_parent(result, move->from);
            flush_parent(result, move->to);
        } else {
            all_back = false;
            if (move->after_previous) {
                hold = true;
                hold_index = index - 1u;
            }
        }
    }

    if (!failed_move && library == LIB_NEWER && all_back) {
        /* The library changed under the run: as for one found before it. */
        KeepReason newer;
        memset(&newer, 0, sizeof(newer));
        keep_reason(&newer, NK_MIGRATE_CODE_NEWER_LIBRARY, first_old_folder(plan), "%s.", why);
        keep_old(plan, result, &newer);
        return;
    }

    result->status = NK_MIGRATE_FAILED;
    if (failed_move) {
        (void)copy_path(result->folder_a, sizeof(result->folder_a), failed_move->from);
        (void)copy_path(result->folder_b, sizeof(result->folder_b), failed_move->to);
        if (failed_error == EXDEV) {
            snprintf(result->message, sizeof(result->message),
                     "%s: Nakagawa could not move your %s folder %s to %s: the two are on different "
                     "disks (or file systems), and Nakagawa does not copy your library, saves or "
                     "settings between disks. ",
                     NK_MIGRATE_CODE_OTHER_DISK, failed_move->label, failed_move->from,
                     failed_move->to);
        } else {
            snprintf(result->message, sizeof(result->message),
                     "%s: Nakagawa could not move your %s folder %s to %s (%s). ",
                     NK_MIGRATE_CODE_FAILED, failed_move->label, failed_move->from, failed_move->to,
                     failed_error ? strerror(failed_error) : "unknown error");
        }
    } else {
        (void)copy_path(result->folder_a, sizeof(result->folder_a), plan->remap_from);
        (void)copy_path(result->folder_b, sizeof(result->folder_b), plan->remap_to);
        snprintf(result->message, sizeof(result->message), "%s: %s. ", NK_MIGRATE_CODE_FAILED, why);
    }
    if (all_back) {
        say(result, "Everything it had moved is back where it was, and nothing was deleted. ");
        if (failed_move) {
            say(result,
                "To use the new folder, move what is in %s into %s yourself (the folders can be "
                "opened from this message), then start Nakagawa again.",
                failed_move->from, failed_move->to);
        } else {
            say(result, "Start Nakagawa again once the library file can be written.");
        }
        return;
    }
    say(result, "Some folders could not be put back. Where each one is now:");
    for (size_t k = 0; k < done_count; k++) {
        const NkMigrateMove *move = &plan->moves[done[k]];
        if (back[k]) {
            say(result, " your %s folder is back in %s;", move->label, move->from);
        } else {
            say(result, " your %s folder is %s (it was %s);", move->label, move->to, move->from);
        }
    }
    say(result, " Nothing was deleted. Do not move these folders by hand: start Nakagawa again, and "
                "it carries on from where it stopped, or names what is in the way.");
}

static void lock_failed(const NkDataMigrationPlan *plan, NkDataMigration *result, int error) {
    bool any_old = false;
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (!move->disposable && !move->after_previous && entry_kind(move->from) != ENTRY_ABSENT) {
            any_old = true;
        }
    }
    if (!any_old) {
        result->status = NK_MIGRATE_NOTHING;
        log_line(result,
                 "%s: Nakagawa could not lock %s (%s); there is no earlier folder to move, and the "
                 "library was not checked for stored paths to update.",
                 NK_MIGRATE_CODE_LOCK, plan->lock_file, strerror(error));
        return;
    }
    KeepReason reason;
    memset(&reason, 0, sizeof(reason));
    keep_reason(&reason, NK_MIGRATE_CODE_LOCK, first_old_folder(plan),
                "Nakagawa could not lock %s (%s), which keeps two starts from moving your data at "
                "once, so it did not move your data.",
                plan->lock_file, strerror(error));
    keep_old(plan, result, &reason);
}

void nk_data_migrate_run(const NkDataMigrationPlan *plan, NkDataMigration *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    if (!plan_is_valid(plan)) {
        result->status = NK_MIGRATE_FAILED;
        snprintf(result->message, sizeof(result->message), "%s: the migration plan is invalid.",
                 NK_MIGRATE_CODE_FAILED);
        return;
    }
    if (plan->notes[0]) log_line(result, "%s", plan->notes);
    int lock_fd = -1;
    if (plan->lock_file[0]) {
        int error = 0;
        if (!take_lock(plan->lock_file, &lock_fd, &error)) {
            lock_failed(plan, result, error);
            return;
        }
    }
    run_locked(plan, result);
    if (lock_fd >= 0) {
        (void)flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

#endif
