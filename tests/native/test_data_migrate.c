/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The one-time move into the unified per-user folder (#324, src/core/nk_data_migrate.c): a fresh
 * install, a migration, both folders present, a failed move and its undo, a run stopped part-way,
 * concurrent starts, linked and overlapping folders, a newer library, the macOS whole-folder rename
 * with its cache and logs, and the host's own plan. Every folder is built in a temporary root; the
 * real profile is never touched. The engine runs on Linux and macOS only; on Windows the test checks
 * that there is nothing to move. */

#if !defined(_WIN32) && !defined(_WIN64)
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "nk_data_migrate.h"
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
    snprintf(plan->lock_file, sizeof(plan->lock_file), "%s/share/.nakagawa-migration.lock", fx->root);
}

static void build_fixture(Fixture *fx, bool with_old) {
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
    if (!with_old) return;
    char dir[NATIVE_TEST_PATH_MAX];
    put_file(fx->old_data, "library.json", fx->library_text);
    snprintf(dir, sizeof(dir), "%s/discs/UCUS98744", fx->old_data);
    put_file(dir, "UCUS98744.iso", "disc bytes");
    snprintf(dir, sizeof(dir), "%s/saves/UCUS98744", fx->old_data);
    put_file(dir, "SAVE.bin", "save bytes");
    put_file(fx->old_config, "settings.json", "{\"a\":1}");
    put_file(fx->cache, "objects.bin", "cache");
    put_file(fx->logs, "game-last.txt", "log");
}

/* Everything the old layout held is in the new one, once. */
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
}

/* Everything is where it was before the run. */
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
}

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

/* ---- Cases ---- */

static void test_fresh_install(void) {
    Fixture fx;
    build_fixture(&fx, false);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_NOTHING, "a fresh install has nothing to move");
    CHECK(r.message[0] == '\0' && r.log[0] == '\0' && r.moved == 0u, "and says nothing");
    CHECK(!exists(fx.new_root), "and creates no folder");
}

static void test_migration(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "the old folders are moved");
    CHECK(r.moved == 3u, "three folders moved (data, its saves, settings)");
    CHECK(r.library_rewritten, "the library paths were rewritten");
    check_migrated(&fx, "data, saves and settings are in the new layout");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.cache, "objects.bin");
    CHECK(file_is(path, "cache"), "the cache stays in the XDG cache folder");
    join(path, sizeof(path), fx.logs, "game-last.txt");
    CHECK(file_is(path, "log"), "the logs stay in the XDG state folder");

    char *library = slurp(plan.library_file);
    char want_iso[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want_iso, sizeof(want_iso), "\"%s/discs/UCUS98744/UCUS98744.iso\"", plan.remap_to);
    CHECK(library && strstr(library, want_iso) != NULL, "a path under the old data folder now points at the new one");
    CHECK(library && strstr(library, "\"/external/games/lbp.iso\"") != NULL,
          "a path elsewhere is untouched");
    char lookalike[NATIVE_TEST_PATH_MAX + 64];
    snprintf(lookalike, sizeof(lookalike), "\"%s-old/not-ours.iso\"", fx.old_data);
    CHECK(library && strstr(library, lookalike) != NULL, "a folder that only starts like the old one is untouched");
    free(library);
    snprintf(path, sizeof(path), "%s.pre-migration", plan.library_file);
    CHECK(file_is(path, fx.library_text), "the original library is kept");
    snprintf(path, sizeof(path), "%s.pre-migration.tmp", plan.library_file);
    CHECK(!exists(path), "and no temporary file is left");

    NkDataMigration again;
    nk_data_migrate_run(&plan, &again);
    CHECK(again.status == NK_MIGRATE_NOTHING, "a second run finds nothing to do");
    join(path, sizeof(path), fx.new_root, "data/discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "and leaves the data alone");
}

