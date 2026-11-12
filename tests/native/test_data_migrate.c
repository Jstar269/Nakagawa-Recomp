/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The one-time move into the unified per-user folder (#324, src/core/nk_data_migrate.c): a fresh
 * install, a migration, both folders present, a failed move, the macOS whole-folder rename and the
 * host's own plan. Every folder is built in a temporary root; the real profile is never touched. */

#include "nk_data_migrate.h"
#include "nk_platform.h"
#include "native_test_isolation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The Linux-shaped old layout under `root`: data (with a library, a converted disc and saves), and
   the XDG config, cache and state folders. */
typedef struct {
    char root[NATIVE_TEST_PATH_MAX];
    char old_data[NATIVE_TEST_PATH_MAX], old_config[NATIVE_TEST_PATH_MAX];
    char old_cache[NATIVE_TEST_PATH_MAX], old_logs[NATIVE_TEST_PATH_MAX];
    char new_root[NATIVE_TEST_PATH_MAX];
    char library_text[1024];
} Fixture;

static void build_plan(const Fixture *fx, NkDataMigrationPlan *plan) {
    memset(plan, 0, sizeof(*plan));
    snprintf(plan->new_root, sizeof(plan->new_root), "%s", fx->new_root);
    char to_data[NATIVE_TEST_PATH_MAX], to_data_saves[NATIVE_TEST_PATH_MAX];
    char to_saves[NATIVE_TEST_PATH_MAX], to_config[NATIVE_TEST_PATH_MAX];
    char to_cache[NATIVE_TEST_PATH_MAX], to_logs[NATIVE_TEST_PATH_MAX];
    join(to_data, sizeof(to_data), fx->new_root, "data");
    snprintf(to_data_saves, sizeof(to_data_saves), "%s/saves", to_data);
    join(to_saves, sizeof(to_saves), fx->new_root, "saves");
    join(to_config, sizeof(to_config), fx->new_root, "config");
    join(to_cache, sizeof(to_cache), fx->new_root, "cache");
    join(to_logs, sizeof(to_logs), fx->new_root, "logs");
#define ADD(f, t, after)                                                              \
    do {                                                                              \
        NkMigrateMove *m = &plan->moves[plan->move_count++];                          \
        snprintf(m->from, sizeof(m->from), "%s", (f));                                \
        snprintf(m->to, sizeof(m->to), "%s", (t));                                    \
        m->after_previous = (after);                                                  \
    } while (0)
    ADD(fx->old_data, to_data, false);
    ADD(to_data_saves, to_saves, true);
    ADD(fx->old_config, to_config, false);
    ADD(fx->old_cache, to_cache, false);
    ADD(fx->old_logs, to_logs, false);
#undef ADD
    snprintf(plan->remap_from, sizeof(plan->remap_from), "%s", fx->old_data);
    snprintf(plan->remap_to, sizeof(plan->remap_to), "%s", to_data);
    snprintf(plan->library_file, sizeof(plan->library_file), "%s/library.json", to_data);
}

static void build_fixture(Fixture *fx, bool with_old) {
    new_case(fx->root, sizeof(fx->root));
    join(fx->old_data, sizeof(fx->old_data), fx->root, "share/nakagawa-recomp");
    join(fx->old_config, sizeof(fx->old_config), fx->root, "config/nakagawa-recomp");
    join(fx->old_cache, sizeof(fx->old_cache), fx->root, "cache/nakagawa-recomp");
    join(fx->old_logs, sizeof(fx->old_logs), fx->root, "state/nakagawa-recomp/logs");
    join(fx->new_root, sizeof(fx->new_root), fx->root, "share/Nakagawa");
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
    put_file(fx->old_cache, "objects.bin", "cache");
    put_file(fx->old_logs, "game-last.txt", "log");
}

/* ---- Rename seam ---- */

static int g_rename_calls = 0;
static int g_fail_at = 0;        /* fail the Nth rename (1-based), 0 never */
static bool g_fail_all_after = false;

static int counting_rename(const char *from, const char *to) {
    g_rename_calls++;
    if (g_fail_at && (g_rename_calls == g_fail_at || (g_fail_all_after && g_rename_calls > g_fail_at))) {
        return -1;
    }
    return nk_rename_utf8(from, to);
}

static void use_seam(int fail_at, bool fail_all_after) {
    g_rename_calls = 0;
    g_fail_at = fail_at;
    g_fail_all_after = fail_all_after;
    nk_data_migrate_set_rename_for_test(counting_rename);
}

static void drop_seam(void) { nk_data_migrate_set_rename_for_test(NULL); }

/* ---- Cases ---- */

static void test_fresh_install(void) {
    Fixture fx;
    build_fixture(&fx, false);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_NOTHING, "a fresh install has nothing to move");
    CHECK(r.message[0] == '\0' && r.moved == 0u, "and says nothing");
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
    CHECK(r.moved == 5u, "five folders moved (data, its saves, config, cache, logs)");
    CHECK(r.library_rewritten, "the library paths were rewritten");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.new_root, "data/discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "the disc copy is in the new data folder");
    join(path, sizeof(path), fx.new_root, "saves/UCUS98744/SAVE.bin");
    CHECK(file_is(path, "save bytes"), "the saves moved up to the root's saves folder");
    join(path, sizeof(path), fx.new_root, "data/saves");
    CHECK(!exists(path), "and are no longer under data");
    join(path, sizeof(path), fx.new_root, "config/settings.json");
    CHECK(file_is(path, "{\"a\":1}"), "settings are in config");
    join(path, sizeof(path), fx.new_root, "cache/objects.bin");
    CHECK(file_is(path, "cache"), "the cache is in cache");
    join(path, sizeof(path), fx.new_root, "logs/game-last.txt");
    CHECK(file_is(path, "log"), "the logs are in logs");
    CHECK(!exists(fx.old_data) && !exists(fx.old_config) && !exists(fx.old_cache) &&
              !exists(fx.old_logs), "the old folders are gone");

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

    NkDataMigration again;
    nk_data_migrate_run(&plan, &again);
    CHECK(again.status == NK_MIGRATE_NOTHING, "a second run finds nothing to do");
    join(path, sizeof(path), fx.new_root, "data/discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "and leaves the data alone");
}

static void test_both_present(void) {
    Fixture fx;
    build_fixture(&fx, true);
    char new_data[NATIVE_TEST_PATH_MAX];
    join(new_data, sizeof(new_data), fx.new_root, "data");
    put_file(new_data, "library.json", "{\"games\":[]}");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_REFUSED, "data in both places is refused");
    CHECK(strstr(r.message, NK_MIGRATE_CODE_BOTH_PRESENT) == r.message, "with its code first");
    CHECK(strstr(r.message, fx.old_data) != NULL && strstr(r.message, new_data) != NULL,
          "naming both folders");
    CHECK(strstr(r.message, "did not move or delete") != NULL, "and saying nothing moved");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.old_data, "discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "the old data is where it was");
    CHECK(exists(fx.old_config) && exists(fx.old_cache) && exists(fx.old_logs),
          "so are the other old folders: a refusal moves none of them");
    join(path, sizeof(path), new_data, "library.json");
    CHECK(file_is(path, "{\"games\":[]}"), "the new data is untouched");
}

