/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Which per-user folders a Linux or macOS start uses (#324, src/core/nk_data_migrate.h): every case
 * of the rule's table (tools/test_prereq_fetcher.py reads the same table) through the production
 * resolver, the edges of the XDG variables and HOME, a decision kept for the whole run, a next start
 * that follows the old folders being moved away, a start that creates nothing outside the layout
 * it uses, and a real-profile guard that watches both layouts' settings folders. Every folder is made
 * in a temporary root on the
 * real file system; the real profile is never touched. Windows has one layout and decides nothing. */

#if !defined(_WIN32) && !defined(_WIN64)
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "nk_data_migrate.h"
#include "nk_platform.h"
#include "native_test_isolation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) && !defined(_WIN64)
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

/* The notice names the old layout in use, then the new folder, on every host. */
static void test_notice(void) {
    static NkUserDataLayout layout;
    char notice[NK_DATA_LAYOUT_NOTICE_MAX];
    memset(&layout, 0, sizeof(layout));
    const char *new_paths[3] = {"/h/.local/share/Nakagawa/config", "/h/.local/share/Nakagawa/data",
                                "/h/.local/share/Nakagawa/saves"};
    const char *old_paths[3] = {"/h/.config/nakagawa-recomp", "/h/.local/share/nakagawa-recomp",
                                "/h/.local/share/nakagawa-recomp/saves"};
    for (int i = 0; i < 3; ++i) {
        snprintf(layout.new_layout.path[i == 0 ? NK_PATH_CONFIG : i == 1 ? NK_PATH_DATA : NK_PATH_SAVES],
                 NK_MAX_PATH, "%s", new_paths[i]);
        snprintf(layout.old_layout.path[i == 0 ? NK_PATH_CONFIG : i == 1 ? NK_PATH_DATA : NK_PATH_SAVES],
                 NK_MAX_PATH, "%s", old_paths[i]);
    }
    layout.old_layout.exists = true;
    layout.old_in_use = true;
    CHECK(!nk_data_layout_notice(&layout, notice, sizeof(notice)) && notice[0] == '\0',
          "no notice when the new folder does not exist");
    layout.new_layout.exists = true;
    CHECK(nk_data_layout_notice(&layout, notice, sizeof(notice)),
          "a notice when the old layout is in use and the new folder exists");
    CHECK(strcmp(notice, "Nakagawa uses /h/.local/share/nakagawa-recomp (settings in "
                         "/h/.config/nakagawa-recomp). Nakagawa data is also in "
                         "/h/.local/share/Nakagawa, which is not used. To use it, see \"Switching "
                         "to the new data folder\" in docs/SETUP.md.") == 0,
          "it names the old data and settings folders in use, then the new folder, and points at "
          "the switching steps");
    CHECK(!nk_data_layout_notice(&layout, notice, 16u) && notice[0] == '\0',
          "a notice that does not fit is not cut short");
}

#if !defined(_WIN32) && !defined(_WIN64)

static char g_base[NATIVE_TEST_PATH_MAX];
static int g_case = 0;
static NkUserDataLayout g_layout;

static void join(char *out, size_t cap, const char *a, const char *b) {
    snprintf(out, cap, "%s/%s", a, b);
}

static void put_file(const char *dir, const char *name) {
    char path[NATIVE_TEST_PATH_MAX];
    CHECK(nk_platform_mkdir_p(dir), "make a folder");
    join(path, sizeof(path), dir, name);
    FILE *f = nk_fopen_utf8(path, "wb");
    CHECK(f != NULL, "write a file");
    if (!f) return;
    fputs("{}", f);
    fclose(f);
}

static bool exists(const char *path) { return path[0] && !nk_platform_path_missing(path); }

static void parent_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0';
}