static void test_both_present(void) {
    Fixture fx;
    build_fixture(&fx, true);
    put_file(fx.new_data, "library.json", "{\"games\":[]}");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_REFUSED, "data in both places is refused");
    CHECK(strstr(r.message, NK_MIGRATE_CODE_BOTH_PRESENT) == r.message, "with its code first");
    CHECK(strstr(r.message, fx.old_data) != NULL && strstr(r.message, fx.new_data) != NULL,
          "naming both folders");
    CHECK(strcmp(r.folder_a, fx.old_data) == 0 && strcmp(r.folder_b, fx.new_data) == 0,
          "and handing both to the dialog's Open both folders");
    CHECK(strstr(r.message, "did not move or delete") != NULL, "and saying nothing moved");
    check_untouched(&fx, "the old data is where it was");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.new_data, "library.json");
    CHECK(file_is(path, "{\"games\":[]}"), "the new data is untouched");
}

static void test_both_present_through_a_later_folder(void) {
    /* The data folder is free but the settings destination holds a file: still nothing moves. */
    Fixture fx;
    build_fixture(&fx, true);
    put_file(fx.new_config, "settings.json", "{\"new\":true}");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_REFUSED, "a taken settings folder refuses the whole move");
    CHECK(strcmp(r.folder_a, fx.old_config) == 0 && strcmp(r.folder_b, fx.new_config) == 0,
          "naming the settings folders");
    check_untouched(&fx, "nothing was moved first");
    CHECK(!exists(fx.new_data), "not even the data folder");
}

static void test_empty_new_folders_do_not_block(void) {
    /* A launch before the migration may have created the new folders empty. */
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
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "empty folders in the way are replaced and the move goes ahead");
    check_migrated(&fx, "the data arrived");
}

/* B1 of the #324 review: the check found the settings destination empty; something wrote into it
   before its move (here, during the data move). Only empty folders are ever removed, so the file
   survives and the move of the folder that matters fails and is undone. */
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
    nk_data_migrate_run(&plan, &r);
    drop_seam();
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), g_late_dir, "late.txt");
    CHECK(file_is(path, "written after the check"), "a file that appeared after the check is never deleted");
    CHECK(r.status == NK_MIGRATE_FAILED && strstr(r.message, NK_MIGRATE_CODE_FAILED) == r.message,
          "the settings move fails instead");
    CHECK(strstr(r.message, "back where it was") != NULL, "and the data move is undone");
    check_untouched(&fx, "everything is where it was");
}

/* B1: the lock is held for the whole run. A probe from another open file description (as a second
   process would have) cannot take it while a move is under way. */
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
        g_lock_free++; /* no lock file at all */
        return;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        g_lock_free++;
        (void)flock(fd, LOCK_UN);
    }
    close(fd);
}

static void test_lock_is_held_during_the_moves(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    snprintf(g_lock_path, sizeof(g_lock_path), "%s", plan.lock_file);
    g_lock_probes = 0;
    g_lock_free = 0;
    use_seam(0, false);
    g_before_rename = probe_lock;
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    drop_seam();
    CHECK(r.status == NK_MIGRATE_DONE, "the run completes");
    CHECK(g_lock_probes == 3 && g_lock_free == 0, "and no other start could take the lock during any move");
    int fd = open(plan.lock_file, O_RDWR | O_CLOEXEC);
    bool free_after = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (fd >= 0) close(fd);
    CHECK(free_after, "the lock is released when the run ends");
}

/* B1: several starts at once, as separate processes through the production entry point. Each one
   waits on a pipe until the parent closes it, so they all begin together. Exactly one moves the
   data; the others find it moved; nothing is lost or left twice. */
