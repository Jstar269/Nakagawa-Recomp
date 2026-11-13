/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The data folder layout of a Linux or macOS install and the move the user starts in Settings (#324,
 * src/core/nk_data_migrate.c): the rule's cases from the table in nk_data_migrate.h (the Python
 * tools' test reads the same table), a start that never moves or blocks, each check of the move
 * refused by name, every crash point of the move and its recovery at the next start, and the move
 * itself (library rewrite, links, flushes, undo, the macOS whole-folder rename). Every folder is
 * built in a temporary root on the real file system; the real profile is never touched. The engine
 * runs on Linux and macOS only; on Windows the test checks that nothing new is taken. */

#if !defined(_WIN32) && !defined(_WIN64)
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "nk_data_migrate.h"
#include "nk_library.h"
#include "nk_platform.h"
#include "native_test_isolation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));     \
        }                                                                        \
    } while (0)

#if !defined(_WIN32) && !defined(_WIN64)

static char g_base[NATIVE_TEST_PATH_MAX];
static int g_case = 0;

/* A fresh folder per case: <base>/c<N>. */
static void new_case(char *root, size_t cap) {
    snprintf(root, cap, "%s/c%d", g_base, ++g_case);
    CHECK(nk_platform_mkdir_p(root), "make the case folder");
}

static void join(char *out, size_t cap, const char *a, const char *b) {
    snprintf(out, cap, "%s/%s", a, b);
}

static void put_file(const char *dir, const char *name, const char *text) {
    char path[NATIVE_TEST_PATH_MAX];
    CHECK(nk_platform_mkdir_p(dir), "make a folder");
    join(path, sizeof(path), dir, name);
    FILE *f = nk_fopen_utf8(path, "wb");
    CHECK(f != NULL, "write a file");
    if (!f) return;
    fwrite(text, 1u, strlen(text), f);
    fclose(f);
}

static char *slurp(const char *path) {
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return NULL;
    char *buffer = (char *)calloc(1u, 65536u);
    size_t n = buffer ? fread(buffer, 1u, 65535u, f) : 0u;
    fclose(f);
    if (buffer) buffer[n] = '\0';
    return buffer;
}

static bool file_is(const char *path, const char *want) {
    char *got = slurp(path);
    bool same = got && strcmp(got, want) == 0;
    free(got);
    return same;
}

static bool exists(const char *path) { return !nk_platform_path_missing(path); }

static bool is_link(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static bool parent_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) >= cap) return false;
    memcpy(out, path, (size_t)(slash - path));
    out[slash - path] = '\0';
    return true;
}

/* The Linux-shaped old layout under `root`: data (with a library, a converted disc and saves) and
   settings; the cache and logs, which stay where they are. The plan is built by hand as the host
   builds it (test_linux_host_plan checks that the host's matches). */
typedef struct {
    char root[NATIVE_TEST_PATH_MAX];
    char old_data[NATIVE_TEST_PATH_MAX], old_config[NATIVE_TEST_PATH_MAX];
    char cache[NATIVE_TEST_PATH_MAX], logs[NATIVE_TEST_PATH_MAX];
    char new_root[NATIVE_TEST_PATH_MAX];
    char new_data[NATIVE_TEST_PATH_MAX], new_saves[NATIVE_TEST_PATH_MAX], new_config[NATIVE_TEST_PATH_MAX];
    char library_text[1024];
} Fixture;

static void add_move(NkDataMigrationPlan *plan, const char *from, const char *to, const char *label,
                     bool after_previous, bool disposable) {
    NkMigrateMove *m = &plan->moves[plan->move_count++];
    snprintf(m->from, sizeof(m->from), "%s", from);
    snprintf(m->to, sizeof(m->to), "%s", to);
    snprintf(m->label, sizeof(m->label), "%s", label);
    m->after_previous = after_previous;
    m->disposable = disposable;
}

/* The locks, journal and marker where the host puts them: beside the new root, and in it. */
static void plan_files(NkDataMigrationPlan *plan, const char *base) {
    snprintf(plan->base, sizeof(plan->base), "%s", base);
    snprintf(plan->lock_file, sizeof(plan->lock_file), "%s/.nakagawa-migration.lock", base);
    snprintf(plan->player_lock_file, sizeof(plan->player_lock_file), "%s/.nakagawa-player.lock", base);
    snprintf(plan->journal_file, sizeof(plan->journal_file), "%s/.nakagawa-move-in-progress", base);
    snprintf(plan->marker_file, sizeof(plan->marker_file), "%s/.nakagawa-layout", plan->new_root);
}

static void build_plan(const Fixture *fx, NkDataMigrationPlan *plan) {
    memset(plan, 0, sizeof(*plan));
    snprintf(plan->new_root, sizeof(plan->new_root), "%s", fx->new_root);
    char data_saves[NATIVE_TEST_PATH_MAX];
    snprintf(data_saves, sizeof(data_saves), "%s/saves", fx->new_data);
    add_move(plan, fx->old_data, fx->new_data, "data", false, false);
    add_move(plan, data_saves, fx->new_saves, "saves", true, false);
    add_move(plan, fx->old_config, fx->new_config, "settings", false, false);
    snprintf(plan->kept[0], sizeof(plan->kept[0]), "%s", fx->cache);
    snprintf(plan->kept_label[0], sizeof(plan->kept_label[0]), "cache");
    snprintf(plan->kept[1], sizeof(plan->kept[1]), "%s", fx->logs);
    snprintf(plan->kept_label[1], sizeof(plan->kept_label[1]), "logs");
    plan->kept_count = 2u;
    snprintf(plan->remap_from, sizeof(plan->remap_from), "%s", fx->old_data);
    snprintf(plan->remap_to, sizeof(plan->remap_to), "%s", fx->new_data);
    snprintf(plan->library_file, sizeof(plan->library_file), "%s/library.json", fx->new_data);
    snprintf(plan->old_library_file, sizeof(plan->old_library_file), "%s/library.json", fx->old_data);
    char base[NATIVE_TEST_PATH_MAX];
    join(base, sizeof(base), fx->root, "share");
    plan_files(plan, base);
}

static void fixture_paths(Fixture *fx) {
    new_case(fx->root, sizeof(fx->root));
    join(fx->old_data, sizeof(fx->old_data), fx->root, "share/nakagawa-recomp");
    join(fx->old_config, sizeof(fx->old_config), fx->root, "config/nakagawa-recomp");
    join(fx->cache, sizeof(fx->cache), fx->root, "cache/nakagawa-recomp");
    join(fx->logs, sizeof(fx->logs), fx->root, "state/nakagawa-recomp/logs");
    join(fx->new_root, sizeof(fx->new_root), fx->root, "share/Nakagawa");
    join(fx->new_data, sizeof(fx->new_data), fx->new_root, "data");
    join(fx->new_saves, sizeof(fx->new_saves), fx->new_root, "saves");
    join(fx->new_config, sizeof(fx->new_config), fx->new_root, "config");
    snprintf(fx->library_text, sizeof(fx->library_text),
             "{\"games\":[{\"disc_id\":\"UCUS98744\",\"iso_path\":\"%s/discs/UCUS98744/UCUS98744.iso\","
             "\"source_image_path\":\"/external/games/lbp.iso\"},"
             "{\"iso_path\":\"%s-old/not-ours.iso\"}]}",
             fx->old_data, fx->old_data);
}

static void build_fixture(Fixture *fx, bool with_old) {
    fixture_paths(fx);
    char dir[NATIVE_TEST_PATH_MAX];
    join(dir, sizeof(dir), fx->root, "share");
    CHECK(nk_platform_mkdir_p(dir), "make the data base");
    if (!with_old) return;
    put_file(fx->old_data, "library.json", fx->library_text);
    snprintf(dir, sizeof(dir), "%s/discs/UCUS98744", fx->old_data);
    put_file(dir, "UCUS98744.iso", "disc bytes");
    snprintf(dir, sizeof(dir), "%s/saves/UCUS98744", fx->old_data);
    put_file(dir, "SAVE.bin", "save bytes");
    put_file(fx->old_config, "settings.json", "{\"a\":1}");
    put_file(fx->cache, "objects.bin", "cache");
    put_file(fx->logs, "game-last.txt", "log");
}

static bool library_names_new_folder(const NkDataMigrationPlan *plan) {
    char *library = slurp(plan->library_file);
    char want[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want, sizeof(want), "\"%s/discs/UCUS98744/UCUS98744.iso\"", plan->remap_to);
    bool ok = library && strstr(library, want) != NULL;
    free(library);
    return ok;
}

/* Everything the old layout held is in the new one, once, and the move is committed. */
static void check_migrated(const Fixture *fx, const char *what) {
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx->new_data, "discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), what);
    join(path, sizeof(path), fx->new_saves, "UCUS98744/SAVE.bin");
    CHECK(file_is(path, "save bytes"), what);
    join(path, sizeof(path), fx->new_config, "settings.json");
    CHECK(file_is(path, "{\"a\":1}"), what);
    join(path, sizeof(path), fx->new_data, "saves");
    CHECK(!exists(path), what);
    CHECK(!exists(fx->old_data) && !exists(fx->old_config), what);
    join(path, sizeof(path), fx->new_root, ".nakagawa-layout");
    CHECK(exists(path), what);
    join(path, sizeof(path), fx->root, "share/.nakagawa-move-in-progress");
    CHECK(!exists(path), what);
}

/* Everything is where it was before the move, and no move is pending. */
static void check_untouched(const Fixture *fx, const char *what) {
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx->old_data, "discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), what);
    join(path, sizeof(path), fx->old_data, "saves/UCUS98744/SAVE.bin");
    CHECK(file_is(path, "save bytes"), what);
    join(path, sizeof(path), fx->old_data, "library.json");
    CHECK(file_is(path, fx->library_text), what);
    join(path, sizeof(path), fx->old_config, "settings.json");
    CHECK(file_is(path, "{\"a\":1}"), what);
    join(path, sizeof(path), fx->root, "share/.nakagawa-move-in-progress");
    CHECK(!exists(path), what);
    join(path, sizeof(path), fx->new_root, ".nakagawa-layout");
    CHECK(!exists(path), what);
    CHECK(!exists(fx->new_data), what);
}

static void decide(const NkDataMigrationPlan *plan, NkLayoutDecision *d) { nk_data_layout_decide(plan, d); }

static void move(const NkDataMigrationPlan *plan, NkDataMigration *r) { nk_data_migrate_move(plan, false, r); }

static bool starts_with(const char *text, const char *code) { return strncmp(text, code, strlen(code)) == 0; }

/* ---- Rename seam ---- */

static int g_rename_calls = 0;
static int g_fail_at = 0;        /* fail the Nth rename (1-based), 0 never */
static int g_fail_also = 0;      /* and this one */
static bool g_fail_all_after = false;
static int g_fail_errno = EACCES;
static void (*g_before_rename)(int call, const char *from, const char *to) = NULL;

static int counting_rename(const char *from, const char *to) {
    g_rename_calls++;
    if (g_before_rename) g_before_rename(g_rename_calls, from, to);
    if ((g_fail_at && (g_rename_calls == g_fail_at || (g_fail_all_after && g_rename_calls > g_fail_at))) ||
        (g_fail_also && g_rename_calls == g_fail_also)) {
        errno = g_fail_errno;
        return -1;
    }
    return nk_rename_utf8(from, to);
}

