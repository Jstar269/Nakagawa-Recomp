/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Feature-test macro first: the per-run cache root uses POSIX setenv, mkdir,
 * open, fdopendir, and getpid on non-Windows hosts. */
#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "nk_types.h"
#include "nk_iso.h"
#include "nk_library.h"
#include "nk_font.h"
#include "nk_launch.h"
#include "nk_platform.h"
#include "generated/nk_title_catalog.h"
#include "native_test_isolation.h"

static void seed_game_entry(NkGameEntry *entry, const char *disc_id, const char *title) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->disc_id, sizeof(entry->disc_id), "%s", disc_id);
    snprintf(entry->title_name, sizeof(entry->title_name), "%s", title);
    snprintf(entry->disc_version, sizeof(entry->disc_version), "1.00");
    snprintf(entry->title_id, sizeof(entry->title_id), "%s-id", disc_id);
    snprintf(entry->iso_path, sizeof(entry->iso_path), "games/%s.iso", disc_id);
    snprintf(entry->boot_executable, sizeof(entry->boot_executable), "EBOOT.BIN");
    entry->status = NK_STATUS_IDENTIFIED;
    entry->iso_size_bytes = 1048576;
}

static void test_nk_library_init_and_null_guards(void) {
    printf("[NATIVE_TEST] Testing nk_library init and NULL guards...\n");

    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    memset(&lib, 0xFF, sizeof(lib));
    nk_library_init(&lib);

    assert(lib.count == 0);
    assert(lib.library_path[0] == '\0');
    assert(nk_library_count(&lib) == 0);
    assert(nk_library_count(NULL) == 0);

    assert(nk_library_get(&lib, 0) == NULL);
    assert(nk_library_get(&lib, -1) == NULL);
    assert(nk_library_get(NULL, 0) == NULL);

    assert(nk_library_find_by_disc_id(&lib, "TEST00001") == NULL);
    assert(nk_library_find_by_disc_id(&lib, "") == NULL);
    assert(nk_library_find_by_disc_id(&lib, NULL) == NULL);
    assert(nk_library_find_by_disc_id(NULL, "TEST00001") == NULL);

    NkGameEntry entry;
    seed_game_entry(&entry, "TEST00001", "Test Title");

    assert(nk_library_add_or_update(NULL, &entry) == NK_ERROR_GENERIC);
    assert(nk_library_add_or_update(&lib, NULL) == NK_ERROR_GENERIC);

    NkGameEntry empty_id;
    memset(&empty_id, 0, sizeof(empty_id));
    assert(nk_library_add_or_update(&lib, &empty_id) == NK_ERROR_GENERIC);

    assert(nk_library_remove(NULL, "TEST00001") == NK_ERROR_GENERIC);
    assert(nk_library_remove(&lib, NULL) == NK_ERROR_GENERIC);
    assert(nk_library_remove(&lib, "") == NK_ERROR_GENERIC);

    assert(nk_library_save(NULL, "path.json") == NK_ERROR_GENERIC);
    assert(nk_library_load(NULL, "path.json") == NK_ERROR_GENERIC);

    nk_library_init(NULL); /* Should not crash */
}

static void test_nk_library_add_update_remove_bounds(void) {
    printf("[NATIVE_TEST] Testing nk_library add, update, remove, and bounds...\n");

    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&lib);

    NkGameEntry g1, g2;
    seed_game_entry(&g1, "TEST00001", "Game One");
    seed_game_entry(&g2, "TEST00002", "Game Two");

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_count(&lib) == 1);

    assert(nk_library_add_or_update(&lib, &g2) == NK_OK);
    assert(nk_library_count(&lib) == 2);

    const NkGameEntry *p1 = nk_library_find_by_disc_id(&lib, "TEST00001");
    assert(p1 != NULL);
    assert(strcmp(p1->title_name, "Game One") == 0);

    const NkGameEntry *idx0 = nk_library_get(&lib, 0);
    assert(idx0 == p1);

    const NkGameEntry *p2 = nk_library_find_by_disc_id(&lib, "TEST00002");
    assert(p2 != NULL);
    assert(strcmp(p2->title_name, "Game Two") == 0);

    /* In-place update */
    NkGameEntry g1_updated = g1;
    snprintf(g1_updated.title_name, sizeof(g1_updated.title_name), "Game One Updated");
    g1_updated.status = NK_STATUS_PLAYABLE;

    assert(nk_library_add_or_update(&lib, &g1_updated) == NK_OK);
    assert(nk_library_count(&lib) == 2);

    p1 = nk_library_find_by_disc_id(&lib, "TEST00001");
    assert(p1 != NULL);
    assert(strcmp(p1->title_name, "Game One Updated") == 0);
    assert(p1->status == NK_STATUS_PLAYABLE);

    /* Removal */
    assert(nk_library_remove(&lib, "NONEXISTENT") == NK_ERROR_FILE_NOT_FOUND);
    assert(nk_library_count(&lib) == 2);

    assert(nk_library_remove(&lib, "TEST00001") == NK_OK);
    assert(nk_library_count(&lib) == 1);
    assert(nk_library_find_by_disc_id(&lib, "TEST00001") == NULL);

    const NkGameEntry *rem = nk_library_get(&lib, 0);
    assert(rem != NULL);
    assert(strcmp(rem->disc_id, "TEST00002") == 0);

    assert(nk_library_remove(&lib, "TEST00002") == NK_OK);
    assert(nk_library_count(&lib) == 0);

    /* Capacity bounds */
    for (int i = 0; i < NK_MAX_GAMES; i++) {
        char disc_id[NK_MAX_DISC_ID_LEN];
        snprintf(disc_id, sizeof(disc_id), "FULL%05d", i);
        NkGameEntry entry;
        seed_game_entry(&entry, disc_id, "Filler Game");
        assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    }
    assert(nk_library_count(&lib) == NK_MAX_GAMES);

    NkGameEntry overflow;
    seed_game_entry(&overflow, "OVERFLOW01", "Overflow Game");
    assert(nk_library_add_or_update(&lib, &overflow) == NK_ERROR_OUT_OF_MEMORY);
    assert(nk_library_count(&lib) == NK_MAX_GAMES);
}