static void test_concurrent_starts_lose_nothing(void) {
    enum { STARTS = 8 };
    Fixture fx;
    build_fixture(&fx, true);
    char dir[NATIVE_TEST_PATH_MAX];
    CHECK(nk_platform_mkdir_p(fx.new_data), "an empty new data folder, as an earlier start leaves");
    join(dir, sizeof(dir), fx.new_root, "saves");
    CHECK(nk_platform_mkdir_p(dir), "and an empty new saves folder");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    int gate[2];
    CHECK(pipe(gate) == 0, "make the start gate");
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
            NkDataMigration r;
            nk_data_migrate_run(&plan, &r);
            _exit((int)r.status);
        }
        CHECK(children[i] > 0, "start a process");
    }
    close(gate[0]);
    close(gate[1]);
    int done = 0, nothing = 0, other = 0;
    for (int i = 0; i < STARTS; i++) {
        int status = 0;
        if (children[i] <= 0 || waitpid(children[i], &status, 0) != children[i] || !WIFEXITED(status)) {
            other++;
            continue;
        }
        int code = WEXITSTATUS(status);
        if (code == NK_MIGRATE_DONE) done++;
        else if (code == NK_MIGRATE_NOTHING) nothing++;
        else other++;
    }
    CHECK(done == 1 && nothing == STARTS - 1 && other == 0,
          "one start moved the data and every other one found it moved");
    check_migrated(&fx, "after concurrent starts, everything is in the new layout once");
    char *library = slurp(plan.library_file);
    char want_iso[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want_iso, sizeof(want_iso), "\"%s/discs/UCUS98744/UCUS98744.iso\"", plan.remap_to);
    CHECK(library && strstr(library, want_iso) != NULL, "and the library was rewritten once, whole");
    free(library);
}

static void test_old_without_saves(void) {
    Fixture fx;
    build_fixture(&fx, false);
    put_file(fx.old_data, "library.json", fx.library_text);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 1u, "an old data folder with no saves still moves");
}

/* A run stopped right after its first move (power lost, the process killed): the old data folder
   is already root/data, its saves are still in root/data/saves, and the rest has not moved. */
static void test_resume_after_the_data_move(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    CHECK(nk_platform_mkdir_p(fx.new_root), "make the new root");
    CHECK(nk_rename_utf8(fx.old_data, plan.moves[0].to) == 0, "the interrupted run's first move");
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "the next run finishes the migration");
    check_migrated(&fx, "the saves move up and the settings follow");
    char *library = slurp(plan.library_file);
    char want_iso[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want_iso, sizeof(want_iso), "\"%s/discs/UCUS98744/UCUS98744.iso\"", plan.remap_to);
    CHECK(r.library_rewritten && library && strstr(library, want_iso) != NULL,
          "and the library's stored paths are rewritten");
    free(library);
}

/* Every folder is in place, but library.json still points into the old data folder. */
static void move_everything_by_hand(const Fixture *fx, const NkDataMigrationPlan *plan) {
    CHECK(nk_platform_mkdir_p(fx->new_root), "make the new root");
    for (size_t i = 0; i < plan->move_count; i++) {
        CHECK(nk_rename_utf8(plan->moves[i].from, plan->moves[i].to) == 0, "the interrupted run's moves");
    }
}

static void test_resume_library_rewrite(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    move_everything_by_hand(&fx, &plan);
    CHECK(file_is(plan.library_file, fx.library_text), "the library still has the old paths");
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 0u && r.library_rewritten,
          "the next run moves nothing and rewrites the library");
    char path[NATIVE_TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s.pre-migration", plan.library_file);
    CHECK(file_is(path, fx.library_text), "the original library is kept");
    NkDataMigration again;
    nk_data_migrate_run(&plan, &again);
    CHECK(again.status == NK_MIGRATE_NOTHING && again.message[0] == '\0' && !again.library_rewritten,
          "a run after that finds nothing to do");
}

/* S1: a backup cut off part-way (a run killed while writing it) is not kept as the original; one
   that parses is (it is the older original). */
static void test_cut_off_backup_is_replaced(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    move_everything_by_hand(&fx, &plan);
    char backup[NATIVE_TEST_PATH_MAX];
    snprintf(backup, sizeof(backup), "%s.pre-migration", plan.library_file);
    put_file(fx.new_data, "library.json.pre-migration", "{\"gam");
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten, "the library is rewritten");
    CHECK(file_is(backup, fx.library_text), "and a cut-off backup is replaced by the whole original");

    Fixture keep;
    build_fixture(&keep, true);
    build_plan(&keep, &plan);
    move_everything_by_hand(&keep, &plan);
    put_file(keep.new_data, "library.json.pre-migration", "{\"games\":[\"older original\"]}");
    nk_data_migrate_run(&plan, &r);
    snprintf(backup, sizeof(backup), "%s.pre-migration", plan.library_file);
    CHECK(r.status == NK_MIGRATE_DONE && file_is(backup, "{\"games\":[\"older original\"]}"),
          "a whole backup already there is kept");
}

