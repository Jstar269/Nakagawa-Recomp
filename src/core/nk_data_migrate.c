/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Which per-user folders a Linux or macOS install uses, and the move the user can ask for (#324).
   Rules: nk_data_migrate.h. */

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
static NkMigrateSyncDirFn s_sync_dir = NULL;
static NkMigratePointFn s_point = NULL;
static long s_euid_for_test = -1;

void nk_data_migrate_set_rename_for_test(NkMigrateRenameFn rename_fn) { s_rename = rename_fn; }
void nk_data_migrate_set_sync_dir_for_test(NkMigrateSyncDirFn sync_fn) { s_sync_dir = sync_fn; }
void nk_data_migrate_set_euid_for_test(long euid) { s_euid_for_test = euid; }
void nk_data_migrate_set_point_hook_for_test(NkMigratePointFn hook) { s_point = hook; }

/* ---- The host ---- */

void nk_data_layout_host(NkLayoutDecision *out) {
    if (!out) return;
    static NkDataMigrationPlan plan;
    memset(&plan, 0, sizeof(plan));
    if (!nk_platform_user_data_plan(&plan)) {
        /* Windows and Android: no older layout, nothing to decide. */
        memset(out, 0, sizeof(*out));
        out->layout = NK_LAYOUT_NEW;
        out->rule = "none";
        return;
    }
    nk_data_layout_decide(&plan, out);
    if (out->layout == NK_LAYOUT_OLD && !nk_platform_set_old_user_data_layout(true)) {
        /* Not reached on a host that has a plan; said rather than started on the wrong folders. */
        out->layout = NK_LAYOUT_BLOCKED;
        snprintf(out->message, sizeof(out->message),
                 "%s: this build cannot use the earlier folders on this host.", NK_MIGRATE_CODE_FAILED);
    }
}

#if defined(_WIN32) || defined(_WIN64)

/* Windows always used the unified layout (nk_platform_user_data_plan is empty there), so the rule
   and the move, which rely on POSIX rename, rmdir, lstat and flock, are not built. */
void nk_data_layout_decide(const NkDataMigrationPlan *plan, NkLayoutDecision *out) {
    (void)plan;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->layout = NK_LAYOUT_NEW;
    out->rule = "none";
}

void nk_data_migrate_release_player_lock(void) {}

void nk_data_migrate_move(const NkDataMigrationPlan *plan, bool game_running, NkDataMigration *result) {
    (void)plan;
    (void)game_running;
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->status = NK_MIGRATE_NOTHING;
    snprintf(result->message, sizeof(result->message), "Windows has no earlier folder layout to move.");
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- Messages ---- */

static void append_v(char *buffer, size_t cap, const char *format, va_list args) {
    size_t used = strlen(buffer);
    if (used >= cap - 1u) return;
    (void)vsnprintf(buffer + used, cap - used, format, args);
}

static void add(char *buffer, size_t cap, const char *format, ...) {
    va_list args;
    va_start(args, format);
    append_v(buffer, cap, format, args);
    va_end(args);
}

#define MIGRATE_LOG_CAP 2048u
/* Both results carry a log of this size; the engine writes either through log_line. */
typedef char migrate_log_sizes_match[(sizeof(((NkDataMigration *)0)->log) == MIGRATE_LOG_CAP &&
                                      sizeof(((NkLayoutDecision *)0)->log) == MIGRATE_LOG_CAP)
                                         ? 1
                                         : -1];

/* One named line for the log; the line ends in '\n'. */
static void log_line(char *log, const char *format, ...) {
    va_list args;
    va_start(args, format);
    append_v(log, MIGRATE_LOG_CAP, format, args);
    va_end(args);
    size_t used = strlen(log);
    if (used > 0u && used < MIGRATE_LOG_CAP - 1u && log[used - 1u] != '\n') {
        log[used] = '\n';
        log[used + 1u] = '\0';
    }
}

#define MIGRATE_LIBRARY_MAX_BYTES (16u * 1024u * 1024u)
#define MIGRATE_JOURNAL_MAX_BYTES (64u * 1024u)
#define MIGRATE_WALK_DEPTH 24
/* The relative-link check walks every folder that moves; deeper than this, it cannot rule a
   link out, and the move is refused. */
#define MIGRATE_LINK_SCAN_DEPTH 64

/* The player lock this process holds shared while it runs (nk_data_layout_decide). */
static int s_player_fd = -1;

void nk_data_migrate_release_player_lock(void) {
    if (s_player_fd >= 0) {
        close(s_player_fd);
        s_player_fd = -1;
    }
}

static void point(const char *what, size_t n) {
    if (!s_point) return;
    char name[48];
    if (n == 0u) snprintf(name, sizeof(name), "%s", what);
    else snprintf(name, sizeof(name), "%s:%zu", what, n);
    s_point(name);
}

static int do_rename(const char *from, const char *to) {
    errno = 0;
    return s_rename ? s_rename(from, to) : nk_rename_utf8(from, to);
}

static bool do_sync_dir(const char *dir) {
    errno = 0;
    return s_sync_dir ? s_sync_dir(dir) : nk_platform_sync_dir(dir);
}

static unsigned long effective_uid(void) {
    return s_euid_for_test >= 0 ? (unsigned long)s_euid_for_test : (unsigned long)geteuid();
}

/* ---- What a folder holds ---- */

typedef enum { TREE_ABSENT = 0, TREE_EMPTY, TREE_CONTENT } TreeState;

typedef struct {
    const char *dir;
    int depth;
    bool content;
} Walk;

static TreeState tree_state(const char *path, int depth);

/* A folder holds files when it has any file, any entry the listing skips (a link or a file kept off
   this PC), or a folder that does. A folder that cannot be read counts as holding files, and so does
   a tree deeper than MIGRATE_WALK_DEPTH: the safe answer is the one that replaces nothing.
   tools/nk_core/prereq_fetcher.py (_tree_state) reads folders the same way. */
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

static const char *state_words(const char *path) {
    switch (tree_state(path, 0)) {
    case TREE_ABSENT:
        return "does not exist";
    case TREE_EMPTY:
        return "is empty";
    case TREE_CONTENT:
        break;
    }
    return "holds files";
}

typedef enum { ENTRY_ABSENT = 0, ENTRY_LINK, ENTRY_PRESENT } EntryKind;

/* The entry itself, not what a link points at. One that cannot be looked at counts as present. */
static EntryKind entry_kind(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) return S_ISLNK(st.st_mode) ? ENTRY_LINK : ENTRY_PRESENT;
    return (errno == ENOENT || errno == ENOTDIR) ? ENTRY_ABSENT : ENTRY_PRESENT;
}

/* A destination that is a link was put there on purpose: it counts as holding files. */
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

static bool is_real_dir(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The device of `path`, or of the nearest folder above it that exists (where it would be made). */
static bool device_of(const char *path, dev_t *device) {
    char at[NK_MAX_PATH];
    if (!copy_path(at, sizeof(at), path)) return false;
    for (;;) {
        struct stat st;
        if (lstat(at, &st) == 0) {
            *device = st.st_dev;
            return true;
        }
        if (errno != ENOENT && errno != ENOTDIR) return false;
        char *cut = strrchr(at, '/');
        if (!cut) return false;
        if (cut == at) {
            at[1] = '\0';
        } else {
            *cut = '\0';
        }
    }
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

/* ---- Locks ---- */

static int open_lock_file(const char *path, int *error) {
    char dir[NK_MAX_PATH];
    if (parent_dir(path, dir, sizeof(dir)) && !nk_platform_mkdir_p(dir)) {
        *error = errno ? errno : EACCES;
        return -1;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) *error = errno;
    return fd;
}

static bool lock_now(int fd, int operation) {
    for (;;) {
        if (flock(fd, operation | LOCK_NB) == 0) return true;
        if (errno != EINTR) return false;
    }
}

/* The run lock: one decision or move at a time. Waits for as long as another holds it (the system
   drops a holder's lock when it ends); said first, so a wait is not a silent one. */
static bool take_run_lock(const char *path, int *fd_out, int *error) {
    *fd_out = -1;
    int fd = open_lock_file(path, error);
    if (fd < 0) return false;
    if (!lock_now(fd, LOCK_EX)) {
        if (errno != EWOULDBLOCK) {
            *error = errno;
            close(fd);
            return false;
        }
        fprintf(stderr, "[nk] %s: another Nakagawa start is deciding or moving the data folders; "
                        "waiting for it to finish (%s).\n", NK_MIGRATE_CODE_LOCK, path);
        while (flock(fd, LOCK_EX) != 0) {
            if (errno != EINTR) {
                *error = errno;
                close(fd);
                return false;
            }
        }
    }
    *fd_out = fd;
    return true;
}

static void release_lock(int fd) {
    if (fd < 0) return;
    (void)flock(fd, LOCK_UN);
    close(fd);
}

/* The player lock exclusively, on an open file of its own: a flock held through another open file,
   this process's shared one included, conflicts with it, so the caller gives that one up first.
   -1, with `error` (EWOULDBLOCK when another process holds it), on failure. */
static int player_lock_exclusive(const char *path, int *error) {
    int fd = open_lock_file(path, error);
    if (fd < 0) return -1;
    if (!lock_now(fd, LOCK_EX)) {
        *error = errno;
        close(fd);
        return -1;
    }
    return fd;
}

/* This process holds the player lock shared from here on. Taken under the run lock, which every
   exclusive holder also holds, so it does not wait. */
static bool player_lock_shared(const char *path) {
    nk_data_migrate_release_player_lock();
    int error = 0;
    int fd = open_lock_file(path, &error);
    if (fd < 0) return false;
    if (!lock_now(fd, LOCK_SH)) {
        close(fd);
        return false;
    }
    s_player_fd = fd;
    return true;
}

static void flush_parent(char *log, const char *path) {
    char dir[NK_MAX_PATH];
    if (parent_dir(path, dir, sizeof(dir)) && !nk_platform_path_missing(dir) && !do_sync_dir(dir)) {
        log_line(log, "%s: the folder %s could not be flushed to the disk after a rename (%s).",
                 NK_MIGRATE_CODE_NOT_FLUSHED, dir, strerror(errno));
    }
}

/* Another open file holds an exclusive flock on `path`. Probed with a shared lock, released at
   once. */
static bool flock_held(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    bool held = !lock_now(fd, LOCK_SH) && errno == EWOULDBLOCK;
    close(fd);
    return held;
}

/* Another process holds an fcntl lock on `path` (the staging lock). */
static bool fcntl_lock_held(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct flock region;
    memset(&region, 0, sizeof(region));
    region.l_type = F_WRLCK;
    region.l_whence = SEEK_SET;
    bool held = fcntl(fd, F_GETLK, &region) == 0 && region.l_type != F_UNLCK;
    close(fd);
    return held;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* The locks this version and the one before #324 take in the data folder while they write:
   locks/<name>.lock (flock: the library and package builds) and games/.staging_<disc>.lock (fcntl).
   True, with the lock named in `what`, when one is held. */
static bool data_locks_held(const char *data_dir, char *what, size_t cap) {
    static const char *const folders[] = {"locks", "games"};
    for (size_t f = 0; f < 2u; f++) {
        char dir[NK_MAX_PATH];
        int written = snprintf(dir, sizeof(dir), "%s/%s", data_dir, folders[f]);
        if (written <= 0 || (size_t)written >= sizeof(dir)) continue;
        DIR *handle = opendir(dir);
        if (!handle) continue;
        bool held = false;
        for (struct dirent *item = readdir(handle); item && !held; item = readdir(handle)) {
            const char *name = item->d_name;
            bool staging = f == 1u && strncmp(name, ".staging_", 9) == 0 && ends_with(name, ".lock");
            if (!(f == 0u && ends_with(name, ".lock")) && !staging) continue;
            char path[NK_MAX_PATH];
            written = snprintf(path, sizeof(path), "%s/%s", dir, name);
            if (written <= 0 || (size_t)written >= sizeof(path)) continue;
            if (staging ? fcntl_lock_held(path) : flock_held(path)) {
                held = true;
                snprintf(what, cap, "the lock %s is held by another process", path);
            }
        }
        closedir(handle);
        if (held) return true;
    }
    return false;
}

/* The library's own lock (locks/library.lock beside it), taken through the library's resolved path:
   nk_library_lock_acquire refuses a lock folder reached through a link, and a lock is the same
   file however its path is spelled. On failure, `why` says whether another process holds it or it
   could not be taken at all. */
static bool lock_library(NkFileLock *lock, const char *library_file, char *why, size_t why_size) {
    memset(lock, 0, sizeof(*lock));
    char resolved[NK_MAX_PATH], dir[NK_MAX_PATH] = "", lock_path[NK_MAX_PATH + 32];
    if (!canonical_path(library_file, resolved, sizeof(resolved))) {
        snprintf(why, why_size, "the library lock beside %s could not be located", library_file);
        return false;
    }
    errno = 0;
    if (nk_library_lock_acquire(lock, resolved, 0u) == NK_OK) return true;
    int saved = errno;
    if (parent_dir(resolved, dir, sizeof(dir)) &&
        (size_t)snprintf(lock_path, sizeof(lock_path), "%s/locks/library.lock", dir) < sizeof(lock_path) &&
        flock_held(lock_path)) {
        snprintf(why, why_size, "the library lock %s is held by another process", lock_path);
    } else {
        snprintf(why, why_size, "the library lock in %s/locks could not be taken (%s)", dir,
                 saved ? strerror(saved) : "unknown error");
    }
    return false;
}

/* ---- Relative links that would point elsewhere after a move ---- */

typedef struct {
    char path[PATH_MAX];      /* the folder being walked; grows and shrinks as the walk goes */
    const char *nested[NK_MIGRATE_MAX_MOVES]; /* folders inside it that move elsewhere */
    size_t nested_count;
    char found[PATH_MAX];     /* the first offending link, or the folder that could not be read */
    char target[PATH_MAX];
    bool unreadable;
} LinkScan;

/* Whether the relative `target` of a link in the folder path[0, dir_length) (`depth` levels below
   the moving folder) stays inside the moving folder all the way and does not enter a nested folder
   that moves elsewhere. Lexical: the walk never follows links, so that folder has no link in it
   below the moving one. */
static bool relative_link_stays(const LinkScan *scan, size_t dir_length, int depth, const char *target) {
    static char at[PATH_MAX];
    if (dir_length >= sizeof(at)) return false;
    memcpy(at, scan->path, dir_length);
    at[dir_length] = '\0';
    const char *p = target;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 1u && start[0] == '.') continue;
        if (n == 2u && start[0] == '.' && start[1] == '.') {
            if (--depth < 0) return false; /* leaves the moving folder */
            char *cut = strrchr(at, '/');
            if (!cut) return false;
            *cut = '\0';
        } else {
            size_t used = strlen(at);
            if (used + 1u + n >= sizeof(at)) return false;
            at[used] = '/';
            memcpy(at + used + 1u, start, n);
            at[used + 1u + n] = '\0';
            depth++;
        }
        for (size_t i = 0; i < scan->nested_count; i++) {
            if (same_or_inside(at, scan->nested[i])) return false;
        }
    }
    return true;
}

/* False, with found set, at the first relative link below scan->path that would point elsewhere
   after the move, or at a folder that cannot be read or is too deep to check. */
static bool links_stay(LinkScan *scan, int depth) {
    if (depth > MIGRATE_LINK_SCAN_DEPTH) {
        scan->unreadable = true;
        (void)copy_path(scan->found, sizeof(scan->found), scan->path);
        return false;
    }
    DIR *handle = opendir(scan->path);
    if (!handle) {
        scan->unreadable = true;
        (void)copy_path(scan->found, sizeof(scan->found), scan->path);
        return false;
    }
    bool ok = true;
    const size_t used = strlen(scan->path);
    for (struct dirent *item = readdir(handle); item && ok; item = readdir(handle)) {
        const char *name = item->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        size_t n = strlen(name);
        if (used + 1u + n >= sizeof(scan->path)) {
            scan->unreadable = true;
            (void)copy_path(scan->found, sizeof(scan->found), scan->path);
            ok = false;
            break;
        }
        scan->path[used] = '/';
        memcpy(scan->path + used + 1u, name, n + 1u);
        struct stat st;
        if (lstat(scan->path, &st) != 0) {
            scan->unreadable = true;
            (void)copy_path(scan->found, sizeof(scan->found), scan->path);
            ok = false;
        } else if (S_ISLNK(st.st_mode)) {
            ssize_t got = readlink(scan->path, scan->target, sizeof(scan->target) - 1u);
            if (got <= 0 || (size_t)got >= sizeof(scan->target) - 1u) {
                scan->unreadable = true;
                (void)copy_path(scan->found, sizeof(scan->found), scan->path);
                ok = false;
            } else {
                scan->target[got] = '\0';
                if (scan->target[0] != '/' && !relative_link_stays(scan, used, depth, scan->target)) {
                    (void)copy_path(scan->found, sizeof(scan->found), scan->path);
                    ok = false;
                }
            }
        } else if (S_ISDIR(st.st_mode)) {
            bool nested = false;
            for (size_t i = 0; i < scan->nested_count; i++) {
                if (strcmp(scan->path, scan->nested[i]) == 0) nested = true;
            }
            if (!nested) ok = links_stay(scan, depth + 1);
        }
        if (ok) scan->path[used] = '\0';
    }
    closedir(handle);
    return ok;
}

/* ---- Files ---- */

static bool read_whole(const char *path, size_t max_bytes, char **data, size_t *size) {
    *data = NULL;
    *size = 0;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return false;
    size_t cap = 4096u, length = 0u;
    char *buffer = (char *)malloc(cap);
    while (buffer) {
        if (length == cap) {
            cap *= 2u;
            char *grown = cap <= max_bytes + 1u ? (char *)realloc(buffer, cap) : NULL;
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
    bool failed = !buffer || ferror(f) != 0 || length > max_bytes;
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

/* Writes `path` whole or not at all: a temporary file beside it, synced, then renamed over it. Once
   the rename has happened the new file is the one in place: a folder flush that fails after it
   (durability unknown) is a named log line, not a failure. */
static bool write_atomic(char *log, const char *path, const char *data, size_t size) {
    char temp[NK_MAX_PATH + 8];
    int written = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (written <= 0 || (size_t)written >= sizeof(temp)) return false;
    bool ok = write_whole(temp, data, size) && nk_rename_utf8(temp, path) == 0;
    if (!ok) {
        (void)nk_remove_utf8(temp);
        return false;
    }
    char dir[NK_MAX_PATH];
    if (parent_dir(path, dir, sizeof(dir)) && !do_sync_dir(dir)) {
        log_line(log, "%s: the folder %s could not be flushed to the disk after %s was written (%s); "
                      "the file is in place.",
                 NK_MIGRATE_CODE_NOT_FLUSHED, dir, path, strerror(errno));
    }
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

static void out_text(OutBuf *b, const char *text) { out_put(b, text, strlen(text)); }

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

/* ---- library.json ---- */

typedef enum { LIB_UNCHANGED = 0, LIB_REWRITTEN, LIB_NEWER, LIB_FAILED } LibResult;

/* A folder in the two spellings a stored path is compared in, lexically cleaned and with links
   resolved (taken before the first rename, while the folder exists), and the folder that replaces
   it. */
typedef struct {
    bool active;
    char lexical[NK_MAX_PATH];
    char resolved[NK_MAX_PATH];
    char to[NK_MAX_PATH];
} Remap;

static void remap_activate(Remap *m) {
    /* A folder at the top of the tree would match every path: never one Nakagawa used. */
    m->active = strlen(m->lexical) > 1u && strlen(m->resolved) > 1u && strlen(m->to) > 1u &&
                m->lexical[0] == '/' && m->resolved[0] == '/' && m->to[0] == '/';
}

/* The part of the absolute path `value` below the folder ("" for the folder itself, else from its
   '/'), or NULL when `value` is not there. `scratch` holds the spelling that matched. */
static const char *remap_rest(const Remap *m, const char *value, char *scratch, size_t cap) {
    if (!m->active || value[0] != '/') return NULL;
    if (lexical_clean(value, scratch, cap) && same_or_inside(scratch, m->lexical)) {
        return scratch + strlen(m->lexical);
    }
    if (canonical_path(value, scratch, cap) && same_or_inside(scratch, m->resolved)) {
        return scratch + strlen(m->resolved);
    }
    return NULL;
}

/* Goes through every JSON string in `data` (keys and titles included, though only paths match in
   practice). With `out`, writes `data` to it with each string that names the folder, or a path
   below it, rewritten to the same path below the new one; every other byte is kept. Each string is
   decoded before it is compared and the new value is escaped as the library writes it, so a folder
   name with a quote or a backslash is found and stays valid JSON. Without `out`, only counts. */
static void remap_strings(const Remap *m, const char *data, size_t size, OutBuf *out, size_t *hits,
                          bool *too_long) {
    static char value[NK_MAX_PATH * 2u];
    static char scratch[NK_MAX_PATH * 2u];
    static char renamed[NK_MAX_PATH * 2u];
    *hits = 0;
    *too_long = false;
    for (size_t i = 0; i < size && !(out && out->failed);) {
        if (data[i] != '"') {
            if (out) out_put(out, data + i, 1u);
            i++;
            continue;
        }
        size_t end = i + 1u;
        while (end < size && data[end] != '"') end += data[end] == '\\' ? 2u : 1u;
        if (end >= size) { /* an unterminated string: keep the rest as it is */
            if (out) out_put(out, data + i, size - i);
            break;
        }
        size_t value_length = 0;
        const char *rest = NULL;
        if (json_unescape(data + i + 1u, end - i - 1u, value, sizeof(value) - 1u, &value_length) &&
            memchr(value, '\0', value_length) == NULL) {
            value[value_length] = '\0';
            rest = remap_rest(m, value, scratch, sizeof(scratch));
        }
        size_t renamed_length = 0;
        if (rest) {
            const size_t to_length = strlen(m->to), rest_length = strlen(rest);
            renamed_length = to_length + rest_length;
            if (renamed_length >= NK_MAX_PATH) {
                *too_long = true;
            } else {
                memcpy(renamed, m->to, to_length);
                memcpy(renamed + to_length, rest, rest_length);
            }
            (*hits)++;
        }
        if (out) {
            if (!rest || *too_long) {
                out_put(out, data + i, end + 1u - i);
            } else {
                out_put(out, "\"", 1u);
                out_put_escaped(out, renamed, renamed_length);
                out_put(out, "\"", 1u);
            }
        }
        i = end + 1u;
    }
}

/* Rewrites library.json's stored paths under m's folder, while the caller holds the library's
   lock. With `backup`, the original is first written whole to library.json.pre-migration (an older
   one is replaced). The result is read back: a path that still names the folder puts the original
   back and fails. A library a newer Nakagawa wrote (schema_version above NK_LIBRARY_SCHEMA_VERSION)
   is not touched: LIB_NEWER. A missing library is LIB_UNCHANGED. */
static LibResult library_apply(const char *library_file, const Remap *m, bool backup, char *log,
                               char *why, size_t why_size) {
    if (!m->active || nk_platform_path_missing(library_file)) return LIB_UNCHANGED;
    LibResult result = LIB_FAILED;
    char *data = NULL, *check = NULL;
    size_t size = 0, check_size = 0, hits = 0;
    bool too_long = false;
    OutBuf out = {NULL, 0u, 0u, false};
    long version = nk_user_data_file_version(library_file, MIGRATE_LIBRARY_MAX_BYTES);
    if (version > (long)NK_LIBRARY_SCHEMA_VERSION) {
        snprintf(why, why_size,
                 "the library file %s was written by a newer version of Nakagawa (schema_version "
                 "%ld), which this version does not change",
                 library_file, version);
        result = LIB_NEWER;
        goto done;
    }
    if (!read_whole(library_file, MIGRATE_LIBRARY_MAX_BYTES, &data, &size)) {
        snprintf(why, why_size, "the library file %s could not be read", library_file);
        goto done;
    }
    remap_strings(m, data, size, &out, &hits, &too_long);
    if (out.failed || too_long) {
        snprintf(why, why_size,
                 too_long ? "a path in the library file %s would be too long in the new folder"
                          : "out of memory while rewriting the library file %s",
                 library_file);
        goto done;
    }
    if (hits == 0u) {
        result = LIB_UNCHANGED;
        goto done;
    }
    char backup_file[NK_MAX_PATH + 16];
    int written = snprintf(backup_file, sizeof(backup_file), "%s.pre-migration", library_file);
    bool ok = written > 0 && (size_t)written < sizeof(backup_file);
    if (ok && backup) ok = write_atomic(log, backup_file, data, size);
    if (!ok || !write_atomic(log, library_file, out.data, out.length)) {
        snprintf(why, why_size, "the library file %s could not be rewritten", library_file);
        goto done;
    }
    if (!read_whole(library_file, MIGRATE_LIBRARY_MAX_BYTES, &check, &check_size)) {
        snprintf(why, why_size, "the rewritten library file %s could not be read back", library_file);
        goto done;
    }
    remap_strings(m, check, check_size, NULL, &hits, &too_long);
    if (hits != 0u) {
        snprintf(why, why_size,
                 "%zu stored path(s) in the library file %s still name %s after it was rewritten",
                 hits, library_file, m->lexical);
        if (!write_atomic(log, library_file, data, size)) {
            log_line(log, "%s: the library file %s could not be put back as it was%s%s.",
                     NK_MIGRATE_CODE_FAILED, library_file, backup ? "; the original is " : "",
                     backup ? backup_file : "");
        }
        goto done;
    }
    result = LIB_REWRITTEN;
done:
    free(data);
    free(check);
    free(out.data);
    return result;
}

/* ---- The journal ---- */

typedef struct {
    char label[32];
    char from[NK_MAX_PATH];
    char to[NK_MAX_PATH];
    bool essential; /* a folder that matters; a cache or logs folder that cannot move stays put */
} Step;

/* The move, step by step, as the journal holds it. Each rename's paths are where its folder is when
   that step runs (Linux's saves are inside the moved data folder by then). The library rewrite, when
   there is one, is the last step. */
typedef struct {
    char new_root[NK_MAX_PATH];
    Step steps[NK_MIGRATE_MAX_MOVES];
    size_t count;
    bool library;
    char library_file[NK_MAX_PATH];
    Remap remap; /* the old data folder (both spellings) to the new one */
} Journal;

static const char k_journal_magic[] = "nakagawa-data-folder-move 1";

static void journal_text(const Journal *j, OutBuf *out) {
    out_text(out, k_journal_magic);
    out_text(out, "\n# Nakagawa is moving your data folders (#324). The next start finishes or undoes "
                  "this move;\n# do not edit or delete this file.\n");
    out_text(out, "root\t");
    out_text(out, j->new_root);
    out_text(out, "\n");
    for (size_t i = 0; i < j->count; i++) {
        const Step *s = &j->steps[i];
        out_text(out, "rename\t");
        out_text(out, s->label);
        out_text(out, s->essential ? "\t1\t" : "\t0\t");
        out_text(out, s->from);
        out_text(out, "\t");
        out_text(out, s->to);
        out_text(out, "\n");
    }
    if (j->library) {
        out_text(out, "library\t");
        out_text(out, j->library_file);
        out_text(out, "\t");
        out_text(out, j->remap.lexical);
        out_text(out, "\t");
        out_text(out, j->remap.resolved);
        out_text(out, "\t");
        out_text(out, j->remap.to);
        out_text(out, "\n");
    }
}

/* Splits `line` at tabs into exactly `want` fields. */
static bool split_fields(char *line, char **fields, size_t want) {
    size_t n = 0;
    char *p = line;
    while (n < want) {
        fields[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    return n == want && strchr(fields[want - 1u], '\t') == NULL;
}

static bool absolute_into(char *out, size_t cap, const char *path) {
    return path[0] == '/' && copy_path(out, cap, path);
}

static bool journal_parse(const char *data, size_t size, Journal *j) {
    memset(j, 0, sizeof(*j));
    static char line[NK_MAX_PATH * 4 + 64];
    bool first = true, root = false;
    for (size_t i = 0; i < size;) {
        size_t end = i;
        while (end < size && data[end] != '\n') end++;
        size_t n = end - i;
        if (n >= sizeof(line) || memchr(data + i, '\0', n)) return false;
        memcpy(line, data + i, n);
        line[n] = '\0';
        i = end + 1u;
        if (first) {
            if (strcmp(line, k_journal_magic) != 0) return false;
            first = false;
            continue;
        }
        if (line[0] == '#' || line[0] == '\0') continue;
        char *f[5];
        if (strncmp(line, "root\t", 5) == 0) {
            if (root || !split_fields(line, f, 2u) || !absolute_into(j->new_root, sizeof(j->new_root), f[1])) {
                return false;
            }
            root = true;
        } else if (strncmp(line, "rename\t", 7) == 0) {
            if (j->library || j->count >= NK_MIGRATE_MAX_MOVES || !split_fields(line, f, 5u)) return false;
            Step *s = &j->steps[j->count];
            if (!copy_path(s->label, sizeof(s->label), f[1]) || (strcmp(f[2], "1") != 0 && strcmp(f[2], "0") != 0) ||
                !absolute_into(s->from, sizeof(s->from), f[3]) || !absolute_into(s->to, sizeof(s->to), f[4])) {
                return false;
            }
            s->essential = f[2][0] == '1';
            j->count++;
        } else if (strncmp(line, "library\t", 8) == 0) {
            if (j->library || !split_fields(line, f, 5u) ||
                !absolute_into(j->library_file, sizeof(j->library_file), f[1]) ||
                !absolute_into(j->remap.lexical, sizeof(j->remap.lexical), f[2]) ||
                !absolute_into(j->remap.resolved, sizeof(j->remap.resolved), f[3]) ||
                !absolute_into(j->remap.to, sizeof(j->remap.to), f[4])) {
                return false;
            }
            remap_activate(&j->remap);
            j->library = j->remap.active;
        } else {
            return false;
        }
    }
    return !first && root && j->count > 0u;
}

/* The journal, synced, and its folder synced, before the first rename. */
static bool journal_write(const char *path, const Journal *j, char *why, size_t why_size) {
    OutBuf out = {NULL, 0u, 0u, false};
    journal_text(j, &out);
    char temp[NK_MAX_PATH + 8];
    int written = snprintf(temp, sizeof(temp), "%s.tmp", path);
    bool ok = !out.failed && written > 0 && (size_t)written < sizeof(temp) &&
              write_whole(temp, out.data, out.length);
    if (ok && nk_rename_utf8(temp, path) != 0) ok = false;
    free(out.data);
    if (!ok) {
        snprintf(why, why_size, "the record of the move %s could not be written (%s)", path,
                 errno ? strerror(errno) : "unknown error");
        (void)nk_remove_utf8(temp);
        return false;
    }
    char dir[NK_MAX_PATH];
    if (!parent_dir(path, dir, sizeof(dir)) || !do_sync_dir(dir)) {
        snprintf(why, why_size, "%s: the record of the move %s could not be flushed to the disk (%s)",
                 NK_MIGRATE_CODE_NOT_FLUSHED, path, errno ? strerror(errno) : "unknown error");
        (void)nk_remove_utf8(path);
        return false;
    }
    return true;
}

/* After a complete undo: the move is over. A journal that cannot be removed is finished or undone
   again by the next start. */
static bool journal_remove(const char *path, char *log) {
    if (nk_remove_utf8(path) != 0 && !nk_platform_path_missing(path)) {
        log_line(log, "%s: the record of the move %s could not be removed (%s); the next start "
                      "finishes or undoes the move again.",
                 NK_MIGRATE_CODE_INTERRUPTED, path, strerror(errno));
        return false;
    }
    flush_parent(log, path);
    return true;
}

/* ---- Steps, read from the disk ---- */

/* One rename forward. Done when the source is gone and the destination is there; to do when the
   source is there and the destination is absent or only empty folders; anything else is an error. */
static bool step_forward(const Step *s, char *log, int *error, char *why, size_t why_size) {
    const EntryKind from = entry_kind(s->from);
    const TreeState to = dest_state(s->to);
    if (from == ENTRY_ABSENT && to != TREE_ABSENT) return true;
    if (from == ENTRY_ABSENT) {
        *error = ENOENT;
        snprintf(why, why_size, "neither %s nor %s exists", s->from, s->to);
        return false;
    }
    if (to == TREE_CONTENT) {
        *error = EEXIST;
        snprintf(why, why_size, "both %s and %s hold files", s->from, s->to);
        return false;
    }
    char parent[NK_MAX_PATH];
    if ((to == TREE_EMPTY && !clear_empty_below(s->to)) ||
        (parent_dir(s->to, parent, sizeof(parent)) && !nk_platform_mkdir_p(parent)) ||
        do_rename(s->from, s->to) != 0) {
        *error = errno;
        if (*error == EXDEV) {
            snprintf(why, why_size, "%s and %s are on different disks (or file systems)", s->from, s->to);
        } else {
            snprintf(why, why_size, "%s could not be moved to %s (%s)", s->from, s->to,
                     *error ? strerror(*error) : "unknown error");
        }
        return false;
    }
    flush_parent(log, s->to);
    flush_parent(log, s->from);
    return true;
}

/* One rename back. Nothing to do when the destination is absent, or only empty folders while the
   source is there (the step never ran); back when the source is gone; an error when both hold
   files. */
static bool step_undo(const Step *s, char *log, char *why, size_t why_size) {
    const EntryKind from = entry_kind(s->from);
    const TreeState to = dest_state(s->to);
    if (to == TREE_ABSENT || (to == TREE_EMPTY && from != ENTRY_ABSENT)) return true;
    if (from != ENTRY_ABSENT) {
        snprintf(why, why_size, "both %s and %s hold files", s->from, s->to);
        return false;
    }
    char parent[NK_MAX_PATH];
    if ((parent_dir(s->from, parent, sizeof(parent)) && !nk_platform_mkdir_p(parent)) ||
        do_rename(s->to, s->from) != 0) {
        int error = errno;
        snprintf(why, why_size, "%s could not be moved back to %s (%s)", s->to, s->from,
                 error ? strerror(error) : "unknown error");
        return false;
    }
    flush_parent(log, s->from);
    flush_parent(log, s->to);
    return true;
}

/* Every step forward, in order. False, with `why`, at the first that cannot complete. A cache or
   logs folder that cannot move stays where it is (a log line). `reached` is the number of renames
   before the one that failed (all of them when the library or the commit failed): that rename did not
   happen, since step_forward found it not done, so neither did any later one. */
static bool roll_forward(const Journal *j, char *log, size_t *moved, bool *rewritten, size_t *reached,
                         int *error, char *why, size_t why_size) {
    *moved = 0;
    *rewritten = false;
    *reached = j->count;
    for (size_t i = 0; i < j->count; i++) {
        const Step *s = &j->steps[i];
        if (!step_forward(s, log, error, why, why_size)) {
            if (s->essential) {
                *reached = i;
                return false;
            }
            log_line(log, "%s: your old %s folder stays where it is: %s.", NK_MIGRATE_CODE_LEFT_BEHIND,
                     s->label, why);
            why[0] = '\0';
            *error = 0;
        } else {
            (*moved)++;
        }
        point("step", i + 1u);
    }
    if (j->library) {
        LibResult lib = library_apply(j->library_file, &j->remap, true, log, why, why_size);
        if (lib == LIB_NEWER || lib == LIB_FAILED) return false;
        *rewritten = lib == LIB_REWRITTEN;
        point("step", j->count + 1u);
    }
    return true;
}

/* The first `reached` steps back, newest first (the library first when every rename was reached).
   Stops at the first that cannot be undone, so what is done is always a run of steps from the first:
   the next start rolls it forward. */
static bool roll_back(const Journal *j, size_t reached, char *log, char *why, size_t why_size) {
    if (j->library && reached == j->count) {
        Remap back;
        memset(&back, 0, sizeof(back));
        (void)copy_path(back.lexical, sizeof(back.lexical), j->remap.to);
        (void)copy_path(back.resolved, sizeof(back.resolved), j->remap.to);
        (void)copy_path(back.to, sizeof(back.to), j->remap.lexical);
        remap_activate(&back);
        LibResult lib = library_apply(j->library_file, &back, false, log, why, why_size);
        /* A newer library was never rewritten forward either. */
        if (lib == LIB_FAILED) return false;
        point("undo", j->count + 1u);
    }
    for (size_t k = reached; k > 0u; k--) {
        const Step *s = &j->steps[k - 1u];
        if (!step_undo(s, log, why, why_size)) {
            if (s->essential) return false;
            log_line(log, "%s: your old %s folder could not be put back: %s.", NK_MIGRATE_CODE_LEFT_BEHIND,
                     s->label, why);
            why[0] = '\0';
        }
        point("undo", k);
    }
    return true;
}

/* Commit: everything the marker vouches for is flushed, then the journal becomes the marker in one
   rename. */
static bool commit_move(const Journal *j, const char *journal_file, const char *marker_file, char *log,
                        char *why, size_t why_size) {
    for (size_t i = 0; i <= j->count; i++) {
        const char *paths[2] = {NULL, NULL};
        if (i < j->count) {
            paths[0] = j->steps[i].from;
            paths[1] = j->steps[i].to;
        } else if (j->library) {
            paths[0] = j->library_file;
        }
        for (size_t p = 0; p < 2u; p++) {
            char dir[NK_MAX_PATH];
            if (!paths[p] || !parent_dir(paths[p], dir, sizeof(dir)) || nk_platform_path_missing(dir)) continue;
            if (!do_sync_dir(dir)) {
                snprintf(why, why_size, "%s: the folder %s could not be flushed to the disk (%s)",
                         NK_MIGRATE_CODE_NOT_FLUSHED, dir, errno ? strerror(errno) : "unknown error");
                return false;
            }
        }
    }
    if (!do_sync_dir(j->new_root)) {
        snprintf(why, why_size, "%s: the folder %s could not be flushed to the disk (%s)",
                 NK_MIGRATE_CODE_NOT_FLUSHED, j->new_root, errno ? strerror(errno) : "unknown error");
        return false;
    }
    if (do_rename(journal_file, marker_file) != 0) {
        int error = errno;
        snprintf(why, why_size, "the record of the move %s could not become %s (%s)", journal_file,
                 marker_file, error ? strerror(error) : "unknown error");
        return false;
    }
    flush_parent(log, marker_file);
    flush_parent(log, journal_file);
    point("commit", 0u);
    return true;
}

/* ---- The plan ---- */

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

static bool is_primary(const NkMigrateMove *move) { return !move->disposable && !move->after_previous; }

static const char *first_old_folder(const NkDataMigrationPlan *plan) {
    for (size_t i = 0; i < plan->move_count; i++) {
        if (is_primary(&plan->moves[i])) return plan->moves[i].from;
    }
    return "";
}

/* The first folder of the old layout that matters and holds files, or NULL. */
static const char *old_folder_with_files(const NkDataMigrationPlan *plan) {
    for (size_t i = 0; i < plan->move_count; i++) {
        if (is_primary(&plan->moves[i]) && tree_state(plan->moves[i].from, 0) == TREE_CONTENT) {
            return plan->moves[i].from;
        }
    }
    return NULL;
}

/* A folder this host keeps using in the new layout (Linux's cache or logs, when XDG homes point at
   each other) is, holds or lies in `path`. */
static bool overlaps_kept(const NkDataMigrationPlan *plan, const char *path) {
    for (size_t k = 0; k < plan->kept_count; k++) {
        if (folders_overlap(path, plan->kept[k])) return true;
    }
    return false;
}

/* Every old folder that matters and holds files, as "a and b"; with `skip_kept`, not one that the
   new layout itself writes (overlaps_kept). Returns the first, or NULL. */
static const char *old_folders_with_files(const NkDataMigrationPlan *plan, bool skip_kept, char *out,
                                          size_t cap) {
    const char *first = NULL;
    out[0] = '\0';
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (!is_primary(move) || tree_state(move->from, 0) != TREE_CONTENT) continue;
        if (skip_kept && overlaps_kept(plan, move->from)) continue;
        add(out, cap, "%s%s", out[0] ? " and " : "", move->from);
        if (!first) first = move->from;
    }
    return first;
}

/* ---- The rule ---- */

/* The paths of the data folder, old and new, whose locks a recovery checks: the journal's, else
   none. */
static bool recovery_locks(const Journal *j, NkFileLock *library_lock, char *what, size_t cap) {
    memset(library_lock, 0, sizeof(*library_lock));
    if (!j->library) return true;
    const char *dirs[2] = {j->remap.lexical, j->remap.to};
    for (size_t d = 0; d < 2u; d++) {
        if (!is_real_dir(dirs[d])) continue;
        if (data_locks_held(dirs[d], what, cap)) return false;
        char library[NK_MAX_PATH + 16];
        int written = snprintf(library, sizeof(library), "%s/library.json", dirs[d]);
        if (written <= 0 || (size_t)written >= sizeof(library)) continue;
        return lock_library(library_lock, library, what, cap);
    }
    return true;
}

static void blocked_where(const Journal *j, NkLayoutDecision *out) {
    add(out->message, sizeof(out->message), " Where your folders are now:");
    for (size_t i = 0; i < j->count; i++) {
        const Step *s = &j->steps[i];
        if (!s->essential) continue;
        add(out->message, sizeof(out->message), " your %s folder: %s %s, and %s %s;", s->label, s->from,
            state_words(s->from), s->to, state_words(s->to));
    }
}

/* Rule 1: an interrupted move is finished, or undone, before anything uses the folders. True when
   the journal is gone; false with out->message set (the start stops). */
static bool recover(const NkDataMigrationPlan *plan, NkLayoutDecision *out) {
    static Journal j;
    memset(&j, 0, sizeof(j));
    (void)copy_path(out->folder_a, sizeof(out->folder_a), first_old_folder(plan));
    (void)copy_path(out->folder_b, sizeof(out->folder_b), plan->new_root);
    int player_fd = -1;
    if (plan->player_lock_file[0]) {
        int error = 0;
        player_fd = player_lock_exclusive(plan->player_lock_file, &error);
        if (player_fd < 0) {
            snprintf(out->message, sizeof(out->message),
                     "%s: A move of your data folders to %s was interrupted, and another Nakagawa "
                     "window is open (%s: %s), so the move can be neither finished nor undone now. "
                     "Nakagawa did not start, so that nothing uses the folders while they are half "
                     "moved. Close every other Nakagawa window and game, then start Nakagawa again.",
                     NK_MIGRATE_CODE_IN_USE, plan->new_root, plan->player_lock_file,
                     error == EWOULDBLOCK ? "held by another process" : strerror(error));
            return false;
        }
    }
    char *data = NULL;
    size_t size = 0;
    bool parsed = read_whole(plan->journal_file, MIGRATE_JOURNAL_MAX_BYTES, &data, &size) &&
                  journal_parse(data, size, &j);
    free(data);
    if (!parsed) {
        release_lock(player_fd);
        snprintf(out->message, sizeof(out->message),
                 "%s: A move of your data folders to %s was interrupted, and its record %s cannot be "
                 "read, so Nakagawa cannot tell how far it got. Nakagawa did not start. Nothing was "
                 "deleted. Your earlier folder %s %s; %s %s.",
                 NK_MIGRATE_CODE_INTERRUPTED, plan->new_root, plan->journal_file, out->folder_a,
                 state_words(out->folder_a), plan->new_root, state_words(plan->new_root));
        return false;
    }
    NkFileLock library_lock;
    char what[PATH_MAX + 128] = "";
    if (!recovery_locks(&j, &library_lock, what, sizeof(what))) {
        release_lock(player_fd);
        add(out->message, sizeof(out->message),
                 "%s: A move of your data folders to %s was interrupted, and %s, so the move can be "
                 "neither finished nor undone now. Nakagawa did not start, so that nothing uses the "
                 "folders while they are half moved. Close every other Nakagawa window, game and tool, "
                 "then start Nakagawa again.",
                 NK_MIGRATE_CODE_LOCK, j.new_root, what);
        return false;
    }
    char why[NK_MAX_PATH * 2 + 256] = "";
    size_t moved = 0;
    size_t reached = 0;
    bool rewritten = false;
    int error = 0;
    bool ok = roll_forward(&j, out->log, &moved, &rewritten, &reached, &error, why, sizeof(why)) &&
              commit_move(&j, plan->journal_file, plan->marker_file, out->log, why, sizeof(why));
    if (ok) {
        log_line(out->log, "%s: an interrupted move of your data folders to %s was finished at this start.",
                 NK_MIGRATE_CODE_INTERRUPTED, j.new_root);
    } else {
        char undo_why[NK_MAX_PATH * 2 + 256] = "";
        ok = roll_back(&j, reached, out->log, undo_why, sizeof(undo_why)) &&
             journal_remove(plan->journal_file, out->log);
        if (ok) {
            log_line(out->log, "%s: an interrupted move of your data folders could not be finished (%s), so "
                               "it was undone; Nakagawa uses your earlier folders.",
                     NK_MIGRATE_CODE_INTERRUPTED, why);
        } else {
            if (j.count > 0u) (void)copy_path(out->folder_a, sizeof(out->folder_a), j.steps[0].from);
            snprintf(out->message, sizeof(out->message),
                     "%s: A move of your data folders to %s that you started was interrupted. Nakagawa "
                     "could not finish it (%s) or undo it (%s), so it did not start: nothing may use the "
                     "folders while they are half moved. Nothing was deleted.",
                     NK_MIGRATE_CODE_INTERRUPTED, j.new_root, why[0] ? why : "unknown error",
                     undo_why[0] ? undo_why : "the record of the move could not be removed");
            blocked_where(&j, out);
            add(out->message, sizeof(out->message),
                " Fix what is named above (free space, permissions, a disk that is not mounted), then "
                "start Nakagawa again: it carries on from where it stopped. The record of the move is "
                "%s; do not edit or delete it.",
                plan->journal_file);
        }
    }
    nk_platform_file_lock_release(&library_lock);
    release_lock(player_fd);
    return ok;
}

static void write_marker(const NkDataMigrationPlan *plan, NkLayoutDecision *out) {
    static const char text[] =
        "Nakagawa uses the folders in this one (#324). Keep this file: it tells Nakagawa and its tools\n"
        "to use this folder even if an earlier folder appears again.\n";
    if (!plan->marker_file[0]) return;
    if (!nk_platform_mkdir_p(plan->new_root) || !write_atomic(out->log, plan->marker_file, text, sizeof(text) - 1u)) {
        log_line(out->log, "%s: %s could not be written (%s); the rule decides again at the next start.",
                 NK_MIGRATE_CODE_MARKER_NOT_WRITTEN, plan->marker_file, errno ? strerror(errno) : "unknown error");
    }
}

static void decide_locked(const NkDataMigrationPlan *plan, NkLayoutDecision *out) {
    if (plan->journal_file[0] && !nk_platform_path_missing(plan->journal_file)) {
        out->rule = "journal";
        if (!recover(plan, out)) {
            out->layout = NK_LAYOUT_BLOCKED;
            return;
        }
        out->recovered = true;
    }
    const char *old = old_folder_with_files(plan);
    if (plan->marker_file[0] && !nk_platform_path_missing(plan->marker_file)) {
        if (!out->recovered) out->rule = "marker";
        out->layout = NK_LAYOUT_NEW;
        (void)copy_path(out->folder_a, sizeof(out->folder_a), plan->new_root);
        out->folder_b[0] = '\0';
        char list[NK_MAX_PATH * 3];
        const char *returned = old_folders_with_files(plan, true, list, sizeof(list));
        if (!returned) return;
        out->notice_kind = NK_LAYOUT_NOTICE_RETURNED;
        (void)copy_path(out->folder_b, sizeof(out->folder_b), returned);
        add(out->notice, sizeof(out->notice),
                 "%s: Nakagawa uses your data in %s. Your earlier folder %s holds files again. They "
                 "were written there after your data was moved, most likely by an older version "
                 "of Nakagawa that was still running. Nakagawa does not use them, and it did not move or "
                 "delete anything.",
                 NK_LAYOUT_CODE_RETURNED, plan->new_root, list);
        return;
    }
    if (old) {
        if (!out->recovered) out->rule = "old";
        out->layout = NK_LAYOUT_OLD;
        out->can_move = true;
        (void)copy_path(out->folder_a, sizeof(out->folder_a), old);
        out->folder_b[0] = '\0';
        if (dest_state(plan->new_root) != TREE_CONTENT) return;
        char list[NK_MAX_PATH * 3];
        (void)old_folders_with_files(plan, false, list, sizeof(list));
        out->notice_kind = NK_LAYOUT_NOTICE_BOTH;
        (void)copy_path(out->folder_b, sizeof(out->folder_b), plan->new_root);
        add(out->notice, sizeof(out->notice),
                 "%s: Nakagawa uses your data in %s. The newer folder %s holds files too. Nakagawa does "
                 "not use them, and it did not merge, move or delete anything.",
                 NK_LAYOUT_CODE_BOTH, list, plan->new_root);
        return;
    }
    if (!out->recovered) out->rule = "fresh";
    out->layout = NK_LAYOUT_NEW;
    (void)copy_path(out->folder_a, sizeof(out->folder_a), plan->new_root);
    out->folder_b[0] = '\0';
    write_marker(plan, out);
}

void nk_data_layout_decide(const NkDataMigrationPlan *plan, NkLayoutDecision *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->layout = NK_LAYOUT_NEW;
    out->rule = "none";
    /* Decided once per process; a second call (a test) starts over. */
    nk_data_migrate_release_player_lock();
    if (!plan_is_valid(plan) || !plan->new_root[0]) {
        log_line(out->log, "%s: the data folder plan is invalid; the new layout is used.", NK_MIGRATE_CODE_FAILED);
        return;
    }
    if (plan->notes[0]) log_line(out->log, "%s", plan->notes);
    int run_fd = -1;
    if (plan->lock_file[0]) {
        int error = 0;
        if (!take_run_lock(plan->lock_file, &run_fd, &error)) {
            /* The player lock still keeps a recovery to one process. */
            log_line(out->log, "%s: %s could not be locked (%s); the layout is decided without it.",
                     NK_MIGRATE_CODE_LOCK, plan->lock_file, strerror(error));
        }
    }
    decide_locked(plan, out);
    if (out->layout != NK_LAYOUT_BLOCKED && plan->player_lock_file[0] &&
        !player_lock_shared(plan->player_lock_file)) {
        log_line(out->log, "%s: %s could not be held while Nakagawa runs (%s); a move started from "
                           "another window cannot see this one.",
                 NK_MIGRATE_CODE_LOCK, plan->player_lock_file, strerror(errno));
    }
    release_lock(run_fd);
}

/* ---- The move ---- */

static void refuse(NkDataMigration *r, const char *code, const char *format, ...) {
    r->status = NK_MIGRATE_REFUSED;
    snprintf(r->message, sizeof(r->message), "%s: ", code);
    va_list args;
    va_start(args, format);
    append_v(r->message, sizeof(r->message), format, args);
    va_end(args);
    add(r->message, sizeof(r->message), " Nothing was moved or deleted.");
}

static bool has_control(const char *path) { return strchr(path, '\t') || strchr(path, '\n'); }

static const char *unsafe_path(const NkDataMigrationPlan *plan) {
    const char *paths[] = {plan->new_root,    plan->journal_file, plan->marker_file,
                           plan->library_file, plan->remap_from,  plan->remap_to};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (has_control(paths[i])) return paths[i];
    }
    for (size_t i = 0; i < plan->move_count; i++) {
        if (has_control(plan->moves[i].from)) return plan->moves[i].from;
        if (has_control(plan->moves[i].to)) return plan->moves[i].to;
    }
    return NULL;
}

/* Every check, before anything is touched, and the steps of the move. False with r refused. */
static bool plan_steps(const NkDataMigrationPlan *plan, Journal *j, NkDataMigration *r) {
    static char now[NK_MIGRATE_MAX_MOVES][NK_MAX_PATH];
    bool included[NK_MIGRATE_MAX_MOVES];
    memset(included, 0, sizeof(included));
    const char *unsafe = unsafe_path(plan);
    if (unsafe) {
        refuse(r, NK_MIGRATE_CODE_UNSAFE_NAME,
               "The folder path %s holds a tab or a line break, which the record of the move cannot "
               "hold.", unsafe);
        return false;
    }
    if (dest_state(plan->new_root) == TREE_CONTENT) {
        refuse(r, NK_MIGRATE_CODE_BOTH_PRESENT,
               "Your data is in %s, and %s holds files too, so moving would mix the two.",
               first_old_folder(plan), plan->new_root);
        return false;
    }
    char base[NK_MAX_PATH];
    dev_t base_device = 0, root_device = 0;
    if (!parent_dir(plan->journal_file, base, sizeof(base)) || !device_of(base, &base_device) ||
        !device_of(plan->new_root, &root_device) || base_device != root_device) {
        refuse(r, NK_MIGRATE_CODE_OTHER_DISK,
               "%s and the folder that holds it are on different disks (or file systems), so the "
               "record of the move cannot be committed there.", plan->new_root);
        return false;
    }
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (move->after_previous) {
            if (!included[i - 1u]) continue;
            const char *rest = move->from + strlen(plan->moves[i - 1u].to);
            int written = snprintf(now[i], sizeof(now[i]), "%s%s", now[i - 1u], rest);
            if (written <= 0 || (size_t)written >= sizeof(now[i])) {
                refuse(r, NK_MIGRATE_CODE_PATH_UNAVAILABLE, "The path of your %s folder is too long.",
                       move->label);
                return false;
            }
        } else if (!copy_path(now[i], sizeof(now[i]), move->from)) {
            continue;
        }
        const EntryKind kind = entry_kind(now[i]);
        if (kind == ENTRY_ABSENT) continue;
        dev_t from_device = 0, to_device = 0;
        const bool same_disk = device_of(now[i], &from_device) && device_of(move->to, &to_device) &&
                               from_device == to_device;
        if (move->disposable) {
            if (kind == ENTRY_LINK) {
                log_line(r->log, "%s: your old %s folder %s is a link, so it is not moved; it stays where "
                                 "it is.", NK_MIGRATE_CODE_LEFT_BEHIND, move->label, now[i]);
            } else if (tree_state(now[i], 0) != TREE_CONTENT) {
                /* nothing worth moving */
            } else if (dest_state(move->to) == TREE_CONTENT || !same_disk) {
                log_line(r->log, "%s: your old %s folder %s is not moved (%s); it stays where it is and "
                                 "can be deleted.", NK_MIGRATE_CODE_LEFT_BEHIND, move->label, now[i],
                         same_disk ? "the new one holds files" : "it is on another disk");
            } else {
                included[i] = true;
            }
            continue;
        }
        if (kind == ENTRY_LINK) {
            char target[NK_MAX_PATH] = "";
            ssize_t n = readlink(now[i], target, sizeof(target) - 1u);
            if (n > 0 && (size_t)n < sizeof(target)) target[n] = '\0';
            else snprintf(target, sizeof(target), "a place that cannot be read");
            struct stat followed;
            const bool dangling = stat(now[i], &followed) != 0 && (errno == ENOENT || errno == ENOTDIR);
            refuse(r, NK_MIGRATE_CODE_LINKED,
                   dangling ? "Your %s folder %s is a link to %s, which does not exist now (a disk that "
                              "is not mounted?). Nakagawa does not move a linked folder."
                            : "Your %s folder %s is a link to %s. Nakagawa does not move a linked "
                              "folder: moving the link can change where it points.",
                   move->label, now[i], target);
            return false;
        }
        struct stat st;
        if (lstat(now[i], &st) == 0 && (unsigned long)st.st_uid != effective_uid()) {
            refuse(r, NK_MIGRATE_CODE_OTHER_OWNER,
                   "Your %s folder %s belongs to user %lu, and Nakagawa runs as user %lu (for example "
                   "under sudo). Start Nakagawa as yourself to move it.",
                   move->label, now[i], (unsigned long)st.st_uid, effective_uid());
            return false;
        }
        if (is_primary(move) && overlaps_kept(plan, now[i])) {
            for (size_t k = 0; k < plan->kept_count; k++) {
                if (!folders_overlap(now[i], plan->kept[k])) continue;
                refuse(r, NK_MIGRATE_CODE_OVERLAP,
                       "Your %s folder %s and Nakagawa's %s folder %s are the same folder, or one is "
                       "inside the other (XDG_DATA_HOME, XDG_CONFIG_HOME, XDG_CACHE_HOME or "
                       "XDG_STATE_HOME point at each other), so one cannot be moved without the other.",
                       move->label, now[i], plan->kept_label[k], plan->kept[k]);
                return false;
            }
        }
        if (!same_disk) {
            refuse(r, NK_MIGRATE_CODE_OTHER_DISK,
                   "Your %s folder %s and its new place %s are on different disks (or file systems), "
                   "and Nakagawa does not copy your library, saves or settings between disks.",
                   move->label, now[i], move->to);
            return false;
        }
        included[i] = true;
    }
    /* Two folders being moved that are the same folder, or one inside the other. */
    for (size_t i = 0; i < plan->move_count; i++) {
        if (!included[i]) continue;
        for (size_t k = i + 1u; k < plan->move_count; k++) {
            if (!included[k] || (plan->moves[k].after_previous && k == i + 1u)) continue;
            if (!folders_overlap(now[i], now[k])) continue;
            const NkMigrateMove *a = &plan->moves[i];
            const NkMigrateMove *b = &plan->moves[k];
            if (a->disposable || b->disposable) {
                const size_t d = a->disposable ? i : k;
                included[d] = false;
                log_line(r->log, "%s: your old %s folder %s is, holds or lies in another folder being "
                                 "moved, so it is not moved on its own; it stays where it is.",
                         NK_MIGRATE_CODE_LEFT_BEHIND, plan->moves[d].label, now[d]);
                continue;
            }
            refuse(r, NK_MIGRATE_CODE_OVERLAP,
                   "Your %s folder %s and %s folder %s are the same folder, or one is inside the other "
                   "(XDG_DATA_HOME and XDG_CONFIG_HOME point at each other), so Nakagawa cannot tell "
                   "which files belong where.",
                   a->label, now[i], b->label, now[k]);
            return false;
        }
    }
    /* A relative link inside that would point elsewhere once the folder has moved. Links in a folder
       that moves on its own (Linux's data/saves) are checked against that folder. */
    for (size_t i = 0; i < plan->move_count; i++) {
        if (!included[i] || !is_primary(&plan->moves[i])) continue;
        static LinkScan scan;
        memset(&scan, 0, sizeof(scan));
        if (!copy_path(scan.path, sizeof(scan.path), now[i])) continue;
        if (i + 1u < plan->move_count && included[i + 1u] && plan->moves[i + 1u].after_previous) {
            scan.nested[scan.nested_count++] = now[i + 1u];
        }
        if (links_stay(&scan, 0)) continue;
        if (scan.unreadable) {
            refuse(r, NK_MIGRATE_CODE_LINKED,
                   "Nakagawa could not look through %s, inside your %s folder %s, for links that moving "
                   "the folder would break. Make it readable (or remove it), then try again.",
                   scan.found, plan->moves[i].label, now[i]);
        } else {
            refuse(r, NK_MIGRATE_CODE_LINKED,
                   "Your %s folder %s holds a link, %s, to %s: a relative path that leads out of the "
                   "folder, so moving the folder would make it point elsewhere. Make that link absolute "
                   "(for example with ln -sfn and the full path), then try again.",
                   plan->moves[i].label, now[i], scan.found, scan.target);
        }
        return false;
    }
    if (plan->old_library_file[0]) {
        long version = nk_user_data_file_version(plan->old_library_file, MIGRATE_LIBRARY_MAX_BYTES);
        if (version > (long)NK_LIBRARY_SCHEMA_VERSION) {
            refuse(r, NK_MIGRATE_CODE_NEWER_LIBRARY,
                   "Your library %s was written by a newer version of Nakagawa (schema_version %ld), "
                   "which this version does not change.",
                   plan->old_library_file, version);
            return false;
        }
    }
    /* The steps, each with the paths it has when it runs. */
    memset(j, 0, sizeof(*j));
    (void)copy_path(j->new_root, sizeof(j->new_root), plan->new_root);
    for (size_t i = 0; i < plan->move_count; i++) {
        if (!included[i]) continue;
        Step *s = &j->steps[j->count++];
        (void)copy_path(s->label, sizeof(s->label), plan->moves[i].label);
        (void)copy_path(s->from, sizeof(s->from), plan->moves[i].from);
        (void)copy_path(s->to, sizeof(s->to), plan->moves[i].to);
        s->essential = !plan->moves[i].disposable;
    }
    if (plan->remap_from[0] && plan->remap_to[0] && plan->library_file[0]) {
        if (!copy_path(j->library_file, sizeof(j->library_file), plan->library_file) ||
            !lexical_clean(plan->remap_from, j->remap.lexical, sizeof(j->remap.lexical)) ||
            !canonical_path(plan->remap_from, j->remap.resolved, sizeof(j->remap.resolved)) ||
            !lexical_clean(plan->remap_to, j->remap.to, sizeof(j->remap.to)) ||
            has_control(j->remap.resolved)) {
            refuse(r, NK_MIGRATE_CODE_PATH_UNAVAILABLE,
                   "The path of your data folder (%s) is too long to compare, or holds a tab or a line "
                   "break.", plan->remap_from);
            return false;
        }
        remap_activate(&j->remap);
        j->library = j->remap.active;
    }
    return true;
}

static void move_checked(const NkDataMigrationPlan *plan, NkDataMigration *r) {
    static Journal j;
    if (!plan_steps(plan, &j, r)) return;
    if (j.count == 0u) {
        r->status = NK_MIGRATE_NOTHING;
        snprintf(r->message, sizeof(r->message), "No earlier folder holds files: there is nothing to move.");
        return;
    }
    /* The old data folder's locks are free, and the library's own lock is held until the rewrite. */
    NkFileLock library_lock;
    memset(&library_lock, 0, sizeof(library_lock));
    char old_data[NK_MAX_PATH] = "";
    char what[PATH_MAX + 128] = "";
    if (plan->old_library_file[0] && parent_dir(plan->old_library_file, old_data, sizeof(old_data)) &&
        is_real_dir(old_data)) {
        if (data_locks_held(old_data, what, sizeof(what)) ||
            !lock_library(&library_lock, plan->old_library_file, what, sizeof(what))) {
            refuse(r, NK_MIGRATE_CODE_LOCK,
                   "Another Nakagawa program is writing your data: %s. Close every other Nakagawa "
                   "window, game and tool, then try again.", what);
            return;
        }
    }
    point("begin", 0u);
    char why[NK_MAX_PATH * 2 + 256] = "";
    if (!journal_write(plan->journal_file, &j, why, sizeof(why))) {
        nk_platform_file_lock_release(&library_lock);
        r->status = NK_MIGRATE_FAILED;
        snprintf(r->message, sizeof(r->message), "%s: %s, so Nakagawa moved nothing.", NK_MIGRATE_CODE_FAILED, why);
        return;
    }
    point("journal", 0u);
    size_t moved = 0;
    size_t reached = 0;
    bool rewritten = false;
    int error = 0;
    bool ok = roll_forward(&j, r->log, &moved, &rewritten, &reached, &error, why, sizeof(why)) &&
              commit_move(&j, plan->journal_file, plan->marker_file, r->log, why, sizeof(why));
    if (ok) {
        nk_platform_file_lock_release(&library_lock);
        r->status = NK_MIGRATE_DONE;
        r->moved = moved;
        r->library_rewritten = rewritten;
        r->must_restart = true;
        snprintf(r->message, sizeof(r->message), "Nakagawa moved your data to %s. Nothing was deleted.%s "
                                                 "Nakagawa restarts now to use it.",
                 plan->new_root,
                 rewritten ? " Your library's stored paths were updated (a copy of the old file is "
                             "library.json.pre-migration)."
                           : "");
        return;
    }
    char undo_why[NK_MAX_PATH * 2 + 256] = "";
    bool back = roll_back(&j, reached, r->log, undo_why, sizeof(undo_why)) &&
                journal_remove(plan->journal_file, r->log);
    nk_platform_file_lock_release(&library_lock);
    r->status = NK_MIGRATE_FAILED;
    snprintf(r->message, sizeof(r->message), "%s: Nakagawa could not move your data: %s. ",
             error == EXDEV ? NK_MIGRATE_CODE_OTHER_DISK : NK_MIGRATE_CODE_FAILED, why[0] ? why : "unknown error");
    if (back) {
        add(r->message, sizeof(r->message), "Everything is back where it was, and nothing was deleted. "
                                            "Nakagawa keeps using %s.", first_old_folder(plan));
        return;
    }
    r->must_restart = true;
    add(r->message, sizeof(r->message),
        "It could not put everything back either (%s). Nothing was deleted. Nakagawa restarts now, and "
        "the start finishes the move or undoes it before anything uses the folders.",
        undo_why[0] ? undo_why : "the record of the move could not be removed");
}

void nk_data_migrate_move(const NkDataMigrationPlan *plan, bool game_running, NkDataMigration *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    if (!plan_is_valid(plan) || !plan->new_root[0] || !plan->journal_file[0] || !plan->marker_file[0]) {
        r->status = NK_MIGRATE_FAILED;
        snprintf(r->message, sizeof(r->message), "%s: the migration plan is invalid.", NK_MIGRATE_CODE_FAILED);
        return;
    }
    (void)copy_path(r->folder_a, sizeof(r->folder_a), first_old_folder(plan));
    (void)copy_path(r->folder_b, sizeof(r->folder_b), plan->new_root);
    if (game_running) {
        refuse(r, NK_MIGRATE_CODE_GAME_RUNNING, "A game started from Nakagawa is running. Quit the game, "
                                                "then try again.");
        return;
    }
    int run_fd = -1;
    if (plan->lock_file[0]) {
        int error = 0;
        if (!take_run_lock(plan->lock_file, &run_fd, &error)) {
            refuse(r, NK_MIGRATE_CODE_LOCK, "Nakagawa could not lock %s (%s), which keeps two moves from "
                                            "running at once. If it belongs to another user (made under "
                                            "sudo), delete it.", plan->lock_file, strerror(error));
            return;
        }
    }
    if (!nk_platform_path_missing(plan->journal_file)) {
        refuse(r, NK_MIGRATE_CODE_INTERRUPTED, "An earlier move of your data folders was interrupted (%s "
                                               "exists). Quit Nakagawa and start it again: the start "
                                               "finishes or undoes that move first.", plan->journal_file);
    } else if (plan->marker_file[0] && !nk_platform_path_missing(plan->marker_file)) {
        r->status = NK_MIGRATE_NOTHING;
        snprintf(r->message, sizeof(r->message), "Your data is already in %s.", plan->new_root);
    } else if (!old_folder_with_files(plan)) {
        r->status = NK_MIGRATE_NOTHING;
        snprintf(r->message, sizeof(r->message), "No earlier folder holds files: there is nothing to move.");
    } else if (plan->player_lock_file[0]) {
        /* No other player of this version: this one gives up its shared hold and takes it alone. */
        nk_data_migrate_release_player_lock();
        int error = 0;
        int player_fd = player_lock_exclusive(plan->player_lock_file, &error);
        if (player_fd < 0) {
            if (error == EWOULDBLOCK) {
                refuse(r, NK_MIGRATE_CODE_IN_USE, "Another Nakagawa window is open (it holds %s). Close "
                                                  "every other Nakagawa window and game, then try again.",
                       plan->player_lock_file);
            } else {
                refuse(r, NK_MIGRATE_CODE_LOCK, "Nakagawa could not lock %s (%s).", plan->player_lock_file,
                       strerror(error));
            }
        } else {
            move_checked(plan, r);
            release_lock(player_fd);
        }
        (void)player_lock_shared(plan->player_lock_file);
    } else {
        move_checked(plan, r);
    }
    release_lock(run_fd);
}

#endif