static void test_nk_library_save_load_escaping_and_samples(void) {
    printf("[NATIVE_TEST] Testing nk_library save, load, escaping, and sample filtering...\n");

    const char *test_root = native_test_get_root();
    char file_path[NATIVE_TEST_PATH_MAX];
    snprintf(file_path, sizeof(file_path), "%s%clibrary_test.json",
             test_root, nk_platform_path_separator());

    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&lib);

    NkGameEntry g1, g2;
    seed_game_entry(&g1, "TEST00001", "Title with \"quotes\" & \n newline & \t tab");
    snprintf(g1.last_played, sizeof(g1.last_played), "2026-03-30 12:34:56");
    g1.is_experimental = true;
    g1.executable_eboot_kind = 1;
    g1.executable_boot_kind = 2;
    g1.executable_selection = 1;
    g1.executable_boot_fallback = true;
    g1.is_prepared = true;
    g1.assets_staged = true;
    g1.extracted_asset_count = 10;
    g1.extracted_audio_count = 5;
    g1.extracted_visual_count = 3;
    g1.extracted_layout_count = 2;

    seed_game_entry(&g2, "TEST00002", "Sample Game");
    g2.is_sample = true; /* Samples must NOT be written to disk */

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_add_or_update(&lib, &g2) == NK_OK);
    assert(nk_library_count(&lib) == 2);

    assert(nk_library_save(&lib, file_path) == NK_OK);
    assert(nk_platform_file_exists(file_path));

    static NkLibrary loaded;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, file_path) == NK_OK);

    assert(nk_library_count(&loaded) == 1);
    assert(strcmp(loaded.library_path, file_path) == 0);

    const NkGameEntry *lg1 = nk_library_find_by_disc_id(&loaded, "TEST00001");
    assert(lg1 != NULL);
    assert(strcmp(lg1->title_name, g1.title_name) == 0);
    assert(strcmp(lg1->disc_version, g1.disc_version) == 0);
    assert(strcmp(lg1->iso_path, g1.iso_path) == 0);
    assert(strcmp(lg1->boot_executable, g1.boot_executable) == 0);
    assert(strcmp(lg1->last_played, g1.last_played) == 0);
    assert(lg1->is_experimental == g1.is_experimental);
    assert(lg1->executable_eboot_kind == g1.executable_eboot_kind);
    assert(lg1->executable_boot_kind == g1.executable_boot_kind);
    assert(lg1->executable_selection == g1.executable_selection);
    assert(lg1->executable_boot_fallback == g1.executable_boot_fallback);
    assert(lg1->is_prepared == g1.is_prepared);
    assert(lg1->assets_staged == g1.assets_staged);
    assert(lg1->extracted_asset_count == g1.extracted_asset_count);
    assert(lg1->extracted_audio_count == g1.extracted_audio_count);
    assert(lg1->extracted_visual_count == g1.extracted_visual_count);
    assert(lg1->extracted_layout_count == g1.extracted_layout_count);

    assert(nk_library_find_by_disc_id(&loaded, "TEST00002") == NULL);
}

/* F-c01f8c97: last_played is escaped like every other string field. The loader
   decodes escapes, so a value read from a hand-edited or tool-written library can
   hold a quote, a backslash or a control character. Failing before: the value was
   written raw, the saved file was not JSON, and the next load refused it. */
static void test_nk_library_last_played_escaped(void) {
    printf("[NATIVE_TEST] Testing that last_played is escaped on save...\n");
    char path[NATIVE_TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s%clibrary_last_played.json", native_test_get_root(),
             nk_platform_path_separator());
    static NkLibrary lib;
    nk_library_init(&lib);
    NkGameEntry g;
    seed_game_entry(&g, "TEST00003", "Last Played");
    snprintf(g.last_played, sizeof(g.last_played), "a\"b\\" "c\x01" "d");
    assert(nk_library_add_or_update(&lib, &g) == NK_OK);
    assert(nk_library_save(&lib, path) == NK_OK);

    static NkLibrary loaded;
    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, path) == NK_OK);
    const NkGameEntry *got = nk_library_find_by_disc_id(&loaded, "TEST00003");
    assert(got != NULL);
    assert(strcmp(got->last_played, g.last_played) == 0);
}

/* A full library (NK_MAX_GAMES titles, the tools' record limit) saves and
   loads whole; one title more is refused, never truncated. The player held 64
   titles before, and the first library of a whole collection loaded as empty. */