/* N3: a library that cannot be read while finishing an earlier run is named in the log. */
static void test_unreadable_library_when_finishing_is_logged(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    move_everything_by_hand(&fx, &plan);
    CHECK(nk_remove_utf8(plan.library_file) == 0, "take the library away");
    CHECK(nk_platform_mkdir_p(plan.library_file), "and put a folder in its place");
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_NOTHING, "the start goes on");
    CHECK(strstr(r.log, NK_MIGRATE_CODE_LIBRARY_NOT_UPDATED) == r.log &&
              strstr(r.log, plan.library_file) != NULL,
          "with a named log line for the library that was not updated");
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
    snprintf(new_data, sizeof(new_data), "%s/Nakagawa/data", root);
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
    put_file(new_data, "library.json", text);

    NkDataMigrationPlan plan;
    memset(&plan, 0, sizeof(plan));
    snprintf(plan.new_root, sizeof(plan.new_root), "%s/Nakagawa", root);
    snprintf(plan.remap_from, sizeof(plan.remap_from), "%s", old_data);
    snprintf(plan.remap_to, sizeof(plan.remap_to), "%s", new_data);
    snprintf(plan.library_file, sizeof(plan.library_file), "%s/library.json", new_data);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
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
}

/* Owner decision 3 (S5): a library a newer Nakagawa wrote. The player starts, nothing is moved or
   rewritten, the old folders stay in use, and the user is told by name. */
static void test_newer_library_keeps_the_old_folders(void) {
    Fixture fx;
    build_fixture(&fx, true);
    snprintf(fx.library_text, sizeof(fx.library_text),
             "{\"schema_version\": 99, \"games\": [{\"iso_path\": \"%s/discs/UCUS98744/UCUS98744.iso\"}]}",
             fx.old_data);
    put_file(fx.old_data, "library.json", fx.library_text);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD, "a library from a newer version does not stop the start");
    CHECK(strstr(r.message, NK_MIGRATE_CODE_NEWER_LIBRARY) == r.message, "it is named by its code");
    CHECK(strstr(r.message, "newer version") != NULL && strstr(r.message, plan.old_library_file) != NULL &&
              strstr(r.message, "waits for a version that understands") != NULL,
          "with the file, and that the move waits for a build that understands it");
    check_untouched(&fx, "nothing was moved or rewritten");
    CHECK(!exists(fx.new_root), "and the new folder was not made");

    /* The same library when the new folders hold data too: neither layout is complete. */
    put_file(fx.new_data, "library.json", "{\"games\":[]}");
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_REFUSED && strcmp(r.folder_b, fx.new_data) == 0,
          "with data in both layouts it is refused, naming both");
    check_untouched(&fx, "still nothing moved");
}

static void test_failed_move_is_undone(void) {
    for (int fail_at = 1; fail_at <= 3; fail_at++) {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        use_seam(fail_at, false);
        NkDataMigration r;
        nk_data_migrate_run(&plan, &r);
        drop_seam();
        CHECK(r.status == NK_MIGRATE_FAILED, "a rename that fails fails the migration");
        CHECK(strstr(r.message, NK_MIGRATE_CODE_FAILED) == r.message, "with its code first");
        CHECK(strstr(r.message, "back where it was") != NULL, "and says everything was put back");
        CHECK(r.folder_a[0] && r.folder_b[0], "and names the two folders for the dialog");
        check_untouched(&fx, "every folder is back where it was");
        CHECK(!exists(fx.new_data), "nothing is left in the new data folder");
    }
}

/* A folder that matters on another disk: refused by name, nothing copied. The seam fails the way
   rename(2) does across file systems; test_real_other_disk does it for real where it can. */
static void test_other_disk_is_named(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    use_seam(3, false);
    g_fail_errno = EXDEV;
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    drop_seam();
    CHECK(r.status == NK_MIGRATE_FAILED && strstr(r.message, NK_MIGRATE_CODE_OTHER_DISK) == r.message,
          "a move across disks is refused by its own code");
    CHECK(strstr(r.message, fx.old_config) != NULL && strstr(r.message, fx.new_config) != NULL &&
              strstr(r.message, "does not copy") != NULL,
          "naming both folders and that nothing is copied");
    CHECK(strcmp(r.folder_a, fx.old_config) == 0 && strcmp(r.folder_b, fx.new_config) == 0,
          "for Open both folders");
    check_untouched(&fx, "and every folder is back");
}