static void test_both_present_through_a_later_folder(void) {
    /* The data folder is free but the config destination holds a file: still nothing moves. */
    Fixture fx;
    build_fixture(&fx, true);
    char new_config[NATIVE_TEST_PATH_MAX];
    join(new_config, sizeof(new_config), fx.new_root, "config");
    put_file(new_config, "settings.json", "{\"new\":true}");
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_REFUSED, "a taken config folder refuses the whole move");
    CHECK(exists(fx.old_data) && exists(fx.old_config), "nothing was moved first");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.new_root, "data");
    CHECK(!exists(path), "not even the data folder");
}

static void test_empty_new_folders_do_not_block(void) {
    /* A launch before the migration may have created the new folders empty. */
    Fixture fx;
    build_fixture(&fx, true);
    const char *leaves[] = {"data", "config", "cache", "logs", "saves", "data/titles"};
    for (size_t i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
        char dir[NATIVE_TEST_PATH_MAX];
        join(dir, sizeof(dir), fx.new_root, leaves[i]);
        CHECK(nk_platform_mkdir_p(dir), "make an empty new folder");
    }
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE, "empty folders in the way are cleared and the move goes ahead");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.new_root, "data/discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "the data arrived");
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
   is already root/data, its saves are still in root/data/saves, and the rest has not moved. The
   next run must lift the saves out; nothing else would ever look in data/saves again. */
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
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.new_root, "saves/UCUS98744/SAVE.bin");
    CHECK(file_is(path, "save bytes"), "the saves left in data/saves move up to the root's saves");
    join(path, sizeof(path), fx.new_root, "data/saves");
    CHECK(!exists(path), "and are no longer under data");
    join(path, sizeof(path), fx.new_root, "config/settings.json");
    CHECK(file_is(path, "{\"a\":1}"), "the folders not yet moved are moved");
    char *library = slurp(plan.library_file);
    char want_iso[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want_iso, sizeof(want_iso), "\"%s/discs/UCUS98744/UCUS98744.iso\"", plan.remap_to);
    CHECK(r.library_rewritten && library && strstr(library, want_iso) != NULL,
          "and the library's stored paths are rewritten");
    free(library);
}