static void test_nk_library_full_library_round_trip(void) {
    printf("[NATIVE_TEST] Testing a full library round trip...\n");

    const char *test_root = native_test_get_root();
    char file_path[NATIVE_TEST_PATH_MAX];
    snprintf(file_path, sizeof(file_path), "%s%cfull_library_test.json",
             test_root, nk_platform_path_separator());

    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&lib);
    for (int i = 0; i < NK_MAX_GAMES; i++) {
        char disc_id[NK_MAX_DISC_ID_LEN];
        NkGameEntry entry;
        snprintf(disc_id, sizeof(disc_id), "FULL%05d", i);
        seed_game_entry(&entry, disc_id, "A synthetic title with a long enough name");
        snprintf(entry.iso_path, sizeof(entry.iso_path),
                 "D:\\PSP collection\\a folder name of typical length\\disc-%05d.iso", i);
        snprintf(entry.prepared_root, sizeof(entry.prepared_root),
                 "D:\\Nakagawa data\\prepared\\%s", disc_id);
        snprintf(entry.title_id, sizeof(entry.title_id), "experimental-full%05d", i);
        assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    }
    assert(nk_library_save(&lib, file_path) == NK_OK);

    static NkLibrary loaded;  /* too large for the stack (NK_MAX_GAMES) */
    assert(nk_library_load(&loaded, file_path) == NK_OK);
    assert(nk_library_count(&loaded) == NK_MAX_GAMES);
    const NkGameEntry *last = nk_library_get(&loaded, NK_MAX_GAMES - 1);
    assert(last != NULL);
    assert(strcmp(last->disc_id, lib.entries[NK_MAX_GAMES - 1].disc_id) == 0);
    assert(strcmp(last->iso_path, lib.entries[NK_MAX_GAMES - 1].iso_path) == 0);
    assert(strcmp(last->title_id, lib.entries[NK_MAX_GAMES - 1].title_id) == 0);

    /* One record over the limit: the load fails and hands back no titles. */
    size_t capacity = (size_t)(NK_MAX_GAMES + 1) * 32u + 64u;
    char *over = (char *)malloc(capacity);
    assert(over != NULL);
    size_t used = (size_t)snprintf(over, capacity, "{\"schema_version\": 1, \"games\": [");
    for (int i = 0; i <= NK_MAX_GAMES; i++) {
        used += (size_t)snprintf(over + used, capacity - used, "%s{\"disc_id\": \"OVER%05d\"}",
                                 i ? "," : "", i);
    }
    used += (size_t)snprintf(over + used, capacity - used, "]}");
    assert(used < capacity);
    FILE *f = fopen(file_path, "wb");
    assert(f != NULL);
    assert(fwrite(over, 1, used, f) == used);
    fclose(f);
    free(over);
    char bak_path[NATIVE_TEST_PATH_MAX + 8];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", file_path);
    remove(bak_path);
    assert(nk_library_load(&loaded, file_path) == NK_ERROR_GENERIC);
    assert(nk_library_count(&loaded) == 0);
    remove(file_path);
}

static void test_nk_library_null_path_default_routing(void) {
    printf("[NATIVE_TEST] Testing nk_library load/save default path routing (file_path == NULL)...\n");

    char expected_default_dir[NK_MAX_PATH];
    assert(nk_platform_get_path(NK_PATH_DATA, expected_default_dir, sizeof(expected_default_dir)));

    char expected_default_file[NK_MAX_PATH];
    int written = snprintf(expected_default_file, sizeof(expected_default_file), "%s%clibrary.json",
                           expected_default_dir, nk_platform_path_separator());
    assert(written > 0 && (size_t)written < sizeof(expected_default_file));

    /* Ensure initial clean state in isolated data root */
    remove(expected_default_file);
    char bak_file[NK_MAX_PATH + 8];
    snprintf(bak_file, sizeof(bak_file), "%s.bak", expected_default_file);
    remove(bak_file);

    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&lib);

    /* 1. Loading with file_path == NULL on non-existent file returns NK_OK with empty library */
    assert(nk_library_load(&lib, NULL) == NK_OK);
    assert(nk_library_count(&lib) == 0);
    assert(strcmp(lib.library_path, expected_default_file) == 0);

    /* 2. Add an entry and save with file_path == NULL (uses lib.library_path) */
    NkGameEntry entry;
    seed_game_entry(&entry, "DEFAULT001", "Default Path Game");
    assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    assert(nk_library_save(&lib, NULL) == NK_OK);
    assert(nk_platform_file_exists(expected_default_file));

    /* 3. Re-load with file_path == NULL and verify entry */
    static NkLibrary reloaded;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&reloaded);
    assert(nk_library_load(&reloaded, NULL) == NK_OK);
    assert(nk_library_count(&reloaded) == 1);
    assert(strcmp(reloaded.library_path, expected_default_file) == 0);

    const NkGameEntry *found = nk_library_find_by_disc_id(&reloaded, "DEFAULT001");
    assert(found != NULL);
    assert(strcmp(found->title_name, "Default Path Game") == 0);

    /* Clean up */
    remove(expected_default_file);
    remove(bak_file);
}

/* A commit re-reads the file under the library lock and changes only its own
 * entry (#1768): a title another writer saved since is kept, in the file and in
 * the caller's library, and the caller's in-memory samples stay out of the file. */
static void test_nk_library_commit_keeps_other_writers_titles(void) {
    printf("[NATIVE_TEST] Testing library commits keep another writer's titles...\n");
    char path[NATIVE_TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s%ccommit_library.json", native_test_get_root(),
             nk_platform_path_separator());
    remove(path);

    static NkLibrary mine;  /* too large for the stack (NK_MAX_GAMES) */
    static NkLibrary other;
    static NkLibrary loaded;
    nk_library_init(&mine);
    NkGameEntry entry;
    seed_game_entry(&entry, "SAMPLE001", "In-memory sample");
    entry.is_sample = true;
    assert(nk_library_add_or_update(&mine, &entry) == NK_OK);
    seed_game_entry(&entry, "COMMIT001", "First");
    assert(nk_library_commit_entry(&mine, &entry, path) == NK_OK);

    assert(nk_library_load(&other, path) == NK_OK);
    seed_game_entry(&entry, "COMMIT002", "Saved by another writer");
    assert(nk_library_add_or_update(&other, &entry) == NK_OK);
    assert(nk_library_save(&other, path) == NK_OK);

    seed_game_entry(&entry, "COMMIT003", "Third");
    assert(nk_library_commit_entry(&mine, &entry, path) == NK_OK);
    assert(nk_library_load(&loaded, path) == NK_OK);
    assert(nk_library_count(&loaded) == 3);
    assert(nk_library_find_by_disc_id(&loaded, "SAMPLE001") == NULL);
    assert(nk_library_count(&mine) == 4);
    assert(strcmp(nk_library_get(&mine, 0)->disc_id, "SAMPLE001") == 0);
    assert(nk_library_get(&mine, 0)->is_sample);
    assert(strcmp(nk_library_get(&mine, 1)->disc_id, "COMMIT001") == 0);
    assert(nk_library_find_by_disc_id(&mine, "COMMIT002") != NULL);

    assert(nk_library_commit_remove(&mine, "COMMIT002", path) == NK_OK);
    /* Already gone: another writer's removal is not an error. */
    assert(nk_library_commit_remove(&mine, "COMMIT002", path) == NK_OK);
    assert(nk_library_load(&loaded, path) == NK_OK);
    assert(nk_library_count(&loaded) == 2);
    assert(nk_library_find_by_disc_id(&loaded, "COMMIT002") == NULL);
    assert(nk_library_count(&mine) == 3);
    assert(nk_library_find_by_disc_id(&mine, "COMMIT002") == NULL);

    char bak_path[NATIVE_TEST_PATH_MAX + 8];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
    remove(path);
    remove(bak_path);
}