static void test_real_other_disk(void) {
    struct stat a, b;
    char other[NATIVE_TEST_PATH_MAX] = "/dev/shm/nk-dm-XXXXXX";
    if (stat("/dev/shm", &b) != 0 || stat(g_base, &a) != 0 || a.st_dev == b.st_dev || !mkdtemp(other)) {
        printf("test_data_migrate: SKIP real other-disk case (no second file system at /dev/shm)\n");
        return;
    }
    Fixture fx;
    build_fixture(&fx, true);
    char config[NATIVE_TEST_PATH_MAX];
    snprintf(config, sizeof(config), "%s/nakagawa-recomp", other);
    put_file(config, "settings.json", "{\"a\":1}");
    char stale[NATIVE_TEST_PATH_MAX];
    join(stale, sizeof(stale), fx.old_config, "settings.json");
    (void)nk_remove_utf8(stale);
    (void)rmdir(fx.old_config);
    snprintf(fx.old_config, sizeof(fx.old_config), "%s", config);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_FAILED && strstr(r.message, NK_MIGRATE_CODE_OTHER_DISK) == r.message,
          "a real cross-file-system rename of the settings is refused by name");
    check_untouched(&fx, "with every folder back where it was");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), config, "settings.json");
    (void)nk_remove_utf8(path);
    (void)rmdir(config);
    (void)rmdir(other);
}

/* S2: the undo of the saves move fails. The data folder holds the saves' old place, so it is not
   put back either; the message says where each folder is now, and the next start finishes. */
static void test_failed_undo_keeps_the_data_with_its_saves(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    use_seam(3, false); /* the settings move fails */
    g_fail_also = 4;    /* and then the undo of the saves move */
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    int calls = g_rename_calls;
    drop_seam();
    CHECK(r.status == NK_MIGRATE_FAILED, "a failed move with a failed undo is a failure");
    CHECK(calls == 4, "the data move is not undone while its saves are stuck");
    char want[NATIVE_TEST_PATH_MAX * 3];
    snprintf(want, sizeof(want), "your saves folder is %s (it was %s/saves)", fx.new_saves, fx.new_data);
    CHECK(strstr(r.message, want) != NULL, "the message says exactly where the saves are");
    snprintf(want, sizeof(want), "your data folder is %s (it was %s)", fx.new_data, fx.old_data);
    CHECK(strstr(r.message, want) != NULL, "and where the data folder is");
    CHECK(strstr(r.message, "Do not move these folders by hand") != NULL, "and not to move them by hand");
    NkDataMigration again;
    nk_data_migrate_run(&plan, &again);
    CHECK(again.status == NK_MIGRATE_DONE, "the next start carries on from there");
    check_migrated(&fx, "and every folder ends up in the new layout");
}

static void test_failed_undo_is_reported(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    use_seam(3, true); /* the third rename fails and every rename after it, the undo included */
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    drop_seam();
    CHECK(r.status == NK_MIGRATE_FAILED, "a failed move with a failed undo is a failure");
    CHECK(strstr(r.message, "could not be put back") != NULL, "that says what was left");
    CHECK(strstr(r.message, fx.new_data) != NULL && strstr(r.message, fx.old_data) != NULL,
          "naming where each folder is and where it was");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.new_data, "discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "nothing was deleted");
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
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_FAILED, "a root that cannot be created fails");
    CHECK(strstr(r.message, "did not move anything") != NULL, "before moving anything");
    check_untouched(&fx, "the old data is untouched");
}

/* B2: a data folder that is a link (GNU stow makes relative ones). Moving the link would change
   where a relative one points, so nothing moves and the old folders stay in use. */