/* A fresh case folder <base>/c<N>, with every variable the resolver reads pointing into it. */
static void new_case(char *root, size_t cap) {
    char value[NATIVE_TEST_PATH_MAX];
    snprintf(root, cap, "%s/c%d", g_base, ++g_case);
    join(value, sizeof(value), root, "home");
    CHECK(nk_platform_mkdir_p(value), "make the case's home");
    native_test_set_env("HOME", value);
    join(value, sizeof(value), root, "share");
    native_test_set_env("XDG_DATA_HOME", value);
    join(value, sizeof(value), root, "config");
    native_test_set_env("XDG_CONFIG_HOME", value);
    join(value, sizeof(value), root, "cache");
    native_test_set_env("XDG_CACHE_HOME", value);
    join(value, sizeof(value), root, "state");
    native_test_set_env("XDG_STATE_HOME", value);
}

/* The decision is kept for the run while HOME and the XDG variables stay the same. A case that has
   just made its folders asks again as a new start would: XDG_CONFIG_HOME spelled with a trailing
   '/' names the same folders, and the changed spelling makes the resolver decide afresh. */
static void decide_as_a_new_start(const char *root) {
    char value[NATIVE_TEST_PATH_MAX];
    snprintf(value, sizeof(value), "%s/config/", root);
    native_test_set_env("XDG_CONFIG_HOME", value);
    CHECK(nk_platform_user_data_layout(&g_layout), "the default folders have two layouts");
}

static void make_state(const NkDataLayoutFolders *f, const char *state) {
    char path[NATIVE_TEST_PATH_MAX];
    if (strcmp(state, "none") == 0) return;
    if (strcmp(state, "empty") == 0) {
        CHECK(nk_platform_mkdir_p(f->path[NK_PATH_CONFIG]) && nk_platform_mkdir_p(f->path[NK_PATH_DATA]) &&
                  nk_platform_mkdir_p(f->path[NK_PATH_SAVES]),
              "make the empty folders");
    } else if (strcmp(state, "emptydata") == 0) {
        CHECK(nk_platform_mkdir_p(f->path[NK_PATH_DATA]), "make the empty data folder");
    } else if (strcmp(state, "emptyconfig") == 0) {
        CHECK(nk_platform_mkdir_p(f->path[NK_PATH_CONFIG]), "make the empty settings folder");
    } else if (strcmp(state, "dangling") == 0) {
        char parent[NATIVE_TEST_PATH_MAX];
        parent_of(f->path[NK_PATH_DATA], parent, sizeof(parent));
        join(path, sizeof(path), parent, "unmounted-drive/nakagawa-recomp");
        CHECK(nk_platform_mkdir_p(parent) && symlink(path, f->path[NK_PATH_DATA]) == 0,
              "make the data folder a link to a path that does not exist");
    } else if (strcmp(state, "fonts") == 0) {
        join(path, sizeof(path), f->path[NK_PATH_DATA], "fonts/v2");
        put_file(path, "jpn0.pgf");
    } else if (strcmp(state, "cache") == 0) {
        put_file(f->path[NK_PATH_CACHE], "shader.bin");
        put_file(f->path[NK_PATH_LOGS], "player.log");
    } else if (strcmp(state, "settings") == 0) {
        put_file(f->path[NK_PATH_CONFIG], "settings.json");
    } else if (strcmp(state, "library") == 0) {
        put_file(f->path[NK_PATH_DATA], "library.json");
    } else if (strcmp(state, "saves") == 0) {
        join(path, sizeof(path), f->path[NK_PATH_SAVES], "ULUS10000");
        put_file(path, "DATA.BIN");
    } else {
        CHECK(false, "a state the table names is known to the test");
    }
}

/* Every folder of the chosen layout is the one the resolver gives, for every kind. */
static bool resolves_to(const NkDataLayoutFolders *f) {
    for (int type = NK_PATH_CONFIG; type <= NK_PATH_SAVES; ++type) {
        char got[NK_MAX_PATH];
        if (!nk_platform_resolve_path((NkPathType)type, got, sizeof(got)) ||
            strcmp(got, f->path[type]) != 0) {
            return false;
        }
    }
    return true;
}