/* The library lock has one holder at a time. It is locks/library.lock beside
 * library.json, the file tools/nk_cli.py's bring-up locks too. */
static void test_nk_library_lock_has_one_holder(void) {
    printf("[NATIVE_TEST] Testing the library lock...\n");
    char sep = nk_platform_path_separator();
    char path[NATIVE_TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s%clock_library.json", native_test_get_root(), sep);
    char lock_path[NATIVE_TEST_PATH_MAX + 32];
    snprintf(lock_path, sizeof(lock_path), "%s%clocks%clibrary.lock", native_test_get_root(),
             sep, sep);

    NkFileLock first;
    NkFileLock second;
    assert(nk_library_lock_acquire(&first, path, 0) == NK_OK);
    assert(first.held);
    assert(nk_platform_file_exists(lock_path));
    assert(nk_library_lock_acquire(&second, path, 0) == NK_ERROR_IO);
    assert(!second.held);
    nk_platform_file_lock_release(&first);
    assert(nk_library_lock_acquire(&second, path, 0) == NK_OK);
    nk_platform_file_lock_release(&second);
    nk_platform_file_lock_release(&second);  /* releasing twice is harmless */
    assert(nk_library_lock_acquire(NULL, path, 0) == NK_ERROR_GENERIC);
    assert(nk_library_lock_acquire(&first, "", 0) == NK_ERROR_GENERIC);
}

static void test_nk_library_corrupt_load_and_backup_recovery(void) {
    printf("[NATIVE_TEST] Testing nk_library corrupt load handling and backup recovery...\n");

    const char *test_root = native_test_get_root();
    char primary_path[NATIVE_TEST_PATH_MAX];
    char bak_path[NATIVE_TEST_PATH_MAX];

    snprintf(primary_path, sizeof(primary_path), "%s%crecovery_lib.json",
             test_root, nk_platform_path_separator());
    snprintf(bak_path, sizeof(bak_path), "%s%crecovery_lib.json.bak",
             test_root, nk_platform_path_separator());

    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&lib);

    NkGameEntry g1;
    seed_game_entry(&g1, "TEST00001", "Valid Game V1");
    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);

    assert(nk_library_save(&lib, primary_path) == NK_OK);
    assert(nk_platform_file_exists(primary_path));

    snprintf(g1.title_name, sizeof(g1.title_name), "Valid Game V2");
    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_save(&lib, primary_path) == NK_OK);
    assert(nk_platform_file_exists(primary_path));
    assert(nk_platform_file_exists(bak_path));

    /* Corrupt primary file */
    FILE *f = fopen(primary_path, "wb");
    assert(f != NULL);
    fputs("{ \"schema_version\": 1, \"games\": [ corrupt_json ", f);
    fclose(f);

    static NkLibrary loaded;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) == NK_OK);
    assert(nk_library_count(&loaded) == 1);

    const NkGameEntry *lg = nk_library_find_by_disc_id(&loaded, "TEST00001");
    assert(lg != NULL);
    assert(strcmp(lg->title_name, "Valid Game V1") == 0);

    /* Corrupt both primary and backup */
    f = fopen(primary_path, "wb");
    assert(f != NULL);
    fputs("{ \"schema_version\": 999 }", f);
    fclose(f);

    f = fopen(bak_path, "wb");
    assert(f != NULL);
    fputs("NOT_JSON", f);
    fclose(f);

    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) != NK_OK);
    assert(nk_library_count(&loaded) == 0);

    /* Primary missing but backup valid */
    remove(primary_path);
    assert(nk_library_save(&lib, bak_path) == NK_OK);
    remove(primary_path);

    nk_library_init(&loaded);
    assert(nk_library_load(&loaded, primary_path) == NK_OK);
    assert(nk_library_count(&loaded) == 1);
}

static void library_test_path(char *out, size_t out_size, const char *name) {
    int written = snprintf(out, out_size, "%s%c%s", native_test_get_root(),
                           nk_platform_path_separator(), name);
    assert(written > 0 && (size_t)written < out_size);
}

static void library_write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(fclose(f) == 0);
}

static long library_read_text(const char *path, char *out, size_t out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(out, 1, out_size - 1u, f);
    fclose(f);
    out[got] = '\0';
    return (long)got;
}

/* A string value longer than its field ends at the last code point that fits:
 * a code point that did not fit used to be dropped while a later, shorter one
 * was still appended, leaving a hole in the middle of a title or path. A value
 * that fits exactly loads whole, and an unknown key's long value is skipped. */
