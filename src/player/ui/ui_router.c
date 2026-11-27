/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "nk_ui_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_UI_ROUTE_DEPTH_MAX 16u

typedef struct {
    PlayerUiRoute route;
    NkUiId focus;
} PlayerUiRouteEntry;

struct NkUiRouter {
    PlayerUiRouteEntry entries[PLAYER_UI_ROUTE_DEPTH_MAX];
    size_t depth;
    NkUiId focus_restore; /* 0: nothing to restore */
};

NkUiRouter *nk_ui_router_create(void) {
    NkUiRouter *router = (NkUiRouter *)calloc(1u, sizeof(*router));
    if (router != NULL) {
        router->entries[0].route = PLAYER_UI_ROUTE_LIBRARY;
        router->depth = 1u;
    }
    return router;
}

void nk_ui_router_destroy(NkUiRouter *router) {
    free(router);
}

/* Every route the player draws. A route missing here is refused by replace and
   push, and its screen can never open (#1281: About was missing). */
static bool nk_ui_router_known(PlayerUiRoute route) {
    return route == PLAYER_UI_ROUTE_LIBRARY || route == PLAYER_UI_ROUTE_TITLE ||
           route == PLAYER_UI_ROUTE_ABOUT || route == PLAYER_UI_ROUTE_REMOVE ||
           route == PLAYER_UI_ROUTE_STOP || route == PLAYER_UI_ROUTE_REBUILD ||
           route == PLAYER_UI_ROUTE_SAVES || route == PLAYER_UI_ROUTE_STEAM ||
           route == PLAYER_UI_ROUTE_QUEUE || route == PLAYER_UI_ROUTE_LOVE_ALL;
}

bool nk_ui_router_replace(NkUiRouter *router, PlayerUiRoute route) {
    if (router == NULL || !nk_ui_router_known(route)) {
        return false;
    }
    router->entries[0].route = route;
    router->entries[0].focus = 0u;
    router->depth = 1u;
    router->focus_restore = 0u;
    return true;
}

bool nk_ui_router_push(NkUiRouter *router, PlayerUiRoute route) {
    PlayerUiRouteEntry *entry;
    if (router == NULL || !nk_ui_router_known(route) || router->depth == 0u ||
        router->depth >= PLAYER_UI_ROUTE_DEPTH_MAX) {
        return false;
    }
    entry = &router->entries[router->depth++];
    entry->route = route;
    entry->focus = 0u;
    router->focus_restore = 0u;
    return true;
}

bool nk_ui_router_pop(NkUiRouter *router) {
    if (router == NULL || router->depth <= 1u) {
        return false;
    }
    --router->depth;
    router->focus_restore = router->entries[router->depth - 1u].focus;
    return true;
}

bool nk_ui_router_handle_event(NkUiRouter *router, const NkUiEvent *event) {
    if (event == NULL || event->type != NK_UI_EV_BACK) {
        return false;
    }
    (void)nk_ui_router_pop(router);
    return true;
}

PlayerUiRoute nk_ui_router_current(const NkUiRouter *router) {
    return router != NULL && router->depth > 0u
        ? router->entries[router->depth - 1u].route : 0;
}

size_t nk_ui_router_depth(const NkUiRouter *router) {
    return router != NULL ? router->depth : 0u;
}

void nk_ui_router_set_focus(NkUiRouter *router, NkUiId id) {
    if (router != NULL && router->depth > 0u) {
        router->entries[router->depth - 1u].focus = id;
    }
}

NkUiId nk_ui_router_take_focus_restore(NkUiRouter *router) {
    NkUiId id;
    if (router == NULL) {
        return 0u;
    }
    id = router->focus_restore;
    router->focus_restore = 0u;
    return id;
}

/* Days since 1970-01-01 to a proleptic Gregorian date (civil_from_days, after
   Howard Hinnant's public-domain date algorithms), so the date never depends on
   the host's gmtime or time zone. */