static void use_seam(int fail_at, bool fail_all_after) {
    g_rename_calls = 0;
    g_fail_at = fail_at;
    g_fail_also = 0;
    g_fail_all_after = fail_all_after;
    g_fail_errno = EACCES;
    g_before_rename = NULL;
    nk_data_migrate_set_rename_for_test(counting_rename);
}

static void drop_seam(void) {
    nk_data_migrate_set_rename_for_test(NULL);
    g_before_rename = NULL;
}

/* ---- Another process (a second window, an older copy) ---- */

typedef enum { HOLD_FLOCK, HOLD_FLOCK_SHARED, HOLD_FCNTL } HoldKind;

typedef struct {
    pid_t pid;
    int release; /* closing it lets the process end */
} Holder;

/* A second process that really holds a lock on `path` until let_go. */
static Holder hold_in_child(HoldKind kind, const char *path) {
    Holder holder = {-1, -1};
    int ready[2], release[2];
    if (pipe(ready) != 0) return holder;
    if (pipe(release) != 0) {
        close(ready[0]);
        close(ready[1]);
        return holder;
    }
    nk_data_migrate_release_player_lock(); /* the child must not inherit this process's own lock */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(release[1]);
        bool ok;
        int fd = open(path, O_RDWR | O_CREAT, 0600);
        if (kind == HOLD_FCNTL) {
            struct flock region;
            memset(&region, 0, sizeof(region));
            region.l_type = F_WRLCK;
            region.l_whence = SEEK_SET;
            ok = fd >= 0 && fcntl(fd, F_SETLK, &region) == 0;
        } else {
            ok = fd >= 0 && flock(fd, kind == HOLD_FLOCK ? LOCK_EX : LOCK_SH) == 0;
        }
        char byte = ok ? 'y' : 'n';
        ssize_t sent = write(ready[1], &byte, 1u);
        ssize_t got = read(release[0], &byte, 1u); /* 0 when the parent closes it */
        (void)sent;
        (void)got;
        _exit(0);
    }
    close(ready[1]);
    close(release[0]);
    char byte = 'n';
    ssize_t got = pid > 0 ? read(ready[0], &byte, 1u) : -1;
    close(ready[0]);
    CHECK(pid > 0 && got == 1 && byte == 'y', "a second process holds the lock");
    holder.pid = pid;
    holder.release = release[1];
    return holder;
}

static void let_go(Holder *holder) {
    if (holder->release >= 0) close(holder->release);
    if (holder->pid > 0) (void)waitpid(holder->pid, NULL, 0);
    holder->release = -1;
    holder->pid = -1;
}

/* ---- Crashes ---- */

static const char *g_kill_at = NULL;

static void kill_at_point(const char *name) {
    if (g_kill_at && strcmp(name, g_kill_at) == 0) _exit(0);
}

/* A player in a child process starts (the rule), then moves, and its process ends at the named point
   as a crash or a power cut would end it: no cleanup runs. `fail_rename_at` makes that rename fail
   first. True when the child ended at that point. */
static bool move_killed_at(const NkDataMigrationPlan *plan, const char *at, int fail_rename_at) {
    nk_data_migrate_release_player_lock();
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        g_kill_at = at;
        nk_data_migrate_set_point_hook_for_test(kill_at_point);
        if (fail_rename_at) use_seam(fail_rename_at, false);
        NkLayoutDecision d;
        nk_data_layout_decide(plan, &d);
        NkDataMigration r;
        nk_data_migrate_move(plan, false, &r);
        _exit(10 + (int)r.status);
    }
    int status = 0;
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ---- The rule, from the table both test suites read ---- */

typedef struct {
    char name[64], journal[16], marker[16], old_data[16], old_config[16], new_data[16], player[16], tools[16],
        notice[16];
} RuleCase;

/* The path of nk_data_migrate.h, from this file's own: the table is read from the source, so the
   test and the header cannot drift apart. */
static bool header_path(char *out, size_t cap) {
    const char *self = __FILE__;
    const char *tail = strstr(self, "tests/native/test_data_migrate.c");
    if (!tail) return false;
    int written = snprintf(out, cap, "%.*ssrc/core/nk_data_migrate.h", (int)(tail - self), self);
    return written > 0 && (size_t)written < cap;
}

static size_t read_rule_cases(RuleCase *cases, size_t cap) {
    char path[NATIVE_TEST_PATH_MAX];
    char *text = header_path(path, sizeof(path)) ? slurp(path) : NULL;
    CHECK(text != NULL, "read the rule table from nk_data_migrate.h");
    if (!text) return 0u;
    const char *begin = strstr(text, "BEGIN DATA FOLDER RULE CASES");
    const char *end = begin ? strstr(begin, "END DATA FOLDER RULE CASES") : NULL;
    size_t count = 0;
    bool header = true;
    for (const char *line = begin ? strchr(begin, '\n') : NULL; line && end && line + 1 < end && count < cap;
         line = strchr(line + 1, '\n')) {
        /* One row at a time: sscanf would otherwise run on into the lines below. */
        char row[256];
        const char *stop = strchr(line + 1, '\n');
        size_t length = stop ? (size_t)(stop - (line + 1)) : strlen(line + 1);
        if (length >= sizeof(row)) continue;
        memcpy(row, line + 1, length);
        row[length] = '\0';
        if (strstr(row, "END DATA FOLDER RULE CASES")) break;
        RuleCase c;
        memset(&c, 0, sizeof(c));
        int got = sscanf(row, " * %63s %15s %15s %15s %15s %15s %15s %15s %15s", c.name, c.journal, c.marker,
                         c.old_data, c.old_config, c.new_data, c.player, c.tools, c.notice);
        if (got != 9) continue;
        if (header) {
            CHECK(strcmp(c.name, "case") == 0 && strcmp(c.notice, "notice") == 0, "the table's header row");
            header = false;
            continue;
        }
        cases[count++] = c;
    }
    free(text);
    return count;
}

static void make_state(const char *path, const char *state, const char *file) {
    if (strcmp(state, "empty") == 0) {
        char inner[NATIVE_TEST_PATH_MAX];
        join(inner, sizeof(inner), path, "empty-inside");
        CHECK(nk_platform_mkdir_p(inner), "an empty folder (with an empty folder in it)");
    } else if (strcmp(state, "files") == 0) {
        put_file(path, file, "{}");
    } else {
        CHECK(strcmp(state, "none") == 0, "a known folder state");
    }
}

static void test_rule_table(void) {
    RuleCase cases[32];
    size_t count = read_rule_cases(cases, 32u);
    CHECK(count >= 12u, "the table has the rule's cases");
    for (size_t i = 0; i < count; i++) {
        const RuleCase *c = &cases[i];
        Fixture fx;
        NkDataMigrationPlan plan;
        bool interrupted = strcmp(c->journal, "yes") == 0;
        if (interrupted) {
            build_fixture(&fx, true);
            build_plan(&fx, &plan);
            CHECK(move_killed_at(&plan, "journal", 0), c->name);
        } else {
            build_fixture(&fx, false);
            build_plan(&fx, &plan);
            make_state(fx.old_data, c->old_data, "library.json");
            make_state(fx.old_config, c->old_config, "settings.json");
            make_state(fx.new_data, c->new_data, "library.json");
            if (strcmp(c->marker, "yes") == 0) put_file(fx.new_root, ".nakagawa-layout", "marker");
        }
        NkLayoutDecision d;
        decide(&plan, &d);
        const NkLayoutChoice want = strcmp(c->player, "old") == 0 ? NK_LAYOUT_OLD : NK_LAYOUT_NEW;
        CHECK(strcmp(c->player, "old") == 0 || strcmp(c->player, "new") == 0, c->name);
        if (d.layout != want) fprintf(stderr, "case %s: layout %d, rule %s\n%s", c->name, (int)d.layout, d.rule, d.log);
        CHECK(d.layout == want, c->name);
        const NkLayoutNotice notice = strcmp(c->notice, "both") == 0       ? NK_LAYOUT_NOTICE_BOTH
                                      : strcmp(c->notice, "returned") == 0 ? NK_LAYOUT_NOTICE_RETURNED
                                                                           : NK_LAYOUT_NOTICE_NONE;
        CHECK(d.notice_kind == notice, c->name);
        if (notice == NK_LAYOUT_NOTICE_BOTH) {
            CHECK(starts_with(d.notice, NK_LAYOUT_CODE_BOTH) && strstr(d.notice, fx.new_root) != NULL &&
                      strstr(d.notice, fx.old_data) != NULL,
                  "the notice names both folders, the one in use first");
        } else if (notice == NK_LAYOUT_NOTICE_RETURNED) {
            CHECK(starts_with(d.notice, NK_LAYOUT_CODE_RETURNED) && strstr(d.notice, fx.new_root) != NULL &&
                      strstr(d.notice, strcmp(c->old_data, "files") == 0 ? fx.old_data : fx.old_config) != NULL &&
                      strstr(d.notice, "does not use them") != NULL,
                  "the notice names the folder in use and the old one, whose files are not used");
            CHECK(strstr(d.notice, "copy") == NULL && strstr(d.notice, "delete it") == NULL,
                  "and gives no instruction that could put a file where Nakagawa does not read it");
        } else {
            CHECK(d.notice[0] == '\0', c->name);
        }
        CHECK(d.can_move == (want == NK_LAYOUT_OLD), "the move is offered only while the old layout is in use");
        /* A start never moves anything: what was put there is still there. */
        char path[NATIVE_TEST_PATH_MAX];
        if (!interrupted) {
            join(path, sizeof(path), fx.old_data, "library.json");
            CHECK(exists(path) == (strcmp(c->old_data, "files") == 0), "the old data is as it was");
            join(path, sizeof(path), fx.old_config, "settings.json");
            CHECK(exists(path) == (strcmp(c->old_config, "files") == 0), "the old settings are as they were");
            join(path, sizeof(path), fx.new_data, "library.json");
            CHECK(exists(path) == (strcmp(c->new_data, "files") == 0), "the new data is as it was");
        } else {
            CHECK(d.recovered && strcmp(d.rule, "journal") == 0, "an interrupted move is finished first");
            check_migrated(&fx, "and all of it is in the new layout");
        }
        /* The new layout is fixed by the marker once it is chosen. */
        join(path, sizeof(path), fx.new_root, ".nakagawa-layout");
        CHECK(exists(path) == (want == NK_LAYOUT_NEW), "the marker exists exactly when the new layout is in use");
        NkLayoutDecision again;
        decide(&plan, &again);
        CHECK(again.layout == d.layout && again.notice_kind == d.notice_kind, "a second start decides the same");
    }
}

/* A start on the old layout never moves or waits, whatever holds the old folders: an older copy
   holding its library lock, a library a newer version wrote, a linked data folder. */