typedef struct {
    char name[64];
    char new_state[16];
    char old_state[16];
    char layout[16];
    char notice[16];
} RuleCase;

/* The table, read from nk_data_migrate.h itself, so the test and the header cannot drift apart. */
static size_t read_rule_cases(RuleCase *cases, size_t cap) {
    char path[NATIVE_TEST_PATH_MAX];
    const char *self = __FILE__;
    const char *tail = strstr(self, "tests/native/test_data_migrate.c");
    if (!tail) return 0u;
    snprintf(path, sizeof(path), "%.*ssrc/core/nk_data_migrate.h", (int)(tail - self), self);
    FILE *f = nk_fopen_utf8(path, "rb");
    CHECK(f != NULL, "read the rule table from nk_data_migrate.h");
    if (!f) return 0u;
    char line[256];
    bool inside = false, header = true;
    size_t count = 0u;
    while (fgets(line, sizeof(line), f) && count < cap) {
        if (strstr(line, "BEGIN DATA FOLDER RULE CASES")) {
            inside = true;
            continue;
        }
        if (strstr(line, "END DATA FOLDER RULE CASES")) break;
        if (!inside) continue;
        RuleCase c;
        memset(&c, 0, sizeof(c));
        if (sscanf(line, " * %63s %15s %15s %15s %15s", c.name, c.new_state, c.old_state, c.layout,
                   c.notice) != 5) {
            continue;
        }
        if (header) {
            CHECK(strcmp(c.name, "case") == 0 && strcmp(c.notice, "notice") == 0, "the table's header row");
            header = false;
            continue;
        }
        cases[count++] = c;
    }
    fclose(f);
    return count;
}

static void test_rule_table(void) {
    RuleCase cases[32];
    size_t count = read_rule_cases(cases, sizeof(cases) / sizeof(cases[0]));
    CHECK(count >= 14u, "the table has its cases");
    for (size_t i = 0; i < count; ++i) {
        const RuleCase *c = &cases[i];
        char root[NATIVE_TEST_PATH_MAX], what[160];
        new_case(root, sizeof(root));
        CHECK(nk_platform_user_data_layout(&g_layout), "the layouts can be named");
        make_state(&g_layout.new_layout, c->new_state);
        make_state(&g_layout.old_layout, c->old_state);
        decide_as_a_new_start(root);
        bool want_old = strcmp(c->layout, "old") == 0;
        snprintf(what, sizeof(what), "%s: the %s layout is in use", c->name, c->layout);
        CHECK(g_layout.old_in_use == want_old, what);
        snprintf(what, sizeof(what), "%s: every folder resolves to that layout", c->name);
        CHECK(resolves_to(want_old ? &g_layout.old_layout : &g_layout.new_layout), what);
        char notice[NK_DATA_LAYOUT_NOTICE_MAX];
        bool shown = nk_data_layout_notice(&g_layout, notice, sizeof(notice));
        snprintf(what, sizeof(what), "%s: the notice is %s", c->name,
                 strcmp(c->notice, "yes") == 0 ? "shown" : "not shown");
        CHECK(shown == (strcmp(c->notice, "yes") == 0), what);
        if (shown) {
            char new_root[NK_MAX_PATH];
            parent_of(g_layout.new_layout.path[NK_PATH_DATA], new_root, sizeof(new_root));
            CHECK(strncmp(notice, "Nakagawa uses ", 14u) == 0 &&
                      strstr(notice, g_layout.old_layout.path[NK_PATH_DATA]) == notice + 14 &&
                      strstr(notice, new_root) != NULL,
                  "the notice names the old folder in use first, then the new one");
        }

        /* A start: every folder it uses is made; nothing appears in the layout it does not use. */
        for (int type = NK_PATH_CONFIG; type <= NK_PATH_SAVES; ++type) {
            char path[NK_MAX_PATH];
            CHECK(nk_platform_get_path((NkPathType)type, path, sizeof(path)), "a start makes its folders");
        }
        const NkDataLayoutFolders *unused = want_old ? &g_layout.new_layout : &g_layout.old_layout;
        const char *unused_state = want_old ? c->new_state : c->old_state;
        if (strcmp(unused_state, "none") == 0) {
            snprintf(what, sizeof(what), "%s: the start made nothing in the layout it does not use",
                     c->name);
            CHECK(!exists(unused->path[NK_PATH_CONFIG]) && !exists(unused->path[NK_PATH_DATA]) &&
                      !exists(unused->path[NK_PATH_SAVES]),
                  what);
            if (want_old) {
                char new_root[NK_MAX_PATH];
                parent_of(g_layout.new_layout.path[NK_PATH_DATA], new_root, sizeof(new_root));
                CHECK(!exists(new_root), "an existing install never makes the new folder");
            }
        }
    }
}