static void test_linked_data_folder_is_not_moved(bool relative) {
    Fixture fx;
    build_fixture(&fx, false);
    char real[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
    join(real, sizeof(real), fx.root, "games/nk");
    put_file(real, "library.json", fx.library_text);
    snprintf(dir, sizeof(dir), "%s/saves/UCUS98744", real);
    put_file(dir, "SAVE.bin", "save bytes");
    CHECK(nk_platform_mkdir_p((snprintf(dir, sizeof(dir), "%s/share", fx.root), dir)), "make the data base");
    CHECK(symlink(relative ? "../games/nk" : real, fx.old_data) == 0, "link the old data folder");
    put_file(fx.old_config, "settings.json", "{\"a\":1}");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD && strstr(r.message, NK_MIGRATE_CODE_LINKED) == r.message,
          relative ? "a relative link is not moved, and the start goes on"
                   : "an absolute link is not moved, and the start goes on");
    CHECK(strstr(r.message, fx.old_data) != NULL, "naming the link");
    CHECK(is_link(fx.old_data), "the link is where it was");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.old_data, "saves/UCUS98744/SAVE.bin");
    CHECK(file_is(path, "save bytes"), "and the saves are still reached through it");
    join(path, sizeof(path), fx.old_config, "settings.json");
    CHECK(file_is(path, "{\"a\":1}"), "the settings were not moved either");
    CHECK(!exists(fx.new_root), "nothing was made in the new place");
    NkDataMigration again;
    nk_data_migrate_run(&plan, &again);
    CHECK(again.status == NK_MIGRATE_KEPT_OLD, "every start keeps them, with the same result");
}

/* macOS: the old root already had config, data and saves beside each other, so the whole folder is
   renamed; Caches and Logs come in as cache and logs, and never stop the start. */
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
    snprintf(plan->lock_file, sizeof(plan->lock_file), "%s/Support/.nakagawa-migration.lock", m->root);
}

static void test_whole_folder_rename(void) {
    MacFixture m;
    NkDataMigrationPlan plan;
    build_mac(&m, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
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
}

/* B3: a cache or logs folder never stops the start. Its move fails (here as across disks), or an
   old cache is written again after the migration: it is left where it is, with a named log line. */
static void test_cache_and_logs_never_block(void) {
    MacFixture m;
    NkDataMigrationPlan plan;
    build_mac(&m, &plan);
    use_seam(2, false);
    g_fail_errno = EXDEV;
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    drop_seam();
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 2u, "a cache that cannot move does not stop the run");
    CHECK(strstr(r.log, NK_MIGRATE_CODE_LEFT_BEHIND) != NULL && strstr(r.log, m.caches) != NULL,
          "the log names the folder left behind");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), m.caches, "c.bin");
    CHECK(file_is(path, "c"), "and it is where it was");
    join(path, sizeof(path), m.new_root, "saves/one.sav");
    CHECK(file_is(path, "sv"), "while the folders that matter moved");

    /* Something writes the old Caches folder again after the move. */
    MacFixture again;
    build_mac(&again, &plan);
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "a first start moves everything");
    put_file(again.caches, "late.bin", "late");
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_NOTHING, "a recreated old cache does not stop a later start");
    CHECK(strstr(r.log, NK_MIGRATE_CODE_LEFT_BEHIND) != NULL, "it is named in the log");
    join(path, sizeof(path), again.caches, "late.bin");
    CHECK(file_is(path, "late"), "and left where it is");
}

/* B4 in the engine: a folder that matters which is a folder kept where it is (the Linux cache, or
   the logs inside the old data folder). */
static void test_overlapping_kept_folder_keeps_the_old_folders(void) {
    Fixture fx;
    build_fixture(&fx, true);
    snprintf(fx.logs, sizeof(fx.logs), "%s/logs", fx.old_data); /* XDG_STATE_HOME == XDG_DATA_HOME */
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD && strstr(r.message, NK_MIGRATE_CODE_OVERLAP) == r.message,
          "logs inside the old data folder keep the old folders, and the start goes on");
    check_untouched(&fx, "nothing moved");
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD, "and every later start does the same, never FAILED");

    /* The same aliasing for a profile that already uses the new folders: the old path is only the
       cache's now, so it is left alone and named. */
    Fixture moved;
    build_fixture(&moved, false);
    put_file(moved.new_data, "library.json", "{\"games\":[]}");
    snprintf(moved.cache, sizeof(moved.cache), "%s", moved.old_data); /* XDG_CACHE_HOME == XDG_DATA_HOME */
    put_file(moved.cache, "objects.bin", "cache");
    build_plan(&moved, &plan);
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_NOTHING && strstr(r.log, NK_MIGRATE_CODE_LEFT_BEHIND) != NULL,
          "a cache at the old data path does not stop a profile on the new folders");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), moved.cache, "objects.bin");
    CHECK(file_is(path, "cache"), "and the cache is left alone");
}

