/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The one-time build tool download: consent, then progress. */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

PlayerUiAction player_ui_screen_prereq(NkUi *ui, const PlayerUiPrereqModel *model) {
    static const NkUiPromptItem prompts_consent[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(PREREQ_MOVE)}, {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(PREREQ_SELECT)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(PREREQ_CANCEL)}
    };
    static const NkUiPromptItem prompts_progress[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(PREREQ_SELECT)}, {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(PREREQ_CANCEL)}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    char a[32];
    char b[32];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 820.0f);
    if (inner <= 0.0f) return result;
    if (!model->in_progress) {
        size_t shown = model->item_count;
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, NK_UI_STR(PREREQ_CONSENT_EYEBROW), theme->info);
        nk_ui_text_role(ui, FLOW_ID_TITLE,
                        NK_UI_STR(PREREQ_CONSENT_HEADING),
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        NK_UI_STR(PREREQ_CONSENT_NOTE),
                        NK_UI_TYPE_BODY_S, theme->muted);
        {
            NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 0.0f);
            NkUiStyle style = scr_flow_box_style(theme, theme->surface);
            rows.padding_top = rows.padding_bottom = 8.0f;
            rows.padding_left = rows.padding_right = 14.0f;
            if (shown > 9u) shown = 9u;
            if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
                NkUiTextStyle line = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
                line.line_height = 20.0f;
                for (size_t i = 0u; i < shown; ++i) {
                    const PlayerUiPrereqItem *item = &model->items[i];
                    const char *license = item->license != NULL &&
                        strcmp(item->license, "NOASSERTION") == 0
                        ? NK_UI_STR(PREREQ_LICENCE_UNREVIEWED) : safe_text(item->license, "");
                    scr_flow_bytes(a, sizeof(a), item->bytes);
                    (void)snprintf(text, sizeof(text), NK_UI_STR(PREREQ_COMPONENT_FORMAT),
                                   safe_text(item->name, "?"), safe_text(item->version, ""),
                                   safe_text(item->host, ""), a, license);
                    nk_ui_text(ui, (NkUiId)(FLOW_ID_ROWS + 1u + (NkUiId)i),
                               nk_ui_text_clamp(ui, text, &line, inner - 28.0f, 1), &line);
                }
                if (model->item_count > shown) {
                    (void)snprintf(text, sizeof(text), NK_UI_STR(PREREQ_MORE_COMPONENTS_FORMAT), model->item_count - shown);
                    nk_ui_text(ui, FLOW_ID_ROWS_MORE, text, &line);
                }
                nk_ui_end_container(ui);
            }
        }
        scr_flow_bytes(a, sizeof(a), model->total_bytes);
        (void)snprintf(text, sizeof(text), NK_UI_STR(PREREQ_TOTAL_FORMAT), a, model->item_count);
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->warning);
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_ACCEPT, NK_UI_STR(PREREQ_DOWNLOAD),
                                 NK_UI_BUTTON_PRIMARY)) {
                    result = action(PLAYER_UI_ACTION_PREREQ_ACCEPT, 0u, PLAYER_UI_PRIMARY_NONE);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_CANCEL, NK_UI_STR(PREREQ_CANCEL), 0u)) {
                    result = action(PLAYER_UI_ACTION_PREREQ_CANCEL, 0u, PLAYER_UI_PRIMARY_NONE);
                }
                nk_ui_end_container(ui);
            }
        }
        scr_flow_end(ui, "", prompts_consent, 3u);
        return result;
    }
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, NK_UI_STR(PREREQ_INSTALLING_HEADING), theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, NK_UI_STR(PREREQ_INSTALLING_NOTE),
                    NK_UI_TYPE_TITLE, theme->paper);
    (void)snprintf(text, sizeof(text), NK_UI_STR(PREREQ_CURRENT_ITEM_FORMAT),
                   safe_text(model->current_item, NK_UI_STR(PREREQ_STARTING_DOWNLOAD)));
    nk_ui_text_role(ui, FLOW_ID_LEAD, text, NK_UI_TYPE_BODY_S, theme->paper);
    nk_ui_progress_bar(ui, FLOW_ID_BAR, inner,
                       model->total_bytes > 0ull
                           ? (float)((double)model->total_received_bytes /
                                     (double)model->total_bytes) : -1.0f, 0.0);
    scr_flow_bytes(a, sizeof(a), model->total_received_bytes);
    scr_flow_bytes(b, sizeof(b), model->total_bytes);
    (void)snprintf(text, sizeof(text), NK_UI_STR(PREREQ_PROGRESS_TOTAL_FORMAT), a, b);
    nk_ui_text_role(ui, FLOW_ID_TOTAL, text, NK_UI_TYPE_DATA, theme->muted);
    scr_flow_bytes(a, sizeof(a), model->item_received_bytes);
    scr_flow_bytes(b, sizeof(b), model->item_total_bytes);
    (void)snprintf(text, sizeof(text), NK_UI_STR(PREREQ_PROGRESS_ITEM_FORMAT), a, b);
    nk_ui_text_role(ui, FLOW_ID_DETAILS, text, NK_UI_TYPE_DATA, theme->dim);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_CANCEL, NK_UI_STR(PREREQ_CANCEL), 0u)) {
                result = action(PLAYER_UI_ACTION_PREREQ_CANCEL, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, NK_UI_STR(PREREQ_INSTALLING_FOOTER), prompts_progress, 2u);
    return result;
}
