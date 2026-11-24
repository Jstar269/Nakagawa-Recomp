/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Remove from library (#1304): the game, what Nakagawa built for it with
   sizes, the promise about the disc image, and two boxes that stay off until
   the user ticks them: the saves, and the decrypted files and disc copy
   (#2003). Cancel holds focus first.

   Saves (#1883) and Add to Steam (#2152), the title page's other dialogs, are
   here too: player_ui_screen_saves and player_ui_screen_steam. */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>

/* English retains the existing control. A translated choice that needs more
   room grows vertically and keeps its original action and focus ID. */
static bool remove_choice_button(NkUi *ui, NkUiId id, const char *label, float width) {
    NkUiLayout layout;
    NkUiStyle style;
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle text = nk_ui_type(nk_ui_button_role(0u), nk_ui_button_text_rgba(ui, 0u));
    bool focused;
    float min_height;
    if (!NK_UI_ALTERNATE_CATALOG || nk_ui_button_width(ui, label, 0u) <= width) {
        return nk_ui_button(ui, id, label, 0u);
    }
    nk_ui_button_configuration(ui, 0u, &layout, &style);
    min_height = layout.height.value;
    layout.width = sizing_fixed(width);
    layout.height = sizing_fit();
    layout.height.min = min_height;
    layout.padding_top = layout.padding_bottom = NK_UI_SPACE_2;
    layout.align_x = NK_UI_ALIGN_START;
    nk_ui_register_button(ui, id, 0u);
    focused = nk_ui_button_focused(ui, id);
    if (nk_ui_pressed(ui, id)) style.fill_rgba = theme->pressed;
    else if (focused || nk_ui_hovered(ui, id)) style.fill_rgba = theme->surface_high;
    style.transition = NK_UI_TRANSITION_COLOR;
    style.transition_s = (float)NK_UI_MOTION_FAST;
    if (!nk_ui_begin_container(ui, id, &layout, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    text.wrap = true;
    nk_ui_text(ui, id, nk_ui_text_break(ui, nk_ui_caps(ui, label), &text,
                                       width - layout.padding_left - layout.padding_right), &text);
    nk_ui_end_container(ui);
    return nk_ui_take_activation(ui, id);
}

PlayerUiAction player_ui_screen_remove(NkUi *ui, const PlayerUiRemoveModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(REMOVE_MOVE)}, {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(REMOVE_SELECT)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(REMOVE_CANCEL)}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    char size[32];
    unsigned long long total = 0ull;
    size_t shown;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_TRIANGLE, NK_UI_STR(REMOVE_HEADING), theme->warning);
    (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_CONFIRM_FORMAT), safe_text(model->title, NK_UI_STR(REMOVE_GAME_FALLBACK)));
    nk_ui_text_role(ui, FLOW_ID_TITLE, nk_ui_frame_text(ui, text), NK_UI_TYPE_TITLE,
                    theme->paper);
    shown = model->items != NULL ? model->item_count : 0u;
    if (shown > PLAYER_UI_REMOVE_ITEM_CAP) shown = PLAYER_UI_REMOVE_ITEM_CAP;
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    model->files_shared
                        ? NK_UI_STR(REMOVE_SHARED_BUILD_NOTE)
                    : shown > 0u
                        ? NK_UI_STR(REMOVE_DELETE_BUILD_NOTE)
                        : NK_UI_STR(REMOVE_NO_BUILD_NOTE),
                    NK_UI_TYPE_BODY_S, theme->muted);
    if (shown > 0u) {
        NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = scr_flow_box_style(theme, theme->surface);
        rows.padding_top = rows.padding_bottom = 10.0f;
        rows.padding_left = rows.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
            for (size_t i = 0u; i < shown; ++i) {
                NkUiLayout line = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                total += model->items[i].bytes;
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_ITEMS + (NkUiId)i), &line,
                                          NULL)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x10u + (NkUiId)i),
                                    safe_text(model->items[i].label, NK_UI_STR(REMOVE_BUILT_FILES)),
                                    NK_UI_TYPE_BODY_S, theme->paper);
                    nk_ui_spacer(ui, (NkUiId)(FLOW_ID_ITEMS + 0x20u + (NkUiId)i),
                                 sizing_grow(), sizing_fixed(1.0f));
                    scr_flow_bytes(size, sizeof(size), model->items[i].bytes);
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x30u + (NkUiId)i),
                                    nk_ui_frame_text(ui, size), NK_UI_TYPE_DATA, theme->muted);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
    }
    /* What Remove frees: the built files listed, and the decrypted files and disc copy
       while their box is ticked. Saves are backed up before they go, so they free
       nothing. */
    if (model->delete_disc_copy && model->has_disc_copy && !model->files_shared) {
        total += model->disc_copy_bytes;
    }
    if (shown > 0u || total > 0ull) {
        scr_flow_bytes(size, sizeof(size), total);
        (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_FREED_SPACE_FORMAT), size);
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->warning);
    }
    nk_ui_text_role(ui, FLOW_ID_DETAILS,
                    FLOW_MARK_CHECK NK_UI_STR(REMOVE_DISC_UNTOUCHED),
                    NK_UI_TYPE_BODY_S, theme->ready);
    if (model->has_saves) {
        scr_flow_bytes(size, sizeof(size), model->saves_bytes);
        (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_INCLUDE_SAVES_FORMAT),
                       model->delete_saves ? FLOW_MARK_CHECK : "", size);
        if (remove_choice_button(ui, PLAYER_UI_ID_REMOVE_SAVES, text, inner)) {
            result = action(PLAYER_UI_ACTION_REMOVE_TOGGLE_SAVES, model->title_index,
                            PLAYER_UI_PRIMARY_NONE);
        }
        nk_ui_text_role(ui, FLOW_ID_MESSAGE,
                        model->delete_saves
                            ? NK_UI_STR(REMOVE_DELETE_SAVES_NOTE)
                            : NK_UI_STR(REMOVE_KEEP_SAVES_NOTE),
                        NK_UI_TYPE_BODY_S, model->delete_saves ? theme->error : theme->muted);
    } else if (!model->files_shared) {
        nk_ui_text_role(ui, FLOW_ID_MESSAGE, NK_UI_STR(REMOVE_NO_SAVES_NOTE), NK_UI_TYPE_BODY_S,
                        theme->muted);
    }
    if (model->has_disc_copy && !model->files_shared) {
        scr_flow_bytes(size, sizeof(size), model->disc_copy_bytes);
        (void)snprintf(text, sizeof(text),
                       NK_UI_STR(REMOVE_INCLUDE_DISC_COPY_FORMAT),
                       model->delete_disc_copy ? FLOW_MARK_CHECK : "", size);
        if (remove_choice_button(ui, PLAYER_UI_ID_REMOVE_DISC_COPY, text, inner)) {
            result = action(PLAYER_UI_ACTION_REMOVE_TOGGLE_DISC_COPY, model->title_index,
                            PLAYER_UI_PRIMARY_NONE);
        }
        nk_ui_text_role(ui, FLOW_ID_NOTICE,
                        model->delete_disc_copy
                            ? NK_UI_STR(REMOVE_DELETE_DISC_COPY_NOTE)
                            : NK_UI_STR(REMOVE_KEEP_DISC_COPY_NOTE),
                        NK_UI_TYPE_BODY_S,
                        model->delete_disc_copy ? theme->error : theme->muted);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_CANCEL, NK_UI_STR(REMOVE_CANCEL), 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_CONFIRM, NK_UI_STR(REMOVE_CONFIRM), NK_UI_BUTTON_DANGER)) {
                unsigned also =
                    (model->delete_saves ? PLAYER_UI_REMOVE_ALSO_SAVES : 0u) |
                    (model->delete_disc_copy && model->has_disc_copy && !model->files_shared
                         ? PLAYER_UI_REMOVE_ALSO_DISC_COPY
                         : 0u);
                result = action(PLAYER_UI_ACTION_REMOVE_CONFIRM, model->title_index,
                                (PlayerUiPrimaryAction)also);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}

