/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The in-game overlay's page state and the frame every page shares: no top bar
   and no waves (the frozen game frame under the scrim is the background), one
   centred card, and the prompt bar along the bottom. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <string.h>

#define OVERLAY_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_OVERLAY, (offset))
#define OVERLAY_ID_ROOT OVERLAY_ID(0x000001u)
#define OVERLAY_ID_BODY OVERLAY_ID(0x000002u)
#define OVERLAY_ID_CARD OVERLAY_ID(0x000003u)
#define OVERLAY_ID_PROMPTS OVERLAY_ID(0x000004u)

void nk_overlay_open(NkOverlayState *state, NkOverlayOpenReason reason) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->open = true;
    state->screen = reason == NK_OVERLAY_OPEN_PAD_LOST ? NK_OVERLAY_SCREEN_PAD_LOST
                                                       : NK_OVERLAY_SCREEN_MENU;
}

void nk_overlay_close(NkOverlayState *state) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}

const char *nk_overlay_scaling_label(int scaling) {
    switch (scaling) {
    case NK_OVERLAY_SCALING_INTEGER: return "Integer";
    case NK_OVERLAY_SCALING_SMOOTH: return "Smooth";
    default: return "Sharp";
    }
}

void nk_overlay_go(NkOverlayState *state, NkOverlayScreen screen, NkUiId focus) {
    state->screen = screen;
    state->restore_focus = focus;
}

NkOverlayAction nk_overlay_action(NkOverlayActionKind kind, int value) {
    NkOverlayAction action;
    action.kind = kind;
    action.value = value;
    return action;
}

NkUiSizing nk_overlay_grow(void) {
    NkUiSizing sizing = {NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    return sizing;
}

NkUiSizing nk_overlay_fit(void) {
    NkUiSizing sizing = {NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    return sizing;
}

NkUiSizing nk_overlay_fixed(float value) {
    NkUiSizing sizing = {NK_UI_SIZE_FIXED, value, 0.0f, 0.0f};
    return sizing;
}

NkUiLayout nk_overlay_layout(NkUiLayoutDirection direction, NkUiSizing width,
                             NkUiSizing height, float gap) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    layout.direction = direction;
    layout.gap = gap;
    return layout;
}

float nk_overlay_card_room(const NkOverlayModel *model) {
    float height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    float room = height - NK_UI_PROMPT_BAR_HEIGHT - 2.0f * NK_UI_SPACE_4;
    return room > 1.0f ? room : 1.0f;
}

float nk_overlay_page_begin(NkUi *ui, const NkOverlayModel *model, float card_width) {
    float width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    float card;
    NkUiLayout layout = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                          nk_overlay_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, OVERLAY_ID_ROOT, &layout, NULL)) {
        return 0.0f;
    }
    layout = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(), nk_overlay_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = layout.padding_right = NK_UI_GUTTER_NARROW;
    layout.padding_top = layout.padding_bottom = NK_UI_SPACE_4;
    if (!nk_ui_begin_container(ui, OVERLAY_ID_BODY, &layout, NULL)) {
        nk_ui_end_container(ui);
        return 0.0f;
    }
    card = width - 2.0f * NK_UI_GUTTER_NARROW;
    if (card > card_width) card = card_width;
    /* The containers are open now, so the inner width returned below must stay positive: a
       page reads 0 or less as "nothing was opened" and would leave them open. A window just
       past the 80 px floor keeps a card one inner pixel wide instead. */
    if (card < NK_UI_DIALOG_PAD_LEFT + NK_UI_DIALOG_PAD_RIGHT + 1.0f)
        card = NK_UI_DIALOG_PAD_LEFT + NK_UI_DIALOG_PAD_RIGHT + 1.0f;
    /* Scrolling is the last resort: a page that fits its room does not move. */
    if (!nk_ui_dialog_begin_scroll(ui, OVERLAY_ID_CARD, card, nk_overlay_card_room(model))) {
        nk_ui_end_container(ui);
        nk_ui_end_container(ui);
        return 0.0f;
    }
    return card - (NK_UI_DIALOG_PAD_LEFT + NK_UI_DIALOG_PAD_RIGHT);
}