static void test_nk_library_overlong_field_cut_cleanly(void) {
    printf("[NATIVE_TEST] Testing nk_library cuts a value longer than its field at its end...\n");
    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    NkGameEntry probe;
    const size_t title_cap = sizeof(probe.title_name);
    char path[NATIVE_TEST_PATH_MAX];
    library_test_path(path, sizeof(path), "overlong_field_lib.json");
    char bak[NATIVE_TEST_PATH_MAX + 8];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    remove(bak);

    /* title_cap - 3 ASCII bytes, then a 3-byte character that does not fit,
       then one ASCII byte that would. */
    char title[NK_MAX_TITLE_LEN + 16];
    memset(title, 'a', title_cap - 3u);
    title[title_cap - 3u] = '\0';
    char json[1024];
    snprintf(json, sizeof(json),
             "{\n  \"schema_version\": 1,\n  \"games\": [\n    {\n"
             "      \"disc_id\": \"LONG00001\",\n"
             "      \"title_name\": \"%s\\u30cax\"\n    }\n  ]\n}\n", title);
    library_write_text(path, json);
    assert(nk_library_load(&lib, path) == NK_OK);
    assert(nk_library_count(&lib) == 1);
    assert(strcmp(nk_library_get(&lib, 0)->title_name, title) == 0);

    /* The longest value the field holds loads whole. */
    memset(title, 'b', title_cap - 1u);
    title[title_cap - 1u] = '\0';
    char unknown[300];
    memset(unknown, 'u', sizeof(unknown) - 1u);
    unknown[sizeof(unknown) - 1u] = '\0';
    snprintf(json, sizeof(json),
             "{\n  \"schema_version\": 1,\n  \"games\": [\n    {\n"
             "      \"disc_id\": \"LONG00001\",\n"
             "      \"some_future_field\": \"%s\",\n"
             "      \"title_name\": \"%s\"\n    }\n  ]\n}\n", unknown, title);
    library_write_text(path, json);
    assert(nk_library_load(&lib, path) == NK_OK);
    assert(nk_library_count(&lib) == 1);
    assert(strcmp(nk_library_get(&lib, 0)->title_name, title) == 0);
    remove(path);
}

/* With library.json missing, a .bak that fails part way is no library: the
 * entries it gave before the damaged member are not handed back as the whole
 * library (a commit would then save the truncation as the primary). */
static void test_nk_library_partial_backup_without_primary(void) {
    printf("[NATIVE_TEST] Testing a damaged .bak with no primary is refused whole...\n");
    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    char path[NATIVE_TEST_PATH_MAX];
    library_test_path(path, sizeof(path), "partial_bak_lib.json");
    char bak[NATIVE_TEST_PATH_MAX + 8];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    remove(path);
    remove(bak);

    nk_library_init(&lib);
    NkGameEntry entry;
    seed_game_entry(&entry, "PART00001", "Before the damage");
    assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    assert(nk_library_save(&lib, bak) == NK_OK);
    char text[8192];
    long length = library_read_text(bak, text, sizeof(text) - 64u);
    assert(length > 0);
    char *close = strstr(text, "  ]\n}\n");
    assert(close != NULL);
    snprintf(close, sizeof(text) - (size_t)(close - text),
             "    ,{ \"disc_id\": 12 }\n  ]\n}\n");
    library_write_text(bak, text);
    assert(!nk_platform_file_exists(path));

    assert(nk_library_load(&lib, path) == NK_ERROR_GENERIC);
    assert(nk_library_count(&lib) == 0);
    assert(strcmp(lib.library_path, path) == 0);
    remove(bak);
}

/* Holds or releases a block on reading path: a handle that shares nothing on
 * Windows, no permissions on POSIX. False where the host cannot block it (a
 * POSIX root reads any file). */
#if defined(_WIN32) || defined(_WIN64)
static HANDLE s_library_block = INVALID_HANDLE_VALUE;
#endif
static bool library_block_reading(const char *path, bool block) {
#if defined(_WIN32) || defined(_WIN64)
    if (block) {
        s_library_block = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
        return s_library_block != INVALID_HANDLE_VALUE;
    }
    if (s_library_block != INVALID_HANDLE_VALUE) CloseHandle(s_library_block);
    s_library_block = INVALID_HANDLE_VALUE;
    return true;
#else
    if (block && geteuid() == 0) return false;
    return chmod(path, block ? 0 : 0600) == 0;
#endif
}

/* A library file that is there but cannot be read now (another program holds
 * it) is not damaged: its older .bak is not loaded in its place, and a commit
 * does not write over it. */
static void test_nk_library_unreadable_primary_kept(void) {
    printf("[NATIVE_TEST] Testing an unreadable library file is neither replaced nor stood in for...\n");
    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    static NkLibrary mine;
    char path[NATIVE_TEST_PATH_MAX];
    library_test_path(path, sizeof(path), "unreadable_lib.json");
    char bak[NATIVE_TEST_PATH_MAX + 8];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    remove(path);
    remove(bak);

    nk_library_init(&lib);
    NkGameEntry entry;
    seed_game_entry(&entry, "READ00001", "Older");
    assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    assert(nk_library_save(&lib, path) == NK_OK);
    seed_game_entry(&entry, "READ00002", "Newer");
    assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    assert(nk_library_save(&lib, path) == NK_OK);
    assert(nk_platform_file_exists(bak));  /* holds READ00001 only */
    char before[8192];
    assert(library_read_text(path, before, sizeof(before)) > 0);

    if (!library_block_reading(path, true)) {
        printf("[NATIVE_TEST] SKIP: this host reads any file (root); the unreadable-library case is not run\n");
        remove(path);
        remove(bak);
        return;
    }
    nk_library_init(&lib);
    NkResult load_result = nk_library_load(&lib, path);
    nk_library_init(&mine);
    seed_game_entry(&entry, "READ00003", "Committed while unreadable");
    NkResult commit_result = nk_library_commit_entry(&mine, &entry, path);
    assert(library_block_reading(path, false));

    assert(load_result == NK_ERROR_IO);
    assert(nk_library_count(&lib) == 0);
    assert(commit_result == NK_ERROR_IO);
    char after[8192];
    assert(library_read_text(path, after, sizeof(after)) > 0);
    assert(strcmp(after, before) == 0);
    assert(nk_library_load(&lib, path) == NK_OK);
    assert(nk_library_count(&lib) == 2);
    remove(path);
    remove(bak);
}