/* A run stopped after its last move and before the library rewrite: every folder is in place, but
   library.json still points into the old data folder, which is gone. The next run rewrites it. */
static void test_resume_library_rewrite(void) {
    Fixture fx;
    build_fixture(&fx, true);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    CHECK(nk_platform_mkdir_p(fx.new_root), "make the new root");
    for (size_t i = 0; i < plan.move_count; i++) {
        CHECK(nk_rename_utf8(plan.moves[i].from, plan.moves[i].to) == 0, "the interrupted run's moves");
    }
    CHECK(file_is(plan.library_file, fx.library_text), "the library still has the old paths");
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 0u && r.library_rewritten,
          "the next run moves nothing and rewrites the library");
    char *library = slurp(plan.library_file);
    char want_iso[NATIVE_TEST_PATH_MAX + 64];
    snprintf(want_iso, sizeof(want_iso), "\"%s/discs/UCUS98744/UCUS98744.iso\"", plan.remap_to);
    CHECK(library && strstr(library, want_iso) != NULL, "a stored path now points at the new folder");
    free(library);
    char path[NATIVE_TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s.pre-migration", plan.library_file);
    CHECK(file_is(path, fx.library_text), "the original library is kept");
    NkDataMigration again;
    nk_data_migrate_run(&plan, &again);
    CHECK(again.status == NK_MIGRATE_NOTHING && again.message[0] == '\0' && !again.library_rewritten,
          "a run after that finds nothing to do");
}

static void test_failed_move_is_undone(void) {
    for (int fail_at = 1; fail_at <= 4; fail_at++) {
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
        CHECK(strstr(r.message, "put back") != NULL, "and says everything was put back");
        char path[NATIVE_TEST_PATH_MAX];
        join(path, sizeof(path), fx.old_data, "discs/UCUS98744/UCUS98744.iso");
        CHECK(file_is(path, "disc bytes"), "the disc copy is back where it was");
        join(path, sizeof(path), fx.old_data, "saves/UCUS98744/SAVE.bin");
        CHECK(file_is(path, "save bytes"), "the saves are back under the old data folder");
        join(path, sizeof(path), fx.old_data, "library.json");
        CHECK(file_is(path, fx.library_text), "the library is not rewritten");
        CHECK(file_is((snprintf(path, sizeof(path), "%s/settings.json", fx.old_config), path), "{\"a\":1}") &&
                  exists(fx.old_cache) && exists(fx.old_logs), "config, cache and logs are back");
        join(path, sizeof(path), fx.new_root, "data");
        CHECK(!exists(path), "nothing is left in the new data folder");
    }
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
    CHECK(strstr(r.message, "still in the new place") != NULL, "that says what was left");
    char new_data[NATIVE_TEST_PATH_MAX];
    join(new_data, sizeof(new_data), fx.new_root, "data");
    CHECK(strstr(r.message, new_data) != NULL && strstr(r.message, fx.old_data) != NULL,
          "naming where each folder is and where it belongs");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.new_root, "data/discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "nothing was deleted");
}

static void test_root_that_cannot_be_made(void) {
    /* A real failure, no seam: the place the new folder belongs is a file. */
    Fixture fx;
    build_fixture(&fx, true);
    put_file(fx.root, "blocker", "not a folder");
    snprintf(fx.new_root, sizeof(fx.new_root), "%s/blocker/Nakagawa", fx.root);
    NkDataMigrationPlan plan;
    build_plan(&fx, &plan);
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_FAILED, "a root that cannot be created fails");
    CHECK(strstr(r.message, "did not move anything") != NULL, "before moving anything");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), fx.old_data, "discs/UCUS98744/UCUS98744.iso");
    CHECK(file_is(path, "disc bytes"), "the old data is untouched");
}