static void test_start_never_moves_or_blocks(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    char lock[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
    join(lock, sizeof(lock), fx.old_data, "locks/library.lock");
    CHECK(parent_of(lock, dir, sizeof(dir)) && nk_platform_mkdir_p(dir), "make the lock folder");
    Holder holder = hold_in_child(HOLD_FLOCK, lock);
    NkLayoutDecision d;
    decide(&plan, &d);
    CHECK(d.layout == NK_LAYOUT_OLD && strcmp(d.rule, "old") == 0 && d.message[0] == '\0',
          "an older copy writing the library does not stop or delay the start");
    let_go(&holder);
    check_untouched(&fx, "and nothing moved");
}

/* ---- The move ---- */

static void test_move(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkLayoutDecision d;
    decide(&plan, &d);
    CHECK(d.layout == NK_LAYOUT_OLD && d.can_move, "an existing install keeps its folders and is offered the move");
    NkDataMigration r;
    move(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.must_restart, "the move is done, and the player must restart");
    CHECK(r.moved == 3u, "three folders moved (data, its saves, settings)");
    CHECK(r.library_rewritten, "the library paths were rewritten");
    check_migrated(&fx, "data, saves and settings are in the new layout");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.cache, "objects.bin");
    CHECK(file_is(path, "cache"), "the cache stays in the XDG cache folder");
    join(path, sizeof(path), fx.logs, "game-last.txt");
    CHECK(file_is(path, "log"), "the logs stay in the XDG state folder");
    CHECK(library_names_new_folder(&plan), "a path under the old data folder now points at the new one");
    char *library = slurp(plan.library_file);
    CHECK(library && strstr(library, "\"/external/games/lbp.iso\"") != NULL, "a path elsewhere is untouched");
    char lookalike[NATIVE_TEST_PATH_MAX + 64];
    snprintf(lookalike, sizeof(lookalike), "\"%s-old/not-ours.iso\"", fx.old_data);
    CHECK(library && strstr(library, lookalike) != NULL, "a folder that only starts like the old one is untouched");
    free(library);
    snprintf(path, sizeof(path), "%s.pre-migration", plan.library_file);
    CHECK(file_is(path, fx.library_text), "the original library is kept");
    snprintf(path, sizeof(path), "%s.pre-migration.tmp", plan.library_file);
    CHECK(!exists(path), "and no temporary file is left");

    decide(&plan, &d);
    CHECK(d.layout == NK_LAYOUT_NEW && strcmp(d.rule, "marker") == 0 && !d.can_move && !d.recovered,
          "the next start uses the new layout, by the marker");
    NkDataMigration again;
    move(&plan, &again);
    CHECK(again.status == NK_MIGRATE_NOTHING, "a second move finds nothing to do");
    join(path, sizeof(path), fx.new_root, "data/discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "and leaves the data alone");
}

static void test_old_without_saves(void) {
    Fixture fx;
    build_fixture(&fx, false);
    put_file(fx.old_data, "library.json", fx.library_text);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    move(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 1u, "an old data folder with no saves or settings still moves");
}

static void test_empty_new_folders_do_not_block(void) {
    /* A launch before the move may have created the new folders empty. */
    Fixture fx;
    build_fixture(&fx, true);
    const char *leaves[] = {"data", "config", "saves", "data/titles/a/b"};
    for (size_t i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
        char dir[NATIVE_TEST_PATH_MAX];
        join(dir, sizeof(dir), fx.new_root, leaves[i]);
        CHECK(nk_platform_mkdir_p(dir), "make an empty new folder");
    }
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    move(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "empty folders in the way are replaced and the move goes ahead");
    check_migrated(&fx, "the data arrived");
}

/* Each check refuses by name, before anything is touched; the start on the old layout goes on. */
static void check_refused(const Fixture *fx, const NkDataMigration *r, const char *code, const char *what) {
    CHECK(r->status == NK_MIGRATE_REFUSED && starts_with(r->message, code), what);
    if (r->status != NK_MIGRATE_REFUSED || !starts_with(r->message, code)) fprintf(stderr, "  got: %s\n", r->message);
    CHECK(strstr(r->message, "Nothing was moved or deleted.") != NULL && !r->must_restart, "and says nothing moved");
    check_untouched(fx, what);
}

static void test_move_refuses_by_name(void) {
    /* A game launched from this player is running. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        NkDataMigration r;
        nk_data_migrate_move(&plan, true, &r);
        check_refused(&fx, &r, NK_MIGRATE_CODE_GAME_RUNNING, "a running game refuses the move");
    }
    /* Another window of this version holds the player lock. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        Holder holder = hold_in_child(HOLD_FLOCK_SHARED, plan.player_lock_file);
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_OLD, "this window starts beside the other one");
        NkDataMigration r;
        move(&plan, &r);
        check_refused(&fx, &r, NK_MIGRATE_CODE_IN_USE, "another open window refuses the move");
        CHECK(strstr(r.message, plan.player_lock_file) != NULL && strstr(r.message, "Close every other") != NULL,
              "naming the lock and what to close");
        let_go(&holder);
        move(&plan, &r);
        CHECK(r.status == NK_MIGRATE_DONE, "once it has closed, the move runs");
        int fd = open(plan.player_lock_file, O_RDWR | O_CLOEXEC);
        bool shared = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK &&
                      flock(fd, LOCK_SH | LOCK_NB) == 0;
        if (fd >= 0) close(fd);
        CHECK(shared, "and this player holds its lock shared again afterwards");
        nk_data_migrate_release_player_lock();
    }
    /* The locks of this and the previous version in the old data folder. */
    static const struct {
        HoldKind kind;
        const char *file;
        const char *what;
    } locks[] = {
        {HOLD_FLOCK, "locks/library.lock", "a player or tool writing the library refuses the move"},
        {HOLD_FLOCK, "locks/build-package-UCUS98744.lock", "a tool building a package refuses the move"},
        {HOLD_FCNTL, "games/.staging_UCUS98744.lock", "a player staging a game refuses the move"},
    };
    for (size_t c = 0; c < sizeof(locks) / sizeof(locks[0]); c++) {
        Fixture fx;
        build_fixture(&fx, true);
        char path[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
        join(path, sizeof(path), fx.old_data, locks[c].file);
        CHECK(parent_of(path, dir, sizeof(dir)) && nk_platform_mkdir_p(dir), "make the lock folder");
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        Holder holder = hold_in_child(locks[c].kind, path);
        NkDataMigration r;
        move(&plan, &r);
        check_refused(&fx, &r, NK_MIGRATE_CODE_LOCK, locks[c].what);
        CHECK(strstr(r.message, path) != NULL, "naming the lock");
        let_go(&holder);
        move(&plan, &r);
        CHECK(r.status == NK_MIGRATE_DONE, "once that process has ended, the move runs");
    }
    /* A data folder that is a link (GNU stow makes relative ones), and one to a disk not mounted. */
    for (int c = 0; c < 3; c++) {
        Fixture fx;
        build_fixture(&fx, false);
        char real[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
        join(real, sizeof(real), fx.root, "games/nk");
        put_file(real, "library.json", fx.library_text);
        snprintf(dir, sizeof(dir), "%s/saves/UCUS98744", real);
        put_file(dir, "SAVE.bin", "save bytes");
        snprintf(dir, sizeof(dir), "%s/discs/UCUS98744", real);
        put_file(dir, "UCUS98744.iso", "disc bytes");
        const char *target = c == 0 ? "../games/nk" : c == 1 ? real : "/nonexistent-disk/nakagawa";
        CHECK(symlink(target, fx.old_data) == 0, "link the old data folder");
        put_file(fx.old_config, "settings.json", "{\"a\":1}");
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_OLD, "a linked data folder is used where it is");
        NkDataMigration r;
        move(&plan, &r);
        CHECK(r.status == NK_MIGRATE_REFUSED && starts_with(r.message, NK_MIGRATE_CODE_LINKED),
              c == 2 ? "a link to nothing refuses the move" : "a linked data folder refuses the move");
        CHECK(strstr(r.message, fx.old_data) != NULL && strstr(r.message, target) != NULL, "naming the link");
        if (c == 2) CHECK(strstr(r.message, "does not exist now") != NULL, "and that its target is missing");
        CHECK(is_link(fx.old_data) && !exists(fx.new_data), "the link is where it was");
    }
    /* A relative link inside that would point elsewhere after the move. */
    for (int c = 0; c < 3; c++) {
        Fixture fx;
        build_fixture(&fx, true);
        char target[NATIVE_TEST_PATH_MAX], link_path[NATIVE_TEST_PATH_MAX];
        const char *text = NULL;
        if (c == 0) {
            join(target, sizeof(target), fx.root, "dotfiles");
            put_file(target, "settings.json", "{\"stowed\":1}");
            join(link_path, sizeof(link_path), fx.old_config, "stowed.json");
            text = "../../dotfiles/settings.json"; /* the settings folder moves to another parent */
        } else if (c == 1) {
            join(target, sizeof(target), fx.root, "isos");
            CHECK(nk_platform_mkdir_p(target), "a folder of images elsewhere");
            join(link_path, sizeof(link_path), fx.old_data, "isos");
            text = "../../isos"; /* the data folder moves one level deeper */
        } else {
            join(link_path, sizeof(link_path), fx.old_data, "save-alias");
            text = "saves/UCUS98744"; /* into data/saves, which moves up on its own */
        }
        CHECK(symlink(text, link_path) == 0, "make the relative link");
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        NkDataMigration r;
        move(&plan, &r);
        check_refused(&fx, &r, NK_MIGRATE_CODE_LINKED,
                      c == 0   ? "a stowed settings file refuses the move"
                      : c == 1 ? "a relative link out of the data folder refuses the move"
                               : "a link into the saves, which move on their own, refuses the move");
        CHECK(strstr(r.message, link_path) != NULL && strstr(r.message, text) != NULL,
              "naming the link and where it points");
    }
    /* A folder of another user (sudo with HOME kept). */
    {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        nk_data_migrate_set_euid_for_test((long)getuid() + 1L);
        NkDataMigration r;
        move(&plan, &r);
        nk_data_migrate_set_euid_for_test(-1);
        check_refused(&fx, &r, NK_MIGRATE_CODE_OTHER_OWNER, "a folder of another user refuses the move");
    }
    /* A folder that matters which is a folder the new layout keeps where it is. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        snprintf(fx.logs, sizeof(fx.logs), "%s/logs", fx.old_data); /* XDG_STATE_HOME == XDG_DATA_HOME */
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        NkDataMigration r;
        move(&plan, &r);
        check_refused(&fx, &r, NK_MIGRATE_CODE_OVERLAP, "logs inside the old data folder refuse the move");
    }
    /* A library a newer Nakagawa wrote. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        snprintf(fx.library_text, sizeof(fx.library_text),
                 "{\"schema_version\": 99, \"games\": [{\"iso_path\": \"%s/discs/UCUS98744/UCUS98744.iso\"}]}",
                 fx.old_data);
        put_file(fx.old_data, "library.json", fx.library_text);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_OLD, "a newer library does not stop the start");
        NkDataMigration r;
        move(&plan, &r);
        check_refused(&fx, &r, NK_MIGRATE_CODE_NEWER_LIBRARY, "a newer library refuses the move");
        CHECK(strstr(r.message, plan.old_library_file) != NULL, "naming the file");
    }
    /* The new folder holds files already. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        put_file(fx.new_config, "settings.json", "{\"new\":true}");
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        NkDataMigration r;
        move(&plan, &r);
        CHECK(r.status == NK_MIGRATE_REFUSED && starts_with(r.message, NK_MIGRATE_CODE_BOTH_PRESENT) &&
                  strstr(r.message, fx.old_data) != NULL && strstr(r.message, fx.new_root) != NULL,
              "files in the new folder refuse the move, naming both folders");
        char path[NATIVE_TEST_PATH_MAX];
        join(path, sizeof(path), fx.new_config, "settings.json");
        CHECK(file_is(path, "{\"new\":true}"), "and both are untouched");
    }
    /* A folder name the journal cannot hold. */
    {
        Fixture fx;
        build_fixture(&fx, false);
        snprintf(fx.old_config, sizeof(fx.old_config), "%s/config/naka\tgawa", fx.root);
        put_file(fx.old_config, "settings.json", "{\"a\":1}");
        put_file(fx.old_data, "library.json", fx.library_text);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        NkDataMigration r;
        move(&plan, &r);
        CHECK(r.status == NK_MIGRATE_REFUSED && starts_with(r.message, NK_MIGRATE_CODE_UNSAFE_NAME),
              "a tab in a folder name refuses the move");
        CHECK(!exists(fx.new_data), "before anything moved");
    }
    /* Another file system: checked before the first rename, for real where a second one exists. */
    {
        struct stat a, b;
        char other[NATIVE_TEST_PATH_MAX] = "/dev/shm/nk-dm-XXXXXX";
        if (stat("/dev/shm", &b) != 0 || stat(g_base, &a) != 0 || a.st_dev == b.st_dev || !mkdtemp(other)) {
            printf("test_data_migrate: SKIP real other-disk case (no second file system at /dev/shm)\n");
        } else {
            Fixture fx;
            build_fixture(&fx, true);
            char config[NATIVE_TEST_PATH_MAX], path[NATIVE_TEST_PATH_MAX];
            snprintf(config, sizeof(config), "%s/nakagawa-recomp", other);
            put_file(config, "settings.json", "{\"a\":1}");
            join(path, sizeof(path), fx.old_config, "settings.json");
            (void)nk_remove_utf8(path);
            (void)rmdir(fx.old_config);
            snprintf(fx.old_config, sizeof(fx.old_config), "%s", config);
            NkDataMigrationPlan plan;
            build_plan(&fx, &plan);
            use_seam(0, false);
            NkDataMigration r;
            move(&plan, &r);
            int renames = g_rename_calls;
            drop_seam();
            check_refused(&fx, &r, NK_MIGRATE_CODE_OTHER_DISK, "settings on another file system refuse the move");
            CHECK(renames == 0, "before the first rename");
            join(path, sizeof(path), config, "settings.json");
            (void)nk_remove_utf8(path);
            (void)rmdir(config);
            (void)rmdir(other);
        }
    }
    /* An interrupted move waits for the next start; a committed one has nothing left to do. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        put_file(fx.root, "share/.nakagawa-move-in-progress", "nakagawa-data-folder-move 1\n");
        NkDataMigration r;
        move(&plan, &r);
        CHECK(r.status == NK_MIGRATE_REFUSED && starts_with(r.message, NK_MIGRATE_CODE_INTERRUPTED),
              "a pending journal refuses a new move");
        (void)nk_remove_utf8(plan.journal_file);
        put_file(fx.new_root, ".nakagawa-layout", "marker");
        move(&plan, &r);
        CHECK(r.status == NK_MIGRATE_NOTHING, "the marker means there is nothing to move");
    }
}

/* ---- Crash points ---- */

/* The move is killed at each point; the next start finishes or undoes it, and a third start sees a
   settled layout. Linux layout: step 1 data, 2 saves, 3 settings, 4 the library rewrite. */
static void test_crash_points(void) {
    static const struct {
        const char *point;
        NkLayoutChoice after;
        bool recovered;
        const char *what;
    } rows[] = {
        {"begin", NK_LAYOUT_OLD, false, "killed before the journal: nothing to do, the old layout is whole"},
        {"journal", NK_LAYOUT_NEW, true, "killed after the journal: the next start rolls the move forward"},
        {"step:1", NK_LAYOUT_NEW, true, "killed after the data rename: rolled forward"},
        {"step:2", NK_LAYOUT_NEW, true, "killed after the saves rename: rolled forward"},
        {"step:3", NK_LAYOUT_NEW, true, "killed after the settings rename: rolled forward"},
        {"step:4", NK_LAYOUT_NEW, true, "killed after the library rewrite: committed at the next start"},
        {"commit", NK_LAYOUT_NEW, false, "killed after the commit: the marker settles it"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        CHECK(move_killed_at(&plan, rows[i].point, 0), rows[i].what);
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == rows[i].after && d.recovered == rows[i].recovered, rows[i].what);
        if (rows[i].after == NK_LAYOUT_NEW) {
            check_migrated(&fx, rows[i].what);
            CHECK(library_names_new_folder(&plan), "with the library rewritten");
        } else {
            check_untouched(&fx, rows[i].what);
        }
        NkLayoutDecision again;
        decide(&plan, &again);
        CHECK(again.layout == rows[i].after && !again.recovered && again.message[0] == '\0',
              "and the start after that has nothing to recover");
    }

    /* Killed during an undo (the settings rename failed, the saves were put back, the data not yet):
       the journal is still there, and the next start rolls forward from what is on the disk. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        CHECK(move_killed_at(&plan, "undo:2", 3), "killed during an undo");
        CHECK(exists(plan.journal_file), "the journal stays");
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_NEW && d.recovered, "the next start finishes the move");
        check_migrated(&fx, "and every folder is in the new layout once");
    }

    /* Killed after the data rename, and by the next start a step cannot complete (something wrote into
       the settings destination): that start undoes the move back to the whole old layout, removes the
       journal and keeps the file it found. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        CHECK(move_killed_at(&plan, "step:1", 0), "a move killed after the data rename");
        put_file(fx.new_config, "written-later.txt", "later");
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_OLD && d.recovered && strcmp(d.rule, "journal") == 0 && d.can_move,
              "a recovery that cannot finish undoes the move, and the old layout is used whole");
        CHECK(!exists(plan.journal_file) && !exists(plan.marker_file), "with the journal gone and no marker");
        char path[NATIVE_TEST_PATH_MAX];
        join(path, sizeof(path), fx.old_data, "library.json");
        CHECK(file_is(path, fx.library_text), "the library is back, as it was");
        join(path, sizeof(path), fx.old_data, "saves/UCUS98744/SAVE.bin");
        CHECK(file_is(path, "save bytes"), "and the saves");
        join(path, sizeof(path), fx.new_config, "written-later.txt");
        CHECK(file_is(path, "later"), "and the file found in the way is kept");
        CHECK(d.notice_kind == NK_LAYOUT_NOTICE_BOTH, "and the notice names both folders");
    }

    /* The undo fails too: the move says so and asks for a restart; the next start can neither finish
       nor undo while the cause is there, so it stops with both folders named, and a start after the
       cause is gone finishes. */
    {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        use_seam(3, true); /* the settings rename and every rename after it fail */
        NkDataMigration r;
        move(&plan, &r);
        CHECK(r.status == NK_MIGRATE_FAILED && r.must_restart &&
                  strstr(r.message, "could not put everything back") != NULL,
              "a move whose undo fails asks for a restart");
        CHECK(exists(plan.journal_file), "and keeps its journal");
        NkLayoutDecision d;
        decide(&plan, &d);
        drop_seam();
        CHECK(d.layout == NK_LAYOUT_BLOCKED && starts_with(d.message, NK_MIGRATE_CODE_INTERRUPTED),
              "a start that can neither finish nor undo stops");
        CHECK(strstr(d.message, fx.old_data) != NULL && strstr(d.message, fx.new_data) != NULL &&
                  strstr(d.message, "holds files") != NULL && strstr(d.message, "does not exist") != NULL,
              "naming both folders and what each holds");
        CHECK(strcmp(d.folder_b, fx.new_root) == 0 && d.folder_a[0], "and handing both to the message");
        char path[NATIVE_TEST_PATH_MAX];
        join(path, sizeof(path), fx.new_data, "discs/UCUS98744/UCUS98744.iso");
        CHECK(file_is(path, "disc bytes"), "nothing was deleted");
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_NEW && d.recovered, "once the cause is gone, the next start finishes");
        check_migrated(&fx, "with everything in the new layout");
    }

    /* A recovery needs the folders to itself: another window, or an older copy's library lock, stops
       the start instead of running on the half-moved layout. */
    for (int c = 0; c < 2; c++) {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        CHECK(move_killed_at(&plan, "step:1", 0), "a move killed after the data rename");
        char lock[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
        if (c == 0) {
            snprintf(lock, sizeof(lock), "%s", plan.player_lock_file);
        } else {
            join(lock, sizeof(lock), fx.new_data, "locks/library.lock");
            CHECK(parent_of(lock, dir, sizeof(dir)) && nk_platform_mkdir_p(dir), "the library lock folder");
        }
        Holder holder = hold_in_child(c == 0 ? HOLD_FLOCK_SHARED : HOLD_FLOCK, lock);
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_BLOCKED &&
                  starts_with(d.message, c == 0 ? NK_MIGRATE_CODE_IN_USE : NK_MIGRATE_CODE_LOCK) &&
                  strstr(d.message, "Close every other Nakagawa window") != NULL,
              c == 0 ? "another window stops the recovery" : "an older copy writing the library stops the recovery");
        CHECK(exists(plan.journal_file) && exists(fx.new_data) && exists(fx.old_config), "and nothing changed");
        let_go(&holder);
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_NEW && d.recovered, "once it has closed, the start finishes the move");
        check_migrated(&fx, "and the data is whole");
    }
}

