/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Controller settings: the PSP buttons in two columns of named rows, each binding on the
   button that rebinds it, beside the stick and trigger card with the live input. The
   mapping being edited is named on the buttons card, so the scope is stated, never
   implied (#520). CONTROLLER and KEYBOARD above the cards choose whose bindings the rows
   show; for the keyboard the stick card holds the stick's four keys and names the keys
   Nakagawa keeps, for the controller it chooses the stick (#1299). Every change is saved
   as it is made. While a guided calibration runs, its step replaces the cards, and so do
   the mouse aiming settings, which MOUSE AIM under the keyboard's stick card opens
   (#1791). Under a top bar with SETTINGS active, as the Settings page. */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>

#define PAD_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_CONTROLS, (offset))
#define PAD_ID_ROOT PAD_ID(0x000001u)
#define PAD_ID_TOP PAD_ID(0x000010u)
#define PAD_ID_BODY PAD_ID(0x000020u)
#define PAD_ID_COLUMN PAD_ID(0x000021u)
#define PAD_ID_HEAD PAD_ID(0x000022u)
#define PAD_ID_CARDS PAD_ID(0x000023u)
#define PAD_ID_BUTTONS PAD_ID(0x000024u)
#define PAD_ID_STICK PAD_ID(0x000025u)
#define PAD_ID_ACTIONS PAD_ID(0x000026u)
#define PAD_ID_GRID PAD_ID(0x000027u)
#define PAD_ID_GRID_SPAN 0x4u
#define PAD_ID_LIVE PAD_ID(0x000030u)
#define PAD_ID_SPACER PAD_ID(0x000031u)
#define PAD_ID_DEVICE PAD_ID(0x000032u)
#define PAD_ID_DEVICE_BUTTONS PAD_ID(0x000033u)
#define PAD_ID_PROMPTS PAD_ID(0x000040u)
#define PAD_ID_WAVES PAD_ID(0x000050u)
#define PAD_ID_ROWS PAD_ID(0x000100u)         /* + control * 4, + 0-2: a binding row's parts */
#define PAD_ID_ROWS_SPAN 0x80u
#define PAD_ID_GLYPH_CELLS PAD_ID(0x000200u)  /* + control: the cell a binding's glyph sits in */
#define PAD_ID_GLYPH_CELLS_SPAN 0x20u
#define PAD_ID_STEPPERS PAD_ID(0x000280u)     /* + k * 4, + 0-2: a stick card row's parts */
#define PAD_ID_STEPPERS_SPAN 0x10u
#define PAD_ID_AIM_ROWS PAD_ID(0x000300u)     /* + k * 4, + 0-3: a mouse aiming row's parts */
#define PAD_ID_AIM_ROWS_SPAN 0x20u
#define PAD_ID_TEXT PAD_ID(0x001000u)
#define PAD_ID_TEXT_SPAN 0x1000u

static PlayerUiAction controller_op(PlayerUiControllerOp op, size_t control) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_CONTROLLER;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = control;
    return result;
}

/* A label cut to the room a small button of `width` has for it. */
static const char *controller_button_label(NkUi *ui, const char *label, float width) {
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL_S, 0u);
    return nk_ui_text_clamp(ui, nk_ui_caps(ui, label), &text,
                            width - 2.0f * NK_UI_SPACE_3 - 2.0f, 1);
}

/* A binding's glyph, centred in a cell as wide as the widest one (CREATE, six letters
   on a pill, is 43.6 px at 20 px tall), so the buttons after it line up. */
#define PAD_GLYPH_HEIGHT 20.0f
#define PAD_GLYPH_CELL 44.0f
static void controller_glyph(NkUi *ui, const PlayerUiControllerModel *model, size_t control) {
    const PlayerUiBindingGlyph *glyph = &model->glyphs[control];
    NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(PAD_GLYPH_CELL), sizing_fit(),
                                  0.0f);
    cell.align_x = NK_UI_ALIGN_CENTER;
    cell.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, (NkUiId)(PAD_ID_GLYPH_CELLS + (NkUiId)control), &cell,
                               NULL)) {
        return;
    }
    if (glyph->shown) {
        (void)nk_ui_prompt_glyph_in(ui, PLAYER_UI_ID_CONTROLLER_GLYPH(control), model->glyph_style,
                                    glyph->action, glyph->label, PAD_GLYPH_HEIGHT);
    }
    nk_ui_end_container(ui);
}

