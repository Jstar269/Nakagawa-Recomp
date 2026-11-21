/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The rows and boxes the setup wizard and Settings share: a dark inset panel, a
   setting's "Name: value" button with the sentence beside it, and the check
   columns. They draw with the setup wizard's ids (the WIZARD screen byte in
   nk_ui.h), so the panel keeps its id wherever it is used. The nickname editor
   that the first launch and Settings share (#2197) is a one-card flow page
   with the FLOW screen's ids, and the on-screen keyboard (ui_keyboard.h). */

#include "player_ui_nk.h"
#include "nk_adhoc_relay.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"
#include "ui_keyboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIZ_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_WIZARD, (offset))
#define WIZ_ID_BOX WIZ_ID(0x000010u)

/* The width a column of check rows takes: its design width, or the widest of
   its entries (the status words in caps, or the names) in `role` when a larger
   text size makes one wider. */
float scr_check_column_width(NkUi *ui, const PlayerUiCheckRow *checks, size_t count,
                                bool status, NkUiTypeRole role, const char *fallback,
                                float design) {
    NkUiTextStyle style = nk_ui_type(role, 0u);
    float width = design;
    for (size_t i = 0u; checks != NULL && i < count; ++i) {
        const char *text = status ? nk_ui_caps(ui, safe_text(checks[i].status, fallback))
                                  : safe_text(checks[i].name, fallback);
        float need = ceilf(nk_ui_text_width(ui, text, &style));
        if (need > width) width = need;
    }
    return width;
}

/* A dark inset panel for the step's facts. Returns true when opened. */
bool scr_wizard_box_begin(NkUi *ui, uint32_t edge) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 6.0f);
    NkUiStyle style = scr_flow_box_style(theme, theme->surface);
    if (edge != 0u) style.border_rgba = edge;
    box.padding_top = box.padding_bottom = 14.0f;
    box.padding_left = box.padding_right = 16.0f;
    return nk_ui_begin_container(ui, WIZ_ID_BOX, &box, &style);
}

/* The button column of a setting row, and the least room its sentence keeps
   beside it before the sentences move under the buttons. */
#define WIZARD_VALUE_CELL 220.0f
#define WIZARD_VALUE_SENTENCE_MIN 260.0f

/* Whether nk_ui_text_clamp cut `text` short to give `shown`. */
static bool text_was_cut(const char *shown, const char *text) {
    static const char ellipsis[] = "\xE2\x80\xA6";
    size_t n = sizeof(ellipsis) - 1u;
    size_t shown_length = strlen(shown);
    size_t text_length = strlen(text);
    return shown_length >= n && memcmp(shown + shown_length - n, ellipsis, n) == 0 &&
           !(text_length >= n && memcmp(text + text_length - n, ellipsis, n) == 0);
}

/* A setting button's label, "name: value", whole. A label wider than the row's `width`
   ends in an ellipsis, cut whole characters at a time by nk_ui_text_clamp (in the
   button's capitals, which it draws), so the button never outgrows its row. The column
   and the button take the same label. Returns `out` or frame storage. */
static const char *scr_wizard_label(NkUi *ui, char *out, size_t cap, const char *name,
                                    const char *value, float width) {
    NkUiTextStyle style;
    float pad;
    (void)snprintf(out, cap, "%s: %s", name, value);
    if (!(width > 0.0f) || nk_ui_button_width(ui, out, 0u) <= width) return out;
    pad = nk_ui_button_width(ui, "", 0u);
    style = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
    return nk_ui_text_clamp(ui, nk_ui_caps(ui, out), &style, width - pad, 1);
}

/* The column a group of rows `width` px across shares: the design's 220 px, or
   as wide as the widest label when that is wider. The buttons line up. At text
   sizes above design, or at design size when Enhanced opts in, a sentence with
   too little room or clipped after two lines makes every sentence go under its
   button at full width. */