/* Several starts at once after an interrupted move, as separate processes: one recovers, every one
   uses the new layout, nothing is lost or left twice. */
static void test_concurrent_starts_after_a_crash(void) {
    enum { STARTS = 8 };
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    CHECK(move_killed_at(&plan, "step:2", 0), "a move killed after the saves rename");
    int gate[2];
    CHECK(pipe(gate) == 0, "make the start gate");
    nk_data_migrate_release_player_lock();
    fflush(stdout);
    fflush(stderr);
    pid_t children[STARTS];
    for (int i = 0; i < STARTS; i++) {
        children[i] = fork();
        if (children[i] == 0) {
            close(gate[1]);
            char byte;
            ssize_t got = read(gate[0], &byte, 1u); /* returns 0 when the parent closes the gate */
            (void)got;
            NkLayoutDecision d;
            nk_data_layout_decide(&plan, &d);
            _exit((int)d.layout * 10 + (d.recovered ? 1 : 0));
        }
        CHECK(children[i] > 0, "start a process");
    }
    close(gate[0]);
    close(gate[1]);
    int recovered = 0, plain_new = 0, other = 0;
    for (int i = 0; i < STARTS; i++) {
        int status = 0;
        if (children[i] <= 0 || waitpid(children[i], &status, 0) != children[i] || !WIFEXITED(status)) {
            other++;
            continue;
        }
        int code = WEXITSTATUS(status);
        if (code == NK_LAYOUT_NEW * 10 + 1) recovered++;
        else if (code == NK_LAYOUT_NEW * 10) plain_new++;
        else other++;
    }
    CHECK(recovered == 1 && plain_new == STARTS - 1 && other == 0,
          "one start finished the move and every other one used the new layout");
    check_migrated(&fx, "after concurrent starts, everything is in the new layout once");
    CHECK(library_names_new_folder(&plan), "and the library was rewritten once, whole");
}

/* ---- The move's core ---- */

/* The check found the settings destination empty; something wrote into it before its rename. Only
   empty folders are ever removed, so the file survives and the move is undone. */
static char g_late_dir[NATIVE_TEST_PATH_MAX];

static void write_late_file(int call, const char *from, const char *to) {
    (void)from;
    (void)to;
    if (call == 1) put_file(g_late_dir, "late.txt", "written after the check");
}