/* One PSP control: its name, the glyph of what it is bound to, then its binding in
   words on the button that rebinds it. The control the status note names (it gave
   its input to the last binding) has its name in the note's colour. */
static void controller_row(NkUi *ui, const PlayerUiControllerModel *model, size_t control,
                           const char *name, float name_width, float button_width,
                           PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_ROWS + (NkUiId)control * 4u);
    bool capturing = model->capturing == (int)control;
    bool noted = model->noted == (int)control;
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S,
                                    model->conflicted[control] || (noted && model->status_error)
                                        ? theme->error
                                        : noted ? theme->accent_dark : theme->paper);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(name_width), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u,
                       nk_ui_text_clamp(ui, safe_text(name, NK_UI_STR(CONTROLLER_BUTTON)), &text, name_width, 1),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    controller_glyph(ui, model, control);
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_BUTTON(control),
                     controller_button_label(ui, capturing ? (model->keyboard ? NK_UI_STR(CONTROLLER_PRESS_A_KEY)
                                                                              : NK_UI_STR(CONTROLLER_PRESS_A_BUTTON))
                                                           : safe_text(model->bindings[control],
                                                                       NK_UI_STR(CONTROLLER_NONE)),
                                             button_width),
                     NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW |
                         (capturing ? NK_UI_BUTTON_PRIMARY : 0u))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_REBIND, control);
    }
    nk_ui_end_container(ui);
}

/* A value in words with the - and + steps that change it. */
static void controller_stepper(NkUi *ui, uint32_t k, const char *value, float width,
                               NkUiId down, NkUiId up, PlayerUiControllerOp down_op,
                               PlayerUiControllerOp up_op, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_STEPPERS + k * 4u);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        /* Each step is its sign and the small button's padding. */
        float text_width = width - 2.0f * (2.0f * NK_UI_SPACE_3 + 10.0f) - 2.0f * NK_UI_SPACE_2;
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), 0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u, nk_ui_text_clamp(ui, safe_text(value, ""), &text,
                                                       text_width, 1), &text);
            nk_ui_end_container(ui);
        }
    }
    if (nk_ui_button(ui, down, NK_UI_STR(CONTROLLER_DECREASE),
        NK_UI_BUTTON_SMALL)) *result = controller_op(down_op, 0u);
    if (nk_ui_button(ui, up, NK_UI_STR(CONTROLLER_INCREASE), NK_UI_BUTTON_SMALL)) *result = controller_op(up_op, 0u);
    nk_ui_end_container(ui);
}

/* Which controller stick drives the PSP stick, named on the button that switches it:
   124 px, or as wide as its longer word at a larger text size. */
#define PAD_STICK_BUTTON_WIDTH 124.0f
static void controller_stick_choice(NkUi *ui, const PlayerUiControllerModel *model, float width,
                                    PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_STEPPERS + 2u * 4u);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
    float button = PAD_STICK_BUTTON_WIDTH;
    {
        /* controller_button_label keeps 2 px spare beside the padding. */
        float need = nk_ui_button_width(ui, NK_UI_STR(CONTROLLER_RIGHT_STICK), NK_UI_BUTTON_SMALL) + 2.0f;
        if (need > button) button = need;
    }
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), 0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u,
                       nk_ui_text_clamp(ui, NK_UI_STR(CONTROLLER_PSP_STICK), &text, width - button - NK_UI_SPACE_2,
                                        1),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(button), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 3u, &cell, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_STICK,
                             controller_button_label(ui, model->stick_right ? NK_UI_STR(CONTROLLER_RIGHT_STICK)
                                                                            : NK_UI_STR(CONTROLLER_LEFT_STICK),
                                                     button),
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW)) {
                *result = controller_op(PLAYER_UI_CONTROLLER_STICK, 0u);
            }
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