/* The macOS shape: the old root already had config, data and saves beside each other, so the whole
   folder is renamed; Caches and Logs come in as cache and logs. */
static void test_whole_folder_rename(void) {
    char root[NATIVE_TEST_PATH_MAX], old_root[NATIVE_TEST_PATH_MAX], caches[NATIVE_TEST_PATH_MAX];
    char logs[NATIVE_TEST_PATH_MAX], new_root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    join(old_root, sizeof(old_root), root, "Support/NakagawaRecomp");
    join(caches, sizeof(caches), root, "Caches/NakagawaRecomp");
    join(logs, sizeof(logs), root, "Logs/NakagawaRecomp");
    join(new_root, sizeof(new_root), root, "Support/Nakagawa");
    snprintf(dir, sizeof(dir), "%s/data", old_root);
    put_file(dir, "library.json", "{\"x\":\"unused\"}");
    snprintf(dir, sizeof(dir), "%s/config", old_root);
    put_file(dir, "settings.json", "s");
    snprintf(dir, sizeof(dir), "%s/saves", old_root);
    put_file(dir, "one.sav", "sv");
    put_file(caches, "c.bin", "c");
    put_file(logs, "l.txt", "l");

    NkDataMigrationPlan plan;
    memset(&plan, 0, sizeof(plan));
    const char *from[3] = {old_root, caches, logs};
    char to_cache[NATIVE_TEST_PATH_MAX], to_logs[NATIVE_TEST_PATH_MAX];
    join(to_cache, sizeof(to_cache), new_root, "cache");
    join(to_logs, sizeof(to_logs), new_root, "logs");
    const char *to[3] = {new_root, to_cache, to_logs};
    for (int i = 0; i < 3; i++) {
        snprintf(plan.moves[i].from, sizeof(plan.moves[i].from), "%s", from[i]);
        snprintf(plan.moves[i].to, sizeof(plan.moves[i].to), "%s", to[i]);
    }
    plan.move_count = 3u;
    NkDataMigration r;
    nk_data_migrate_run(&plan, &r);
    CHECK(r.status == NK_MIGRATE_DONE && r.moved == 3u, "the macOS-shaped move runs");
    char path[NATIVE_TEST_PATH_MAX];
    join(path, sizeof(path), new_root, "config/settings.json");
    CHECK(file_is(path, "s"), "config arrived with the whole folder");
    join(path, sizeof(path), new_root, "saves/one.sav");
    CHECK(file_is(path, "sv"), "saves too");
    join(path, sizeof(path), new_root, "cache/c.bin");
    CHECK(file_is(path, "c"), "Caches became cache");
    join(path, sizeof(path), new_root, "logs/l.txt");
    CHECK(file_is(path, "l"), "Logs became logs");
    CHECK(!exists(old_root) && !exists(caches) && !exists(logs), "the old folders are gone");
}

/* ---- The host's own plan and default folders ---- */

#if !defined(_WIN32) && !defined(_WIN64)
static void set_env(const char *name, const char *value) { native_test_set_env(name, value); }

