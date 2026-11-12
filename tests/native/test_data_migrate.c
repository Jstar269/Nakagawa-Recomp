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

/* ---- Another process using the old folders (B1 of the second #324 review) ---- */

typedef enum { HOLD_FLOCK, HOLD_FLOCK_SHARED, HOLD_FCNTL, HOLD_OPEN_FILE, HOLD_CWD, HOLD_NOTHING } HoldKind;

typedef struct {
    pid_t pid;
    int release; /* closing it lets the process end */
} Holder;

/* A second process that really holds `path` (a lock, an open file, its working folder), or just
   runs (HOLD_NOTHING), until let_go. */
static Holder hold_in_child(HoldKind kind, const char *path) {
    Holder holder = {-1, -1};
    int ready[2], release[2];
    if (pipe(ready) != 0) return holder;
    if (pipe(release) != 0) {
        close(ready[0]);
        close(ready[1]);
        return holder;
    }
    nk_data_migrate_release_in_use(); /* the child must not inherit this process's own lock */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(release[1]);
        bool ok = true;
        int fd = -1;
        if (kind == HOLD_FLOCK || kind == HOLD_FLOCK_SHARED) {
            fd = open(path, O_RDWR | O_CREAT, 0600);
            ok = fd >= 0 && flock(fd, kind == HOLD_FLOCK ? LOCK_EX : LOCK_SH) == 0;
        } else if (kind == HOLD_FCNTL) {
            fd = open(path, O_RDWR | O_CREAT, 0600);
            struct flock region;
            memset(&region, 0, sizeof(region));
            region.l_type = F_WRLCK;
            region.l_whence = SEEK_SET;
            ok = fd >= 0 && fcntl(fd, F_SETLK, &region) == 0;
        } else if (kind == HOLD_OPEN_FILE) {
            fd = open(path, O_RDONLY);
            ok = fd >= 0;
        } else if (kind == HOLD_CWD) {
            ok = chdir(path) == 0;
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
    CHECK(pid > 0 && got == 1 && byte == 'y', "a second process holds what it should");
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

static bool parent_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) >= cap) return false;
    memcpy(out, path, (size_t)(slash - path));
    out[slash - path] = '\0';
    return true;
}

static void check_kept_in_use(const Fixture *fx, const NkDataMigration *r, const char *what) {
    CHECK(r->status == NK_MIGRATE_KEPT_OLD && strstr(r->message, NK_MIGRATE_CODE_IN_USE) == r->message,
          what);
    CHECK(strstr(r->log, NK_MIGRATE_CODE_IN_USE) != NULL, "the log says what is using them");
    check_untouched(fx, "nothing moved while another process may use the old folders");
    CHECK(!exists(fx->new_data), "and nothing was made in the new place");
}

static void test_old_locks_held_keep_the_old_folders(void) {
    static const struct {
        HoldKind kind;
        const char *file;
        const char *what;
    } cases[] = {
        {HOLD_FLOCK, "locks/library.lock", "an older player or tool writing the library"},
        {HOLD_FLOCK, "locks/build-package-UCUS98744.lock", "an older tool building a package"},
        {HOLD_FCNTL, "games/.staging_UCUS98744.lock", "an older player staging a game"},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        Fixture fx;
        build_fixture(&fx, true);
        char path[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
        join(path, sizeof(path), fx.old_data, cases[c].file);
        CHECK(parent_of(path, dir, sizeof(dir)) && nk_platform_mkdir_p(dir), "make the lock folder");
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        Holder holder = hold_in_child(cases[c].kind, path);
        NkDataMigration r;
        nk_data_migrate_run(&plan, &r);
        check_kept_in_use(&fx, &r, cases[c].what);
        CHECK(strstr(r.log, path) != NULL, "naming the lock");
        let_go(&holder);
        nk_data_migrate_run(&plan, &r);
        CHECK(r.status == NK_MIGRATE_DONE, "once that process has ended, the next start moves the data");
        check_migrated(&fx, "and all of it arrives");
    }
}

/* A process of this version (a tool, or a player that kept the old folders) holds the in-use lock. */
static void test_in_use_lock_keeps_the_old_folders(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    snprintf(plan.in_use_file, sizeof(plan.in_use_file), "%s/share/.nakagawa-in-use.lock", fx.root);
    Holder holder = hold_in_child(HOLD_FLOCK_SHARED, plan.in_use_file);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    check_kept_in_use(&fx, &r, "a tool of this version holding the in-use lock keeps the old folders");
    let_go(&holder);
    plan.hold_in_use = true;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "the next start moves them");
    int fd = open(plan.in_use_file, O_RDWR | O_CLOEXEC);
    bool shared_held = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    bool shared_ok = fd >= 0 && flock(fd, LOCK_SH | LOCK_NB) == 0;
    if (fd >= 0) close(fd);
    CHECK(shared_held && shared_ok, "and then holds the in-use lock, shared, for the rest of its life");
    nk_data_migrate_release_in_use();
}