static void controller_live(NkUi *ui, const PlayerUiControllerModel *model, float width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout lines = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 2.0f);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
    if (!nk_ui_begin_container(ui, PAD_ID_LIVE, &lines, NULL)) return;
    for (size_t i = 0u; i < model->live_count && i < PLAYER_UI_CONTROLLER_LIVE_LINES; ++i) {
        nk_ui_text(ui, (NkUiId)(PAD_ID_TEXT + 0x40u + (NkUiId)i),
                   nk_ui_text_clamp(ui, safe_text(model->live[i], " "), &text, width, 1), &text);
    }
    nk_ui_end_container(ui);
}

/* CONTROLLER and KEYBOARD: whose bindings the rows show, the one shown filled. The
   pair is 240 px, or as wide as a larger text size makes its two words. */
#define PAD_DEVICE_WIDTH 240.0f
static float controller_device_width(NkUi *ui) {
    /* controller_button_label keeps 2 px spare beside the padding. */
    float each = nk_ui_button_width(ui, NK_UI_STR(CONTROLLER_CONTROLLER), NK_UI_BUTTON_SMALL) + 2.0f;
    float pair = 2.0f * each + NK_UI_SPACE_2;
    return pair > PAD_DEVICE_WIDTH ? pair : PAD_DEVICE_WIDTH;
}

static void controller_device(NkUi *ui, const PlayerUiControllerModel *model,
                              PlayerUiAction *result) {
    float width = controller_device_width(ui);
    NkUiLayout pair = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(width), sizing_fit(),
                                  NK_UI_SPACE_2);
    float each = (width - NK_UI_SPACE_2) * 0.5f;
    uint32_t flags = NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW | NK_UI_BUTTON_ROW;
    if (!nk_ui_begin_container(ui, PAD_ID_DEVICE_BUTTONS, &pair, NULL)) return;
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SHOW_PAD,
                     controller_button_label(ui, NK_UI_STR(CONTROLLER_CONTROLLER), each),
                     flags | (model->keyboard ? 0u : NK_UI_BUTTON_PRIMARY))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_SHOW_CONTROLLER, 0u);
    }
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SHOW_KEYBOARD,
                     controller_button_label(ui, NK_UI_STR(CONTROLLER_KEYBOARD), each),
                     flags | (model->keyboard ? NK_UI_BUTTON_PRIMARY : 0u))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_SHOW_KEYBOARD, 0u);
    }
    nk_ui_end_container(ui);
}

/* For the keyboard, the stick card holds the stick's four keys, each rebound like a
   button's (#1299), then says which keys stay with Nakagawa. */
#define PAD_STICK_NAME_WIDTH 48.0f
static void controller_keyboard_card(NkUi *ui, const PlayerUiControllerModel *model, float card,
                                     PlayerUiAction *result) {
    static const char *const kept[] = {NK_UI_STR(CONTROLLER_PAUSE_KEY_HINT), NK_UI_STR(CONTROLLER_FRAME_RATE_KEY_HINT)};
    static const char *const directions[PLAYER_UI_CONTROLLER_STICK_KEYS] = {
        NK_UI_STR(CONTROLLER_UP), NK_UI_STR(CONTROLLER_DOWN),
        NK_UI_STR(CONTROLLER_LEFT), NK_UI_STR(CONTROLLER_RIGHT)
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
    float inner = card - 2.0f * NK_UI_SPACE_5;
    if (!nk_ui_card_begin(ui, PAD_ID_STICK, sizing_fixed(card), sizing_grow(), NK_UI_SPACE_2)) {
        return;
    }
    nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x21u, NK_UI_STR(CONTROLLER_ANALOG_STICK));
    for (size_t i = 0u; i < PLAYER_UI_CONTROLLER_STICK_KEYS; ++i) {
        controller_row(ui, model, PLAYER_UI_CONTROLLER_STICK_KEY + i, directions[i],
                       PAD_STICK_NAME_WIDTH,
                       inner - PAD_STICK_NAME_WIDTH - PAD_GLYPH_CELL - 2.0f * NK_UI_SPACE_2,
                       result);
    }
    nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x20u, NK_UI_STR(CONTROLLER_KEYS_NAKAGAWA_KEEPS));
    for (size_t i = 0u; i < sizeof(kept) / sizeof(kept[0]); ++i) {
        nk_ui_text(ui, (NkUiId)(PAD_ID_TEXT + 0x50u + (NkUiId)i),
                   nk_ui_text_clamp(ui, kept[i], &text, inner, 2), &text);
    }
    nk_ui_end_container(ui);
}

