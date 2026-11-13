/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The player's notice when both data layouts hold data (#324, nk_data_migrate.h). The rule itself is
   in the platform (nk_platform_posix.c), which every process resolves its folders through. */

#include "nk_data_migrate.h"
#include "nk_platform.h"

#include <stdio.h>
#include <string.h>

/* The folder that holds `path`, or "" when it has no separator. */
static void parent_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
    size_t length = slash ? (size_t)(slash - path) : 0u;
    if (length >= cap) length = 0u;
    memcpy(out, path, length);
    out[length] = '\0';
}

/* How to name a layout: the one folder that holds its config, data and saves folders (the new
   layout, and macOS's old one), else its data folder with its settings folder (Linux's old one). */
static bool name_layout(const NkDataLayoutFolders *folders, char *out, size_t cap) {
    char config[NK_MAX_PATH], data[NK_MAX_PATH], saves[NK_MAX_PATH];
    parent_of(folders->path[NK_PATH_CONFIG], config, sizeof(config));
    parent_of(folders->path[NK_PATH_DATA], data, sizeof(data));
    parent_of(folders->path[NK_PATH_SAVES], saves, sizeof(saves));
    int written = config[0] && strcmp(config, data) == 0 && strcmp(data, saves) == 0
                      ? snprintf(out, cap, "%s", data)
                      : snprintf(out, cap, "%s (settings in %s)", folders->path[NK_PATH_DATA],
                                 folders->path[NK_PATH_CONFIG]);
    return written > 0 && (size_t)written < cap;
}

bool nk_data_layout_notice(const NkUserDataLayout *layout, char *out, size_t cap) {
    if (!out || cap == 0u) return false;
    out[0] = '\0';
    if (!layout || !layout->new_layout.holds_data || !layout->old_layout.holds_data) return false;
    const NkDataLayoutFolders *in_use = layout->old_in_use ? &layout->old_layout : &layout->new_layout;
    const NkDataLayoutFolders *other = layout->old_in_use ? &layout->new_layout : &layout->old_layout;
    char used[2 * NK_MAX_PATH + 16], unused[2 * NK_MAX_PATH + 16];
    if (!name_layout(in_use, used, sizeof(used)) || !name_layout(other, unused, sizeof(unused))) {
        return false;
    }
    int written = snprintf(out, cap, "Nakagawa uses %s. Nakagawa data is also in %s, which is not used.",
                           used, unused);
    if (written <= 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}