#if defined(__linux__)
/* Linux: an older copy that holds no lock, seen through /proc. */
static void test_processes_using_the_old_folders_are_seen(void) {
    char exe[NATIVE_TEST_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1u);
    CHECK(n > 0, "this program's name");
    if (n <= 0) return;
    exe[n] = '\0';
    const char *name = strrchr(exe, '/') ? strrchr(exe, '/') + 1 : exe;
    for (int c = 0; c < 3; c++) {
        Fixture fx;
        build_fixture(&fx, true);
        NkDataMigrationPlan plan;
        build_plan(&fx, &plan);
        plan.scan_processes = true;
        char path[NATIVE_TEST_PATH_MAX];
        Holder holder;
        if (c == 0) {
            join(path, sizeof(path), fx.old_data, "discs/UCUS98744/UCUS98744.iso");
            holder = hold_in_child(HOLD_OPEN_FILE, path); /* a game reading its disc */
        } else if (c == 1) {
            holder = hold_in_child(HOLD_CWD, fx.old_config); /* a tool started in the folder */
        } else {
            snprintf(plan.instance_name, sizeof(plan.instance_name), "%s", name);
            holder = hold_in_child(HOLD_NOTHING, ""); /* another copy of this program */
        }
        NkDataMigration r;
        nk_data_migrate_run(&plan, &r);
        char pid[32];
        snprintf(pid, sizeof(pid), "process %ld", (long)holder.pid);
        check_kept_in_use(&fx, &r, c == 0   ? "a process with a file open in the old data folder"
                                   : c == 1 ? "a process working in the old settings folder"
                                            : "another copy of the player");
        CHECK(strstr(r.log, pid) != NULL, "the log names the process");
        let_go(&holder);
        nk_data_migrate_run(&plan, &r);
        CHECK(r.status == NK_MIGRATE_DONE, "once it has ended, the next start moves the data");
    }
}
#endif

/* The other side of B1: an older copy that could not be seen wrote into the old folder after the
   move. The record of the completed run tells that apart from a second copy of the user's data. */
static void test_late_writer_is_named(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    snprintf(plan.done_file, sizeof(plan.done_file), "%s/.nakagawa-migration-done", fx.new_root);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && exists(plan.done_file), "a completed run leaves its record");
    put_file(fx.old_data, "library.json", "{\"games\":[{\"iso_path\":\"/late.iso\"}]}");
    char late_settings[NATIVE_TEST_PATH_MAX];
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_REFUSED && strstr(r.message, NK_MIGRATE_CODE_LATE_WRITES) == r.message,
          "an old folder made again after the move is named as later writes, not as a second copy");
    char want[NATIVE_TEST_PATH_MAX * 2];
    snprintf(want, sizeof(want), "Nakagawa uses your data folder %s", fx.new_data);
    CHECK(strstr(r.message, want) != NULL, "saying which folder is in use");
    CHECK(strstr(r.message, "written there later") != NULL &&
              strstr(r.message, "still running") != NULL,
          "and that the other holds what an older copy wrote after the move");
    CHECK(strcmp(r.folder_a, fx.old_data) == 0 && strcmp(r.folder_b, fx.new_data) == 0,
          "with both folders for Open both folders");
    join(late_settings, sizeof(late_settings), fx.old_data, "library.json");
    CHECK(file_is(late_settings, "{\"games\":[{\"iso_path\":\"/late.iso\"}]}"), "nothing is merged or deleted");
    join(late_settings, sizeof(late_settings), fx.new_data, "discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(late_settings, "disc bytes"), "and the moved data stays where it is in use");
    /* Without a record (a second copy made by hand), it is the both-present case. */
    CHECK(nk_remove_utf8(plan.done_file) == 0, "forget the record");
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_REFUSED && strstr(r.message, NK_MIGRATE_CODE_BOTH_PRESENT) == r.message,
          "a genuine conflict is still named as data in both places");
}