bool player_ui_build_date(long long epoch, char *out, size_t cap) {
    long long z;
    long long era;
    long long doe;
    long long yoe;
    long long doy;
    long long mp;
    long long day;
    long long month;
    long long year;
    int written;
    if (epoch < 0 || out == NULL || cap < 11u) {
        return false;
    }
    z = epoch / 86400 + 719468;
    era = z / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    if (year < 1 || year > 9999) {
        return false;
    }
    written = snprintf(out, cap, NK_UI_STR(SHARED_ABOUT_BUILD_DATE), month, day, year);
    return written == 10;
}

void player_ui_version_line(const char *version, long long epoch, char *out, size_t cap) {
    char date[16];
    if (out == NULL || cap == 0u) {
        return;
    }
    if (version == NULL || version[0] == '\0') {
        version = NK_PLAYER_VERSION;
    }
    if (player_ui_build_date(epoch, date, sizeof(date))) {
        (void)snprintf(out, cap, NK_UI_STR(SHARED_ABOUT_VERSION_DATE), version, date);
    } else {
        (void)snprintf(out, cap, NK_UI_STR(SHARED_ABOUT_VERSION_DEV), version);
    }
}

void player_ui_apply_display(NkUi *ui, NkUiViewport *viewport, const PlayerUiDisplay *display,
                             NkUiPromptStyle prompt_style) {
    NkUiPrefs prefs;
    if (ui == NULL || viewport == NULL || display == NULL) {
        return;
    }
    viewport->ui_scale_pct = display->interface_size_pct;
    viewport->text_size_pct = display->text_size_pct == 125 || display->text_size_pct == 150
                                  ? display->text_size_pct : 100;
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = display->reduce_motion;
    prefs.high_contrast_focus = display->high_contrast_focus;
    prefs.prompt_style = prompt_style;
    prefs.touch = display->touch;
    nk_ui_set_prefs(ui, &prefs);
}

bool player_ui_host_is_pc(NkHostKind host) {
    return host != NK_HOST_ANDROID;
}

PlayerUiActionKind player_ui_top_bar(NkUi *ui, NkUiId id, PlayerUiRoute active) {
    NkUiSection sections[3];
    int chosen;
    sections[0].label = NK_UI_STR(SHARED_NAV_LIBRARY);
    sections[0].active = active != PLAYER_UI_ROUTE_ABOUT &&
                         active != PLAYER_UI_ROUTE_SETTINGS;
    sections[0].id = PLAYER_UI_ID_TAB_LIBRARY;
    sections[1].label = NK_UI_STR(SHARED_NAV_ABOUT);
    sections[1].active = active == PLAYER_UI_ROUTE_ABOUT;
    sections[1].id = PLAYER_UI_ID_TAB_ABOUT;
    sections[2].label = NK_UI_STR(SHARED_NAV_SETTINGS);
    sections[2].active = active == PLAYER_UI_ROUTE_SETTINGS;
    sections[2].id = PLAYER_UI_ID_TAB_SETTINGS;
    /* Down from a top-bar tab goes to the nearest control under it. */
    sections[0].down = sections[1].down = sections[2].down = 0u;
    chosen = nk_ui_top_bar(ui, id, sections, 3u,
                           nk_ui_input_chip_label(ui),
                           nk_ui_prompt_style(ui) != NK_UI_PROMPT_KEYCAPS);
    /* LIBRARY stays highlighted on a title page, its parent, and choosing it
       there goes back to the grid; only on the grid itself does it do nothing.
       SETTINGS on the Settings or Controls page closes it, as Start does. */
    if (chosen == 0 && active != PLAYER_UI_ROUTE_LIBRARY) return PLAYER_UI_ACTION_OPEN_LIBRARY;
    if (chosen == 1 && !sections[1].active) return PLAYER_UI_ACTION_OPEN_ABOUT;
    if (chosen == 2 || (chosen < 0 && nk_ui_take_event(ui, NK_UI_EV_MENU, NULL))) {
        return PLAYER_UI_ACTION_OPEN_SETTINGS;
    }
    return PLAYER_UI_ACTION_NONE;
}