static void controller_mapping(NkUi *ui, const PlayerUiControllerModel *model, float width,
                               float height, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool tall = height >= 680.0f;
    float column = width - 2.0f * (width < NK_UI_NARROW_WIDTH ? NK_UI_GUTTER_NARROW
                                                              : NK_UI_GUTTER);
    float stick_card;
    float buttons_inner;
    float half;
    float name_width;
    char eyebrow[160];
    NkUiLayout layout;
    if (column > 1120.0f) column = 1120.0f;
    stick_card = column >= 1000.0f ? 320.0f : 260.0f;
    /* Cards pad 20 px either side; the two columns of rows have 16 px between. */
    buttons_inner = column - NK_UI_SPACE_4 - stick_card - 2.0f * NK_UI_SPACE_5;
    half = (buttons_inner - NK_UI_SPACE_4) * 0.5f;
    /* The longest name, "L Trigger (L1)", needs about 100 px; the rest of the row is
       the binding's glyph and its words. */
    name_width = half * 0.36f;
    if (name_width > 104.0f) name_width = 104.0f;
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column), sizing_fit(),
                         tall ? NK_UI_SPACE_4 : NK_UI_SPACE_2);
    if (!nk_ui_begin_container(ui, PAD_ID_COLUMN, &layout, NULL)) return;
    {
        NkUiLayout head = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                      NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, PAD_ID_HEAD, &head, NULL)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S,
                                            model->status_error ? theme->error : theme->muted);
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.align_y = NK_UI_ALIGN_CENTER;
            /* The SETTINGS tab names the area, so a short window drops the
               heading to keep every row on screen. */
            if (tall) {
                nk_ui_text_role(ui, PAD_ID_TEXT + 0u, NK_UI_STR(CONTROLLER_TITLE), NK_UI_TYPE_DISPLAY,
                                theme->paper);
            }
            if (nk_ui_begin_container(ui, PAD_ID_DEVICE, &row, NULL)) {
                controller_device(ui, model, result);
                nk_ui_text(ui, PAD_ID_TEXT + 1u,
                           nk_ui_text_clamp(ui, safe_text(model->status,
                                                          NK_UI_STR(CONTROLLER_REMAP_INSTRUCTIONS)),
                                            &text,
                                            column - controller_device_width(ui) - NK_UI_SPACE_3,
                                            1),
                           &text);
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    if (nk_ui_begin_container(ui, PAD_ID_CARDS, &layout, NULL)) {
        if (nk_ui_card_begin(ui, PAD_ID_BUTTONS, sizing_grow(), sizing_grow(), NK_UI_SPACE_2)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_EYEBROW,
                                            model->scope_title ? theme->ready : theme->muted);
            (void)snprintf(eyebrow, sizeof(eyebrow), NK_UI_STR(CONTROLLER_MAPPING_SCOPE_FORMAT),
                           model->keyboard ? NK_UI_STR(CONTROLLER_KEYS) : NK_UI_STR(CONTROLLER_BUTTONS),
                           safe_text(model->scope, NK_UI_STR(CONTROLLER_GLOBAL_MAPPING)));
            nk_ui_text(ui, PAD_ID_TEXT + 0x10u,
                       nk_ui_text_clamp(ui, nk_ui_caps(ui, eyebrow), &text, buttons_inner, 1),
                       &text);
            layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
            if (nk_ui_begin_container(ui, PAD_ID_GRID, &layout, NULL)) {
                for (size_t col = 0u; col < 2u; ++col) {
                    NkUiLayout list = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(half),
                                                  sizing_fit(),
                                                  tall ? NK_UI_SPACE_2 : NK_UI_SPACE_1);
                    if (!nk_ui_begin_container(ui, (NkUiId)(PAD_ID_GRID + 1u + (NkUiId)col),
                                               &list, NULL)) {
                        continue;
                    }
                    for (size_t row = 0u; row < PLAYER_UI_CONTROLLER_BUTTONS / 2u; ++row) {
                        size_t control = col * (PLAYER_UI_CONTROLLER_BUTTONS / 2u) + row;
                        controller_row(ui, model, control, model->names[control], name_width,
                                       half - name_width - PAD_GLYPH_CELL - 2.0f * NK_UI_SPACE_2,
                                       result);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
        if (model->keyboard) {
            controller_keyboard_card(ui, model, stick_card, result);
        } else if (nk_ui_card_begin(ui, PAD_ID_STICK, sizing_fixed(stick_card), sizing_grow(),
                                    NK_UI_SPACE_2)) {
            float inner = stick_card - 2.0f * NK_UI_SPACE_5;
            nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x20u, NK_UI_STR(CONTROLLER_STICK_AND_TRIGGERS));
            controller_stick_choice(ui, model, inner, result);
            controller_stepper(ui, 0u, model->deadzone, inner,
                               PLAYER_UI_ID_CONTROLLER_DEADZONE_DOWN,
                               PLAYER_UI_ID_CONTROLLER_DEADZONE_UP,
                               PLAYER_UI_CONTROLLER_DEADZONE_DOWN,
                               PLAYER_UI_CONTROLLER_DEADZONE_UP, result);
            controller_stepper(ui, 1u, model->trigger, inner,
                               PLAYER_UI_ID_CONTROLLER_TRIGGER_DOWN,
                               PLAYER_UI_ID_CONTROLLER_TRIGGER_UP,
                               PLAYER_UI_CONTROLLER_TRIGGER_DOWN,
                               PLAYER_UI_CONTROLLER_TRIGGER_UP, result);
            if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATE, NK_UI_STR(CONTROLLER_CALIBRATE_STICK),
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW)) {
                *result = controller_op(PLAYER_UI_CONTROLLER_CALIBRATE, 0u);
            }
            nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x21u, NK_UI_STR(CONTROLLER_LIVE_INPUT));
            controller_live(ui, model, inner);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    /* Back sits at the right end, under the stick card, so Down from Calibrate
       reaches it rather than a binding in the grid; Left and Right stay in the row. */
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
    if (nk_ui_begin_container(ui, PAD_ID_ACTIONS, &layout, NULL)) {
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_RESET, NK_UI_STR(CONTROLLER_RESET_TO_DEFAULTS),
                         NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_RESET, 0u);
        }
        /* Global or this game's own, offered only with a game selected (#520). */
        if (model->offer_scope &&
            nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SCOPE,
                         model->scope_title ? NK_UI_STR(CONTROLLER_USE_THE_GLOBAL_MAPPING)
                                            : NK_UI_STR(CONTROLLER_USE_THIS_GAMES_OWN_MAPPING),
                         NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_TOGGLE_SCOPE, 0u);
        }
        nk_ui_spacer(ui, PAD_ID_SPACER, sizing_grow(), sizing_fixed(1.0f));
        /* Mouse aiming (#1791) is the keyboard and mouse's: under the stick card, whose stick
           it moves, its button says whether it is on and opens its settings. The card itself
           has no room for it at the largest text size. */
        if (model->keyboard &&
            nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_AIM_OPEN,
                         safe_text(model->aim, NK_UI_STR(CONTROLLER_AIM_OFF_LABEL)), NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_AIM_OPEN, 0u);
        }
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_BACK, NK_UI_STR(CONTROLLER_BACK), NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_BACK, 0u);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* A mouse aiming row: its control in a cell on the left, then the setting in words. The
   controls are full height (NK_UI_CONTROL_HEIGHT, the smallest pointer target), not the
   cards' small inline ones. The cell is as wide in every row, so the controls read as one
   column, as a Settings page's do, and Up and Down move straight through it: 112 px, or as
   wide as a larger text size makes the switch's longer word or a stepper's two steps. Left
   and Right stay in the row. */