static void test_file_appearing_in_an_empty_destination_survives(void) {
    Fixture fx;
    build_fixture(&fx, true);
    join(g_late_dir, sizeof(g_late_dir), fx.new_config, "profiles/sub");
    CHECK(nk_platform_mkdir_p(g_late_dir), "an empty tree where the settings go");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    use_seam(0, false);
    g_before_rename = write_late_file;
    NkDataMigration r;
    move(&plan, &r);
    drop_seam();
    if (r.status != NK_MIGRATE_FAILED || r.must_restart)
        fprintf(stderr, "late file: status %d: %s\n%s\n", (int)r.status, r.message, r.log);
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), g_late_dir, "late.txt");
    CHECK(file_is(path, "written after the check"), "a file that appeared after the check is never deleted");
    CHECK(r.status == NK_MIGRATE_FAILED && starts_with(r.message, NK_MIGRATE_CODE_FAILED) && !r.must_restart,
          "the settings step fails instead");
    CHECK(strstr(r.message, "back where it was") != NULL, "and the data move is undone");
    char saved[NATIVE_TEST_PATH_MAX];
    join(saved, sizeof(saved), fx.old_data, "library.json");
    CHECK(file_is(saved, fx.library_text), "with the library as it was");
    join(saved, sizeof(saved), fx.old_data, "saves/UCUS98744/SAVE.bin");
    CHECK(file_is(saved, "save bytes") && !exists(plan.journal_file), "and the saves back in place");
}

/* The run lock is held for the whole move: a probe from another open file description (as a second
   process would have) cannot take it at any rename, the commit included. */
static char g_lock_path[NATIVE_TEST_PATH_MAX];
static int g_lock_probes = 0;
static int g_lock_free = 0;

static void probe_lock(int call, const char *from, const char *to) {
    (void)call;
    (void)from;
    (void)to;
    int fd = open(g_lock_path, O_RDWR | O_CLOEXEC);
    g_lock_probes++;
    if (fd < 0) {
        g_lock_free++;
        return;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        g_lock_free++;
        (void)flock(fd, LOCK_UN);
    }
    close(fd);
}

static void test_locks_are_held_during_the_move(void) {
    for (int which = 0; which < 2; which++) {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        snprintf(g_lock_path, sizeof(g_lock_path), "%s", which == 0 ? plan.lock_file : plan.player_lock_file);
        g_lock_probes = 0;
        g_lock_free = 0;
        use_seam(0, false);
        g_before_rename = probe_lock;
        NkDataMigration r;
        move(&plan, &r);
        drop_seam();
        CHECK(r.status == NK_MIGRATE_DONE, "the move completes");
        CHECK(g_lock_probes == 4 && g_lock_free == 0,
              which == 0 ? "no other start could take the run lock at any rename"
                         : "no other player could take the player lock at any rename");
        nk_data_migrate_release_player_lock();
    }
}

static char g_old_library_lock[NATIVE_TEST_PATH_MAX], g_new_library_lock[NATIVE_TEST_PATH_MAX];
static char g_new_library[NATIVE_TEST_PATH_MAX];
static int g_library_probes = 0, g_library_free = 0;
static pid_t g_writer = -1;

/* Another writer, as nk_library.c and tools/nk_cli.py write: under the lock, read, add, write. */
static void library_writer_child(void) {
    /* A separate program has none of the move's open files; a forked copy would share its locks. */
    for (int fd = 3; fd < 1024; fd++) (void)close(fd);
    NkFileLock lock;
    if (nk_library_lock_acquire(&lock, g_new_library, NK_LIBRARY_LOCK_TIMEOUT_MS) != NK_OK) _exit(2);
    char *text = slurp(g_new_library);
    char *end = text ? strstr(text, "]}") : NULL;
    if (!end) _exit(3);
    *end = '\0';
    FILE *f = fopen(g_new_library, "wb");
    if (!f) _exit(4);
    fprintf(f, "%s,{\"iso_path\":\"/writer/added.iso\"}]}", text);
    fclose(f);
    nk_platform_file_lock_release(&lock);
    _exit(0);
}

static void probe_library_lock(int call, const char *from, const char *to) {
    (void)from;
    (void)to;
    const char *paths[] = {g_old_library_lock, g_new_library_lock};
    for (size_t i = 0; i < 2u; i++) {
        int fd = open(paths[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        g_library_probes++;
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            g_library_free++;
            (void)flock(fd, LOCK_UN);
        }
        close(fd);
    }
    if (call == 2 && g_writer < 0) {
        fflush(stdout);
        fflush(stderr);
        g_writer = fork();
        if (g_writer == 0) library_writer_child();
    }
}

static void test_library_lock_is_held_for_the_rewrite(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    join(g_old_library_lock, sizeof(g_old_library_lock), fx.old_data, "locks/library.lock");
    join(g_new_library_lock, sizeof(g_new_library_lock), fx.new_data, "locks/library.lock");
    snprintf(g_new_library, sizeof(g_new_library), "%s", plan.library_file);
    g_library_probes = 0;
    g_library_free = 0;
    g_writer = -1;
    use_seam(0, false);
    g_before_rename = probe_library_lock;
    NkDataMigration r;
    move(&plan, &r);
    drop_seam();
    int status = -1;
    bool writer_ok = g_writer > 0 && waitpid(g_writer, &status, 0) == g_writer && WIFEXITED(status) &&
                     WEXITSTATUS(status) == 0;
    CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten, "the move completes");
    CHECK(g_library_probes == 4 && g_library_free == 0,
          "no other writer could take the library lock at any rename or before the commit");
    CHECK(writer_ok, "a writer that came along during the move waited for the lock, then wrote");
    char *library = slurp(plan.library_file);
    char want_iso[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want_iso, sizeof(want_iso), "\"%s/discs/UCUS98744/UCUS98744.iso\"", plan.remap_to);
    CHECK(library && strstr(library, want_iso) != NULL && strstr(library, "/writer/added.iso") != NULL,
          "so the library has both the rewritten paths and the other writer's entry");
    free(library);
    nk_data_migrate_release_player_lock();
}

/* `s` as JSON string content: escaped as the library writes it, and with every '/' written as the
   equally valid "\/" when `slash_escapes` (another writer's form, which must still match). */