WizardColumn scr_wizard_column(NkUi *ui, const WizardRow *rows, size_t count, float width,
                              bool stack_at_design_size) {
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    WizardColumn column = {WIZARD_VALUE_CELL, false};
    char label[SCR_WIZARD_LABEL_MAX];
    float room;
    for (size_t i = 0u; i < count; ++i) {
        float need = nk_ui_button_width(
            ui, scr_wizard_label(ui, label, sizeof(label), rows[i].name, rows[i].value, width),
            0u);
        if (need > column.cell) column.cell = need;
    }
    if (!stack_at_design_size && !(nk_ui_text_scale(ui) > 1.0f)) return column;
    room = width - column.cell - NK_UI_SPACE_4;
    column.stacked = room < WIZARD_VALUE_SENTENCE_MIN;
    for (size_t i = 0u; i < count && !column.stacked; ++i) {
        column.stacked = text_was_cut(nk_ui_text_clamp(ui, rows[i].sentence, &text, room, 2),
                                      rows[i].sentence);
    }
    return column;
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
/* Whether a setting row's button label or its sentence was cut short, each time that changes
   for the row (by its text id), so a test can see that a row shows whole at a window size. */
static void scr_wizard_report_cut(NkUiId text_id, const char *name, bool label_cut,
                                  bool sentence_cut) {
    static struct {
        NkUiId id;
        int state;
    } reported[64];
    static size_t used;
    const int state = (label_cut ? 1 : 0) | (sentence_cut ? 2 : 0);
    size_t i = 0u;
    while (i < used && reported[i].id != text_id) i++;
    if (i < used && reported[i].state == state) return;
    if (i == used) {
        if (used == sizeof reported / sizeof reported[0]) return;
        reported[used++].id = text_id;
    }
    reported[i].state = state;
    printf("[PLAYER_UI_TEST] next_ui_wizard_row id=%08x label_cut=%d sentence_cut=%d name=%s\n",
           (unsigned)text_id, label_cut ? 1 : 0, sentence_cut ? 1 : 0, safe_text(name, ""));
}
#endif

/* A setting's button, "Name: value", with the sentence that explains it beside
   it (or under it, as `column` says; NULL lays the row out on its own); the
   button is outlined in the accent while the setting is in effect (NK_UI_BUTTON_ON), apart
   from the focus ring and the filled primary action. Returns true when the
   button is activated. */

bool scr_wizard_value(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                         const char *value, const char *sentence, bool on, float width,
                         const WizardColumn *column) {
    return scr_wizard_value_state(ui, id, text_id, name, value, sentence, on, false, width, column);
}

/* As scr_wizard_value; a disabled row cannot be focused and its words are dim, sentence included. */
bool scr_wizard_value_state(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                               const char *value, const char *sentence, bool on, bool disabled,
                               float width, const WizardColumn *column) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, disabled ? theme->dim : theme->muted);
    WizardColumn own;
    char buffer[SCR_WIZARD_LABEL_MAX];
    const char *label;
    float room;
    bool pressed = false;
    if (column == NULL) {
        WizardRow self = {name, value, sentence};
        own = scr_wizard_column(ui, &self, 1u, width, false);
        column = &own;
    }
    label = scr_wizard_label(ui, buffer, sizeof(buffer), name, value, width);
    room = width - column->cell - NK_UI_SPACE_4;
    if (column->stacked) {
        /* The sentence sits close under its own button, and the next row's
           button further off, so each pair reads as one. */
        row = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), NK_UI_SPACE_1);
        row.padding_bottom = NK_UI_SPACE_3;
        room = width;
    }
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, text_id, &row, NULL)) return false;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(column->cell), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, text_id + 1u, &cell, NULL)) {
            pressed = nk_ui_button(ui, id, label,
                                   NK_UI_BUTTON_GROW | (on && !disabled ? NK_UI_BUTTON_ON : 0u) |
                                       (disabled ? NK_UI_BUTTON_DISABLED : 0u));
            nk_ui_end_container(ui);
        }
    }
    /* Under its button the sentence has the full width and wraps whole: the rows stack so
       that no sentence is cut, and it says what the setting does nowhere else. */
    {
        const char *shown =
            column->stacked ? sentence : nk_ui_text_clamp(ui, sentence, &text, room, 2);
        nk_ui_text(ui, text_id + 2u, shown, &text);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
        scr_wizard_report_cut(text_id, name, text_was_cut(label, buffer),
                              shown != sentence && text_was_cut(shown, safe_text(sentence, "")));
#endif
    }
    nk_ui_end_container(ui);
    return pressed;
}

/* One option switch: a button reading "Name: On" or "Name: Off" (accent while
   on) with its sentence beside it. Returns true when activated. */
bool scr_wizard_switch(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                          const char *sentence, bool on, float width,
                          const WizardColumn *column) {
    return scr_wizard_value(ui, id, text_id, name, on ? "On" : "Off", sentence, on, width, column);
}

bool player_ui_nickname_accepts(uint32_t codepoint, void *user) {
    /* The rule takes a character anywhere in a nickname exactly when it takes it
       after a letter: nk_nickname_valid checks each character alone, then the
       length, and refuses a name made only of spaces. The letter is what lets a
       space through here; Done checks the whole name. */
    char bytes[6] = {'x'};
    (void)user;
    return codepoint != 0u && ui_keyboard_utf8(codepoint, bytes + 1) > 0u &&
           nk_nickname_valid(bytes);
}