#define PAD_AIM_ROW(k) ((NkUiId)(PAD_ID_AIM_ROWS + (uint32_t)(k) * 4u))
#define PAD_AIM_CELL_WIDTH 112.0f
static float controller_aim_cell_width(NkUi *ui) {
    float word = nk_ui_button_width(ui, NK_UI_STR(CONTROLLER_OFF), 0u);
    float steps = 2.0f * nk_ui_button_width(ui, NK_UI_STR(CONTROLLER_INCREASE), 0u) + NK_UI_SPACE_2;
    float cell = PAD_AIM_CELL_WIDTH;
    if (word > cell) cell = word;
    if (steps > cell) cell = steps;
    return cell;
}

/* Opens row k and its control cell. False: nothing is open. */
static bool controller_aim_row_begin(NkUi *ui, uint32_t k, float cell_width) {
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
    NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(cell_width), sizing_fit(),
                                  NK_UI_SPACE_2);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, PAD_AIM_ROW(k), &row, NULL)) return false;
    if (!nk_ui_begin_container(ui, PAD_AIM_ROW(k) + 1u, &cell, NULL)) {
        nk_ui_end_container(ui);
        return false;
    }
    return true;
}

/* Closes the control cell, writes the setting's words in the room beside it, and closes
   the row. */