void nk_overlay_page_end(NkUi *ui, const char *hint, const NkUiPromptItem *prompts,
                         size_t count) {
    nk_ui_end_container(ui); /* card */
    nk_ui_end_container(ui); /* body */
    nk_ui_prompt_bar(ui, OVERLAY_ID_PROMPTS, hint != NULL ? hint : "", prompts, count);
    nk_ui_end_container(ui); /* root */
}

bool nk_overlay_row_open(NkUi *ui, NkUiId id, const NkUiLayout *layout) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool focused = nk_ui_button_focused(ui, id);
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    style.fill_rgba = nk_ui_pressed(ui, id) ? theme->pressed :
                      focused || nk_ui_hovered(ui, id) ? theme->surface_high : 0u;
    nk_ui_register_button(ui, id, NK_UI_BUTTON_OWNS_HORIZONTAL);
    if (!nk_ui_begin_container(ui, id, layout, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    return true;
}

NkOverlayRowInput nk_overlay_row_input(NkUi *ui, NkUiId id, bool focused, NkUiNavDir *dir) {
    if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, dir)) return NK_OVERLAY_ROW_STEP;
    if (nk_ui_take_activation(ui, id)) return NK_OVERLAY_ROW_ACTIVATE;
    return NK_OVERLAY_ROW_NONE;
}

/* How many of `text`'s bytes fit in `room`, cut before a whole UTF-8 sequence. */
static size_t utf8_fit(const char *text, size_t room) {
    size_t length = strlen(text);
    if (length <= room) return length;
    while (room > 0u && ((unsigned char)text[room] & 0xC0u) == 0x80u) --room;
    return room;
}

void nk_overlay_eyebrow(NkUi *ui, NkUiId id, const char *marker, const char *text,
                        uint32_t rgba) {
    char line[160];
    size_t used = 0u;
    size_t t;
    if (marker != NULL && marker[0] != '\0') {
        /* The marker, a space, and at least the terminator always fit. */
        size_t m = utf8_fit(marker, sizeof(line) - 2u);
        memcpy(line, marker, m);
        line[m] = ' ';
        used = m + 1u;
    }
    t = utf8_fit(text, sizeof(line) - 1u - used);
    memcpy(line + used, text, t);
    line[used + t] = '\0';
    nk_ui_text_role(ui, id, nk_ui_caps(ui, line), NK_UI_TYPE_EYEBROW, rgba);
}

NkOverlayAction nk_overlay_frame(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model) {
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    bool menu;
    if (ui == NULL || state == NULL || model == NULL || !state->open) {
        return result;
    }
    if (state->restore_focus != 0u) {
        nk_ui_focus(ui, state->restore_focus);
        state->restore_focus = 0u;
    }
    /* F1 and the pad's Start (the menu button) close the overlay from any page,
       as Esc does from the menu. Guide is the host's (it never reaches nk_ui). The
       page is still built: a frame without it would drop the focus it keeps. */
    menu = nk_ui_take_event(ui, NK_UI_EV_MENU, NULL);
    /* The developer pages exist only in developer mode: without it the menu is drawn, and
       the menu offers no way to them. */
    if ((state->screen == NK_OVERLAY_SCREEN_VRAM || state->screen == NK_OVERLAY_SCREEN_PROFILE) &&
        !model->developer) {
        state->screen = NK_OVERLAY_SCREEN_MENU;
    }
    switch (state->screen) {
    case NK_OVERLAY_SCREEN_SETTINGS:
        result = nk_overlay_page_settings(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_CONTROLS:
        result = nk_overlay_page_controls(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_QUIT:
        result = nk_overlay_page_quit(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_PAD_LOST:
        result = nk_overlay_page_pad_lost(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_VRAM:
        result = nk_overlay_page_vram(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_PROFILE:
        result = nk_overlay_page_profile(ui, state, model);
        break;
    case NK_OVERLAY_SCREEN_MENU:
    default:
        result = nk_overlay_page_menu(ui, state, model);
        break;
    }
    return menu ? nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0) : result;
}