static void json_escape(char *out, size_t cap, const char *s, bool slash_escapes) {
    size_t o = 0;
    for (; *s && o + 7u < cap; s++) {
        if (*s == '"' || *s == '\\' || (slash_escapes && *s == '/')) out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = '\0';
}

/* An old data folder whose path has a quote and a backslash: library.json stores it escaped. */
static void test_library_paths_that_need_escaping(void) {
    char root[NATIVE_TEST_PATH_MAX], old_data[NATIVE_TEST_PATH_MAX], new_data[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    snprintf(old_data, sizeof(old_data), "%s/sh\"are\\x/nakagawa-recomp", root);
    snprintf(new_data, sizeof(new_data), "%s/sh\"are\\x/Nakagawa/data", root);
    char old_plain[NATIVE_TEST_PATH_MAX * 2], old_slashes[NATIVE_TEST_PATH_MAX * 3];
    json_escape(old_plain, sizeof(old_plain), old_data, false);
    json_escape(old_slashes, sizeof(old_slashes), old_data, true);
    char text[NATIVE_TEST_PATH_MAX * 8];
    snprintf(text, sizeof(text),
             "{\n  \"schema_version\": 1,\n  \"games\": [\n"
             "    {\"iso_path\": \"%s/discs/A.iso\"},\n"
             "    {\"iso_path\": \"%s\\/discs\\/B.iso\"},\n"
             "    {\"iso_path\": \"%s-x/C.iso\"}\n  ]\n}\n",
             old_plain, old_slashes, old_plain);
    put_file(old_data, "library.json", text);

    NkDataMigrationPlan plan;
    memset(&plan, 0, sizeof(plan));
    snprintf(plan.new_root, sizeof(plan.new_root), "%s/sh\"are\\x/Nakagawa", root);
    add_move(&plan, old_data, new_data, "data", false, false);
    snprintf(plan.remap_from, sizeof(plan.remap_from), "%s", old_data);
    snprintf(plan.remap_to, sizeof(plan.remap_to), "%s", new_data);
    snprintf(plan.library_file, sizeof(plan.library_file), "%s/library.json", new_data);
    snprintf(plan.old_library_file, sizeof(plan.old_library_file), "%s/library.json", old_data);
    char base[NATIVE_TEST_PATH_MAX];
    snprintf(base, sizeof(base), "%s/sh\"are\\x", root);
    plan_files(&plan, base);
    NkDataMigration r;
    move(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten,
          "stored paths under a folder whose name needs JSON escaping are rewritten");
    char new_plain[NATIVE_TEST_PATH_MAX * 2], want[NATIVE_TEST_PATH_MAX * 3];
    json_escape(new_plain, sizeof(new_plain), new_data, false);
    char *library = slurp(plan.library_file);
    snprintf(want, sizeof(want), "\"%s/discs/A.iso\"", new_plain);
    CHECK(library && strstr(library, want) != NULL, "an escaped path now points at the new folder");
    snprintf(want, sizeof(want), "\"%s/discs/B.iso\"", new_plain);
    CHECK(library && strstr(library, want) != NULL,
          "so does one another writer escaped differently (\\/), written back as the library writes it");
    snprintf(want, sizeof(want), "\"%s-x/C.iso\"", old_plain);
    CHECK(library && strstr(library, want) != NULL, "a folder that only starts like the old one is untouched");
    CHECK(library && strstr(library, "\"schema_version\": 1") != NULL, "the other bytes are kept");
    free(library);
    nk_data_migrate_release_player_lock();
}

/* Stored paths spelled another way than the plan's are still found. */
static void test_library_paths_in_other_spellings(void) {
    char root[NATIVE_TEST_PATH_MAX], real_share[NATIVE_TEST_PATH_MAX], link_share[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    join(real_share, sizeof(real_share), root, "share");
    join(link_share, sizeof(link_share), root, "linked-share");
    char old_real[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
    join(old_real, sizeof(old_real), real_share, "nakagawa-recomp");
    snprintf(dir, sizeof(dir), "%s/discs", old_real);
    put_file(dir, "A.iso", "a");
    CHECK(symlink(real_share, link_share) == 0, "a linked data base, as a linked HOME gives");
    char text[NATIVE_TEST_PATH_MAX * 8];
    snprintf(text, sizeof(text),
             "{\"games\":[{\"iso_path\":\"%s/discs/A.iso\"},{\"iso_path\":\"%s//discs/./A.iso\"},"
             "{\"iso_path\":\"%s/../nakagawa-recomp/discs/A.iso\"},{\"iso_path\":\"%s/nakagawa-recomp/\"}]}",
             old_real, old_real, old_real, link_share);
    put_file(old_real, "library.json", text);
    NkDataMigrationPlan plan;
    memset(&plan, 0, sizeof(plan));
    /* The plan spells the old folder through the link, with a trailing '/'; the library does not. */
    snprintf(plan.new_root, sizeof(plan.new_root), "%s/Nakagawa", link_share);
    char from[NATIVE_TEST_PATH_MAX], to[NATIVE_TEST_PATH_MAX];
    snprintf(from, sizeof(from), "%s/nakagawa-recomp", link_share);
    snprintf(to, sizeof(to), "%s/Nakagawa/data", link_share);
    add_move(&plan, from, to, "data", false, false);
    snprintf(plan.remap_from, sizeof(plan.remap_from), "%s/nakagawa-recomp/", link_share);
    snprintf(plan.remap_to, sizeof(plan.remap_to), "%s/Nakagawa/data", link_share);
    snprintf(plan.library_file, sizeof(plan.library_file), "%s/Nakagawa/data/library.json", link_share);
    snprintf(plan.old_library_file, sizeof(plan.old_library_file), "%s/nakagawa-recomp/library.json",
             link_share);
    plan_files(&plan, link_share);
    NkDataMigration r;
    move(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten, "the move completes and rewrites");
    char *library = slurp(plan.library_file);
    char want[NATIVE_TEST_PATH_MAX * 2];
    snprintf(want, sizeof(want), "\"%s/Nakagawa/data/discs/A.iso\"", link_share);
    int found = 0;
    for (const char *p = library; p && (p = strstr(p, want)) != NULL; p++) found++;
    CHECK(found == 3, "the resolved spelling, '//' and '.', and '..' all point at the new folder");
    snprintf(want, sizeof(want), "\"%s/Nakagawa/data\"", link_share);
    CHECK(library && strstr(library, want) != NULL, "and so does the folder itself, spelled with a '/'");
    CHECK(library && strstr(library, old_real) == NULL && strstr(library, "nakagawa-recomp") == NULL,
          "no stored path names the old folder");
    free(library);
    snprintf(want, sizeof(want), "%s/Nakagawa/data/discs/A.iso", link_share);
    CHECK(file_is(want, "a"), "and the rewritten path reaches the disc");
    nk_data_migrate_release_player_lock();
}

/* An absolute link, and a relative one that stays inside the folder, both still work after. */
static void test_links_that_stay_valid(void) {
    Fixture fx;
    build_fixture(&fx, true);
    char target[NATIVE_TEST_PATH_MAX], link_path[NATIVE_TEST_PATH_MAX];
    join(target, sizeof(target), fx.root, "isos");
    put_file(target, "B.iso", "b");
    join(link_path, sizeof(link_path), fx.old_data, "isos");
    CHECK(symlink(target, link_path) == 0, "an absolute link");
    join(link_path, sizeof(link_path), fx.old_data, "disc-alias");
    CHECK(symlink("discs/UCUS98744", link_path) == 0, "a relative link inside the data folder");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    move(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "do not stop the move");
    join(link_path, sizeof(link_path), fx.new_data, "isos/B.iso");
    CHECK(file_is(link_path, "b"), "the absolute link still reaches its target");
    join(link_path, sizeof(link_path), fx.new_data, "disc-alias/UCUS98744.iso");
    CHECK(file_is(link_path, "disc bytes"), "and so does the relative one inside");
    nk_data_migrate_release_player_lock();
}

static void test_failed_rename_is_undone(void) {
    /* 1 data, 2 saves, 3 settings, 4 the commit. */
    for (int fail_at = 1; fail_at <= 4; fail_at++) {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        use_seam(fail_at, false);
        NkDataMigration r;
        move(&plan, &r);
        drop_seam();
        CHECK(r.status == NK_MIGRATE_FAILED && starts_with(r.message, NK_MIGRATE_CODE_FAILED),
              "a rename that fails fails the move, with its code first");
        CHECK(strstr(r.message, "back where it was") != NULL && !r.must_restart,
              "and says everything was put back");
        check_untouched(&fx, "every folder is back where it was");
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_OLD && d.can_move, "the next start uses the old layout and offers the move again");
    }
    /* Across file systems, as rename(2) fails there. */
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    use_seam(3, false);
    g_fail_errno = EXDEV;
    NkDataMigration r;
    move(&plan, &r);
    drop_seam();
    CHECK(r.status == NK_MIGRATE_FAILED && starts_with(r.message, NK_MIGRATE_CODE_OTHER_DISK) &&
              strstr(r.message, fx.old_config) != NULL && strstr(r.message, fx.new_config) != NULL,
          "a rename across disks is named by its own code, with both folders");
    check_untouched(&fx, "and every folder is back");
    nk_data_migrate_release_player_lock();
}

/* The undo of the saves rename fails: the undo stops there (the data folder holds the saves' old
   place), the journal stays, and the next start finishes from the disk. */
static void test_failed_undo_stops_and_the_next_start_finishes(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    use_seam(3, false); /* the settings rename fails */
    g_fail_also = 4;    /* and then the undo of the saves rename */
    NkDataMigration r;
    move(&plan, &r);
    int calls = g_rename_calls;
    drop_seam();
    CHECK(r.status == NK_MIGRATE_FAILED && r.must_restart, "a failed move with a failed undo asks for a restart");
    CHECK(calls == 4, "the data is not put back while its saves are not");
    CHECK(exists(plan.journal_file), "the journal stays");
    NkLayoutDecision d;
    decide(&plan, &d);
    CHECK(d.layout == NK_LAYOUT_NEW && d.recovered, "the next start carries on from there");
    check_migrated(&fx, "and every folder ends up in the new layout");
    nk_data_migrate_release_player_lock();
}

static void test_root_that_cannot_be_made(void) {
    /* A real failure, no seam: the place the new folder belongs is a file. */
    Fixture fx;
    build_fixture(&fx, true);
    put_file(fx.root, "blocker", "not a folder");
    snprintf(fx.new_root, sizeof(fx.new_root), "%s/blocker/Nakagawa", fx.root);
    join(fx.new_data, sizeof(fx.new_data), fx.new_root, "data");
    join(fx.new_saves, sizeof(fx.new_saves), fx.new_root, "saves");
    join(fx.new_config, sizeof(fx.new_config), fx.new_root, "config");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    move(&plan, &r);
    CHECK(r.status == NK_MIGRATE_FAILED && strstr(r.message, "back where it was") != NULL,
          "a root that cannot be created fails and puts everything back");
    check_untouched(&fx, "the old data is untouched");
    nk_data_migrate_release_player_lock();
}

/* A folder flush that fails. The journal's own flush failing stops the move before the first
   rename; a flush failing after the renames stops the commit, and the move is undone. */
static int g_sync_calls = 0;
static int g_sync_fail_after = 0;

static bool failing_sync_dir(const char *dir) {
    (void)dir;
    if (++g_sync_calls <= g_sync_fail_after) return true;
    errno = EIO;
    return false;
}

static void test_failed_flush_is_never_committed(void) {
    for (int after = 0; after < 2; after++) {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        g_sync_calls = 0;
        g_sync_fail_after = after; /* 0: the journal's flush fails; 1: every flush after it fails */
        nk_data_migrate_set_sync_dir_for_test(failing_sync_dir);
        NkDataMigration r;
        move(&plan, &r);
        nk_data_migrate_set_sync_dir_for_test(NULL);
        CHECK(r.status == NK_MIGRATE_FAILED && strstr(r.message, NK_MIGRATE_CODE_NOT_FLUSHED) != NULL,
              after == 0 ? "a journal that cannot be flushed stops the move" : "a commit whose folders cannot be "
                                                                              "flushed is not made");
        check_untouched(&fx, "and everything is where it was");
        if (after == 1) CHECK(strstr(r.log, NK_MIGRATE_CODE_NOT_FLUSHED) != NULL, "each failed flush is a log line");
        NkLayoutDecision d;
        decide(&plan, &d);
        CHECK(d.layout == NK_LAYOUT_OLD, "and the next start uses the old layout");
    }
    nk_data_migrate_release_player_lock();
}

/* The rewritten library is read back: a stored path that still names the old folder (a writer that
   ignores the lock, simulated in the folder flush after the rewrite) undoes the move. */
static char g_sneak_library[NATIVE_TEST_PATH_MAX], g_sneak_old[NATIVE_TEST_PATH_MAX];
static char g_sneak_new[NATIVE_TEST_PATH_MAX];
static bool g_sneaked = false;

static bool sneaking_sync_dir(const char *dir) {
    (void)dir;
    char *text = g_sneaked ? NULL : slurp(g_sneak_library);
    char *end = text ? strstr(text, "]}") : NULL;
    if (end && strstr(text, g_sneak_new) != NULL) {
        *end = '\0';
        FILE *f = fopen(g_sneak_library, "wb");
        if (f) {
            fprintf(f, "%s,{\"iso_path\":\"%s/sneaked.iso\"}]}", text, g_sneak_old);
            fclose(f);
            g_sneaked = true;
        }
    }
    free(text);
    return true;
}

static void test_rewritten_library_is_checked(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    snprintf(g_sneak_library, sizeof(g_sneak_library), "%s", plan.library_file);
    snprintf(g_sneak_old, sizeof(g_sneak_old), "%s", fx.old_data);
    snprintf(g_sneak_new, sizeof(g_sneak_new), "%s/discs", fx.new_data);
    g_sneaked = false;
    nk_data_migrate_set_sync_dir_for_test(sneaking_sync_dir);
    NkDataMigration r;
    move(&plan, &r);
    nk_data_migrate_set_sync_dir_for_test(NULL);
    CHECK(g_sneaked, "an old path was put back after the rewrite");
    CHECK(r.status == NK_MIGRATE_FAILED && strstr(r.message, "still name") != NULL,
          "the move fails, naming the stored path that still points at the old folder");
    check_untouched(&fx, "and the renames and the library are undone");
    nk_data_migrate_release_player_lock();
}

/* macOS: the old root already had config, data and saves beside each other, so the whole folder is
   renamed; Caches and Logs come in as cache and logs, and never stop the move. */
typedef struct {
    char root[NATIVE_TEST_PATH_MAX], old_root[NATIVE_TEST_PATH_MAX], caches[NATIVE_TEST_PATH_MAX];
    char logs[NATIVE_TEST_PATH_MAX], new_root[NATIVE_TEST_PATH_MAX];
} MacFixture;

static void build_mac(MacFixture *m, NkDataMigrationPlan *plan) {
    char dir[NATIVE_TEST_PATH_MAX];
    new_case(m->root, sizeof(m->root));
    join(m->old_root, sizeof(m->old_root), m->root, "Support/NakagawaRecomp");
    join(m->caches, sizeof(m->caches), m->root, "Caches/NakagawaRecomp");
    join(m->logs, sizeof(m->logs), m->root, "Logs/NakagawaRecomp");
    join(m->new_root, sizeof(m->new_root), m->root, "Support/Nakagawa");
    snprintf(dir, sizeof(dir), "%s/data", m->old_root);
    put_file(dir, "library.json", "{\"x\":\"unused\"}");
    snprintf(dir, sizeof(dir), "%s/config", m->old_root);
    put_file(dir, "settings.json", "s");
    snprintf(dir, sizeof(dir), "%s/saves", m->old_root);
    put_file(dir, "one.sav", "sv");
    put_file(m->caches, "c.bin", "c");
    put_file(m->logs, "l.txt", "l");
    memset(plan, 0, sizeof(*plan));
    char to_cache[NATIVE_TEST_PATH_MAX], to_logs[NATIVE_TEST_PATH_MAX];
    join(to_cache, sizeof(to_cache), m->new_root, "cache");
    join(to_logs, sizeof(to_logs), m->new_root, "logs");
    add_move(plan, m->old_root, m->new_root, "data, saves and settings", false, false);
    add_move(plan, m->caches, to_cache, "cache", false, true);
    add_move(plan, m->logs, to_logs, "logs", false, true);
    snprintf(plan->new_root, sizeof(plan->new_root), "%s", m->new_root);
    snprintf(dir, sizeof(dir), "%s/Support", m->root);
    plan_files(plan, dir);
}

static void test_whole_folder_rename(void) {
    MacFixture m;
    NkDataMigrationPlan plan;
    build_mac(&m, &plan);
    NkLayoutDecision d;
    decide(&plan, &d);
    CHECK(d.layout == NK_LAYOUT_OLD, "the macOS-shaped install keeps its folder until the move");
    NkDataMigration r;
    move(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 3u, "the macOS-shaped move runs");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), m.new_root, "config/settings.json");
    CHECK(file_is(path, "s"), "config arrived with the whole folder");
    join(path, sizeof(path), m.new_root, "saves/one.sav");
    CHECK(file_is(path, "sv"), "saves too");
    join(path, sizeof(path), m.new_root, "cache/c.bin");
    CHECK(file_is(path, "c"), "Caches became cache");
    join(path, sizeof(path), m.new_root, "logs/l.txt");
    CHECK(file_is(path, "l"), "Logs became logs");
    CHECK(!exists(m.old_root) && !exists(m.caches) && !exists(m.logs), "the old folders are gone");
    join(path, sizeof(path), m.new_root, ".nakagawa-layout");
    CHECK(exists(path), "and the move is committed in the renamed folder");
    nk_data_migrate_release_player_lock();
}

/* A cache or logs folder never stops the move, and an old cache written again later is no notice. */
static void test_cache_and_logs_never_block(void) {
    MacFixture m;
    NkDataMigrationPlan plan;
    build_mac(&m, &plan);
    use_seam(2, false);
    g_fail_errno = EXDEV;
    NkDataMigration r;
    move(&plan, &r);
    drop_seam();
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 2u, "a cache that cannot move does not stop the move");
    CHECK(strstr(r.log, NK_MIGRATE_CODE_LEFT_BEHIND) != NULL && strstr(r.log, m.caches) != NULL,
          "the log names the folder left behind");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), m.caches, "c.bin");
    CHECK(file_is(path, "c"), "and it is where it was");
    join(path, sizeof(path), m.new_root, "saves/one.sav");
    CHECK(file_is(path, "sv"), "while the folders that matter moved");
    put_file(m.caches, "late.bin", "late");
    NkLayoutDecision d;
    decide(&plan, &d);
    CHECK(d.layout == NK_LAYOUT_NEW && d.notice_kind == NK_LAYOUT_NOTICE_NONE,
          "an old cache written again is not a returned folder");
    nk_data_migrate_release_player_lock();
}