/* The port offset (#1139): decimal digits only; Done checks the range. */
bool player_ui_port_accepts(uint32_t codepoint, void *user) {
    (void)user;
    return codepoint >= '0' && codepoint <= '9';
}

bool player_ui_server_accepts(uint32_t codepoint, void *user) {
    (void)user;
    return (codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z') ||
           (codepoint >= '0' && codepoint <= '9') || codepoint == '.' || codepoint == '-' ||
           codepoint == ':' || codepoint == ',';
}

/* The nickname / server editor (#2197, #1139): the box with what is typed (the value in use,
   dimmed, while nothing is), how many bytes it uses, why the last Done was refused, then the
   on-screen keyboard with Done and Skip (first launch) or Cancel (Settings). A physical keyboard
   also types into the box (player_ui_nk.c); Back is Skip or Cancel. */
PlayerUiAction player_ui_screen_nickname(NkUi *ui, const PlayerUiNicknameModel *model) {
    static const NkUiPromptItem prompts_first[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Type"},
        {NK_UI_PROMPT_ACT_OPTIONS, "Delete"}, {NK_UI_PROMPT_ACT_MENU, "Done"},
        {NK_UI_PROMPT_ACT_BACK, "Skip"}
    };
    static const NkUiPromptItem prompts_settings[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Type"},
        {NK_UI_PROMPT_ACT_OPTIONS, "Delete"}, {NK_UI_PROMPT_ACT_MENU, "Done"},
        {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    char scratch[NK_ADHOC_SERVER_MAX_LEN + 1u];
    UiKeyboard empty;
    UiKeyboard *keyboard;
    UiKeyboardView view;
    const char *typed;
    float inner;
    if (ui == NULL || model == NULL) return result;
    keyboard = model->keyboard;
    if (keyboard == NULL || keyboard->text == NULL) {
        /* Nothing to type into: the keys show, and what they type is dropped. */
        scratch[0] = '\0';
        ui_keyboard_init(&empty, scratch,
                         model->port_mode ? 5u
                         : model->server_mode ? NK_ADHOC_SERVER_MAX_LEN : NK_NICKNAME_MAX_BYTES,
                         model->port_mode ? player_ui_port_accepts
                         : model->server_mode ? player_ui_server_accepts
                                              : player_ui_nickname_accepts,
                         NULL);
        keyboard = &empty;
    }
    theme = nk_ui_theme(ui);
    typed = keyboard->text;
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                           model->logical_height > 80.0f ? model->logical_height : 800.0f,
                           640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND,
                     model->server_mode || model->port_mode ? "Online play" : "Nickname",
                     theme->info);
    nk_ui_text_role(ui, FLOW_ID_TITLE,
                    model->port_mode ? "Port offset"
                    : model->server_mode
                        ? "Other server"
                        : model->first_launch ? "Choose a nickname" : "Change your nickname",
                    NK_UI_TYPE_TITLE, theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    model->port_mode
                        ? "A whole number from 0 to 65535, added to the game's ports on the "
                          "server (10000 unless you change it). Players who want to play "
                          "together need the same value. It applies from the next game you start."
                        : model->server_mode
                        ? "Enter a community ad hoc relay server: host, host:lobby_port, or "
                          "host:lobby_port,data_port (default ports 27312 and 27313; press Shift "
                          "for ':'). It is kept only on this PC. Done opens the Online play card "
                          "for it; nothing connects until you choose TURN ON there."
                        : model->first_launch
                        ? "Games show it as your PSP's nickname. Type up to 32 bytes of text "
                          "(32 letters, about 10 Japanese characters), or skip to keep \"" NK_NICKNAME_DEFAULT "\". "
                          "You can change it in Settings."
                        : "Games show it as your PSP's nickname when they start; a game that "
                          "saved its own nickname keeps that one. Up to 32 bytes of text "
                          "(32 letters, about 10 Japanese characters).",
                    NK_UI_TYPE_BODY_S, theme->muted);
    {
        NkUiLayout field = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(),
                                       sizing_fixed(NK_UI_CONTROL_HEIGHT), 0.0f);
        NkUiStyle style;
        memset(&style, 0, sizeof(style));
        style.fill_rgba = theme->surface_high;
        style.border_rgba = theme->line_strong;
        style.border_width = NK_UI_BORDER_HAIRLINE;
        style.corner_radius = NK_UI_RADIUS_CONTROL;
        field.align_y = NK_UI_ALIGN_CENTER;
        field.padding_left = NK_UI_SPACE_4;
        field.padding_right = NK_UI_SPACE_4;
        if (nk_ui_begin_container(ui, FLOW_ID_NICKNAME_FIELD, &field, &style)) {
            char shown[NK_ADHOC_SERVER_MAX_LEN + 2u];
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY,
                                            typed[0] != '\0' ? theme->paper : theme->muted);
            text.wrap = false;
            if (typed[0] != '\0') {
                (void)snprintf(shown, sizeof(shown), "%s|", typed);
            } else {
                (void)snprintf(shown, sizeof(shown), "%s",
                               safe_text(model->current,
                                         model->port_mode ? "10000"
                                         : model->server_mode ? NK_ADHOC_DEFAULT_SERVER
                                                              : NK_NICKNAME_DEFAULT));
            }
            nk_ui_text(ui, FLOW_ID_NICKNAME_TEXT,
                       nk_ui_text_clamp(ui, shown, &text, inner - 2.0f * NK_UI_SPACE_4, 1),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    {
        /* The count, in the warning colour once the text is full. */
        char count[32];
        bool full = strlen(typed) >= keyboard->cap;
        (void)ui_keyboard_count_text(keyboard, count, sizeof(count));
        nk_ui_text_role(ui, FLOW_ID_NICKNAME_COUNT, count, NK_UI_TYPE_CAPTION,
                        full ? theme->warning : theme->muted);
    }
    if (model->refused) {
        char server_note[320];
        /* A refused server says why first, by name; the server in use stays. */
        (void)snprintf(server_note, sizeof(server_note),
                       "Not used: %s. Enter a host name or IPv4 address, with optional "
                       ":lobby_port or :lobby_port,data_port from 1 to 65535.",
                       model->refused_reason ? model->refused_reason
                                             : "it is not a server address");
        nk_ui_text_role(ui, FLOW_ID_NICKNAME_NOTE,
                        model->port_mode
                            ? "Enter a whole number from 0 to 65535."
                        : model->server_mode
                            ? nk_ui_frame_text(ui, server_note)
                            : "A nickname is 1 to 32 bytes of text (32 plain letters; accented or "
                              "Japanese letters take 2 to 4 bytes each), with no control characters and not only spaces.",
                        NK_UI_TYPE_BODY_S, theme->warning);
    }
    memset(&view, 0, sizeof(view));
    view.key_base = PLAYER_UI_ID_NICKNAME_KEY_BASE;
    view.part_base = FLOW_ID_NICKNAME_KEYS;
    view.done_id = PLAYER_UI_ID_NICKNAME_SAVE;
    view.cancel_id = PLAYER_UI_ID_NICKNAME_SKIP;
    view.done_label = "Done";
    view.cancel_label = model->first_launch ? "Skip" : "Cancel";
    switch (ui_keyboard_draw(ui, keyboard, &view)) {
    case UI_KEYBOARD_DONE:
        result = action(PLAYER_UI_ACTION_NICKNAME, 0u,
                        (PlayerUiPrimaryAction)PLAYER_UI_NICKNAME_SAVE);
        break;
    case UI_KEYBOARD_CANCEL:
        result = action(PLAYER_UI_ACTION_NICKNAME, 0u,
                        (PlayerUiPrimaryAction)PLAYER_UI_NICKNAME_SKIP);
        break;
    case UI_KEYBOARD_EDITED:
        result = action(PLAYER_UI_ACTION_NICKNAME, 0u,
                        (PlayerUiPrimaryAction)PLAYER_UI_NICKNAME_EDITED);
        break;
    case UI_KEYBOARD_NONE:
    default:
        break;
    }
    scr_flow_end(ui, "", model->first_launch ? prompts_first : prompts_settings,
                 sizeof(prompts_first) / sizeof(prompts_first[0]));
    return result;
}

WizardColumn scr_online_developer_column(NkUi *ui, bool online_on, bool developer, float width) {
    /* The Online mode row is there only while the stored setting is On. */
    const WizardRow rows[] = {
        {"Online mode", "On", SCR_ONLINE_STORED_ON_SENTENCE},
        {"Developer mode", developer ? "On" : "Off", WIZARD_DEVELOPER_SENTENCE}
    };
    return online_on ? scr_wizard_column(ui, rows, 2u, width, false)
                     : scr_wizard_column(ui, rows + 1, 1u, width, false);
}