/* Saves (#1883): what the title's saves are, Back up saves, Add a save and Open
   save folder, the backups a page at a time with a Restore on each, and the
   question Restore and a replacing Add ask first, which holds the card until it
   is answered (Cancel holds its focus). A game that runs keeps every change off. */
PlayerUiAction player_ui_screen_saves(NkUi *ui, const PlayerUiSavesModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(REMOVE_MOVE)}, {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(REMOVE_SELECT)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(REMOVE_BACK)}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    char size[32];
    size_t shown;
    size_t reach;
    bool blocked;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    blocked = model->blocked != NULL && model->blocked[0] != '\0';
    shown = model->rows != NULL ? model->row_count : 0u;
    if (shown > PLAYER_UI_SAVES_ROW_CAP) shown = PLAYER_UI_SAVES_ROW_CAP;
    /* The pages reach only the backups listed: Older stops at the last of them. */
    reach = model->listed != 0u && model->listed < model->total ? model->listed : model->total;
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                           model->logical_height > 80.0f ? model->logical_height : 800.0f,
                           640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, NK_UI_STR(REMOVE_SAVES_HEADING), theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, safe_text(model->title, NK_UI_STR(REMOVE_SAVES_GAME_FALLBACK)), NK_UI_TYPE_TITLE,
                    theme->paper);
    if (model->has_saves) {
        scr_flow_bytes(size, sizeof(size), model->saves_bytes);
        (void)snprintf(text, sizeof(text),
                       NK_UI_STR(REMOVE_SAVES_USAGE_FORMAT),
                       size);
    } else {
        (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_EMPTY_FORMAT),
                       NK_UI_STR(REMOVE_SAVES_EMPTY));
    }
    nk_ui_text_role(ui, FLOW_ID_LEAD, nk_ui_frame_text(ui, text), NK_UI_TYPE_BODY_S,
                    theme->muted);
    if (blocked) {
        nk_ui_text_role(ui, FLOW_ID_NOTICE, model->blocked, NK_UI_TYPE_BODY_S, theme->warning);
    }
    if (model->ask != PLAYER_UI_SAVES_ASK_NONE && !blocked) {
        bool restore = model->ask == PLAYER_UI_SAVES_ASK_RESTORE;
        const char *when = restore && model->ask_row < shown
            ? safe_text(model->rows[model->ask_row].when, NK_UI_STR(REMOVE_SAVES_BACKUP_FALLBACK)) : NK_UI_STR(REMOVE_SAVES_BACKUP_FALLBACK);
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        if (restore) {
            (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_RESTORE_FORMAT), when);
        } else {
            (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_REPLACE_FORMAT),
                           safe_text(model->ask_name, NK_UI_STR(REMOVE_SAVES_REPLACE_FALLBACK)));
        }
        nk_ui_text_role(ui, FLOW_ID_MESSAGE, nk_ui_frame_text(ui, text), NK_UI_TYPE_BODY,
                        theme->paper);
        nk_ui_text_role(ui, FLOW_ID_DETAILS,
                        restore ? NK_UI_STR(REMOVE_SAVES_RESTORE_NOTE)
                                : NK_UI_STR(REMOVE_SAVES_REPLACE_NOTE),
                        NK_UI_TYPE_BODY_S, theme->muted);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_CANCEL, NK_UI_STR(REMOVE_CANCEL), 0u)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_CANCEL);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_CONFIRM, restore ? NK_UI_STR(REMOVE_SAVES_RESTORE) : NK_UI_STR(REMOVE_SAVES_REPLACE),
                             restore ? NK_UI_BUTTON_PRIMARY : NK_UI_BUTTON_DANGER)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_CONFIRM);
            }
            nk_ui_end_container(ui);
        }
        /* Back (B or Esc) answers the question with Cancel and keeps the card:
           player_ui_nk.c gives Back to the screen while a question is up. */
        if (result.kind == PLAYER_UI_ACTION_NONE && nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
            result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                            (PlayerUiPrimaryAction)PLAYER_UI_SAVES_CANCEL);
        }
        scr_flow_end(ui, "", prompts, 3u);
        return result;
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        NkUiFlowRow flow;
        uint32_t change = blocked ? NK_UI_BUTTON_DISABLED : 0u;
        uint32_t back_up = model->has_saves && !blocked ? NK_UI_BUTTON_PRIMARY
                                                        : NK_UI_BUTTON_DISABLED;
        if (nk_ui_flow_begin(ui, &flow, FLOW_ID_ACTIONS, &row, inner)) {
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(REMOVE_SAVES_BACK_UP), back_up));
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_BACK_UP, NK_UI_STR(REMOVE_SAVES_BACK_UP), back_up)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_BACK_UP);
            }
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(REMOVE_SAVES_ADD), change));
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_ADD, NK_UI_STR(REMOVE_SAVES_ADD), change)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_ADD);
            }
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(REMOVE_SAVES_OPEN_FOLDER), 0u));
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_OPEN_FOLDER, NK_UI_STR(REMOVE_SAVES_OPEN_FOLDER), 0u)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_OPEN_FOLDER);
            }
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(REMOVE_BACK), 0u));
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_BACK, NK_UI_STR(REMOVE_BACK), 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_flow_end(ui, &flow);
        }
    }
    if (model->status != NULL && model->status[0] != '\0') {
        nk_ui_text_role(ui, FLOW_ID_STATUS, model->status, NK_UI_TYPE_BODY_S,
                        note_rgba(theme, model->status_kind));
    }
    if (model->total == 0u) {
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, NK_UI_STR(REMOVE_SAVES_NO_BACKUPS)),
                        NK_UI_TYPE_LABEL_S, theme->muted);
    } else {
        if (shown > 0u) {
            (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_BACKUPS_PAGE_FORMAT),
                           (unsigned long)(model->first_row + 1u),
                           (unsigned long)(model->first_row + shown),
                           (unsigned long)model->total);
            if (reach < model->total) {
                size_t used = strlen(text);
                (void)snprintf(text + used, sizeof(text) - used, NK_UI_STR(REMOVE_SAVES_BACKUPS_LIMIT_FORMAT),
                               (unsigned long)reach);
            }
        } else {
            (void)snprintf(text, sizeof(text), NK_UI_STR(REMOVE_SAVES_BACKUPS_COUNT_FORMAT), (unsigned long)model->total);
        }
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->muted);
    }
    if (shown > 0u) {
        NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = scr_flow_box_style(theme, theme->surface);
        rows.padding_top = rows.padding_bottom = 8.0f;
        rows.padding_left = rows.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
            for (size_t i = 0u; i < shown; ++i) {
                NkUiLayout line = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                line.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_ITEMS + (NkUiId)i), &line,
                                          NULL)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x10u + (NkUiId)i),
                                    safe_text(model->rows[i].when, NK_UI_STR(REMOVE_SAVES_BACKUP_ROW_FALLBACK)),
                                    NK_UI_TYPE_BODY_S, theme->paper);
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x20u + (NkUiId)i),
                                    safe_text(model->rows[i].detail, ""), NK_UI_TYPE_DATA,
                                    theme->muted);
                    nk_ui_spacer(ui, (NkUiId)(FLOW_ID_ITEMS + 0x30u + (NkUiId)i),
                                 sizing_grow(), sizing_fixed(1.0f));
                    if (nk_ui_button(ui, (NkUiId)(PLAYER_UI_ID_SAVES_RESTORE_BASE + (NkUiId)i),
                                     NK_UI_STR(REMOVE_SAVES_RESTORE), NK_UI_BUTTON_SMALL | NK_UI_BUTTON_NOT_DEFAULT |
                                                    (blocked ? NK_UI_BUTTON_DISABLED : 0u))) {
                        result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                        (PlayerUiPrimaryAction)(PLAYER_UI_SAVES_RESTORE_BASE +
                                                                (int)i));
                    }
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
    }
    if (model->first_row > 0u || model->first_row + shown < reach) {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_REPORT_ROW, &row, NULL)) {
            if (model->first_row > 0u &&
                nk_ui_button(ui, PLAYER_UI_ID_SAVES_NEWER, NK_UI_STR(REMOVE_SAVES_NEWER),
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_NOT_DEFAULT)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_NEWER);
            }
            if (model->first_row + shown < reach &&
                nk_ui_button(ui, PLAYER_UI_ID_SAVES_OLDER, NK_UI_STR(REMOVE_SAVES_OLDER),
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_NOT_DEFAULT)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_OLDER);
            }
            nk_ui_end_container(ui);
        }
    }
    /* A Back given to the screen after its question closed (a game started)
       still closes the card, as the Back button does. */
    if (result.kind == PLAYER_UI_ACTION_NONE && nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        result = action(PLAYER_UI_ACTION_BACK, model->title_index, PLAYER_UI_PRIMARY_NONE);
    }
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}