/* Decided once for the run: an old folder that appears during it changes nothing. */
static void test_decision_is_kept_for_the_run(void) {
    char root[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    CHECK(nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use, "a fresh install");
    make_state(&g_layout.old_layout, "library");
    CHECK(nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use &&
              resolves_to(&g_layout.new_layout),
          "an old folder that appears during the run does not switch it");
}

/* Nothing is recorded: the next start decides from what is there. With both layouts present the
   old one is used; once the user moves the old folders away, the next start uses the new folder. */
static void test_old_removed_between_starts(void) {
    char root[NATIVE_TEST_PATH_MAX], moved[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    CHECK(nk_platform_user_data_layout(&g_layout), "the layouts can be named");
    make_state(&g_layout.old_layout, "library");
    make_state(&g_layout.old_layout, "settings");
    make_state(&g_layout.new_layout, "library");
    decide_as_a_new_start(root);
    CHECK(g_layout.old_in_use && resolves_to(&g_layout.old_layout),
          "first start: the old layout exists and is used, though the new folder exists too");
    const NkPathType kinds[2] = {NK_PATH_CONFIG, NK_PATH_DATA};
    for (size_t i = 0; i < 2u; ++i) {
        snprintf(moved, sizeof(moved), "%s/moved-away-%u", root, (unsigned)i);
        CHECK(rename(g_layout.old_layout.path[kinds[i]], moved) == 0, "move an old folder away");
    }
    char value[NATIVE_TEST_PATH_MAX];
    join(value, sizeof(value), root, "config");
    native_test_set_env("XDG_CONFIG_HOME", value); /* the same folders, decided afresh as a new start */
    CHECK(nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use &&
              resolves_to(&g_layout.new_layout),
          "second start: the old folders are gone and the new folder is used");
}

/* The XDG variables and HOME: a relative value is ignored by the new layout, as the XDG Base
   Directory specification says; the old layout reads them exactly as before #324. */
static void test_variables(void) {
    char root[NATIVE_TEST_PATH_MAX], home[NATIVE_TEST_PATH_MAX], want[NATIVE_TEST_PATH_MAX];
    char got[NK_MAX_PATH];
    new_case(root, sizeof(root));
    join(home, sizeof(home), root, "home");

#if !defined(__APPLE__)
    native_test_set_env("XDG_DATA_HOME", "relative/share");
    native_test_set_env("XDG_CACHE_HOME", "relative/cache");
    native_test_set_env("XDG_STATE_HOME", "relative/state");
    CHECK(nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use, "a fresh install");
    snprintf(want, sizeof(want), "%s/.local/share/Nakagawa/data", home);
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, got, sizeof(got)) && strcmp(got, want) == 0,
          "a relative XDG_DATA_HOME is ignored: the data is under HOME");
    snprintf(want, sizeof(want), "%s/.local/share/Nakagawa/config", home);
    CHECK(nk_platform_resolve_path(NK_PATH_CONFIG, got, sizeof(got)) && strcmp(got, want) == 0,
          "and so are the settings");
    snprintf(want, sizeof(want), "%s/.cache/nakagawa-recomp", home);
    CHECK(nk_platform_resolve_path(NK_PATH_CACHE, got, sizeof(got)) && strcmp(got, want) == 0,
          "a relative XDG_CACHE_HOME is ignored: the cache stays in ~/.cache");
    snprintf(want, sizeof(want), "%s/.local/state/nakagawa-recomp/logs", home);
    CHECK(nk_platform_resolve_path(NK_PATH_LOGS, got, sizeof(got)) && strcmp(got, want) == 0,
          "a relative XDG_STATE_HOME is ignored: the logs stay in ~/.local/state");
    CHECK(strcmp(g_layout.old_layout.path[NK_PATH_DATA], "relative/share/nakagawa-recomp") == 0,
          "the old layout reads a relative XDG_DATA_HOME as it always did");

    native_test_set_env("XDG_DATA_HOME", NULL);
    native_test_set_env("XDG_CONFIG_HOME", NULL);
    native_test_set_env("XDG_CACHE_HOME", NULL);
    native_test_set_env("XDG_STATE_HOME", NULL);
    CHECK(nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use, "unset XDG variables");
    snprintf(want, sizeof(want), "%s/.local/share/Nakagawa/saves", home);
    CHECK(strcmp(g_layout.new_layout.path[NK_PATH_SAVES], want) == 0, "the new saves are under HOME");
    snprintf(want, sizeof(want), "%s/.config/nakagawa-recomp", home);
    CHECK(strcmp(g_layout.old_layout.path[NK_PATH_CONFIG], want) == 0,
          "the old settings are in ~/.config, as before");
    /* A path too long to name (NK_MAX_PATH) is no folder, as in the tools. */
    char long_home[NATIVE_TEST_PATH_MAX];
    snprintf(long_home, sizeof(long_home), "%s/%0200d/%0200d/%0200d", root, 0, 0, 0);
    native_test_set_env("XDG_DATA_HOME", long_home);
    CHECK(nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use &&
              !g_layout.new_layout.path[NK_PATH_DATA][0] && !g_layout.old_layout.path[NK_PATH_DATA][0],
          "an overlong XDG_DATA_HOME names no data folder in either layout");
    CHECK(!nk_platform_resolve_path(NK_PATH_DATA, got, sizeof(got)), "so there is none to use");
    native_test_set_env("XDG_DATA_HOME", NULL);

    CHECK(nk_platform_user_data_layout(&g_layout), "the layouts again");
    make_state(&g_layout.old_layout, "settings");
    /* HOME with a trailing '/' names the same folders and makes the resolver decide again. */
    snprintf(want, sizeof(want), "%s/", home);
    native_test_set_env("HOME", want);
    CHECK(nk_platform_user_data_layout(&g_layout) && g_layout.old_in_use &&
              resolves_to(&g_layout.old_layout),
          "old settings found through HOME choose the old layout");

    native_test_set_env("HOME", NULL);
    native_test_set_env("XDG_CONFIG_HOME", NULL);
    CHECK(nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use, "no HOME, no XDG");
    CHECK(!nk_platform_resolve_path(NK_PATH_DATA, got, sizeof(got)) &&
              !nk_platform_resolve_path(NK_PATH_CONFIG, got, sizeof(got)),
          "without HOME or an absolute XDG variable there is no folder, as before");
    native_test_set_env("XDG_DATA_HOME", "relative/share");
    CHECK(!nk_platform_resolve_path(NK_PATH_SAVES, got, sizeof(got)),
          "a relative XDG_DATA_HOME without HOME names no new folder");
#else
    native_test_set_env("HOME", NULL);
    CHECK(nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use, "no HOME");
    CHECK(!nk_platform_resolve_path(NK_PATH_DATA, got, sizeof(got)), "without HOME there is no folder");
    native_test_set_env("HOME", home);
    CHECK(nk_platform_user_data_layout(&g_layout), "HOME");
    snprintf(want, sizeof(want), "%s/Library/Application Support/Nakagawa/data", home);
    CHECK(strcmp(g_layout.new_layout.path[NK_PATH_DATA], want) == 0, "the new data folder");
#endif
    (void)want;
}