/* ---- The host's own plan and default folders ---- */

#if !defined(__APPLE__) && !defined(__ANDROID__)
/* XDG homes inside `root`, as a Linux user has them. */
static void linux_env(const char *root, const char *data, const char *config, const char *cache,
                      const char *state) {
    char value[NATIVE_TEST_PATH_MAX];
    join(value, sizeof(value), root, "home");
    native_test_set_env("HOME", value);
    join(value, sizeof(value), root, data);
    native_test_set_env("XDG_DATA_HOME", value);
    join(value, sizeof(value), root, config);
    native_test_set_env("XDG_CONFIG_HOME", value);
    join(value, sizeof(value), root, cache);
    native_test_set_env("XDG_CACHE_HOME", value);
    join(value, sizeof(value), root, state);
    native_test_set_env("XDG_STATE_HOME", value);
}

static void host_decide(NkLayoutDecision *d) {
    nk_data_layout_host(d);
}

static void host_move(NkDataMigration *r) {
    static NkDataMigrationPlan plan;
    CHECK(nk_platform_user_data_plan(&plan), "the host has a plan");
    nk_data_migrate_move(&plan, false, r);
}

static void host_reset(void) {
    (void)nk_platform_set_old_user_data_layout(false);
    nk_data_migrate_release_player_lock();
}