static void controller_aim_row_end(NkUi *ui, uint32_t k, const char *words, float room) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
    NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), 0.0f);
    nk_ui_end_container(ui);
    if (nk_ui_begin_container(ui, PAD_AIM_ROW(k) + 2u, &cell, NULL)) {
        nk_ui_text(ui, PAD_AIM_ROW(k) + 3u, nk_ui_text_clamp(ui, words, &text, room, 1), &text);
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* A switch: On or Off on the button that changes it, filled while it is on. */
static void controller_aim_switch(NkUi *ui, uint32_t k, const char *name, bool on, NkUiId id,
                                  PlayerUiControllerOp op, float cell_width, float width,
                                  PlayerUiAction *result) {
    if (!controller_aim_row_begin(ui, k, cell_width)) return;
    if (nk_ui_button(ui, id, on ? NK_UI_STR(CONTROLLER_ON) : NK_UI_STR(CONTROLLER_OFF),
                     NK_UI_BUTTON_GROW | NK_UI_BUTTON_ROW | (on ? NK_UI_BUTTON_PRIMARY : 0u))) {
        *result = controller_op(op, 0u);
    }
    controller_aim_row_end(ui, k, name, width - cell_width - NK_UI_SPACE_3);
}

/* A value in words after the - and + steps that change it. */
static void controller_aim_stepper(NkUi *ui, uint32_t k, const char *value, NkUiId down,
                                   NkUiId up, PlayerUiControllerOp down_op,
                                   PlayerUiControllerOp up_op, float cell_width, float width,
                                   PlayerUiAction *result) {
    uint32_t flags = NK_UI_BUTTON_GROW | NK_UI_BUTTON_ROW;
    if (!controller_aim_row_begin(ui, k, cell_width)) return;
    if (nk_ui_button(ui, down, NK_UI_STR(CONTROLLER_DECREASE), flags)) *result = controller_op(down_op, 0u);
    if (nk_ui_button(ui, up, NK_UI_STR(CONTROLLER_INCREASE), flags)) *result = controller_op(up_op, 0u);
    controller_aim_row_end(ui, k, value, width - cell_width - NK_UI_SPACE_3);
}

/* The mouse aiming settings (#1791), in place of the cards while they are open: the switch,
   the speed and the smoothing in steps, and each direction's inversion, for the mapping the
   eyebrow names. Every change is saved as it is made; Done returns to the keyboard keys. */
static void controller_aim(NkUi *ui, const PlayerUiControllerModel *model, float width,
                           PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    char eyebrow[160];
    float card = width - 2.0f * NK_UI_GUTTER_NARROW;
    float inner;
    NkUiLayout row;
    if (card > 720.0f) card = 720.0f;
    inner = card - 56.0f;
    if (!nk_ui_dialog_begin(ui, FLOW_ID_CARD, card)) return;
    (void)snprintf(eyebrow, sizeof(eyebrow), NK_UI_STR(CONTROLLER_MOUSE_AIM_SCOPE_FORMAT),
                   safe_text(model->scope, NK_UI_STR(CONTROLLER_GLOBAL_MAPPING)));
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, eyebrow,
                     model->scope_title ? theme->ready : theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, NK_UI_STR(CONTROLLER_AIM_WITH_THE_MOUSE), NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY, theme->muted);
        nk_ui_text(ui, FLOW_ID_LEAD,
                   nk_ui_text_clamp(ui,
                                    NK_UI_STR(CONTROLLER_MOUSE_AIM_LEAD),
                                    &text, inner, 3),
                   &text);
    }
    if (scr_wizard_box_begin(ui, 0u)) {
        float rows = inner - 32.0f;
        float cell = controller_aim_cell_width(ui);
        controller_aim_switch(ui, 0u, NK_UI_STR(CONTROLLER_MOUSE_AIM), model->aim_on,
                              PLAYER_UI_ID_CONTROLLER_AIM_TOGGLE,
                              PLAYER_UI_CONTROLLER_AIM_TOGGLE, cell, rows, result);
        controller_aim_stepper(ui, 1u, safe_text(model->aim_speed, NK_UI_STR(CONTROLLER_DEFAULT_AIM_SPEED_LABEL)),
                               PLAYER_UI_ID_CONTROLLER_AIM_SPEED_DOWN,
                               PLAYER_UI_ID_CONTROLLER_AIM_SPEED_UP,
                               PLAYER_UI_CONTROLLER_AIM_SPEED_DOWN,
                               PLAYER_UI_CONTROLLER_AIM_SPEED_UP, cell, rows, result);
        controller_aim_stepper(ui, 2u, safe_text(model->aim_smoothing,
            NK_UI_STR(CONTROLLER_DEFAULT_AIM_SMOOTHING_LABEL)),
                               PLAYER_UI_ID_CONTROLLER_AIM_SMOOTHING_DOWN,
                               PLAYER_UI_ID_CONTROLLER_AIM_SMOOTHING_UP,
                               PLAYER_UI_CONTROLLER_AIM_SMOOTHING_DOWN,
                               PLAYER_UI_CONTROLLER_AIM_SMOOTHING_UP, cell, rows, result);
        controller_aim_switch(ui, 3u, NK_UI_STR(CONTROLLER_INVERT_LEFT_AND_RIGHT), model->aim_invert_x,
                              PLAYER_UI_ID_CONTROLLER_AIM_INVERT_X,
                              PLAYER_UI_CONTROLLER_AIM_INVERT_X, cell, rows, result);
        controller_aim_switch(ui, 4u, NK_UI_STR(CONTROLLER_INVERT_UP_AND_DOWN), model->aim_invert_y,
                              PLAYER_UI_ID_CONTROLLER_AIM_INVERT_Y,
                              PLAYER_UI_CONTROLLER_AIM_INVERT_Y, cell, rows, result);
        nk_ui_end_container(ui);
    }
    row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
    row.padding_top = NK_UI_SPACE_1;
    if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_AIM_DONE, NK_UI_STR(CONTROLLER_DONE),
                         NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_AIM_DONE, 0u);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* A calibration step: what to do, its numbers, and its buttons. */
