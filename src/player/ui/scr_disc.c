/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Game found: the card a disc gets when it is added from the library (#1302): the game's
   name, one sentence on what Nakagawa can do with it, every check in plain words, and Add
   to library when the disc can be added. Only a check that keeps the disc out of the
   library reads as an error. */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#define DISC_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_DISC, (offset))
#define DISC_ID_CHIPS DISC_ID(0x000001u)
#define DISC_ID_CHIPS_SPAN 0x8u
#define DISC_ID_BOX DISC_ID(0x000010u)
#define DISC_ID_ROWS DISC_ID(0x000100u)
#define DISC_ID_ROWS_SPAN 0x200u
#define DISC_ID_DETAILS DISC_ID(0x000020u)
#define DISC_ID_DETAILS_SPAN 0x8u
#define DISC_ID_TEXT DISC_ID(0x001000u)
#define DISC_ID_TEXT_SPAN 0x1000u
#define DISC_ID_GUIDE DISC_ID(0x000300u)

static PlayerUiAction disc_op(PlayerUiDiscOp op) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_DISC;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = 0u;
    return result;
}

static void disc_text(NkUi *ui, uint32_t k, const char *text, NkUiTypeRole role, uint32_t rgba) {
    if (text != NULL && text[0] != '\0') {
        nk_ui_text_role(ui, (NkUiId)(DISC_ID_TEXT + k), text, role, rgba);
    }
}

/* The check rows: the status word in its colour, what was checked, and the
   sentence, two lines of it when the window has the room. */