/* N6 and N8: after a completed run, nothing is finished again. */
static void test_completed_run_is_not_finished_again(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    snprintf(plan.done_file, sizeof(plan.done_file), "%s/.nakagawa-migration-done", fx.new_root);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "the first start moves everything");
    char dir[NATIVE_TEST_PATH_MAX], path[NATIVE_TEST_PATH_MAX];
    join(dir, sizeof(dir), fx.new_data, "saves/ULUS10001");
    put_file(dir, "ROOT.SAV", "a --user-data-root's own save");
    put_file(fx.new_data, "library.json", fx.library_text); /* old paths, as a restored .bak has */
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_NOTHING, "a later start has nothing to do");
    join(path, sizeof(path), dir, "ROOT.SAV");
    CHECK(file_is(path, "a --user-data-root's own save"), "a saves folder made in data later stays (N6)");
    join(path, sizeof(path), fx.new_saves, "ULUS10001");
    CHECK(!exists(path), "and is not moved up");
    CHECK(file_is(plan.library_file, fx.library_text), "the library is not read or rewritten again (N8)");
}

/* S1: library.json is rewritten under the library's own lock, held from before the first move. */
static char g_old_library_lock[NATIVE_TEST_PATH_MAX], g_new_library_lock[NATIVE_TEST_PATH_MAX];
static char g_new_library[NATIVE_TEST_PATH_MAX];
static int g_library_probes = 0, g_library_free = 0;
static pid_t g_writer = -1;

/* Another writer, as nk_library.c and tools/nk_cli.py write: under the lock, read, add, write. */
static void library_writer_child(void) {
    /* A separate program has none of the run's open files; a forked copy would share its lock. */
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
    nk_data_migrate_run(&plan, &r);
    drop_seam();
    int status = -1;
    bool writer_ok = g_writer > 0 && waitpid(g_writer, &status, 0) == g_writer && WIFEXITED(status) &&
                     WEXITSTATUS(status) == 0;
    CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten, "the run completes");
    CHECK(g_library_probes == 3 && g_library_free == 0,
          "no other writer could take the library lock during any move or before the rewrite");
    CHECK(writer_ok, "a writer that came along during the run waited for the lock, then wrote");
    char *library = slurp(plan.library_file);
    char want_iso[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want_iso, sizeof(want_iso), "\"%s/discs/UCUS98744/UCUS98744.iso\"", plan.remap_to);
    CHECK(library && strstr(library, want_iso) != NULL && strstr(library, "/writer/added.iso") != NULL,
          "so the library has both the rewritten paths and the other writer's entry");
    free(library);
}

/* S2: stored paths spelled another way than the plan's are still found. */
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
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten, "the run completes and rewrites");
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
}

/* S3: a relative link (GNU stow's kind) that leads out of the folder moving with it. */
static void test_relative_links_that_leave_the_folder(void) {
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
        nk_data_migrate_run(&plan, &r);
        CHECK(r.status == NK_MIGRATE_KEPT_OLD && strstr(r.message, NK_MIGRATE_CODE_LINKED) == r.message,
              c == 0   ? "a stowed settings file keeps the old folders"
              : c == 1 ? "a relative link out of the data folder keeps the old folders"
                       : "a link into the saves, which move on their own, keeps the old folders");
        CHECK(strstr(r.message, link_path) != NULL && strstr(r.message, text) != NULL,
              "naming the link and where it points");
        check_untouched(&fx, "nothing moved");
        CHECK(is_link(link_path), "and the link is as it was");
    }

    /* An absolute link, and a relative one that stays inside the folder: both still work after. */
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
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "do not stop the move");
    join(link_path, sizeof(link_path), fx.new_data, "isos/B.iso");
    CHECK(file_is(link_path, "b"), "the absolute link still reaches its target");
    join(link_path, sizeof(link_path), fx.new_data, "disc-alias/UCUS98744.iso");
    CHECK(file_is(link_path, "disc bytes"), "and so does the relative one inside");
}

/* S4: the folder flush after the library's rename fails (EIO, as the reviewer's LD_PRELOAD shim made
   it). The rename happened: the run is done, says so by name once, and the next start goes on. */