static void test_linux_default_folders_and_plan(void) {
    native_test_set_env("HOME", "/synthetic/profile");
    native_test_set_env("XDG_DATA_HOME", "/synthetic/xdg");
    native_test_set_env("XDG_CONFIG_HOME", "/synthetic/xdg-config");
    native_test_set_env("XDG_CACHE_HOME", "/synthetic/xdg-cache");
    native_test_set_env("XDG_STATE_HOME", "/synthetic/xdg-state");
    char out[NATIVE_TEST_PATH_MAX];
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/Nakagawa/data") == 0, "data is root/data");
    CHECK(nk_platform_resolve_path(NK_PATH_CONFIG, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/Nakagawa/config") == 0, "settings are root/config");
    CHECK(nk_platform_resolve_path(NK_PATH_SAVES, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/Nakagawa/saves") == 0, "saves are root/saves");
    CHECK(nk_platform_resolve_path(NK_PATH_CACHE, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg-cache/nakagawa-recomp") == 0,
          "the cache stays in the XDG cache home, as before #324");
    CHECK(nk_platform_resolve_path(NK_PATH_LOGS, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg-state/nakagawa-recomp/logs") == 0,
          "the logs stay in the XDG state home, as before #324");
    NkDataMigrationPlan plan;
    CHECK(nk_platform_user_data_plan(&plan), "Linux has a plan");
    CHECK(strcmp(plan.new_root, "/synthetic/xdg/Nakagawa") == 0 && strcmp(plan.base, "/synthetic/xdg") == 0,
          "to the unified root, in the data base");
    CHECK(plan.move_count == 3u, "three moves: data, its saves, settings");
    CHECK(strcmp(plan.moves[0].from, "/synthetic/xdg/nakagawa-recomp") == 0 &&
              strcmp(plan.moves[0].to, "/synthetic/xdg/Nakagawa/data") == 0 && !plan.moves[0].disposable,
          "from the old data folder");
    CHECK(plan.moves[1].after_previous &&
              strcmp(plan.moves[1].from, "/synthetic/xdg/Nakagawa/data/saves") == 0 &&
              strcmp(plan.moves[1].to, "/synthetic/xdg/Nakagawa/saves") == 0, "with the saves lifted out");
    CHECK(strcmp(plan.moves[2].from, "/synthetic/xdg-config/nakagawa-recomp") == 0 &&
              strcmp(plan.moves[2].to, "/synthetic/xdg/Nakagawa/config") == 0, "and the XDG config folder");
    CHECK(plan.kept_count == 2u && strcmp(plan.kept[0], "/synthetic/xdg-cache/nakagawa-recomp") == 0 &&
              strcmp(plan.kept[1], "/synthetic/xdg-state/nakagawa-recomp/logs") == 0,
          "the cache and logs are kept where they are, not moved (so never across disks)");
    CHECK(strcmp(plan.remap_from, "/synthetic/xdg/nakagawa-recomp") == 0 &&
              strcmp(plan.remap_to, "/synthetic/xdg/Nakagawa/data") == 0 &&
              strcmp(plan.library_file, "/synthetic/xdg/Nakagawa/data/library.json") == 0 &&
              strcmp(plan.old_library_file, "/synthetic/xdg/nakagawa-recomp/library.json") == 0,
          "and the library's stored paths are remapped");
    CHECK(strcmp(plan.lock_file, "/synthetic/xdg/.nakagawa-migration.lock") == 0 &&
              strcmp(plan.player_lock_file, "/synthetic/xdg/.nakagawa-player.lock") == 0 &&
              strcmp(plan.journal_file, "/synthetic/xdg/.nakagawa-move-in-progress") == 0,
          "the locks and the journal are beside the new folder, outside every rename");
    CHECK(strcmp(plan.marker_file, "/synthetic/xdg/Nakagawa/.nakagawa-layout") == 0, "the marker is in it");
    CHECK(plan.notes[0] == '\0', "every folder was located");

    CHECK(nk_platform_set_old_user_data_layout(true), "the old layout can be used for a run");
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/nakagawa-recomp") == 0, "old data");
    CHECK(nk_platform_resolve_path(NK_PATH_SAVES, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/nakagawa-recomp/saves") == 0, "old saves inside it");
    CHECK(nk_platform_resolve_path(NK_PATH_CONFIG, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg-config/nakagawa-recomp") == 0, "old settings");
    CHECK(nk_platform_resolve_path(NK_PATH_CACHE, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg-cache/nakagawa-recomp") == 0, "the same cache");
    CHECK(nk_platform_set_old_user_data_layout(false), "and switched back");

    native_test_set_env("XDG_DATA_HOME", "");
    native_test_set_env("XDG_CONFIG_HOME", "");
    native_test_set_env("XDG_CACHE_HOME", "");
    native_test_set_env("XDG_STATE_HOME", "");
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/.local/share/Nakagawa/data") == 0, "HOME is the fallback");
    CHECK(nk_platform_resolve_path(NK_PATH_CACHE, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/.cache/nakagawa-recomp") == 0, "for the cache too");
    CHECK(nk_platform_user_data_plan(&plan) &&
              strcmp(plan.moves[0].from, "/synthetic/profile/.local/share/nakagawa-recomp") == 0 &&
              strcmp(plan.moves[2].from, "/synthetic/profile/.config/nakagawa-recomp") == 0,
          "the old XDG defaults come from HOME");
}

/* The host from end to end: a fresh install, an existing one, the move, and an old folder that an
   older copy writes again afterwards. */
static void test_host_fresh_existing_and_move(void) {
    char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX], out[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "cache", "state");
    NkLayoutDecision d;
    host_decide(&d);
    CHECK(d.layout == NK_LAYOUT_NEW && strcmp(d.rule, "fresh") == 0 && !d.can_move,
          "a fresh install uses the new folder");
    snprintf(dir, sizeof(dir), "%s/share/Nakagawa/.nakagawa-layout", root);
    CHECK(exists(dir), "and writes the marker");
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) && strstr(out, "/share/Nakagawa/data") != NULL,
          "its data is in the new folder");
    host_reset();

    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "cache", "state");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves/ULUS10001", root);
    put_file(dir, "SAVE.bin", "save bytes");
    snprintf(dir, sizeof(dir), "%s/config/nakagawa-recomp", root);
    put_file(dir, "settings.json", "{}");
    snprintf(dir, sizeof(dir), "%s/cache/nakagawa-recomp", root);
    put_file(dir, "objects.bin", "cache");
    host_decide(&d);
    CHECK(d.layout == NK_LAYOUT_OLD && d.can_move, "an existing install keeps working where its data is");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp", root);
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) && strcmp(out, dir) == 0,
          "this run uses the old data folder");
    NkDataMigration r;
    host_move(&r);
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 3u && r.must_restart, "the move from Settings runs");
    host_reset();
    snprintf(dir, sizeof(dir), "%s/cache/nakagawa-recomp/objects.bin", root);
    CHECK(file_is(dir, "cache"), "the cache stays where it was");
    host_decide(&d);
    CHECK(d.layout == NK_LAYOUT_NEW && strcmp(d.rule, "marker") == 0 && d.notice_kind == NK_LAYOUT_NOTICE_NONE,
          "the restarted player uses the new folder");
    host_reset();

    /* An older copy that was still running writes its old folder again. */
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves/ULUS10001", root);
    put_file(dir, "SAVE.bin", "written by an older copy after the move");
    host_decide(&d);
    CHECK(d.layout == NK_LAYOUT_NEW && d.notice_kind == NK_LAYOUT_NOTICE_RETURNED &&
              strstr(d.notice, "/share/nakagawa-recomp") != NULL,
          "the next start goes on with the new folder and names the old one that holds files again");
    snprintf(dir, sizeof(dir), "%s/share/Nakagawa/saves/ULUS10001/SAVE.bin", root);
    CHECK(file_is(dir, "save bytes"), "nothing is merged");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves/ULUS10001/SAVE.bin", root);
    CHECK(file_is(dir, "written by an older copy after the move"), "or deleted");
    host_reset();
}

/* One folder whose path cannot be formed leaves out that folder only, by name. */
static void test_unlocatable_settings_are_named(void) {
    char root[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "cache", "state");
    native_test_set_env("HOME", NULL);
    native_test_set_env("XDG_CONFIG_HOME", NULL);
    char dir[NATIVE_TEST_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves", root);
    put_file(dir, "SAVE.bin", "save bytes");
    NkDataMigrationPlan plan;
    CHECK(nk_platform_user_data_plan(&plan), "a plan is still made");
    CHECK(plan.move_count == 2u, "with the data and saves moves");
    CHECK(strstr(plan.notes, NK_MIGRATE_CODE_PATH_UNAVAILABLE) == plan.notes &&
              strstr(plan.notes, "settings") != NULL &&
              strstr(plan.notes, "XDG_CONFIG_HOME is not set to an absolute path, and HOME is not set") != NULL,
          "and a named line for the settings folder");
    NkLayoutDecision d;
    host_decide(&d);
    CHECK(d.layout == NK_LAYOUT_OLD && strstr(d.log, NK_MIGRATE_CODE_PATH_UNAVAILABLE) != NULL,
          "the old data is used, and the log says what was left out");
    NkDataMigration r;
    host_move(&r);
    CHECK(r.status == NK_MIGRATE_DONE, "the data still moves");
    snprintf(dir, sizeof(dir), "%s/share/Nakagawa/saves/SAVE.bin", root);
    CHECK(file_is(dir, "save bytes"), "the saves arrived");
    host_reset();
}

/* XDG homes that point at each other: the start goes on, the move refuses by name, and a fresh
   install whose cache is the old data folder stays on the new layout with no notice. */
static void test_host_aliased_xdg_homes(void) {
    const char *cases[][4] = {
        {"share", "config", "cache", "share"}, /* XDG_STATE_HOME == XDG_DATA_HOME */
        {"share", "config", "share", "state"}, /* XDG_CACHE_HOME == XDG_DATA_HOME */
        {"share", "share", "cache", "state"},  /* XDG_CONFIG_HOME == XDG_DATA_HOME */
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
        new_case(root, sizeof(root));
        linux_env(root, cases[c][0], cases[c][1], cases[c][2], cases[c][3]);
        snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves", root);
        put_file(dir, "SAVE.bin", "save bytes");
        NkLayoutDecision d;
        host_decide(&d);
        CHECK(d.layout == NK_LAYOUT_OLD, "aliased XDG homes keep the old folders and the player starts");
        NkDataMigration r;
        host_move(&r);
        CHECK(r.status == NK_MIGRATE_REFUSED && starts_with(r.message, NK_MIGRATE_CODE_OVERLAP),
              "and the move refuses by name");
        host_reset();
        CHECK(file_is((snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves/SAVE.bin", root), dir),
                      "save bytes"),
              "the saves stay where the old layout finds them");
    }
    char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "share", "state");
    NkLayoutDecision d;
    host_decide(&d);
    CHECK(d.layout == NK_LAYOUT_NEW, "a fresh install whose cache is the old data folder uses the new layout");
    host_reset();
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp", root);
    put_file(dir, "objects.bin", "cache");
    host_decide(&d);
    CHECK(d.layout == NK_LAYOUT_NEW && d.notice_kind == NK_LAYOUT_NOTICE_NONE,
          "and its own cache there is no returned folder");
    host_reset();
}

/* An older player holding its library lock: the start goes on with the old folders, the move
   refuses naming the lock, and once it has closed the move runs. */
static void test_host_older_player(void) {
    char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX], lock_path[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "cache", "state");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp", root);
    put_file(dir, "library.json", "{\"games\":[]}");
    snprintf(lock_path, sizeof(lock_path), "%s/locks/library.lock", dir);
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/locks", root);
    CHECK(nk_platform_mkdir_p(dir), "make the old lock folder");
    Holder holder = hold_in_child(HOLD_FLOCK, lock_path);
    NkLayoutDecision d;
    host_decide(&d);
    CHECK(d.layout == NK_LAYOUT_OLD && d.message[0] == '\0', "the start goes on beside an older copy");
    NkDataMigration r;
    host_move(&r);
    CHECK(r.status == NK_MIGRATE_REFUSED && starts_with(r.message, NK_MIGRATE_CODE_LOCK) &&
              strstr(r.message, lock_path) != NULL,
          "the move refuses, naming the older copy's lock");
    let_go(&holder);
    host_move(&r);
    CHECK(r.status == NK_MIGRATE_DONE, "once it has closed, the move runs");
    host_reset();
}

/* The same folders spelled through a linked HOME, with a trailing '/', or with a relative XDG value
   (which the XDG rules say to ignore). */
static void test_host_spellings_of_the_data_home(void) {
    for (int c = 0; c < 3; c++) {
        char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX], value[NATIVE_TEST_PATH_MAX];
        new_case(root, sizeof(root));
        linux_env(root, "home/.local/share", "home/.config", "cache", "state");
        char real_home[NATIVE_TEST_PATH_MAX], link_home[NATIVE_TEST_PATH_MAX];
        join(real_home, sizeof(real_home), root, "home");
        join(link_home, sizeof(link_home), root, "linked-home");
        snprintf(dir, sizeof(dir), "%s/.local/share/nakagawa-recomp/discs", real_home);
        put_file(dir, "A.iso", "a");
        char text[NATIVE_TEST_PATH_MAX * 2];
        /* The library spells the folder as the old version saw it: resolved. */
        snprintf(text, sizeof(text), "{\"games\":[{\"iso_path\":\"%s/.local/share/nakagawa-recomp/discs/A.iso\"}]}",
                 real_home);
        snprintf(dir, sizeof(dir), "%s/.local/share/nakagawa-recomp", real_home);
        put_file(dir, "library.json", text);
        if (c == 0) {
            CHECK(symlink(real_home, link_home) == 0, "HOME through a link");
            native_test_set_env("HOME", link_home);
            native_test_set_env("XDG_DATA_HOME", NULL);
            native_test_set_env("XDG_CONFIG_HOME", NULL);
        } else if (c == 1) {
            snprintf(value, sizeof(value), "%s/.local/share/", real_home);
            native_test_set_env("XDG_DATA_HOME", value);
        } else {
            native_test_set_env("XDG_DATA_HOME", "relative/share");
        }
        NkDataMigrationPlan plan;
        CHECK(nk_platform_user_data_plan(&plan), "a plan");
        if (c == 2) {
            CHECK(strstr(plan.notes, NK_MIGRATE_CODE_XDG_IGNORED) != NULL &&
                      strstr(plan.notes, "XDG_DATA_HOME") != NULL,
                  "a relative XDG value is ignored, by name");
            snprintf(value, sizeof(value), "%s/.local/share/Nakagawa", real_home);
            CHECK(strcmp(plan.new_root, value) == 0, "and the default under HOME is used");
        }
        NkLayoutDecision d;
        host_decide(&d);
        CHECK(d.layout == NK_LAYOUT_OLD, "the old folder is found in every spelling");
        if (c == 2) CHECK(strstr(d.log, NK_MIGRATE_CODE_XDG_IGNORED) != NULL, "the log says so");
        NkDataMigration r;
        host_move(&r);
        CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten, "the data moves and the library is rewritten");
        snprintf(dir, sizeof(dir), "%s/.local/share/Nakagawa/data/library.json", real_home);
        char *library = slurp(dir);
        CHECK(library && strstr(library, "nakagawa-recomp") == NULL && strstr(library, "/Nakagawa/data/discs/A.iso"),
              c == 0   ? "a library spelled through the resolved HOME is rewritten"
              : c == 1 ? "a trailing '/' on XDG_DATA_HOME changes nothing"
                       : "and the rewritten library is the default's");
        free(library);
        host_reset();
        native_test_set_env("XDG_DATA_HOME", NULL);
    }
}
#elif defined(__APPLE__)
static void test_macos_default_folders_and_plan(void) {
    native_test_set_env("HOME", "/synthetic/profile");
    char out[NATIVE_TEST_PATH_MAX];
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/Library/Application Support/Nakagawa/data") == 0,
          "data is root/data");
    CHECK(nk_platform_resolve_path(NK_PATH_CACHE, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/Library/Application Support/Nakagawa/cache") == 0,
          "cache is inside the root");
    NkDataMigrationPlan plan;
    CHECK(nk_platform_user_data_plan(&plan) && plan.move_count == 3u && !plan.moves[0].disposable &&
              plan.moves[1].disposable && plan.moves[2].disposable && plan.kept_count == 0u,
          "the whole root moves, and Caches and Logs never block");
    CHECK(strcmp(plan.journal_file, "/synthetic/profile/Library/Application Support/.nakagawa-move-in-progress") == 0 &&
              strcmp(plan.marker_file, "/synthetic/profile/Library/Application Support/Nakagawa/.nakagawa-layout") == 0,
          "the journal is beside the root and the marker in it");
    CHECK(nk_platform_set_old_user_data_layout(true) &&
              nk_platform_resolve_path(NK_PATH_CACHE, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/Library/Caches/NakagawaRecomp") == 0,
          "the old layout is the one before #324");
    (void)nk_platform_set_old_user_data_layout(false);
}
#endif

#else

static void test_windows_takes_no_new_path(void) {
    NkDataMigrationPlan plan;
    memset(&plan, 0xAB, sizeof(plan));
    CHECK(!nk_platform_user_data_plan(&plan), "Windows already had the unified layout");
    NkLayoutDecision d;
    nk_data_layout_host(&d);
    CHECK(d.layout == NK_LAYOUT_NEW && strcmp(d.rule, "none") == 0 && d.notice[0] == '\0' &&
              d.log[0] == '\0' && !d.can_move,
          "so the start decides nothing and offers no move");
    memset(&plan, 0, sizeof(plan));
    NkDataMigration r;
    nk_data_migrate_move(&plan, false, &r);
    CHECK(r.status == NK_MIGRATE_NOTHING && !r.must_restart, "and a move has nothing to do");
    CHECK(!nk_platform_set_old_user_data_layout(true), "and Windows has no older layout to use");
}

#endif

int main(void) {
    native_test_create_root("data_migrate");
#if !defined(_WIN32) && !defined(_WIN64)
    snprintf(g_base, sizeof(g_base), "%s", native_test_get_root());
    test_rule_table();
    test_start_never_moves_or_blocks();
    test_move();
    test_old_without_saves();
    test_empty_new_folders_do_not_block();
    test_move_refuses_by_name();
    test_crash_points();
    test_concurrent_starts_after_a_crash();
    test_file_appearing_in_an_empty_destination_survives();
    test_locks_are_held_during_the_move();
    test_library_lock_is_held_for_the_rewrite();
    test_library_paths_that_need_escaping();
    test_library_paths_in_other_spellings();
    test_links_that_stay_valid();
    test_failed_rename_is_undone();
    test_failed_undo_stops_and_the_next_start_finishes();
    test_root_that_cannot_be_made();
    test_failed_flush_is_never_committed();
    test_rewritten_library_is_checked();
    test_whole_folder_rename();
    test_cache_and_logs_never_block();
#if !defined(__APPLE__) && !defined(__ANDROID__)
    test_linux_default_folders_and_plan();
    test_host_fresh_existing_and_move();
    test_unlocatable_settings_are_named();
    test_host_aliased_xdg_homes();
    test_host_older_player();
    test_host_spellings_of_the_data_home();
#elif defined(__APPLE__)
    test_macos_default_folders_and_plan();
#endif
#else
    test_windows_takes_no_new_path();
#endif
    if (g_failures) {
        fprintf(stderr, "test_data_migrate: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_data_migrate: ALL %d CHECKS PASSED\n", g_checks);
    return 0;
}