/* The default library path (the data folder plus library.json) that does not
 * fit is refused by name, not cut to a file of another name. */
static void test_nk_library_default_path_too_long(void) {
    printf("[NATIVE_TEST] Testing a default library path that does not fit is refused...\n");
    char sep = nk_platform_path_separator();
    char data_dir[NK_MAX_PATH];
    size_t used = (size_t)snprintf(data_dir, sizeof(data_dir), "%s%clong-data",
                                   native_test_get_root(), sep);
    assert(used + 20u < NK_MAX_PATH);
    /* Folder names of at most 64 bytes up to a few bytes short of the limit, so
       the folder fits but "<sep>library.json" after it does not. */
    while (used < NK_MAX_PATH - 8u) {
        size_t room = NK_MAX_PATH - 8u - used;
        size_t name = room > 65u ? 64u : room - 1u;
        if (name == 0) break;
        data_dir[used++] = sep;
        memset(data_dir + used, 'd', name);
        used += name;
        data_dir[used] = '\0';
    }
    assert(used + 14u > NK_MAX_PATH && used < NK_MAX_PATH);
    assert(nk_platform_set_app_data_dir_override(data_dir));

    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&lib);
    NkResult load_result = nk_library_load(&lib, NULL);
    NkGameEntry entry;
    seed_game_entry(&entry, "PATH00001", "Default path");
    nk_library_init(&lib);
    assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    NkResult save_result = nk_library_save(&lib, NULL);
    assert(nk_platform_set_app_data_dir_override(NULL));

    assert(load_result == NK_ERROR_PATH_TOO_LONG);
    assert(save_result == NK_ERROR_PATH_TOO_LONG);
}

#if !defined(_WIN32) && !defined(_WIN64)
/* A save whose bytes cannot be made durable (here the temp file is /dev/null,
 * where fsync fails) is not promoted over the library. */
static void test_nk_library_save_refuses_unsynced_file(void) {
    printf("[NATIVE_TEST] Testing a library save that cannot sync leaves the library as it was...\n");
    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    char path[NATIVE_TEST_PATH_MAX];
    library_test_path(path, sizeof(path), "unsynced_lib.json");
    char bak[NATIVE_TEST_PATH_MAX + 8];
    char tmp[NATIVE_TEST_PATH_MAX + 8];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    remove(path);
    remove(bak);
    remove(tmp);

    nk_library_init(&lib);
    NkGameEntry entry;
    seed_game_entry(&entry, "SYNC00001", "Saved");
    assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    assert(nk_library_save(&lib, path) == NK_OK);
    char before[8192];
    assert(library_read_text(path, before, sizeof(before)) > 0);

    assert(symlink("/dev/null", tmp) == 0);
    seed_game_entry(&entry, "SYNC00002", "Not durable");
    assert(nk_library_add_or_update(&lib, &entry) == NK_OK);
    assert(nk_library_save(&lib, path) == NK_ERROR_IO);
    /* One descriptor answers what the library is and what it holds, so both
       are said of the same file; O_NOFOLLOW refuses a link promoted over it. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
    FILE *held = fdopen(fd, "rb");
    assert(held != NULL);
    char after[8192];
    size_t after_length = fread(after, 1, sizeof(after) - 1u, held);
    after[after_length] = '\0';
    assert(fclose(held) == 0);
    assert(after_length > 0);
    assert(strcmp(after, before) == 0);
    assert(lstat(tmp, &st) != 0);
    remove(path);
    remove(bak);
}
#endif

static void test_nk_font_classify_verdict(void) {
    printf("[NATIVE_TEST] Testing nk_font_classify_verdict...\n");
    char reason[128];
    PgfVerdict verdict;

    /* 1. NULL verdict pointer */
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(NULL, reason, sizeof(reason)) == -1);
    assert(strcmp(reason, "the reader refused the file") == 0);

    /* 2. Non-zero refusal status */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_MAGIC;
    verdict.has_latin = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == -1);
    assert(strcmp(reason, "the reader refused the file") == 0);

    /* 3. Ambiguous verdict with both has_hangul and has_kana */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    verdict.has_hangul = 1;
    verdict.has_kana = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == -1);
    assert(strcmp(reason, "covers both Hangul and kana, so it names no slot") == 0);

    /* 4. Korean slot classification (has_hangul = 1) */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    verdict.has_hangul = 1;
    verdict.has_latin = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == NK_FONT_SLOT_KOREAN);

    /* 5. Japanese slot classification (has_kana = 1) */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    verdict.has_kana = 1;
    verdict.has_latin = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == NK_FONT_SLOT_JAPANESE);

    /* 6. Latin slot classification (has_latin = 1) */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    verdict.has_latin = 1;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == NK_FONT_SLOT_LATIN);

    /* 7. No probe flags set */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    memset(reason, 0, sizeof(reason));
    assert(nk_font_classify_verdict(&verdict, reason, sizeof(reason)) == -1);
    assert(strcmp(reason, "covers none of the slot probe characters") == 0);

    /* 8. Null / zero-length reason buffer edge cases */
    memset(&verdict, 0, sizeof(verdict));
    verdict.refusal = PGF_REFUSE_NONE;
    assert(nk_font_classify_verdict(&verdict, NULL, 0) == -1);
    assert(nk_font_classify_verdict(&verdict, reason, 0) == -1);

    /* 9. Truncated reason buffer */
    char small_buf[5];
    assert(nk_font_classify_verdict(&verdict, small_buf, sizeof(small_buf)) == -1);
    assert(strcmp(small_buf, "cove") == 0);
}