static bool failing_sync_dir(const char *dir) {
    (void)dir;
    errno = EIO;
    return false;
}

static void test_failed_folder_flush_is_named_not_fatal(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    nk_data_migrate_set_sync_dir_for_test(failing_sync_dir);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    nk_data_migrate_set_sync_dir_for_test(NULL);
    CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten,
          "a folder that cannot be flushed after a rename that succeeded does not fail the run");
    char want[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want, sizeof(want), "%s could not be flushed", fx.new_data);
    CHECK(strstr(r.log, NK_MIGRATE_CODE_NOT_FLUSHED) != NULL && strstr(r.log, want) != NULL,
          "it is a named log line");
    check_migrated(&fx, "the folders are where the renames put them");
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_NOTHING, "and the next start goes on");
}

/* S2's last rule: the rewritten library is read back, and a stored path that still names the old
   folder undoes the run. The folder flush after the rewrite is where a writer that ignores the lock
   (simulated here) puts an old path back. */
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
    nk_data_migrate_run(&plan, &r);
    nk_data_migrate_set_sync_dir_for_test(NULL);
    CHECK(g_sneaked, "an old path was put back after the rewrite");
    CHECK(r.status != NK_MIGRATE_DONE, "the run is not reported done");
    CHECK(strstr(r.message, "still name") != NULL || strstr(r.log, "still name") != NULL,
          "it names the stored path that still points at the old folder");
    check_untouched(&fx, "and the moves and the library are undone");
}

/* N1: a folder of another user (sudo with HOME kept) is not moved from here. */
static void test_folder_of_another_user_is_not_moved(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    nk_data_migrate_set_euid_for_test((long)getuid() + 1L);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    nk_data_migrate_set_euid_for_test(-1);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD && strstr(r.message, NK_MIGRATE_CODE_OTHER_OWNER) == r.message,
          "a folder of another user keeps the old folders");
    check_untouched(&fx, "nothing moved");
}

/* N2: an earlier run stopped part-way, and now the lock cannot be taken: neither layout is whole,
   and the message says so instead of advising to keep one folder. */
static void test_lock_failure_after_a_stopped_run(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    CHECK(nk_platform_mkdir_p(fx.new_root), "make the new root");
    CHECK(nk_rename_utf8(fx.old_data, fx.new_data) == 0, "the stopped run's first move");
    put_file(fx.root, "not-a-folder", "x");
    snprintf(plan.lock_file, sizeof(plan.lock_file), "%s/not-a-folder/.nakagawa-migration.lock", fx.root);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_REFUSED && strstr(r.message, NK_MIGRATE_CODE_LOCK) == r.message,
          "the start stops, by name");
    CHECK(strstr(r.message, "stopped") != NULL && strstr(r.message, "finishes the move") != NULL &&
              strstr(r.message, "Keep the folder you want") == NULL,
          "saying a move is unfinished, not that two copies exist");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.old_config, "settings.json");
    CHECK(file_is(path, "{\"a\":1}"), "the settings stay where they are");
}

/* N9: a data folder that is a link to nothing (a disk that is not mounted). */
static void test_dangling_link_is_named(void) {
    Fixture fx;
    build_fixture(&fx, false);
    char dir[NATIVE_TEST_PATH_MAX];
    join(dir, sizeof(dir), fx.root, "share");
    CHECK(nk_platform_mkdir_p(dir), "make the data base");
    CHECK(symlink("/nonexistent-disk/nakagawa", fx.old_data) == 0, "a link to a disk that is not there");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD && strstr(r.message, "does not exist") != NULL &&
              strstr(r.message, "mount it") != NULL,
          "says that the link's target is missing");
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
    CHECK(strcmp(plan.in_use_file, "/synthetic/xdg/.nakagawa-in-use.lock") == 0 &&
              strcmp(plan.done_file, "/synthetic/xdg/Nakagawa/.nakagawa-migration-done") == 0,
          "so is the in-use lock, and the record of a completed run is in the new root");
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
              strstr(plan.notes, "XDG_CONFIG_HOME is not set to an absolute path, and HOME is not set") != NULL,
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

/* B1 through the player's own entry point: an older player holding its library lock keeps this
   start on the old folders, and a later start, once it has closed, moves them. */
