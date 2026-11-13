// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// fbcap_policy.c - frame-capture slot policy (issue #57)
//
// Pure policy, no Vulkan/SDL dependencies: unit-testable without a GPU (exercised by
// gpu_capture_selftest.c before any Vulkan object exists).
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fbcap_policy.h"

int sr_fbcap_env_on(const char *name) {
    if (!name) return 0;
    const char *v = getenv(name);
    if (!v) return 0;
    return strtol(v, NULL, 10) != 0;
}

int sr_fbcap_snap_every(const char *fbsnap_value, int windows_configured) {
    if (!fbsnap_value || !fbsnap_value[0]) return windows_configured ? 1 : 0;
    long long every = strtoll(fbsnap_value, NULL, 10);
    if (every <= 0) return 0;
    return every > INT_MAX ? INT_MAX : (int)every;
}

int sr_fbcap_owner(int fbdu, int fbsnap) {
    if (fbdu) return SR_FBCAP_FBDUMP;
    if (fbsnap) return SR_FBCAP_FBSNAP;
    return SR_FBCAP_NONE;
}

int sr_fbcap_path(int owner, int index, char *out, size_t outsz) {
    if (!out || !outsz) return 0;
    out[0] = '\0';
    int n;
    switch (owner) {
    case SR_FBCAP_FBSNAP:
        n = snprintf(out, outsz, "build/snapshots/frame_%04u.ppm", (unsigned)index);
        break;
    case SR_FBCAP_FBDUMP:
        n = snprintf(out, outsz, "present_source.ppm");
        break;
    default:
        return 0;
    }
    return n > 0 && (size_t)n < outsz;
}

int sr_fbcap_exit_status(int owner, int capture_result) {
    if (owner != SR_FBCAP_FBDUMP) return 0;
    if (capture_result == 1) return 0;
    return 1;
}

int sr_screenshot_name(const char *disc_id, int year, int month, int day, int hour, int minute,
                       int second, int seq, char *out, size_t outsz) {
    if (!out || outsz == 0u) return 0;
    out[0] = '\0';
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60 ||
        seq < 1 || seq > 99)
        return 0;
    char disc[33];
    size_t n = 0;
    for (const char *p = disc_id ? disc_id : ""; *p && n < sizeof disc - 1u; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            disc[n++] = c;
    }
    disc[n] = '\0';
    if (n == 0u) memcpy(disc, "game", 5u);
    char suffix[4] = "";
    if (seq > 1) snprintf(suffix, sizeof suffix, "-%d", seq);
    int w = snprintf(out, outsz, "%s_%04d%02d%02d-%02d%02d%02d%s.png", disc, year, month, day,
                     hour, minute, second, suffix);
    if (w < 0 || (size_t)w >= outsz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

int sr_texdump_folder(const char *user_root, const char *disc_id, char *out, size_t outsz) {
    if (!out || outsz == 0u) return 0;
    out[0] = '\0';
    if (!user_root || !user_root[0]) return 0;
    /* The runtime holds SR_USER_DATA_ROOT in Windows' extended form (\\?\ and a drive, or
       \\?\UNC\server\share\...), where '/' is not a separator. The dump writes through paths
       joined with '/', as the pack reader reads them, so the folder takes the root's plain form. */
    const char *unc = "";
    if (strncmp(user_root, "\\\\?\\UNC\\", 8) == 0) {
        user_root += 8;
        unc = "\\\\";
    } else if (strncmp(user_root, "\\\\?\\", 4) == 0) {
        /* Only the drive form has a plain spelling; any other extended root is refused. */
        if (!user_root[4] || user_root[5] != ':') return 0;
        user_root += 4;
    }
    if (!user_root[0]) return 0;
    char disc[33];
    size_t n = 0;
    for (const char *p = disc_id ? disc_id : ""; *p && n < sizeof disc - 1u; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            disc[n++] = c;
    }
    disc[n] = '\0';
    if (n == 0u) return 0;
    int w = snprintf(out, outsz, "%s%s/texdump/%s", unc, user_root, disc);
    if (w < 0 || (size_t)w >= outsz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* ---- screenshot key ----------------------------------------------------------------------- */
/* Set by the presenter's key handler and taken by the display path (fbcap.h, screenshot key).
 * Both run on the thread that pumps the presenter's events and presents (the guest thread), so a
 * plain flag is enough. */
static volatile int s_screenshot_pending;

void sr_screenshot_request(void) { s_screenshot_pending = 1; }

int sr_screenshot_pending(void) { return s_screenshot_pending != 0; }

void sr_screenshot_clear(void) { s_screenshot_pending = 0; }
