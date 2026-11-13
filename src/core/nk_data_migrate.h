/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Which per-user folders a Linux or macOS install uses (#324).
 *
 * A fresh install keeps its settings, data and saves in one `Nakagawa` folder in the host's own data
 * base, as Windows always has (docs/SETUP.md, "Where your data lives"). An install from before #324
 * keeps the folders it has, for good: Linux's `nakagawa-recomp` in the XDG data home (saves inside
 * it) with its settings in the XDG config home, macOS's `NakagawaRecomp`. Nothing is ever moved,
 * copied, merged or deleted between them. Windows and Android have one layout and decide nothing.
 *
 * The rule, a pure function of what is on disk, decided once per process for its whole run
 * (src/core/nk_platform_posix.c, and the same rule in tools/nk_core/prereq_fetcher.py):
 *   1. the new layout holds data: the new layout;
 *   2. else the old layout holds data: the old layout, every folder exactly as before #324;
 *   3. else the new layout (a fresh install).
 * When both hold data, line 1 decides, and the player shows one notice that names both folders
 * (nk_data_layout_notice). Nothing blocks a start.
 *
 * A layout holds data when at least one of these is true, and only then:
 *   - its config folder has a regular file `settings.json` (the player's settings);
 *   - its data folder has a regular file `library.json` (the library);
 *   - its saves folder has at least one entry.
 * A link to a regular file counts as one. Empty folders, any other file (a stray file, the cache,
 * the logs) and a folder that cannot be read are not data.
 *
 * The cases, which tests/native/test_data_migrate.c and tools/test_prereq_fetcher.py both read from
 * here. The `new` and `old` columns say what each layout holds before the start: none (nothing);
 * empty (its config, data and saves folders, empty); stray (its config and data folders, each with
 * an unrelated file); cache (only its cache and logs folders, each with a file); settings, library
 * or saves (that one item, the saves as a folder with a file in it); savesdir (a saves folder whose
 * only entry is an empty folder). `layout` is the one in use; `notice` whether the player shows it.
 * BEGIN DATA FOLDER RULE CASES
 * case                  new       old       layout notice
 * fresh                 none      none      new    no
 * empty_folders         empty     empty     new    no
 * stray_files           stray     stray     new    no
 * cache_only            cache     cache     new    no
 * new_settings          settings  none      new    no
 * new_library           library   none      new    no
 * new_saves             saves     none      new    no
 * old_settings          none      settings  old    no
 * old_library           none      library   old    no
 * old_saves             none      saves     old    no
 * old_saves_folder      none      savesdir  old    no
 * old_beside_empty_new  empty     library   old    no
 * old_beside_stray_new  stray     settings  old    no
 * new_beside_stray_old  library   stray     new    no
 * both_hold_data        library   library   new    yes
 * both_mixed            settings  saves     new    yes
 * END DATA FOLDER RULE CASES
 */

#ifndef NK_DATA_MIGRATE_H
#define NK_DATA_MIGRATE_H

#include "nk_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_DATA_LAYOUT_SETTINGS_FILE "settings.json" /* in the config folder (player_state.c) */
#define NK_DATA_LAYOUT_LIBRARY_FILE "library.json"   /* in the data folder (nk_library.c) */
#define NK_DATA_LAYOUT_FOLDERS 5                     /* NK_PATH_CONFIG to NK_PATH_SAVES */

/* One layout's folders, by NkPathType; "" for one that cannot be named. */
typedef struct NkDataLayoutFolders {
    char path[NK_DATA_LAYOUT_FOLDERS][NK_MAX_PATH];
    bool holds_data;
} NkDataLayoutFolders;

/* What this process decided (nk_platform_user_data_layout). */
typedef struct NkUserDataLayout {
    bool old_in_use;
    NkDataLayoutFolders new_layout;
    NkDataLayoutFolders old_layout;
} NkUserDataLayout;

/* The player's notice when both layouts hold data: both folders named, the one in use first.
   Writes "" and returns false otherwise, or when it does not fit (NK_DATA_LAYOUT_NOTICE_MAX
   always does). */
#define NK_DATA_LAYOUT_NOTICE_MAX (4u * NK_MAX_PATH + 96u)
bool nk_data_layout_notice(const NkUserDataLayout *layout, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