static void test_host_waits_for_an_older_player(void) {
    char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX], lock_path[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "cache", "state");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp", root);
    put_file(dir, "library.json", "{\"games\":[]}");
    snprintf(lock_path, sizeof(lock_path), "%s/locks/library.lock", dir);
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/locks", root);
    CHECK(nk_platform_mkdir_p(dir), "make the old lock folder");
    Holder holder = hold_in_child(HOLD_FLOCK, lock_path);
    NkDataMigration r;
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_KEPT_OLD && strstr(r.message, NK_MIGRATE_CODE_IN_USE) == r.message,
          "the host keeps the old folders while an older copy holds its library lock");
    char out[NATIVE_TEST_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp", root);
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) && strcmp(out, dir) == 0,
          "and this run uses the old data folder, as the older copy does");
    (void)nk_platform_set_old_user_data_layout(false);
    let_go(&holder);
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_DONE, "the next start, after the older copy closed, moves the data");
    nk_data_migrate_release_in_use();
}

/* B1's other side through the host: the late writer's dialog after a completed move. */
static void test_host_names_late_writes(void) {
    char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    linux_env(root, "share", "config", "cache", "state");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves/ULUS10001", root);
    put_file(dir, "SAVE.bin", "save bytes");
    NkDataMigration r;
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_DONE, "the first start moves the data");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves/ULUS10001", root);
    put_file(dir, "SAVE.bin", "written by an older copy after the move");
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_REFUSED && strstr(r.message, NK_MIGRATE_CODE_LATE_WRITES) == r.message,
          "a start after a late write names it, from the record of the completed move");
    char data[NATIVE_TEST_PATH_MAX];
    snprintf(data, sizeof(data), "%s/share/Nakagawa/data", root);
    CHECK(strcmp(r.folder_b, data) == 0 && strstr(r.folder_a, "/share/nakagawa-recomp") != NULL,
          "with the folder in use and the other for Open both folders");
    snprintf(dir, sizeof(dir), "%s/share/Nakagawa/saves/ULUS10001/SAVE.bin", root);
    CHECK(file_is(dir, "save bytes"), "nothing is merged");
    snprintf(dir, sizeof(dir), "%s/share/nakagawa-recomp/saves/ULUS10001/SAVE.bin", root);
    CHECK(file_is(dir, "written by an older copy after the move"), "or deleted");
    nk_data_migrate_release_in_use();
}

/* S2 through the host: the same folders spelled through a linked HOME, with a trailing '/', or
   with a relative XDG value (which the XDG rules say to ignore). */
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
        NkDataMigration r;
        nk_data_migrate_host(&r);
        CHECK(r.status == NK_MIGRATE_DONE && r.library_rewritten, "the data moves and the library is rewritten");
        if (c == 2) CHECK(strstr(r.log, NK_MIGRATE_CODE_XDG_IGNORED) != NULL, "the log says so");
        snprintf(dir, sizeof(dir), "%s/.local/share/Nakagawa/data/library.json", real_home);
        char *library = slurp(dir);
        CHECK(library && strstr(library, "nakagawa-recomp") == NULL && strstr(library, "/Nakagawa/data/discs/A.iso"),
              c == 0   ? "a library spelled through the resolved HOME is rewritten"
              : c == 1 ? "a trailing '/' on XDG_DATA_HOME changes nothing"
                       : "and the rewritten library is the default's");
        free(library);
        nk_data_migrate_release_in_use();
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
    test_old_locks_held_keep_the_old_folders();
    test_in_use_lock_keeps_the_old_folders();
#if defined(__linux__)
    test_processes_using_the_old_folders_are_seen();
#endif
    test_late_writer_is_named();
    test_completed_run_is_not_finished_again();
    test_library_lock_is_held_for_the_rewrite();
    test_library_paths_in_other_spellings();
    test_relative_links_that_leave_the_folder();
    test_failed_folder_flush_is_named_not_fatal();
    test_rewritten_library_is_checked();
    test_folder_of_another_user_is_not_moved();
    test_lock_failure_after_a_stopped_run();
    test_dangling_link_is_named();
#if !defined(__APPLE__) && !defined(__ANDROID__)
    test_linux_default_folders_and_plan();
    test_unlocatable_settings_are_named();
    test_host_keeps_linked_folders_in_use();
    test_host_aliased_xdg_homes();
    test_host_cache_stays();
    test_host_waits_for_an_older_player();
    test_host_names_late_writes();
    test_host_spellings_of_the_data_home();
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
