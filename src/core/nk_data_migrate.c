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
static NkMigrateSyncDirFn s_sync_dir = NULL;
static long s_euid_for_test = -1;

void nk_data_migrate_set_rename_for_test(NkMigrateRenameFn rename_fn) { s_rename = rename_fn; }
void nk_data_migrate_set_sync_dir_for_test(NkMigrateSyncDirFn sync_fn) { s_sync_dir = sync_fn; }
void nk_data_migrate_set_euid_for_test(long euid) { s_euid_for_test = euid; }

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
    /* The player: it holds the in-use lock for its life, and looks for older copies of itself. */
    plan.hold_in_use = true;
    plan.scan_processes = true;
    char exe[NK_MAX_PATH];
    if (nk_platform_get_executable_path(exe, sizeof(exe))) {
        const char *name = strrchr(exe, '/');
        name = name ? name + 1 : exe;
        if (strlen(name) < sizeof(plan.instance_name)) {
            memcpy(plan.instance_name, name, strlen(name) + 1u);
        }
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

void nk_data_migrate_release_in_use(void) {}

#else

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
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
/* The relative-link check walks every folder that moves; deeper than this, it cannot rule a
   link out, and the folders are kept. */
#define MIGRATE_LINK_SCAN_DEPTH 64
#define MIGRATE_RECORD_MAX_BYTES (64u * 1024u)

/* The shared in-use lock a run kept for this process (plan.hold_in_use). */
static int s_in_use_fd = -1;

void nk_data_migrate_release_in_use(void) {
    if (s_in_use_fd >= 0) {
        close(s_in_use_fd);
        s_in_use_fd = -1;
    }
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

static bool take_lock(const char *path, int *fd_out, int *error) {
    *fd_out = -1;
    int fd = open_lock_file(path, error);
    if (fd < 0) return false;
    if (!lock_now(fd, LOCK_EX)) {
        if (errno != EWOULDBLOCK) {
            *error = errno;
            close(fd);
            return false;
        }
        /* Waits for as long as another start holds it; the system drops a holder's lock when it
           ends. Said first, so a wait behind a slow disk is not a silent one. */
        fprintf(stderr, "[nk] %s: another Nakagawa start is moving the data folders; waiting for it "
                        "to finish (%s).\n", NK_MIGRATE_CODE_LOCK, path);
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

static void flush_parent(NkDataMigration *r, const char *path) {
    char dir[NK_MAX_PATH];
    if (parent_dir(path, dir, sizeof(dir)) && !do_sync_dir(dir)) {
        log_line(r, "%s: the folder %s could not be flushed to the disk after a move (%s).",
                 NK_MIGRATE_CODE_NOT_FLUSHED, dir, strerror(errno));
    }
}

/* ---- Is anything else using the old folders? (B1 of the second #324 review) ---- */

/* Another open file holds an exclusive flock on `path` (the older version's library and build
   locks). Probed with a shared lock, released at once. */
static bool flock_held(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    bool held = !lock_now(fd, LOCK_SH) && errno == EWOULDBLOCK;
    close(fd);
    return held;
}

/* Another process holds an fcntl lock on `path` (the older version's staging lock). */
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

/* The locks the version before #324 takes in its data folder: locks/<name>.lock (flock: the library
   and package builds) and games/.staging_<disc>.lock (fcntl). True, with what in `what`, when one is
   held. */
static bool old_locks_held(const char *data_dir, char *what, size_t cap) {
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

#if defined(__linux__)
/* `link` (a /proc symbolic link) points into one of `folders` (canonical paths). */
static bool proc_link_inside(const char *link, const char (*folders)[NK_MAX_PATH], size_t count,
                             char *target, size_t cap) {
    ssize_t n = readlink(link, target, cap - 1u);
    if (n <= 0 || (size_t)n >= cap - 1u) return false;
    target[n] = '\0';
    if (target[0] != '/') return false; /* pipes, sockets, anonymous memory */
    for (size_t i = 0; i < count; i++) {
        if (same_or_inside(target, folders[i])) return true;
    }
    return false;
}

/* A process of this user, other than this one, whose program, working folder or open file is in
   one of `folders`, or that runs a program called `instance` (an older copy of the player). */
static bool processes_using(const char (*folders)[NK_MAX_PATH], size_t count, const char *instance,
                            char *what, size_t cap) {
    DIR *proc = opendir("/proc");
    if (!proc) return false;
    const pid_t self = getpid();
    const uid_t uid = geteuid();
    bool found = false;
    char path[64];
    static char target[PATH_MAX];
    for (struct dirent *item = readdir(proc); item && !found; item = readdir(proc)) {
        char *end = NULL;
        long pid = strtol(item->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0 || (pid_t)pid == self) continue;
        struct stat st;
        snprintf(path, sizeof(path), "/proc/%ld", pid);
        if (stat(path, &st) != 0 || st.st_uid != uid) continue;
        snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
        ssize_t n = readlink(path, target, sizeof(target) - 1u);
        if (n > 0 && (size_t)n < sizeof(target) - 1u) {
            target[n] = '\0';
            char *deleted = strstr(target, " (deleted)");
            if (deleted && deleted[10] == '\0') *deleted = '\0'; /* replaced by an update */
            const char *name = strrchr(target, '/');
            name = name ? name + 1 : target;
            if (instance && instance[0] && strcmp(name, instance) == 0) {
                snprintf(what, cap, "process %ld runs %s, another copy of this program", pid, target);
                found = true;
                break;
            }
            for (size_t i = 0; i < count && !found; i++) {
                if (same_or_inside(target, folders[i])) {
                    snprintf(what, cap, "process %ld runs %s, from an earlier folder", pid, target);
                    found = true;
                }
            }
            if (found) break;
        }
        snprintf(path, sizeof(path), "/proc/%ld/cwd", pid);
        if (proc_link_inside(path, folders, count, target, sizeof(target))) {
            snprintf(what, cap, "process %ld works in %s", pid, target);
            found = true;
            break;
        }
        snprintf(path, sizeof(path), "/proc/%ld/fd", pid);
        DIR *fds = opendir(path);
        if (!fds) continue;
        for (struct dirent *fd = readdir(fds); fd && !found; fd = readdir(fds)) {
            if (fd->d_name[0] == '.') continue;
            char link[320];
            snprintf(link, sizeof(link), "/proc/%ld/fd/%s", pid, fd->d_name);
            if (proc_link_inside(link, folders, count, target, sizeof(target))) {
                snprintf(what, cap, "process %ld has %s open", pid, target);
                found = true;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return found;
}
#endif

/* ---- Relative links that would point elsewhere after a move (S3 of the second review) ---- */

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

/* Writes `path` whole or not at all: a temporary file beside it, synced, then renamed over it. Once
   the rename has happened the new file is the one in place: a folder flush that fails after it
   (durability unknown) is a named log line, not a failure (S4 of the second review). */
static bool write_atomic(NkDataMigration *r, const char *path, const char *suffix, const char *data,
                         size_t size) {
    char temp[NK_MAX_PATH];
    int written = snprintf(temp, sizeof(temp), "%s%s", path, suffix);
    if (written <= 0 || (size_t)written >= sizeof(temp)) return false;
    bool ok = write_whole(temp, data, size) && nk_rename_utf8(temp, path) == 0;
    if (!ok) {
        (void)nk_remove_utf8(temp);
        return false;
    }
    char dir[NK_MAX_PATH];
    if (parent_dir(path, dir, sizeof(dir)) && !do_sync_dir(dir)) {
        log_line(r, "%s: the folder %s could not be flushed to the disk after %s was written (%s); "
                    "the file is in place.",
                 NK_MIGRATE_CODE_NOT_FLUSHED, dir, path, strerror(errno));
    }
    return true;
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

/* The old data folder in the two spellings a stored path is compared in (S2 of the second review):
   lexically cleaned, and with links resolved (taken before the first move, while it exists). */
typedef struct {
    bool active;
    char lexical[NK_MAX_PATH];
    char resolved[NK_MAX_PATH];
    char to[NK_MAX_PATH];
} Remap;

static bool remap_init(const NkDataMigrationPlan *plan, Remap *m) {
    memset(m, 0, sizeof(*m));
    if (!plan->remap_from[0] || !plan->remap_to[0] || !plan->library_file[0]) return true;
    if (!lexical_clean(plan->remap_from, m->lexical, sizeof(m->lexical)) ||
        !canonical_path(plan->remap_from, m->resolved, sizeof(m->resolved)) ||
        !lexical_clean(plan->remap_to, m->to, sizeof(m->to))) {
        return false;
    }
    /* A folder at the top of the tree would match every path: never one Nakagawa used. */
    m->active = strlen(m->lexical) > 1u && strlen(m->resolved) > 1u;
    return true;
}

/* The part of the absolute path `value` below the old data folder ("" for the folder itself, else
   from its '/'), or NULL when `value` is not there. `scratch` holds the spelling that matched. */
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
   practice). With `out`, writes `data` to it with each string that names the old data folder, or a
   path below it, rewritten to the same path below the new one; every other byte is kept. Each
   string is decoded before it is compared and the new value is escaped as the library writes it, so
   a folder name with a quote or a backslash is found and stays valid JSON. Without `out`, only
   counts. */
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

/* Rewrites library.json's stored paths into the old data folder (remap_strings) under the library's
   own lock, which every writer of the file takes: held by the caller from before the first move
   (`held`), else taken here. The rewritten file is read back, and a path that still names the old
   folder fails the rewrite. A library a newer Nakagawa wrote (schema_version above
   NK_LIBRARY_SCHEMA_VERSION) is not touched: LIB_NEWER. */
/* The library's own lock (locks/library.lock beside it), taken through the library's resolved path:
   nk_library_lock_acquire refuses a lock folder reached through a link, and a lock is the same
   file however its path is spelled. On failure, `why` says whether another process holds it or it
   could not be taken at all. */
static bool lock_library(NkFileLock *lock, const char *library_file, unsigned timeout_ms, char *why,
                         size_t why_size) {
    memset(lock, 0, sizeof(*lock));
    char resolved[NK_MAX_PATH], dir[NK_MAX_PATH] = "", lock_path[NK_MAX_PATH + 32];
    if (!canonical_path(library_file, resolved, sizeof(resolved))) {
        snprintf(why, why_size, "the library lock beside %s could not be located", library_file);
        return false;
    }
    errno = 0;
    if (nk_library_lock_acquire(lock, resolved, timeout_ms) == NK_OK) return true;
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

static LibResult rewrite_library(const NkDataMigrationPlan *plan, const Remap *m, bool held,
                                 NkDataMigration *r, char *why, size_t why_size) {
    if (!m->active) return LIB_UNCHANGED;
    if (nk_platform_path_missing(plan->library_file)) return LIB_UNCHANGED;
    NkFileLock lock;
    memset(&lock, 0, sizeof(lock));
    if (!held) {
        char lock_why[NK_MAX_PATH * 2];
        if (!lock_library(&lock, plan->library_file, NK_LIBRARY_LOCK_TIMEOUT_MS, lock_why, sizeof(lock_why))) {
            snprintf(why, why_size, "the library file %s could not be locked: %s (after waiting up to %u s)",
                     plan->library_file, lock_why, NK_LIBRARY_LOCK_TIMEOUT_MS / 1000u);
            return LIB_FAILED;
        }
    }
    LibResult result = LIB_FAILED;
    char *data = NULL, *check = NULL;
    size_t size = 0, check_size = 0, hits = 0;
    bool too_long = false;
    OutBuf out = {NULL, 0u, 0u, false};
    long version = nk_user_data_file_version(plan->library_file, MIGRATE_LIBRARY_MAX_BYTES);
    if (version > (long)NK_LIBRARY_SCHEMA_VERSION) {
        snprintf(why, why_size,
                 "the library file %s was written by a newer version of Nakagawa (schema_version "
                 "%ld), which this version does not change",
                 plan->library_file, version);
        result = LIB_NEWER;
        goto done;
    }
    if (!read_whole(plan->library_file, &data, &size)) {
        snprintf(why, why_size, "the library file %s could not be read", plan->library_file);
        goto done;
    }
    remap_strings(m, data, size, &out, &hits, &too_long);
    if (out.failed || too_long) {
        snprintf(why, why_size,
                 too_long ? "a path in the library file %s would be too long in the new folder"
                          : "out of memory while rewriting the library file %s",
                 plan->library_file);
        goto done;
    }
    if (hits == 0u) {
        result = LIB_UNCHANGED;
        goto done;
    }
    char backup[NK_MAX_PATH];
    int written = snprintf(backup, sizeof(backup), "%s.pre-migration", plan->library_file);
    bool ok = written > 0 && (size_t)written < sizeof(backup);
    /* The original is kept once, written whole. A backup already there is the older original when
       it parses (a run that stopped before its rewrite left it); one that does not is replaced. */
    if (ok && (nk_platform_path_missing(backup) || !backup_is_whole(backup))) {
        ok = write_atomic(r, backup, ".tmp", data, size);
    }
    if (!ok || !write_atomic(r, plan->library_file, ".migrating", out.data, out.length)) {
        snprintf(why, why_size, "the library file %s could not be rewritten", plan->library_file);
        goto done;
    }
    /* Read back: no stored path may still name the old folder. */
    if (!read_whole(plan->library_file, &check, &check_size)) {
        snprintf(why, why_size, "the rewritten library file %s could not be read back",
                 plan->library_file);
        goto done;
    }
    remap_strings(m, check, check_size, NULL, &hits, &too_long);
    if (hits != 0u) {
        snprintf(why, why_size, "%zu stored path(s) in the library file %s still name %s after it was "
                                "rewritten",
                 hits, plan->library_file, plan->remap_from);
        /* The moves are undone next: the file goes back to the paths they restore. */
        if (!write_atomic(r, plan->library_file, ".migrating", data, size)) {
            log_line(r, "%s: the library file %s could not be put back as it was; the original is "
                        "%s.",
                     NK_MIGRATE_CODE_LIBRARY_NOT_UPDATED, plan->library_file, backup);
        }
        goto done;
    }
    result = LIB_REWRITTEN;
done:
    free(data);
    free(check);
    free(out.data);
    if (!held) nk_platform_file_lock_release(&lock);
    return result;
}

/* ---- The record of a completed run ---- */

/* When a run finished, and the old folders it retired (canonical paths). */
typedef struct {
    bool present;
    long long when;
    char from[NK_MIGRATE_MAX_MOVES][NK_MAX_PATH];
    size_t count;
} DoneRecord;

static void read_record(const char *path, DoneRecord *record) {
    memset(record, 0, sizeof(*record));
    if (!path[0] || nk_platform_path_missing(path)) return;
    char *data = NULL;
    size_t size = 0;
    if (!read_whole(path, &data, &size) || size > MIGRATE_RECORD_MAX_BYTES) {
        free(data);
        return;
    }
    for (size_t i = 0; i < size;) {
        size_t end = i;
        while (end < size && data[end] != '\n') end++;
        size_t n = end - i;
        const char *line = data + i;
        if (n > 5u && memcmp(line, "time ", 5u) == 0) {
            char number[32];
            size_t digits = n - 5u < sizeof(number) - 1u ? n - 5u : sizeof(number) - 1u;
            memcpy(number, line + 5, digits);
            number[digits] = '\0';
            char *stop = NULL;
            long long when = strtoll(number, &stop, 10);
            if (stop && *stop == '\0' && when > 0) {
                record->when = when;
                record->present = true;
            }
        } else if (n > 5u && memcmp(line, "from ", 5u) == 0 && record->count < NK_MIGRATE_MAX_MOVES &&
                   n - 5u < NK_MAX_PATH) {
            memcpy(record->from[record->count], line + 5, n - 5u);
            record->from[record->count][n - 5u] = '\0';
            record->count++;
        }
        i = end + 1u;
    }
    free(data);
}

/* The record lists `folder` (compared with links resolved) as one a completed run retired. */
static bool record_lists(const DoneRecord *record, const char *folder) {
    char canonical[NK_MAX_PATH];
    if (!record->present || !canonical_path(folder, canonical, sizeof(canonical))) return false;
    for (size_t i = 0; i < record->count; i++) {
        if (strcmp(record->from[i], canonical) == 0) return true;
    }
    return false;
}

static void format_when(long long when, char *out, size_t cap) {
    time_t t = (time_t)when;
    struct tm local;
    if (!localtime_r(&t, &local) || strftime(out, cap, "%Y-%m-%d at %H:%M", &local) == 0u) {
        snprintf(out, cap, "an earlier start");
    }
}

/* Written once the folders that matter are in the new layout and the library is rewritten. */
static void write_record(const NkDataMigrationPlan *plan, const char (*canonical_from)[NK_MAX_PATH],
                         NkDataMigration *r) {
    if (!plan->done_file[0]) return;
    OutBuf out = {NULL, 0u, 0u, false};
    char line[NK_MAX_PATH + 32];
    static const char header[] =
        "Nakagawa moved its earlier folders into this one (#324). This file records that move: when\n"
        "it finished, and the earlier folders it retired. Anything in one of them now was written\n"
        "there after the move.\n";
    out_put(&out, header, sizeof(header) - 1u);
    int written = snprintf(line, sizeof(line), "time %lld\n", (long long)time(NULL));
    if (written > 0 && (size_t)written < sizeof(line)) out_put(&out, line, (size_t)written);
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (move->disposable || move->after_previous || !canonical_from[i][0]) continue;
        if (strchr(canonical_from[i], '\n')) continue; /* not a line; such a folder is not listed */
        written = snprintf(line, sizeof(line), "from %s\n", canonical_from[i]);
        if (written > 0 && (size_t)written < sizeof(line)) out_put(&out, line, (size_t)written);
    }
    if (out.failed || !write_atomic(r, plan->done_file, ".tmp", out.data, out.length)) {
        log_line(r, "%s: %s could not be written, so a later start cannot tell files an older copy of "
                    "Nakagawa writes into an earlier folder from a second copy of your data.",
                 NK_MIGRATE_CODE_RECORD_NOT_WRITTEN, plan->done_file);
    }
    free(out.data);
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
    char after[1024];  /* what the user can do; "Nothing needs doing." when empty */
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

static void keep_after(KeepReason *reason, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(reason->after, sizeof(reason->after), format, args);
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

/* An earlier start moved some folders that matter and stopped: one of them is in the new layout
   and gone from the old one, while another still waits in the old one. */
static bool earlier_move_unfinished(const NkDataMigrationPlan *plan) {
    bool moved = false, waiting = false;
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (move->disposable || move->after_previous) continue;
        bool old_has = tree_state(move->from, 0) == TREE_CONTENT;
        if (old_has) waiting = true;
        else if (dest_state(move->to) == TREE_CONTENT) moved = true;
    }
    return moved && waiting;
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
    const char *after = reason->after[0] ? reason->after : "Nothing needs doing.";
    if (new_layout_holds_data(plan, &taken)) {
        result->status = NK_MIGRATE_REFUSED;
        (void)copy_path(result->folder_a, sizeof(result->folder_a), reason->folder);
        (void)copy_path(result->folder_b, sizeof(result->folder_b), taken);
        if (earlier_move_unfinished(plan)) {
            /* N2 of the second review: a stopped run cannot be finished now, and the old layout is
               no longer whole. */
            snprintf(result->message, sizeof(result->message),
                     "%s: %s An earlier start moved part of your data into %s and stopped, so Nakagawa "
                     "can neither finish that move now nor use the earlier folders. It did not move or "
                     "delete anything. %s Then start Nakagawa again, and it finishes the move.",
                     reason->code, reason->text, plan->new_root, reason->after);
            return;
        }
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
             "folders (your library and saves stay in %s). %s",
             reason->code, reason->text, first_old_folder(plan), after);
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

/* Each move's old folder with links resolved, for the record. */
static void canonical_froms(const NkDataMigrationPlan *plan, char (*out)[NK_MAX_PATH]) {
    for (size_t i = 0; i < plan->move_count; i++) {
        if (!canonical_path(plan->moves[i].from, out[i], NK_MAX_PATH)) out[i][0] = '\0';
    }
}

/* No old folder holds data. A run stopped after its last move and before the library rewrite
   leaves stored paths into an old data folder that no longer exists; finish the rewrite. Not after
   a completed run (its record exists): then there is nothing to finish (N8). */
static void finish_earlier_run(const NkDataMigrationPlan *plan, const DoneRecord *record,
                               NkDataMigration *result) {
    result->status = NK_MIGRATE_NOTHING;
    if (record->present) return;
    if (!plan->remap_from[0] || !nk_platform_path_missing(plan->remap_from)) return;
    Remap remap;
    if (!remap_init(plan, &remap)) return;
    char why[NK_MAX_PATH + 192] = "";
    switch (rewrite_library(plan, &remap, false, result, why, sizeof(why))) {
    case LIB_REWRITTEN: {
        result->status = NK_MIGRATE_DONE;
        result->library_rewritten = true;
        snprintf(result->message, sizeof(result->message),
                 "Nakagawa finished an earlier move of your data to %s: your library's stored "
                 "paths were updated (a copy of the old file is library.json.pre-migration).",
                 plan->new_root[0] ? plan->new_root : "its new folder");
        char froms[NK_MIGRATE_MAX_MOVES][NK_MAX_PATH];
        canonical_froms(plan, froms);
        write_record(plan, (const char (*)[NK_MAX_PATH])froms, result);
        return;
    }
    case LIB_UNCHANGED:
        return;
    case LIB_NEWER:
    case LIB_FAILED:
        log_line(result, "%s: %s; its stored paths into %s were left as they are.",
                 NK_MIGRATE_CODE_LIBRARY_NOT_UPDATED, why, plan->remap_from);
        return;
    }
}

/* B1 of the second review: anything that may be using the old folders. True, with `what` set, when
   something is; on false the old library lock is held in `library_lock` (when there is an old data
   folder) until the rewrite is done. */
static bool old_folders_in_use(const NkDataMigrationPlan *plan, const MoveState *state, int in_use_fd,
                               NkFileLock *library_lock, char *what, size_t cap) {
    memset(library_lock, 0, sizeof(*library_lock));
    if (in_use_fd >= 0 && !lock_now(in_use_fd, LOCK_EX)) {
        snprintf(what, cap, "another Nakagawa of this version holds %s", plan->in_use_file);
        return true;
    }
    char old_data[NK_MAX_PATH] = "";
    struct stat st;
    if (plan->old_library_file[0] && parent_dir(plan->old_library_file, old_data, sizeof(old_data)) &&
        lstat(old_data, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (old_locks_held(old_data, what, cap)) return true;
        /* The library's own lock: held from here until the rewrite, so no writer can slip in. */
        if (!lock_library(library_lock, plan->old_library_file, 0u, what, cap)) return true;
    }
#if defined(__linux__)
    if (plan->scan_processes) {
        char folders[NK_MIGRATE_MAX_MOVES][NK_MAX_PATH];
        size_t count = 0;
        for (size_t i = 0; i < plan->move_count; i++) {
            if (!state[i].active) continue;
            if (canonical_path(state[i].now, folders[count], NK_MAX_PATH)) count++;
        }
        if (processes_using((const char (*)[NK_MAX_PATH])folders, count, plan->instance_name, what, cap)) {
            nk_platform_file_lock_release(library_lock);
            return true;
        }
    }
#else
    (void)state;
#endif
    return false;
}

static void run_locked(const NkDataMigrationPlan *plan, NkDataMigration *result, int in_use_fd) {
    MoveState state[NK_MIGRATE_MAX_MOVES];
    memset(state, 0, sizeof(state));
    KeepReason reason;
    memset(&reason, 0, sizeof(reason));
    DoneRecord record;
    read_record(plan->done_file, &record);
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
        if (move->after_previous && record.present && !state[i - 1u].active) {
            /* N6: after a completed run, a data/saves folder that appears later (a
               --user-data-root naming the new data folder) is that root's own: it stays. */
            continue;
        }
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
        struct stat followed;
        if (stat(state[i].now, &followed) != 0 && (errno == ENOENT || errno == ENOTDIR)) {
            /* N9: a link to nothing (a disk that is not mounted); lstat sees the link itself. */
            keep_reason(&reason, NK_MIGRATE_CODE_LINKED, state[i].now,
                        "Your %s folder %s is a link to %s, which does not exist now (a disk that is "
                        "not mounted?). Nakagawa does not move a linked folder.",
                        move->label, state[i].now, target);
            keep_after(&reason, "If %s is on a disk that is not mounted, mount it.", target);
        } else {
            keep_reason(&reason, NK_MIGRATE_CODE_LINKED, state[i].now,
                        "Your %s folder %s is a link to %s. Nakagawa does not move a linked folder: "
                        "moving the link can change where it points.",
                        move->label, state[i].now, target);
        }
    }

    /* The other side of B1 (second review): an old folder that appeared again after a completed
       run, beside the new folders that hold the data. Everything in it was written after the move,
       most likely by an older copy that was still running. Neither folder is merged or deleted. */
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (!state[i].active || move->disposable || move->after_previous) continue;
        if (!record_lists(&record, state[i].now) || dest_state(move->to) != TREE_CONTENT) continue;
        char when[64];
        format_when(record.when, when, sizeof(when));
        result->status = NK_MIGRATE_REFUSED;
        (void)copy_path(result->folder_a, sizeof(result->folder_a), state[i].now);
        (void)copy_path(result->folder_b, sizeof(result->folder_b), move->to);
        snprintf(result->message, sizeof(result->message),
                 "%s: Nakagawa uses your %s folder %s, where it moved it on %s. Your earlier folder "
                 "%s was made again after that move, so everything in it was written there later, "
                 "most likely by an older copy of Nakagawa (a window, a game or a tool) that was "
                 "still running. Nakagawa did not merge, move or delete anything. Open both "
                 "folders, copy anything you want to keep from %s into %s, then delete %s (or move "
                 "it outside Nakagawa's folders) and start Nakagawa again.",
                 NK_MIGRATE_CODE_LATE_WRITES, move->label, move->to, when, state[i].now,
                 state[i].now, move->to, state[i].now);
        return;
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

    bool essential_active = false;
    for (size_t i = 0; i < plan->move_count; i++) {
        const NkMigrateMove *move = &plan->moves[i];
        if (!state[i].active || move->disposable) continue;
        essential_active = true;
        if (reason.code) continue;
        /* N1: a folder of another user (sudo with HOME kept) is not moved from here. */
        struct stat st;
        if (lstat(state[i].now, &st) == 0 && (unsigned long)st.st_uid != effective_uid()) {
            keep_reason(&reason, NK_MIGRATE_CODE_OTHER_OWNER, state[i].now,
                        "Your %s folder %s belongs to user %lu, and Nakagawa runs as user %lu (for "
                        "example under sudo), so it does not move it from here.",
                        move->label, state[i].now, (unsigned long)st.st_uid, effective_uid());
            keep_after(&reason, "Start Nakagawa as yourself to move it.");
            continue;
        }
        /* S3: a relative link inside that would point elsewhere once the folder has moved. Links in
           a folder that moves on its own (Linux's data/saves) are checked against that folder. */
        static LinkScan scan;
        memset(&scan, 0, sizeof(scan));
        if (!copy_path(scan.path, sizeof(scan.path), state[i].now)) continue;
        for (size_t j = i + 1u; j < plan->move_count; j++) {
            if (state[j].active && plan->moves[j].after_previous && j == i + 1u) {
                scan.nested[scan.nested_count++] = state[j].now;
            }
        }
        if (links_stay(&scan, 0)) continue;
        if (scan.unreadable) {
            keep_reason(&reason, NK_MIGRATE_CODE_LINKED, state[i].now,
                        "Nakagawa could not look through %s, inside your %s folder %s, for links that "
                        "moving the folder would break.",
                        scan.found, move->label, state[i].now);
            keep_after(&reason, "Make %s readable (or remove it), and a later start moves your data.",
                       scan.found);
        } else {
            keep_reason(&reason, NK_MIGRATE_CODE_LINKED, state[i].now,
                        "Your %s folder %s holds a link, %s, to %s: a relative path that leads out of "
                        "the folder, so moving the folder would make it point elsewhere.",
                        move->label, state[i].now, scan.found, scan.target);
            keep_after(&reason, "To have your data moved, make that link absolute (for example with "
                                "ln -sfn and the full path), and a later start moves it.");
        }
    }

    /* A library a newer Nakagawa wrote: neither moved nor rewritten. */
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
        finish_earlier_run(plan, &record, result);
        return;
    }

    /* B1: never while anything else may be using the old folders. */
    NkFileLock library_lock;
    char what[PATH_MAX + 128];
    if (old_folders_in_use(plan, state, in_use_fd, &library_lock, what, sizeof(what))) {
        log_line(result, "%s: %s.", NK_MIGRATE_CODE_IN_USE, what);
        KeepReason busy;
        memset(&busy, 0, sizeof(busy));
        keep_reason(&busy, NK_MIGRATE_CODE_IN_USE, first_old_folder(plan),
                    "Another Nakagawa program (a window, a game or a tool, perhaps of an earlier "
                    "version) may be using your earlier folders, so they were not moved now.");
        keep_after(&busy, "Close the other Nakagawa windows, games and tools when you can; a later "
                          "start then moves your data into %s.",
                   plan->new_root);
        keep_old(plan, result, &busy);
        return;
    }

    /* The old data folder's spellings, while it still exists (S2). */
    Remap remap;
    if (!remap_init(plan, &remap)) {
        nk_platform_file_lock_release(&library_lock);
        result->status = NK_MIGRATE_FAILED;
        snprintf(result->message, sizeof(result->message),
                 "%s: the path of your data folder (%s) is too long to compare, so Nakagawa did not "
                 "move anything.",
                 NK_MIGRATE_CODE_FAILED, plan->remap_from);
        return;
    }
    char froms[NK_MIGRATE_MAX_MOVES][NK_MAX_PATH];
    canonical_froms(plan, froms);

    if (plan->new_root[0] && !nk_platform_mkdir_p(plan->new_root)) {
        nk_platform_file_lock_release(&library_lock);
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

    char why[NK_MAX_PATH * 2 + 192] = "";
    LibResult library = LIB_UNCHANGED;
    if (!failed_move) {
        library = rewrite_library(plan, &remap, library_lock.held, result, why, sizeof(why));
    }
    nk_platform_file_lock_release(&library_lock);
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
        write_record(plan, (const char (*)[NK_MAX_PATH])froms, result);
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

static void lock_failed(const NkDataMigrationPlan *plan, NkDataMigration *result, const char *file,
                        int error) {
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
                 NK_MIGRATE_CODE_LOCK, file, strerror(error));
        return;
    }
    KeepReason reason;
    memset(&reason, 0, sizeof(reason));
    keep_reason(&reason, NK_MIGRATE_CODE_LOCK, first_old_folder(plan),
                "Nakagawa could not lock %s (%s), which keeps two starts from moving your data at "
                "once, so it did not move your data.",
                file, strerror(error));
    keep_after(&reason, "If %s belongs to another user (made under sudo), delete it.", file);
    keep_old(plan, result, &reason);
}

void nk_data_migrate_run(const NkDataMigrationPlan *plan, NkDataMigration *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    /* This process is never another user of the folders for its own run. */
    nk_data_migrate_release_in_use();
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
            lock_failed(plan, result, plan->lock_file, error);
            return;
        }
    }
    int in_use_fd = -1;
    if (plan->in_use_file[0]) {
        int error = 0;
        in_use_fd = open_lock_file(plan->in_use_file, &error);
        if (in_use_fd < 0) {
            lock_failed(plan, result, plan->in_use_file, error);
            if (lock_fd >= 0) close(lock_fd);
            return;
        }
    }
    run_locked(plan, result, in_use_fd);
    if (in_use_fd >= 0) {
        /* From exclusive (when the run took it) to shared, while the run lock still keeps every
           other run out: this process now uses the folders it chose. */
        if (plan->hold_in_use && lock_now(in_use_fd, LOCK_SH)) {
            s_in_use_fd = in_use_fd;
        } else {
            if (plan->hold_in_use) {
                log_line(result, "%s: %s could not be held while Nakagawa runs (%s).",
                         NK_MIGRATE_CODE_LOCK, plan->in_use_file, strerror(errno));
            }
            close(in_use_fd);
        }
    }
    if (lock_fd >= 0) {
        (void)flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

#endif