static void controller_calibration(NkUi *ui, const PlayerUiControllerModel *model, float width,
                                   PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const char *step;
    const char *title;
    const char *lead;
    const char *primary = NULL;
    PlayerUiControllerOp primary_op = PLAYER_UI_CONTROLLER_NONE;
    float card = width - 2.0f * NK_UI_GUTTER_NARROW;
    float inner;
    NkUiLayout row;
    if (card > 720.0f) card = 720.0f;
    inner = card - 56.0f;
    switch (model->page) {
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST:
            step = NK_UI_STR(CONTROLLER_CALIBRATION_REST_STEP);
            title = NK_UI_STR(CONTROLLER_CALIBRATION_REST_TITLE);
            lead = NK_UI_STR(CONTROLLER_CALIBRATION_REST_LEAD);
            break;
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_EXTREMES:
            step = NK_UI_STR(CONTROLLER_CALIBRATION_RANGE_STEP);
            title = NK_UI_STR(CONTROLLER_CALIBRATION_RANGE_TITLE);
            lead = NK_UI_STR(CONTROLLER_CALIBRATION_RANGE_LEAD);
            primary = NK_UI_STR(CONTROLLER_FINISH);
            primary_op = PLAYER_UI_CONTROLLER_CALIBRATION_FINISH;
            break;
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_RESULT:
        default:
            step = NK_UI_STR(CONTROLLER_CALIBRATION_CONFIRM_STEP);
            title = NK_UI_STR(CONTROLLER_CALIBRATION_CONFIRM_TITLE);
            lead = NK_UI_STR(CONTROLLER_CALIBRATION_CONFIRM_LEAD);
            primary = NK_UI_STR(CONTROLLER_ACCEPT);
            primary_op = PLAYER_UI_CONTROLLER_CALIBRATION_ACCEPT;
            break;
    }
    if (!nk_ui_dialog_begin(ui, FLOW_ID_CARD, card)) return;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, step, theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, title, NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY, theme->muted);
        nk_ui_text(ui, FLOW_ID_LEAD, nk_ui_text_clamp(ui, lead, &text, inner, 3), &text);
    }
    if (model->page == PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST) {
        nk_ui_progress_bar(ui, FLOW_ID_BAR, inner, model->calibration_fraction, 0.0);
    }
    if (scr_wizard_box_begin(ui, 0u)) {
        controller_live(ui, model, inner - 32.0f);
        nk_ui_end_container(ui);
    }
    row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
    row.padding_top = NK_UI_SPACE_1;
    if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
        if (primary != NULL &&
            nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY, primary,
                         NK_UI_BUTTON_PRIMARY)) {
            *result = controller_op(primary_op, 0u);
        }
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL, NK_UI_STR(CONTROLLER_CANCEL),
                         primary == NULL ? NK_UI_BUTTON_PRIMARY : 0u)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_CALIBRATION_CANCEL, 0u);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