static void disc_rows(NkUi *ui, const PlayerUiDiscModel *model, float inner, int lines) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
    NkUiStyle style = scr_flow_box_style(theme, theme->surface);
    size_t rows = model->check_count < PLAYER_UI_DISC_CHECKS ? model->check_count
                                                              : PLAYER_UI_DISC_CHECKS;
    float status_w = scr_check_column_width(ui, model->checks, rows, true, NK_UI_TYPE_LABEL,
                                        NK_UI_STR(DISC_CHECKED), 132.0f);
    float name_w = scr_check_column_width(ui, model->checks, rows, false, NK_UI_TYPE_BODY_S, NK_UI_STR(DISC_CHECK),
                                      124.0f);
    float message = inner - 32.0f - status_w - name_w - 2.0f * NK_UI_SPACE_3;
    box.padding_top = box.padding_bottom = lines > 1 ? 12.0f : 8.0f;
    box.padding_left = box.padding_right = 16.0f;
    if (lines < 2) box.gap = 2.0f;
    if (model->check_count == 0u || !nk_ui_begin_container(ui, DISC_ID_BOX, &box, &style)) return;
    for (size_t i = 0u; i < rows; ++i) {
        const PlayerUiCheckRow *check = &model->checks[i];
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
        NkUiId base = (NkUiId)(DISC_ID_ROWS + (NkUiId)i * 8u);
        if (nk_ui_begin_container(ui, base, &row, NULL)) {
            NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(status_w), sizing_fit(),
                                          0.0f);
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
            cell.padding_top = 2.0f;
            if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
                nk_ui_text_role(ui, base + 4u, nk_ui_caps(ui, safe_text(check->status, NK_UI_STR(DISC_CHECKED))),
                                NK_UI_TYPE_LABEL, check->rgba);
                nk_ui_end_container(ui);
            }
            cell.width = sizing_fixed(name_w);
            cell.padding_top = 0.0f;
            if (nk_ui_begin_container(ui, base + 2u, &cell, NULL)) {
                nk_ui_text_role(ui, base + 5u, safe_text(check->name, NK_UI_STR(DISC_CHECK)), NK_UI_TYPE_BODY_S,
                                theme->paper);
                nk_ui_end_container(ui);
            }
            text.rgba = theme->muted;
            nk_ui_text(ui, base + 3u,
                       nk_ui_text_clamp(ui, safe_text(check->message, ""), &text, message, lines),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

PlayerUiAction player_ui_screen_disc(NkUi *ui, const PlayerUiDiscModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(DISC_MOVE)}, {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(DISC_SELECT)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(DISC_BACK_TO_LIBRARY)}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    const char *marker = FLOW_MARK_DOT;
    const char *badge = NK_UI_STR(DISC_GAME_FOUND);
    const char *lead = NULL;
    uint32_t tone;
    float inner;
    float height;
    bool guided;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    guided = model->kind == PLAYER_UI_DISC_BLOCKED && scr_input_guide_present(&model->input_guide);
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       height, 820.0f);
    if (inner <= 0.0f) return result;
    tone = theme->ready;
    switch (model->kind) {
        case PLAYER_UI_DISC_CHECKING:
            marker = FLOW_MARK_DIAMOND;
            badge = NK_UI_STR(DISC_CHECKING_BADGE);
            tone = theme->accent_dark;
            lead = NK_UI_STR(DISC_CHECKING_DETAILS);
            break;
        case PLAYER_UI_DISC_SUPPORTED:
            lead = NK_UI_STR(DISC_SUPPORTED_DETAILS);
            break;
        case PLAYER_UI_DISC_UNTESTED:
            marker = "?";
            badge = NK_UI_STR(DISC_UNTESTED_BADGE);
            tone = theme->info;
            lead = NK_UI_STR(DISC_UNTESTED_DETAILS);
            break;
        case PLAYER_UI_DISC_BLOCKED:
            marker = guided ? FLOW_MARK_TRIANGLE : FLOW_MARK_CROSS;
            badge = guided ? NK_UI_STR(DISC_NEEDS_FILES_BADGE) : NK_UI_STR(DISC_BLOCKED_BADGE);
            tone = guided ? theme->warning : theme->error;
            lead = NK_UI_STR(DISC_BLOCKED_DETAILS);
            break;
        case PLAYER_UI_DISC_UNSUPPORTED:
        default:
            marker = FLOW_MARK_CROSS;
            badge = NK_UI_STR(DISC_UNSUPPORTED_BADGE);
            tone = theme->error;
            lead = model->profile_details != NULL && model->profile_details[0] != '\0'
                ? NK_UI_STR(DISC_INVALID_PROFILE_DETAILS)
                : NK_UI_STR(DISC_UNSUPPORTED_DETAILS);
            break;
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_2);
        if (nk_ui_begin_container(ui, DISC_ID_CHIPS, &row, NULL)) {
            nk_ui_status_pill(ui, DISC_ID_CHIPS + 1u, marker, badge, tone);
            if (model->disc_id != NULL && model->disc_id[0] != '\0') {
                nk_ui_tag(ui, DISC_ID_CHIPS + 2u, model->disc_id);
            }
            if (model->kind != PLAYER_UI_DISC_CHECKING) {
                nk_ui_tag(ui, DISC_ID_CHIPS + 3u, safe_text(model->badge, NK_UI_STR(DISC_YOUR_DISC)));
            }
            nk_ui_end_container(ui);
        }
    }
    if (model->kind == PLAYER_UI_DISC_CHECKING) {
        disc_text(ui, 0u, NK_UI_STR(DISC_CHECKING_HEADING), NK_UI_TYPE_TITLE, theme->paper);
    } else {
        disc_text(ui, 0u, safe_text(model->title_name, NK_UI_STR(DISC_GAME_FALLBACK)), NK_UI_TYPE_TITLE,
                  theme->paper);
    }
    if (guided) {
        /* Encrypted program files the user can supply (#308): the next step,
           not a dead end. */
        lead = NK_UI_STR(DISC_NEEDS_FILES_DETAILS);
    }
    disc_text(ui, 1u, lead, NK_UI_TYPE_BODY, model->kind == PLAYER_UI_DISC_BLOCKED && !guided
                                                 ? theme->error : theme->muted);
    if (guided) {
        scr_input_guide_box(ui, DISC_ID_GUIDE, &model->input_guide, inner,
                        height >= 680.0f ? PLAYER_UI_INPUT_FILES : 2u);
    } else if (model->kind == PLAYER_UI_DISC_BLOCKED && model->reason != NULL) {
        /* The reason names the folder a file goes in, so it is kept whole. */
        NkUiTextStyle reason = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
        nk_ui_text(ui, DISC_ID_TEXT + 2u,
                   nk_ui_text_clamp(ui, model->reason, &reason, inner, 5), &reason);
    }
    disc_text(ui, 3u, model->profile_note, NK_UI_TYPE_BODY_S, theme->warning);
    {
        if (model->profile_details != NULL && model->profile_details[0] != '\0') {
            NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
            NkUiStyle style = scr_flow_box_style(theme, theme->surface);
            box.padding_top = box.padding_bottom = 10.0f;
            box.padding_left = box.padding_right = 14.0f;
            if (nk_ui_begin_container(ui, DISC_ID_DETAILS, &box, &style)) {
                NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
                data.wrap = true;
                nk_ui_eyebrow(ui, DISC_ID_DETAILS + 1u, NK_UI_STR(DISC_DETAILS_HEADING));
                nk_ui_text(ui, DISC_ID_DETAILS + 2u,
                           nk_ui_text_clamp(ui, model->profile_details, &data, inner - 28.0f, 2),
                           &data);
                nk_ui_end_container(ui);
            }
        }
    }
    if (model->kind == PLAYER_UI_DISC_CHECKING) {
        NkUiTextStyle path = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
        if (model->iso_path != NULL && model->iso_path[0] != '\0') {
            nk_ui_text(ui, DISC_ID_TEXT + 4u,
                       nk_ui_text_clamp(ui, model->iso_path, &path, inner, 2), &path);
        }
        nk_ui_progress_bar(ui, DISC_ID_TEXT + 5u, inner, -1.0f, model->now_s);
    } else if (!guided && (model->kind != PLAYER_UI_DISC_BLOCKED || height >= 680.0f)) {
        /* A guided card gives the room to the files it needs; the other checks
           come back once Check again lets the disc in. */
        /* Two lines of each sentence from a 680 px tall window; one below it,
           so the card still fits the 540 px minimum. A disc that can't be added
           gives the room to its reason there. */
        disc_rows(ui, model, inner, height >= 680.0f ? 2 : 1);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, DISC_ID_TEXT + 0x40u, &row, NULL)) {
            switch (model->kind) {
                case PLAYER_UI_DISC_CHECKING:
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, NK_UI_STR(DISC_CANCEL), 0u)) {
                        result = disc_op(PLAYER_UI_DISC_BACK);
                    }
                    break;
                case PLAYER_UI_DISC_SUPPORTED:
                case PLAYER_UI_DISC_UNTESTED:
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, NK_UI_STR(DISC_ADD_TO_LIBRARY),
                                     NK_UI_BUTTON_PRIMARY)) {
                        result = disc_op(PLAYER_UI_DISC_ADD);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_SECOND, NK_UI_STR(DISC_BACK), 0u)) {
                        result = disc_op(PLAYER_UI_DISC_BACK);
                    }
                    break;
                case PLAYER_UI_DISC_BLOCKED:
                    /* Adding it would only fail later, at build time (#1280).
                       Files the user can supply get Check again, which reads
                       the same disc again, and the folder they go in (#308). */
                    if (guided) {
                        if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, NK_UI_STR(DISC_CHECK_AGAIN),
                                         NK_UI_BUTTON_PRIMARY)) {
                            result = disc_op(PLAYER_UI_DISC_CHECK_AGAIN);
                        }
                        if (model->input_guide.folder != NULL &&
                            model->input_guide.folder[0] != '\0' &&
                            nk_ui_button(ui, PLAYER_UI_ID_DISC_OPEN_FOLDER, NK_UI_STR(DISC_OPEN_FOLDER), 0u)) {
                            result = disc_op(PLAYER_UI_DISC_OPEN_FOLDER);
                        }
                    } else if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, NK_UI_STR(DISC_CHOOSE_ANOTHER),
                                            NK_UI_BUTTON_PRIMARY)) {
                        result = disc_op(PLAYER_UI_DISC_CHOOSE_ANOTHER);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_SECOND, NK_UI_STR(DISC_BACK), 0u)) {
                        result = disc_op(PLAYER_UI_DISC_BACK);
                    }
                    break;
                case PLAYER_UI_DISC_UNSUPPORTED:
                default:
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, NK_UI_STR(DISC_BACK_TO_LIBRARY),
                                     NK_UI_BUTTON_PRIMARY)) {
                        result = disc_op(PLAYER_UI_DISC_BACK);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_SECOND, NK_UI_STR(DISC_CHOOSE_ANOTHER), 0u)) {
                        result = disc_op(PLAYER_UI_DISC_CHOOSE_ANOTHER);
                    }
                    break;
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, model->kind == PLAYER_UI_DISC_SUPPORTED || model->kind == PLAYER_UI_DISC_UNTESTED
                     ? NK_UI_STR(DISC_PRIVACY_NOTE)
                     : "",
             prompts, 3u);
    return result;
}
