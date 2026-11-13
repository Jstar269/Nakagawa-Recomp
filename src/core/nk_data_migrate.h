/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Which per-user folders a Linux or macOS install uses (#324).
 *
 * A fresh install keeps its settings, data and saves in one `Nakagawa` folder in the host's own data
 * base, as Windows always has (docs/SETUP.md, "Where your data lives"). An install from before #324
 * keeps the folders it has: Linux's `nakagawa-recomp` in the XDG data home (saves inside it) with its
 * settings in the XDG config home, macOS's `NakagawaRecomp`. Nothing is ever moved, copied, merged or
 * deleted between them; a user who wants the new folder moves the old folders away. Windows and
 * Android have one layout and decide nothing.
 *
 * The rule, decided once per process for its whole run (src/core/nk_platform_posix.c, and the same
 * rule in tools/nk_core/prereq_fetcher.py), with nothing recorded on disk:
 *   1. the old layout exists: the old layout, every folder exactly as before #324;
 *   2. otherwise the new layout.
 * A layout exists when any of its settings, data or saves folders is present, by lstat: an empty
 * folder, a dangling link, or a folder holding only fonts, a disc list, packages or prerequisites
 * counts. Nothing inside a folder is read. The cache and logs folders do not count.
 * When the old layout is in use and the new one exists too, the player shows one notice that names
 * both (nk_data_layout_notice). Nothing blocks a start. A start that cannot see the old layout at all
 * (a link to a drive not mounted yet) uses the new folder for that run; the next start that sees the
 * old layout uses it again, and the notice names the new folder.
 *
 * The cases, which tests/native/test_data_migrate.c and tools/test_prereq_fetcher.py both read from
 * here. The `new` and `old` columns say what each layout has before the start: none (nothing); empty
 * (its settings, data and saves folders, empty); emptydata or emptyconfig (only its data or settings
 * folder, empty); dangling (its data folder is a link to a path that does not exist); fonts (only a
 * file in data/fonts, as the tools import it); cache (only its cache and logs folders, each with a
 * file); settings, library or saves (that one item, the saves as a folder with a file in it).
 * `layout` is the one in use; `notice` whether the player shows it.
 * BEGIN DATA FOLDER RULE CASES
 * case                  new       old          layout notice
 * neither               none      none         new    no
 * new_only              library   none         new    no
 * new_empty             empty     none         new    no
 * old_cache_only        none      cache        new    no
 * old_empty_data        none      emptydata    old    no
 * old_empty_settings    none      emptyconfig  old    no
 * old_dangling_link     none      dangling     old    no
 * old_tools_only        none      fonts        old    no
 * old_settings          none      settings     old    no
 * old_library           none      library      old    no
 * old_saves             none      saves        old    no
 * both_exist            library   library      old    yes
 * both_old_empty        settings  emptydata    old    yes
 * empty_new_beside_old  empty     library      old    yes
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

#define NK_DATA_LAYOUT_FOLDERS 5 /* NK_PATH_CONFIG to NK_PATH_SAVES */

/* One layout's folders, by NkPathType; "" for one that cannot be named. `exists`: its settings,
   data or saves folder is present (the rule above). */
typedef struct NkDataLayoutFolders {
    char path[NK_DATA_LAYOUT_FOLDERS][NK_MAX_PATH];
    bool exists;
} NkDataLayoutFolders;

/* What this process decided (nk_platform_user_data_layout). */
typedef struct NkUserDataLayout {
    bool old_in_use;
    NkDataLayoutFolders new_layout;
    NkDataLayoutFolders old_layout;
} NkUserDataLayout;

/* The player's notice when the old layout is in use and the new one exists too: "Nakagawa uses
   <old>. Nakagawa data is also in <new>, which is not used." Writes "" and returns false otherwise,
   or when it does not fit (NK_DATA_LAYOUT_NOTICE_MAX always does). */
#define NK_DATA_LAYOUT_NOTICE_MAX (4u * NK_MAX_PATH + 96u)
bool nk_data_layout_notice(const NkUserDataLayout *layout, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