static void test_posix_default_folders_and_plan(void) {
    set_env("HOME", "/synthetic/profile");
    set_env("XDG_DATA_HOME", "/synthetic/xdg");
    set_env("XDG_CONFIG_HOME", "/synthetic/xdg-config");
    set_env("XDG_CACHE_HOME", "/synthetic/xdg-cache");
    set_env("XDG_STATE_HOME", "/synthetic/xdg-state");
    char out[NATIVE_TEST_PATH_MAX];
#if !defined(__APPLE__) && !defined(__ANDROID__)
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/Nakagawa/data") == 0, "data is root/data");
    CHECK(nk_platform_resolve_path(NK_PATH_CONFIG, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/Nakagawa/config") == 0, "config ignores XDG_CONFIG_HOME");
    CHECK(nk_platform_resolve_path(NK_PATH_CACHE, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/Nakagawa/cache") == 0, "cache ignores XDG_CACHE_HOME");
    CHECK(nk_platform_resolve_path(NK_PATH_LOGS, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/Nakagawa/logs") == 0, "logs ignores XDG_STATE_HOME");
    CHECK(nk_platform_resolve_path(NK_PATH_SAVES, out, sizeof(out)) &&
              strcmp(out, "/synthetic/xdg/Nakagawa/saves") == 0, "saves is root/saves");
    NkDataMigrationPlan plan;
    CHECK(nk_platform_user_data_plan(&plan), "Linux has a plan");
    CHECK(strcmp(plan.new_root, "/synthetic/xdg/Nakagawa") == 0, "to the unified root");
    CHECK(plan.move_count == 5u && strcmp(plan.moves[0].from, "/synthetic/xdg/nakagawa-recomp") == 0 &&
              strcmp(plan.moves[0].to, "/synthetic/xdg/Nakagawa/data") == 0, "from the old data folder");
    CHECK(plan.moves[1].after_previous &&
              strcmp(plan.moves[1].from, "/synthetic/xdg/Nakagawa/data/saves") == 0 &&
              strcmp(plan.moves[1].to, "/synthetic/xdg/Nakagawa/saves") == 0, "with the saves lifted out");
    CHECK(strcmp(plan.moves[2].from, "/synthetic/xdg-config/nakagawa-recomp") == 0 &&
              strcmp(plan.moves[3].from, "/synthetic/xdg-cache/nakagawa-recomp") == 0 &&
              strcmp(plan.moves[4].from, "/synthetic/xdg-state/nakagawa-recomp/logs") == 0,
          "and the XDG config, cache and state folders");
    CHECK(strcmp(plan.remap_from, "/synthetic/xdg/nakagawa-recomp") == 0 &&
              strcmp(plan.remap_to, "/synthetic/xdg/Nakagawa/data") == 0 &&
              strcmp(plan.library_file, "/synthetic/xdg/Nakagawa/data/library.json") == 0,
          "and the library's stored paths are remapped");
    set_env("XDG_DATA_HOME", "");
    set_env("XDG_CONFIG_HOME", "");
    set_env("XDG_CACHE_HOME", "");
    set_env("XDG_STATE_HOME", "");
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/.local/share/Nakagawa/data") == 0, "HOME is the fallback");
    CHECK(nk_platform_user_data_plan(&plan) &&
              strcmp(plan.moves[2].from, "/synthetic/profile/.config/nakagawa-recomp") == 0 &&
              strcmp(plan.moves[3].from, "/synthetic/profile/.cache/nakagawa-recomp") == 0 &&
              strcmp(plan.moves[4].from, "/synthetic/profile/.local/state/nakagawa-recomp/logs") == 0,
          "the old XDG defaults come from HOME");
#elif defined(__APPLE__)
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/Library/Application Support/Nakagawa/data") == 0,
          "data is root/data");
    CHECK(nk_platform_resolve_path(NK_PATH_CACHE, out, sizeof(out)) &&
              strcmp(out, "/synthetic/profile/Library/Application Support/Nakagawa/cache") == 0,
          "cache is inside the root");
#endif
}
#else
static void test_windows_has_nothing_to_move(void) {
    NkDataMigrationPlan plan;
    memset(&plan, 0xAB, sizeof(plan));
    CHECK(!nk_platform_user_data_plan(&plan), "Windows already had the unified layout");
    NkDataMigration r;
    nk_data_migrate_host(&r);
    CHECK(r.status == NK_MIGRATE_NOTHING && r.message[0] == '\0', "so the host run does nothing");
}
#endif

int main(void) {
    native_test_create_root("data_migrate");
    snprintf(g_base, sizeof(g_base), "%s", native_test_get_root());
    /* Forward slashes everywhere: library.json stores paths as JSON strings, and the engine leaves
       a path with a backslash alone (it would need escaping), which only Windows temp folders have. */
    for (char *c = g_base; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    test_fresh_install();
    test_migration();
    test_both_present();
    test_both_present_through_a_later_folder();
    test_empty_new_folders_do_not_block();
    test_old_without_saves();
    test_resume_after_the_data_move();
    test_resume_library_rewrite();
    test_failed_move_is_undone();
    test_failed_undo_is_reported();
    test_root_that_cannot_be_made();
    test_whole_folder_rename();
#if !defined(_WIN32) && !defined(_WIN64)
    test_posix_default_folders_and_plan();
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