/* The once-only notice for a start that keeps the old folders. */
static void test_notice_is_shown_once(void) {
    char root[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    char cache_home[NATIVE_TEST_PATH_MAX];
    join(cache_home, sizeof(cache_home), root, "home");
    native_test_set_env("HOME", cache_home);
    join(cache_home, sizeof(cache_home), root, "xdg-cache");
    native_test_set_env("XDG_CACHE_HOME", cache_home);
    NkDataMigration r;
    memset(&r, 0, sizeof(r));
    r.status = NK_MIGRATE_KEPT_OLD;
    snprintf(r.message, sizeof(r.message), "%s: first", NK_MIGRATE_CODE_NEWER_LIBRARY);
    CHECK(nk_data_migrate_notice_is_new(&r), "a notice is shown the first time");
    CHECK(!nk_data_migrate_notice_is_new(&r), "and not again");
    snprintf(r.message, sizeof(r.message), "%s: second", NK_MIGRATE_CODE_LINKED);
    CHECK(nk_data_migrate_notice_is_new(&r), "a different notice is shown");
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
    CHECK(strcmp(plan.new_root, "/synthetic/xdg/Nakagawa") == 0, "to the unified root");
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
    CHECK(strcmp(plan.lock_file, "/synthetic/xdg/.nakagawa-migration.lock") == 0,
          "the lock is beside the new folder, outside every move");
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

/* S4: one folder whose path cannot be formed leaves out that folder only, by name. */
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
              strstr(plan.notes, "XDG_CONFIG_HOME and HOME are not set") != NULL,
          "and a named line for the settings folder");
    NkDataMigration r;
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_DONE, "the data still moves");
    CHECK(strstr(r.log, NK_MIGRATE_CODE_PATH_UNAVAILABLE) != NULL, "and the log says what was left out");
    snprintf(dir, sizeof(dir), "%s/share/Nakagawa/saves/SAVE.bin", root);
    CHECK(file_is(dir, "save bytes"), "the saves arrived");
}

/* B2 and decision 3 through the host: the old folders stay in use for the run. */
static void test_host_keeps_linked_folders_in_use(void) {
    char root[NATIVE_TEST_PATH_MAX], real[NATIVE_TEST_PATH_MAX], link_path[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "cache", "state");
    join(real, sizeof(real), root, "games/nk");
    put_file(real, "library.json", "{\"games\":[]}");
    char dir[NATIVE_TEST_PATH_MAX];
    join(dir, sizeof(dir), root, "share");
    CHECK(nk_platform_mkdir_p(dir), "make the data base");
    join(link_path, sizeof(link_path), dir, "nakagawa-recomp");
    CHECK(symlink("../games/nk", link_path) == 0, "a relative link, as GNU stow makes");
    NkDataMigration r;
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD, "the host keeps the linked folder");
    char out[NATIVE_TEST_PATH_MAX];
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) && strcmp(out, link_path) == 0,
          "and this run's data folder is the linked one");
    join(dir, sizeof(dir), out, "library.json");
    CHECK(file_is(dir, "{\"games\":[]}"), "where the library is in view");
    CHECK(nk_data_migrate_notice_is_new(&r) && !nk_data_migrate_notice_is_new(&r),
          "and the user is told once");
    (void)nk_platform_set_old_user_data_layout(false);
}