int main(void) {
    native_test_create_root("core-catalog");
    native_test_isolate_user_data_roots();
    native_test_assert_cache_root_isolated();
    native_test_assert_config_root_isolated();

    test_nk_library_init_and_null_guards();
    test_nk_library_add_update_remove_bounds();
    test_nk_library_save_load_escaping_and_samples();
    test_nk_library_last_played_escaped();
    test_nk_library_full_library_round_trip();
    test_nk_library_null_path_default_routing();
    test_nk_library_corrupt_load_and_backup_recovery();
    test_nk_library_commit_keeps_other_writers_titles();
    test_nk_library_lock_has_one_holder();
    test_nk_library_overlong_field_cut_cleanly();
    test_nk_library_partial_backup_without_primary();
    test_nk_library_unreadable_primary_kept();
    test_nk_library_default_path_too_long();
#if !defined(_WIN32) && !defined(_WIN64)
    test_nk_library_save_refuses_unsynced_file();
#endif
    test_nk_font_classify_verdict();

    /* Catalog SIZE is deliberately not asserted here. It is a literal that every
       new public title has to come back and edit, and it never caught anything:
       tools/title_catalog_codegen.py --verify and the codegen drift test already
       prove the catalog matches assets/titles/ exactly. What the native side can
       usefully check is that the table it compiled is internally coherent. */
    printf("[NATIVE_TEST] Verifying public title catalog integrity...\n");
    assert(nk_title_catalog_count > 0);
    for (int i = 0; i < nk_title_catalog_count; i++) {
        const NkTitleEntry *e = &nk_title_catalog_entries[i];
        assert(e->id != NULL && e->id[0] != '\0');
        assert(e->game_name != NULL && e->game_name[0] != '\0');
        assert(e->display_name != NULL && e->display_name[0] != '\0');
        assert(e->primary_disc_id != NULL && e->primary_disc_id[0] != '\0');
        /* Every disc id resolves, and resolves to THIS entry. Two entries sharing
           a disc id used to be reachable: unassigned synthetic titles all shared a
           "TEST00000" sentinel, so lookup silently returned whichever came first. */
        NkTitleEntrySnapshot by_disc = {0};
        NkTitleEntrySnapshot by_id = {0};
        assert(nk_title_catalog_find_by_disc_id(e->primary_disc_id, &by_disc));
        assert(nk_title_catalog_find_by_id(e->id, &by_id));
        assert(strcmp(by_disc.entry.id, e->id) == 0);
        assert(strcmp(by_id.entry.id, e->id) == 0);
        nk_title_catalog_snapshot_release(&by_disc);
        nk_title_catalog_snapshot_release(&by_id);
    }

    printf("[NATIVE_TEST] Verifying public source-owned title lookups...\n");
    NkTitleEntrySnapshot t_synth1 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00001", &t_synth1));
    assert(strcmp(t_synth1.entry.id, "synthetic-allegrex-v1") == 0);
    assert(t_synth1.entry.kind == NK_TITLE_KIND_SYNTHETIC);

    NkTitleEntrySnapshot t_p5 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00005", &t_p5));
    assert(strcmp(t_p5.entry.id, "pspdev-phase5-v1") == 0);
    assert(t_p5.entry.kind == NK_TITLE_KIND_SYNTHETIC);

    NkTitleEntrySnapshot t_synth2 = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00002", &t_synth2));
    assert(strcmp(t_synth2.entry.id, "synthetic-title2-v1") == 0);

    /* The display fixture is the one public title built under the layout
       src/core/nk_launch.c can actually resolve, so its addresses are load
       bearing for the launch path, not just for the catalog. */
    NkTitleEntrySnapshot t_disp = {0};
    assert(nk_title_catalog_find_by_disc_id("TEST00006", &t_disp));
    assert(strcmp(t_disp.entry.id, "display-smoke-v1") == 0);
    assert(t_disp.entry.kind == NK_TITLE_KIND_SYNTHETIC);
    assert(strcmp(t_disp.entry.game_name, "display-smoke") == 0);
    assert(t_disp.entry.executable_base == 0x08810000u);
    assert(t_disp.entry.executable_entry == 0x08810000u);

    /* Verify normalization (hyphens/spaces) */
    NkTitleEntrySnapshot normalized = {0};
    assert(nk_title_catalog_find_by_disc_id("test-00001", &normalized));
    assert(strcmp(normalized.entry.id, t_synth1.entry.id) == 0);
    nk_title_catalog_snapshot_release(&normalized);
    assert(nk_title_catalog_find_by_disc_id("  TEST_00005  ", &normalized));
    assert(strcmp(normalized.entry.id, t_p5.entry.id) == 0);
    nk_title_catalog_snapshot_release(&normalized);

    printf("[NATIVE_TEST] Verifying the public HST manifest is the one catalogued retail title...\n");
    NkTitleEntrySnapshot t_hst = {0};
    assert(nk_title_catalog_find_by_disc_id("UCUS98701", &t_hst));
    assert(strcmp(t_hst.entry.id, "hst-ucus98701-v1") == 0);
    assert(t_hst.entry.kind == NK_TITLE_KIND_RETAIL);
    assert(strcmp(t_hst.entry.codegen_profile, "hst") == 0);
    assert(strcmp(t_hst.entry.hle_profile, "hst-current") == 0);
    nk_title_catalog_snapshot_release(&t_hst);

    printf("[NATIVE_TEST] Verifying other retail IDs are NOT present in public catalog...\n");
    NkTitleEntrySnapshot missing = {0};
    assert(!nk_title_catalog_find_by_disc_id("UCES01402", &missing));
    assert(!nk_title_catalog_find_by_disc_id("ULUS99999", &missing));

    printf("[NATIVE_TEST] Verifying in-memory private overlay registration...\n");
    NkTitleEntry private_overlay;
    memset(&private_overlay, 0, sizeof(private_overlay));
    private_overlay.id = "private-local-test-v1";
    private_overlay.display_name = "Local Acceptance Private Title";
    private_overlay.kind = NK_TITLE_KIND_RETAIL;
    private_overlay.primary_disc_id = "UCES01402";

    /* Before registration: NULL */
    assert(!nk_title_catalog_find_by_disc_id("UCES01402", &missing));

    /* Register overlay */
    nk_title_catalog_register_overlay(&private_overlay);
    NkTitleEntrySnapshot found_ov = {0};
    NkTitleEntrySnapshot found_ov_by_id = {0};
    assert(nk_title_catalog_find_by_disc_id("UCES-01402", &found_ov));
    assert(strcmp(found_ov.entry.id, "private-local-test-v1") == 0);
    assert(nk_title_catalog_find_by_id("private-local-test-v1",
                                       &found_ov_by_id));
    assert(strcmp(found_ov_by_id.entry.id, found_ov.entry.id) == 0);

    /* Clear overlay */
    nk_title_catalog_clear_overlay();
    assert(strcmp(found_ov.entry.display_name,
                  "Local Acceptance Private Title") == 0);
    assert(!nk_title_catalog_find_by_disc_id("UCES01402", &missing));
    assert(!nk_title_catalog_find_by_id("private-local-test-v1", &missing));

    printf("[NATIVE_TEST] Verifying library in-memory operations...\n");
    static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
    nk_library_init(&lib);
    assert(nk_library_count(&lib) == 0);

    NkGameEntry g1;
    memset(&g1, 0, sizeof(g1));
    snprintf(g1.disc_id, sizeof(g1.disc_id), "TEST00001");
    snprintf(g1.title_name, sizeof(g1.title_name), "%s", t_synth1.entry.display_name);
    snprintf(g1.title_id, sizeof(g1.title_id), "%s", t_synth1.entry.id);
    g1.status = NK_STATUS_VERIFIED;
    g1.is_prepared = true;

    assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
    assert(nk_library_count(&lib) == 1);
    assert(nk_library_find_by_disc_id(&lib, "TEST00001") != NULL);

    assert(nk_library_remove(&lib, "TEST00001") == NK_OK);
    assert(nk_library_count(&lib) == 0);

    printf("[NATIVE_TEST] Verifying structured data directory routing...\n");
    char path_buf[512];
    assert(nk_platform_get_path(NK_PATH_CONFIG, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_DATA, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_CACHE, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_LOGS, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    assert(nk_platform_get_path(NK_PATH_SAVES, path_buf, sizeof(path_buf)));
    assert(strlen(path_buf) > 0 && nk_platform_dir_exists(path_buf));

    printf("[NATIVE_TEST] Verifying Unicode directory & file handling...\n");
    char cache_dir[512];
    assert(nk_platform_get_path(NK_PATH_CACHE, cache_dir, sizeof(cache_dir)));

    /* Test 1: Japanese characters (日本語) */
    char u_jp[600];
    snprintf(u_jp, sizeof(u_jp), "%s%c%s", cache_dir, nk_platform_path_separator(), "テスト_日本語_ディレクトリ");
    assert(nk_platform_mkdir_p(u_jp));
    assert(nk_platform_dir_exists(u_jp));

    /* Test 2: Latin accents and spaces */
    char u_latin[600];
    snprintf(u_latin, sizeof(u_latin), "%s%c%s", cache_dir, nk_platform_path_separator(), "Carpeta con Espacios y Acentos áéíóú");
    assert(nk_platform_mkdir_p(u_latin));
    assert(nk_platform_dir_exists(u_latin));

    /* Test 3: Emoji directory */
    char u_emoji[600];
    snprintf(u_emoji, sizeof(u_emoji), "%s%c%s", cache_dir, nk_platform_path_separator(), "🎮_PSP_Game_Directory");
    assert(nk_platform_mkdir_p(u_emoji));
    assert(nk_platform_dir_exists(u_emoji));

    /* Test 4: File existence in Unicode directory */
    char u_file[700];
    snprintf(u_file, sizeof(u_file), "%s%c%s", u_jp, nk_platform_path_separator(), "テスト_file.txt");
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wfile[1024];
    MultiByteToWideChar(CP_UTF8, 0, u_file, -1, wfile, 1024);
    FILE *f_uni = _wfopen(wfile, L"wb");
#else
    FILE *f_uni = fopen(u_file, "wb");
#endif
    assert(f_uni != NULL);
    const char test_data[] = "Unicode content check 12345";
    fwrite(test_data, 1, sizeof(test_data), f_uni);
    fclose(f_uni);

    assert(nk_platform_file_exists(u_file));
    assert(nk_platform_get_file_size(u_file) == (int64_t)sizeof(test_data));

    printf("[NATIVE_TEST] Verifying nk_platform_get_file_size edge cases...\n");
    /* NULL path returns -1 */
    assert(nk_platform_get_file_size(NULL) == -1);

    /* Non-existent path returns -1 */
    char missing_file_path[700];
    snprintf(missing_file_path, sizeof(missing_file_path), "%s%c%s", cache_dir, nk_platform_path_separator(), "non_existent_file_12345.dat");
    assert(nk_platform_get_file_size(missing_file_path) == -1);

    /* Zero-byte file returns 0 */
    char zero_file_path[700];
    snprintf(zero_file_path, sizeof(zero_file_path), "%s%c%s", cache_dir, nk_platform_path_separator(), "zero_byte_file.dat");
    FILE *f_zero = nk_fopen_utf8(zero_file_path, "wb");
    assert(f_zero != NULL);
    fclose(f_zero);
    assert(nk_platform_get_file_size(zero_file_path) == 0);

    printf("[NATIVE_TEST] ALL CORE NATIVE C TESTS PASSED SUCCESSFULLY!\n");
    nk_title_catalog_snapshot_release(&found_ov);
    nk_title_catalog_snapshot_release(&found_ov_by_id);
    nk_title_catalog_snapshot_release(&t_synth1);
    nk_title_catalog_snapshot_release(&t_p5);
    nk_title_catalog_snapshot_release(&t_synth2);
    nk_title_catalog_snapshot_release(&t_disp);
    native_test_assert_real_config_untouched();
    return 0;
}