/* Add to Steam (#2152): what the shortcut starts and what Nakagawa changes, the account when
   Steam has several, then Add to Steam and Back, and what the last add did. Add to Steam
   holds focus first: it only adds, after a backup. */
PlayerUiAction player_ui_screen_steam(NkUi *ui, const PlayerUiSteamModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(REMOVE_MOVE)}, {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(REMOVE_SELECT)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(REMOVE_BACK)}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    bool blocked;
    size_t accounts;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    blocked = model->blocked != NULL && model->blocked[0] != '\0';
    accounts = model->accounts != NULL ? model->account_count : 0u;
    if (accounts > PLAYER_UI_STEAM_ACCOUNT_CAP) accounts = PLAYER_UI_STEAM_ACCOUNT_CAP;
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                           model->logical_height > 80.0f ? model->logical_height : 800.0f,
                           640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, NK_UI_STR(REMOVE_STEAM_ADD), theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, safe_text(model->title, NK_UI_STR(REMOVE_SAVES_GAME_FALLBACK)), NK_UI_TYPE_TITLE,
                    theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    NK_UI_STR(REMOVE_STEAM_ADD_NOTE),
                    NK_UI_TYPE_BODY_S, theme->muted);
    if (blocked) {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        nk_ui_text_role(ui, FLOW_ID_NOTICE, model->blocked, NK_UI_TYPE_BODY_S, theme->warning);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_STEAM_BACK, NK_UI_STR(REMOVE_BACK), 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
        scr_flow_end(ui, "", prompts, 3u);
        return result;
    }
    {
        /* What the shortcut starts, and the account it goes to. */
        NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = scr_flow_box_style(theme, theme->surface);
        rows.padding_top = rows.padding_bottom = 8.0f;
        rows.padding_left = rows.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
            nk_ui_text_role(ui, FLOW_ID_ITEMS, nk_ui_caps(ui, NK_UI_STR(REMOVE_STEAM_STARTS)), NK_UI_TYPE_LABEL_S,
                            theme->muted);
            nk_ui_text_role(ui, FLOW_ID_ITEMS + 1u, safe_text(model->command, ""),
                            NK_UI_TYPE_DATA, theme->paper);
            if (accounts == 1u) {
                nk_ui_text_role(ui, FLOW_ID_ITEMS + 2u, nk_ui_caps(ui, NK_UI_STR(REMOVE_STEAM_ACCOUNT)),
                                NK_UI_TYPE_LABEL_S, theme->muted);
                nk_ui_text_role(ui, FLOW_ID_ITEMS + 3u, safe_text(model->accounts[0], ""),
                                NK_UI_TYPE_BODY_S, theme->paper);
            }
            nk_ui_end_container(ui);
        }
    }
    if (accounts > 1u) {
        /* Several accounts have signed in on this PC: the user picks one. The chosen one
           says Chosen, so colour is not the only sign. */
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        NkUiFlowRow flow;
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, NK_UI_STR(REMOVE_STEAM_CHOOSE_ACCOUNT)),
                        NK_UI_TYPE_LABEL_S, theme->muted);
        if (nk_ui_flow_begin(ui, &flow, FLOW_ID_REPORT_ROW, &row, inner)) {
            for (size_t i = 0u; i < accounts; ++i) {
                char label[96];
                uint32_t flags = NK_UI_BUTTON_SMALL | NK_UI_BUTTON_NOT_DEFAULT |
                                 (i == model->chosen ? NK_UI_BUTTON_PRIMARY : 0u);
                (void)snprintf(label, sizeof(label), NK_UI_STR(REMOVE_STEAM_ACCOUNT_LABEL_FORMAT),
                               i == model->chosen ? NK_UI_STR(REMOVE_STEAM_CHOSEN_PREFIX) : "",
                               safe_text(model->accounts[i], NK_UI_STR(REMOVE_STEAM_ACCOUNT)));
                nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, label, flags));
                if (nk_ui_button(ui, (NkUiId)(PLAYER_UI_ID_STEAM_ACCOUNT_BASE + (NkUiId)i),
                                 nk_ui_frame_text(ui, label), flags)) {
                    result = action(PLAYER_UI_ACTION_STEAM, model->title_index,
                                    (PlayerUiPrimaryAction)(PLAYER_UI_STEAM_ACCOUNT_BASE +
                                                            (int)i));
                }
            }
            nk_ui_flow_end(ui, &flow);
        }
    }
    nk_ui_text_role(ui, FLOW_ID_DETAILS,
                    model->portable
                        ? NK_UI_STR(REMOVE_STEAM_PORTABLE_NOTE)
                        : NK_UI_STR(REMOVE_STEAM_EXIT_NOTE),
                    NK_UI_TYPE_BODY_S, theme->muted);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        NkUiFlowRow flow;
        if (nk_ui_flow_begin(ui, &flow, FLOW_ID_ACTIONS, &row, inner)) {
            nk_ui_flow_item(ui, &flow,
                            nk_ui_button_width(ui, NK_UI_STR(REMOVE_STEAM_ADD), NK_UI_BUTTON_PRIMARY));
            if (nk_ui_button(ui, PLAYER_UI_ID_STEAM_ADD, NK_UI_STR(REMOVE_STEAM_ADD),
                             NK_UI_BUTTON_PRIMARY)) {
                result = action(PLAYER_UI_ACTION_STEAM, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_STEAM_ADD);
            }
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(REMOVE_BACK), 0u));
            if (nk_ui_button(ui, PLAYER_UI_ID_STEAM_BACK, NK_UI_STR(REMOVE_BACK), 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_flow_end(ui, &flow);
        }
    }
    if (model->status != NULL && model->status[0] != '\0') {
        nk_ui_text_role(ui, FLOW_ID_STATUS, model->status, NK_UI_TYPE_BODY_S,
                        note_rgba(theme, model->status_kind));
    }
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}