/* B4 through the host: XDG homes that point at each other. Each keeps the old folders and starts,
   on every start, instead of failing; a fresh install with such homes stays on the old layout. */
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
        for (int start = 0; start < 2; start++) {
            NkDataMigration r;
            nk_data_migrate_host(&r);
            CHECK(r.status == NK_MIGRATE_KEPT_OLD && strstr(r.message, NK_MIGRATE_CODE_OVERLAP) == r.message,
                  "aliased XDG homes keep the old folders and the player starts");
            (void)nk_platform_set_old_user_data_layout(false);
        }
        CHECK(file_is((snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves/SAVE.bin", root), dir),
                      "save bytes"),
              "and the saves stay where the old layout finds them");
    }
    char root[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "share", "state");
    NkDataMigration r;
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD, "a fresh install whose cache is the old data folder keeps the old layout");
    (void)nk_platform_set_old_user_data_layout(false);
}

/* B3 on Linux: the cache is not moved at all, so a cache on another disk or one written again after
   the migration changes nothing. */
static void test_host_cache_stays(void) {
    char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "cache", "state");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp", root);
    put_file(dir, "library.json", "{\"games\":[]}");
    snprintf(dir, sizeof(dir), "%s/cache/nakagawa-recomp", root);
    put_file(dir, "objects.bin", "cache");
    NkDataMigration r;
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 1u, "only the data folder moves");
    snprintf(dir, sizeof(dir), "%s/cache/nakagawa-recomp/objects.bin", root);
    CHECK(file_is(dir, "cache"), "the cache is untouched");
    snprintf(dir, sizeof(dir), "%s/cache/nakagawa-recomp", root);
    put_file(dir, "late.bin", "late");
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_NOTHING && r.log[0] == '\0', "a cache written again changes nothing");
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
    CHECK(nk_platform_set_old_user_data_layout(true) &&
              nk_platform_resolve_path(NK_PATH_CACHE, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/Library/Caches/NakagawaRecomp") == 0,
          "the old layout is the one before #324");
    (void)nk_platform_set_old_user_data_layout(false);
}
#endif

#else

static void test_windows_has_nothing_to_move(void) {
    NkDataMigrationPlan plan;
    memset(&plan, 0xAB, sizeof(plan));
    CHECK(!nk_platform_user_data_plan(&plan), "Windows already had the unified layout");
    NkDataMigration r;
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_NOTHING && r.message[0] == '\0' && r.log[0] == '\0',
          "so the host run does nothing");
    CHECK(!nk_platform_set_old_user_data_layout(true), "and Windows has no older layout to use");
}

#endif

int main(void) {
    native_test_create_root("data_migrate");
#if !defined(_WIN32) && !defined(_WIN64)
    snprintf(g_base, sizeof(g_base), "%s", native_test_get_root());
    test_fresh_install();
    test_migration();
    test_both_present();
    test_both_present_through_a_later_folder();
    test_empty_new_folders_do_not_block();
    test_file_appearing_in_an_empty_destination_survives();
    test_lock_is_held_during_the_moves();
    test_concurrent_starts_lose_nothing();
    test_old_without_saves();
    test_resume_after_the_data_move();
    test_resume_library_rewrite();
    test_cut_off_backup_is_replaced();
    test_unreadable_library_when_finishing_is_logged();
    test_library_paths_that_need_escaping();
    test_newer_library_keeps_the_old_folders();
    test_failed_move_is_undone();
    test_other_disk_is_named();
    test_real_other_disk();
    test_failed_undo_keeps_the_data_with_its_saves();
    test_failed_undo_is_reported();
    test_root_that_cannot_be_made();
    test_linked_data_folder_is_not_moved(true);
    test_linked_data_folder_is_not_moved(false);
    test_whole_folder_rename();
    test_cache_and_logs_never_block();
    test_overlapping_kept_folder_keeps_the_old_folders();
    test_notice_is_shown_once();
#if !defined(__APPLE__) && !defined(__ANDROID__)
    test_linux_default_folders_and_plan();
    test_unlocatable_settings_are_named();
    test_host_keeps_linked_folders_in_use();
    test_host_aliased_xdg_homes();
    test_host_cache_stays();
#elif defined(__APPLE__)
    test_macos_default_folders_and_plan();
#endif
#else
    test_windows_has_nothing_to_move();
#endif
    if (g_failures) {
        fprintf(stderr, "test_data_migrate: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_data_migrate: ALL %d CHECKS PASSED\n", g_checks);
    return 0;
}