/* --user-data-root and portable mode name the folders: no layout, no notice. */
static void test_override(void) {
    char root[NATIVE_TEST_PATH_MAX], got[NK_MAX_PATH], data[NATIVE_TEST_PATH_MAX];
    new_case(root, sizeof(root));
    join(data, sizeof(data), root, "portable");
    CHECK(nk_platform_set_app_data_dir_override(data), "set an override");
    CHECK(!nk_platform_user_data_layout(&g_layout) && !g_layout.old_in_use,
          "an override has no layouts to choose from");
    CHECK(nk_platform_resolve_path(NK_PATH_DATA, got, sizeof(got)) && strcmp(got, data) == 0,
          "and its folder is the data folder");
    CHECK(nk_platform_set_app_data_dir_override(NULL), "clear the override");
}

/* The real-profile guard (native_test_isolation.c) watches the settings folders of both layouts: a
   test that creates the settings folder of the layout not in use trips it too, since by the rule an
   old folder a test created would switch a developer's install. Checked on a stand-in profile in the
   temporary root, never the real one. */
static void test_guard_watches_both_layouts(void) {
    char root[NATIVE_TEST_PATH_MAX], problem[2 * NATIVE_TEST_PATH_MAX + 64];
    for (int old = 0; old < 2; ++old) {
        new_case(root, sizeof(root));
        CHECK(nk_platform_user_data_layout(&g_layout), "the layouts can be named");
        if (old) make_state(&g_layout.old_layout, "settings");
        decide_as_a_new_start(root);
        CHECK(g_layout.old_in_use == (old != 0), "the stand-in profile uses the layout meant");
        const NkDataLayoutFolders *in_use = old ? &g_layout.old_layout : &g_layout.new_layout;
        const NkDataLayoutFolders *other = old ? &g_layout.new_layout : &g_layout.old_layout;
        native_test_recapture_real_config_probe();
        CHECK(strcmp(native_test_get_real_config_probe(), in_use->path[NK_PATH_CONFIG]) == 0,
              "the guard watches the settings folder in use");
        CHECK(strcmp(native_test_get_real_config_probe_at(1), other->path[NK_PATH_CONFIG]) == 0,
              "and the other layout's settings folder");
        CHECK(native_test_real_config_unchanged(problem, sizeof(problem)), "nothing touched yet");
        CHECK(nk_platform_mkdir_p(other->path[NK_PATH_CONFIG]),
              "a test creates the other layout's settings folder");
        CHECK(!native_test_real_config_unchanged(problem, sizeof(problem)) &&
                  strstr(problem, other->path[NK_PATH_CONFIG]) != NULL,
              old ? "the guard trips on a new settings folder beside the old layout"
                  : "the guard trips on an old settings folder beside the new layout");
    }
}

#endif

int main(void) {
    native_test_create_root("data_migrate");
    test_notice();
#if !defined(_WIN32) && !defined(_WIN64)
    snprintf(g_base, sizeof(g_base), "%s", native_test_get_root());
    test_rule_table();
    test_decision_is_kept_for_the_run();
    test_old_removed_between_starts();
    test_variables();
    test_override();
    test_guard_watches_both_layouts();
#else
    {
        static NkUserDataLayout layout;
        CHECK(!nk_platform_user_data_layout(&layout) && !layout.old_in_use,
              "Windows has one layout and decides nothing");
    }
#endif
    if (g_failures) {
        fprintf(stderr, "test_data_migrate: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_data_migrate: ALL %d CHECKS PASSED\n", g_checks);
    return 0;
}