PlayerUiAction player_ui_screen_controller(NkUi *ui, const PlayerUiControllerModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(CONTROLLER_MOVE)}, {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(CONTROLLER_CHANGE)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(CONTROLLER_BACK_TO_SETTINGS)}
    };
    static const NkUiPromptItem prompts_calibrating[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(CONTROLLER_SELECT)}, {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(CONTROLLER_CANCEL)}
    };
    static const NkUiPromptItem prompts_aim[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(CONTROLLER_MOVE)}, {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(CONTROLLER_CHANGE)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(CONTROLLER_BACK_TO_THE_KEYS)}
    };
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    bool aiming;
    bool calibrating;
    NkUiLayout layout;
    float width;
    float height;
    if (ui == NULL || model == NULL) return result;
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    aiming = model->page == PLAYER_UI_CONTROLLER_PAGE_AIM;
    calibrating = model->page != PLAYER_UI_CONTROLLER_PAGE_MAPPING && !aiming;
    (void)nk_ui_waves_element(ui, PAD_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, PAD_ID_ROOT, &layout, NULL)) return result;
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, PAD_ID_TOP, PLAYER_UI_ROUTE_SETTINGS);
        if (tab != PLAYER_UI_ACTION_NONE) result = action(tab, 0u, PLAYER_UI_PRIMARY_NONE);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    if (calibrating || aiming) {
        layout.align_y = NK_UI_ALIGN_CENTER;
        layout.padding_top = layout.padding_bottom = NK_UI_SPACE_4;
    } else {
        layout.padding_top = height >= 680.0f ? NK_UI_SPACE_7 : NK_UI_SPACE_4;
    }
    if (nk_ui_begin_container(ui, PAD_ID_BODY, &layout, NULL)) {
        if (aiming) controller_aim(ui, model, width, &result);
        else if (calibrating) controller_calibration(ui, model, width, &result);
        else controller_mapping(ui, model, width, height, &result);
        nk_ui_end_container(ui);
    }
    nk_ui_prompt_bar(ui, PAD_ID_PROMPTS, "",
                     aiming ? prompts_aim : calibrating ? prompts_calibrating : prompts,
                     calibrating ? 2u : 3u);
    nk_ui_end_container(ui);
    return result;
}
