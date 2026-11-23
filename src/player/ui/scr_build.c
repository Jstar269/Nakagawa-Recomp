/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Building the package: the live stages, the elapsed time, the log and the
   outcome, in one centred dialog card; and the build queue's card. */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

PlayerUiAction player_ui_screen_build(NkUi *ui, const PlayerUiBuildModel *model) {
    static const char *const stages[4] = {NK_UI_STR(BUILD_PREFLIGHT), NK_UI_STR(BUILD_EXTRACT), NK_UI_STR(BUILD_COMPILE), NK_UI_STR(BUILD_PACKAGE)};
    static const NkUiPromptItem prompts_running[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(BUILD_SELECT)}, {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(BUILD_CANCEL_BUILD)}
    };
    static const NkUiPromptItem prompts_done[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(BUILD_SELECT)}, {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(BUILD_BACK)}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char elapsed[48];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 720.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, model->built ? FLOW_MARK_DOT : FLOW_MARK_DIAMOND,
                 model->built ? NK_UI_STR(BUILD_PACKAGE_BUILT) : NK_UI_STR(BUILD_BUILDING_PACKAGE),
                 model->built ? theme->ready : theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, safe_text(model->title_line, NK_UI_STR(BUILD_PACKAGE_BUILD)),
                    NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                     NK_UI_SPACE_2);
        if (nk_ui_begin_container(ui, FLOW_ID_STAGES, &row, NULL)) {
            for (int i = 0; i < 4; ++i) {
                bool done = model->built || model->stage > i;
                bool current = !model->built && model->stage == i;
                NkUiLayout chip = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(),
                                              sizing_fixed(NK_UI_CHIP_HEIGHT + 4.0f), 6.0f);
                NkUiStyle style;
                char label[48];
                memset(&style, 0, sizeof(style));
                style.corner_radius = NK_UI_RADIUS_CHIP;
                style.border_width = NK_UI_BORDER_HAIRLINE;
                style.fill_rgba = current ? theme->accent_dark : theme->surface_high;
                style.border_rgba = current ? theme->accent_dark
                                            : done ? theme->ready : theme->line_strong;
                chip.align_x = NK_UI_ALIGN_CENTER;
                chip.align_y = NK_UI_ALIGN_CENTER;
                (void)snprintf(label, sizeof(label), NK_UI_STR(BUILD_STAGE_LABEL_FORMAT), done ? FLOW_MARK_CHECK : "",
                               stages[i]);
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_STAGES + 1u + (NkUiId)i),
                                          &chip, &style)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_STAGE_LABELS + (NkUiId)i),
                                    nk_ui_caps(ui, label), NK_UI_TYPE_LABEL_S,
                                    current ? theme->ink : done ? theme->ready : theme->muted);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
    }
    /* Determinate while the compile stage counts its parts; a sweep otherwise. */
    nk_ui_progress_bar(ui, FLOW_ID_BAR, inner, model->built ? 1.0f : model->fraction,
                       model->now_s);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                     NK_UI_SPACE_4);
        row.align_y = NK_UI_ALIGN_CENTER;
        if (nk_ui_begin_container(ui, FLOW_ID_STATUS, &row, NULL)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text(ui, FLOW_ID_MESSAGE,
                       nk_ui_text_clamp(ui, model->built
                           ? NK_UI_STR(BUILD_CHECKING_PACKAGE)
                           : safe_text(model->message, NK_UI_STR(BUILD_PROGRESS_FALLBACK)),
                           &text, inner - 140.0f, 2), &text);
            nk_ui_spacer(ui, FLOW_ID_STATUS_SPACER, sizing_grow(), sizing_fixed(1.0f));
            (void)snprintf(elapsed, sizeof(elapsed), NK_UI_STR(BUILD_ELAPSED_SECONDS_FORMAT),
                           model->elapsed_ms / 1000u, (model->elapsed_ms % 1000u) / 100u);
            nk_ui_text_role(ui, FLOW_ID_ELAPSED, elapsed, NK_UI_TYPE_DATA, theme->muted);
            nk_ui_end_container(ui);
        }
    }
    if (!model->built && model->speed_line != NULL && model->speed_line[0] != '\0') {
        /* The build speed from Settings, the job count it gave and what bound it: data,
           in the muted colour, so it reads as a fact about the build. */
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
        nk_ui_text(ui, FLOW_ID_SPEED, nk_ui_text_clamp(ui, model->speed_line, &text, inner, 2),
                   &text);
    }
    if (!model->built && model->notice != NULL && model->notice[0] != '\0') {
        /* A calm note that other apps may feel slower: muted body text, no warning
           colour or mark, because nothing is wrong. */
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
        nk_ui_text(ui, FLOW_ID_NOTICE, nk_ui_text_clamp(ui, model->notice, &text, inner, 2),
                   &text);
    }
    if (!model->built && model->queue_line != NULL && model->queue_line[0] != '\0') {
        /* The build queue goes on after this build (the library's Queue shows it). */
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
        nk_ui_text(ui, FLOW_ID_QUEUE_LINE,
                   nk_ui_text_clamp(ui, model->queue_line, &text, inner, 1), &text);
    }
    {
        NkUiLayout log = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fixed(104.0f),
                                     2.0f);
        NkUiStyle style = scr_flow_box_style(theme, 0x080B0FFFu);
        log.padding_top = log.padding_bottom = 10.0f;
        log.padding_left = log.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_LOG, &log, &style)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
            text.line_height = 20.0f;
            for (size_t i = 0u; i < model->line_count && i < 4u; ++i) {
                nk_ui_text(ui, (NkUiId)(FLOW_ID_LOG + 1u + (NkUiId)i),
                           nk_ui_text_clamp(ui, safe_text(model->lines[i], " "), &text,
                                            inner - 28.0f, 1), &text);
            }
            if (model->line_count == 0u) {
                nk_ui_text(ui, FLOW_ID_LOG + 8u, NK_UI_STR(BUILD_WAITING_FOR_OUTPUT), &text);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_BUILD_ACTION,
                             model->built ? NK_UI_STR(BUILD_BACK_TO_LIBRARY) : NK_UI_STR(BUILD_CANCEL_BUILD),
                             model->built ? NK_UI_BUTTON_PRIMARY : NK_UI_BUTTON_DANGER)) {
                result = action(model->built ? PLAYER_UI_ACTION_LEAVE_BUILD
                                             : PLAYER_UI_ACTION_CANCEL_BUILD,
                                0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, NK_UI_STR(BUILD_BUILD_HELP),
             model->built ? prompts_done : prompts_running, 2u);
    return result;
}

/* The build queue: what builds, in order, a page at a time. The one building
   leads with its progress; each game that waits has Up, Down and Remove, kept
   in their columns (a first or last game's Up or Down is off, not gone). One
   button pauses the queue and resumes it; a paused queue says so in its title,
   its lead and its hint. A build cannot stop halfway, so Pause lets the one
   that runs finish. */
PlayerUiAction player_ui_screen_queue(NkUi *ui, const PlayerUiQueueModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(BUILD_MOVE)}, {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(BUILD_SELECT)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(BUILD_BACK)}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[160];
    size_t shown;
    bool stopped;
    const char *lead;
    if (ui == NULL || model == NULL) return result;
    stopped = model->total > 0u && (model->paused || model->held);
    theme = nk_ui_theme(ui);
    shown = model->rows != NULL ? model->row_count : 0u;
    if (shown > PLAYER_UI_QUEUE_ROW_CAP) shown = PLAYER_UI_QUEUE_ROW_CAP;
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                           model->logical_height > 80.0f ? model->logical_height : 800.0f,
                           720.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, NK_UI_STR(BUILD_QUEUE_HEADING), theme->accent_dark);
    if (model->total == 0u) {
        (void)snprintf(text, sizeof(text), NK_UI_STR(BUILD_QUEUE_EMPTY_FORMAT), NK_UI_STR(BUILD_QUEUE_EMPTY));
    } else if (model->total == 1u) {
        (void)snprintf(text, sizeof(text), NK_UI_STR(BUILD_QUEUE_SINGLE_FORMAT),
                       stopped ? NK_UI_STR(BUILD_QUEUE_PAUSED_SINGLE) : NK_UI_STR(BUILD_QUEUE_SINGLE));
    } else {
        (void)snprintf(text, sizeof(text), NK_UI_STR(BUILD_QUEUE_COUNT_FORMAT),
                       stopped ? NK_UI_STR(BUILD_QUEUE_PAUSED_PREFIX) : "", (unsigned long)model->total);
    }
    nk_ui_text_role(ui, FLOW_ID_TITLE, nk_ui_frame_text(ui, text), NK_UI_TYPE_TITLE,
                    theme->paper);
    if (model->total == 0u) {
        lead = NK_UI_STR(BUILD_QUEUE_ADD_HELP);
    } else if (model->paused) {
        lead = NK_UI_STR(BUILD_QUEUE_START_FAILED);
    } else if (model->held && model->building) {
        lead = NK_UI_STR(BUILD_QUEUE_ACTIVE_PAUSE_HELP);
    } else if (model->held) {
        lead = NK_UI_STR(BUILD_QUEUE_PAUSED_HELP);
    } else {
        lead = NK_UI_STR(BUILD_QUEUE_RUNNING_HELP);
    }
    nk_ui_text_role(ui, FLOW_ID_LEAD, lead, NK_UI_TYPE_BODY_S,
                    model->total > 0u && model->paused ? theme->warning : theme->muted);
    if (shown > 0u) {
        NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 6.0f);
        NkUiStyle style = scr_flow_box_style(theme, theme->surface);
        rows.padding_top = rows.padding_bottom = 10.0f;
        rows.padding_left = rows.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
            for (size_t i = 0u; i < shown; ++i) {
                const PlayerUiQueueRow *row = &model->rows[i];
                NkUiLayout line = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                NkUiTextStyle name = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
                uint32_t small = NK_UI_BUTTON_SMALL | NK_UI_BUTTON_ROW;
                float buttons = row->building ? 0.0f
                    : nk_ui_button_width(ui, NK_UI_STR(BUILD_QUEUE_MOVE_UP), small) +
                      nk_ui_button_width(ui, NK_UI_STR(BUILD_QUEUE_MOVE_DOWN), small) +
                      nk_ui_button_width(ui, NK_UI_STR(BUILD_QUEUE_REMOVE), small) + 3.0f * NK_UI_SPACE_3;
                float name_width = inner - 28.0f - buttons - 120.0f;
                line.align_y = NK_UI_ALIGN_CENTER;
                if (name_width < 80.0f) name_width = 80.0f;
                if (!nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_ITEMS + (NkUiId)i), &line,
                                           NULL)) {
                    continue;
                }
                nk_ui_text(ui, (NkUiId)(FLOW_ID_ITEMS + 0x10u + (NkUiId)i),
                           nk_ui_text_clamp(ui, safe_text(row->title, NK_UI_STR(BUILD_QUEUE_GAME_FALLBACK)), &name,
                                            name_width, 1), &name);
                nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x20u + (NkUiId)i),
                                nk_ui_caps(ui, safe_text(row->status, NK_UI_STR(BUILD_QUEUE_STATUS_FALLBACK))),
                                NK_UI_TYPE_LABEL_S,
                                row->building ? theme->accent_dark : theme->muted);
                nk_ui_spacer(ui, (NkUiId)(FLOW_ID_ITEMS + 0x30u + (NkUiId)i), sizing_grow(),
                             sizing_fixed(1.0f));
                if (!row->building) {
                    if (nk_ui_button(ui, (NkUiId)(PLAYER_UI_ID_QUEUE_UP_BASE + (NkUiId)i), NK_UI_STR(BUILD_QUEUE_MOVE_UP),
                                     small | (row->can_move_up ? 0u : NK_UI_BUTTON_DISABLED))) {
                        result = action(PLAYER_UI_ACTION_QUEUE, row->title_index,
                                        (PlayerUiPrimaryAction)PLAYER_UI_QUEUE_EARLIER);
                    }
                    if (nk_ui_button(ui, (NkUiId)(PLAYER_UI_ID_QUEUE_DOWN_BASE + (NkUiId)i),
                                     NK_UI_STR(BUILD_QUEUE_MOVE_DOWN),
                                     small | (row->can_move_down ? 0u : NK_UI_BUTTON_DISABLED))) {
                        result = action(PLAYER_UI_ACTION_QUEUE, row->title_index,
                                        (PlayerUiPrimaryAction)PLAYER_UI_QUEUE_LATER);
                    }
                    if (nk_ui_button(ui, (NkUiId)(PLAYER_UI_ID_QUEUE_REMOVE_BASE + (NkUiId)i),
                                     NK_UI_STR(BUILD_QUEUE_REMOVE), small)) {
                        result = action(PLAYER_UI_ACTION_QUEUE, row->title_index,
                                        (PlayerUiPrimaryAction)PLAYER_UI_QUEUE_REMOVE);
                    }
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
        if (model->total > shown) {
            (void)snprintf(text, sizeof(text), NK_UI_STR(BUILD_QUEUE_PAGE_FORMAT),
                           (unsigned long)(model->first_row + 1u),
                           (unsigned long)(model->first_row + shown),
                           (unsigned long)model->total);
            nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                            theme->muted);
        }
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        NkUiFlowRow flow;
        bool earlier = model->first_row > 0u;
        bool later = model->first_row + shown < model->total;
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_flow_begin(ui, &flow, FLOW_ID_ACTIONS, &row, inner)) {
            if (model->total > 0u) {
                /* One button, so the focus stays on it: Pause, then Resume. */
                const char *label = stopped ? NK_UI_STR(BUILD_QUEUE_RESUME) : NK_UI_STR(BUILD_QUEUE_PAUSE);
                uint32_t look = stopped ? NK_UI_BUTTON_PRIMARY : 0u;
                nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, label, look));
                if (nk_ui_button(ui, PLAYER_UI_ID_QUEUE_PAUSE, label, look)) {
                    result = action(PLAYER_UI_ACTION_QUEUE, 0u,
                                    (PlayerUiPrimaryAction)(stopped ? PLAYER_UI_QUEUE_RESUME
                                                                    : PLAYER_UI_QUEUE_PAUSE));
                }
            }
            if (earlier || later) {
                uint32_t back = earlier ? 0u : NK_UI_BUTTON_DISABLED;
                uint32_t next = later ? 0u : NK_UI_BUTTON_DISABLED;
                nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(BUILD_QUEUE_EARLIER), back));
                if (nk_ui_button(ui, PLAYER_UI_ID_QUEUE_PAGE_BACK, NK_UI_STR(BUILD_QUEUE_EARLIER), back)) {
                    result = action(PLAYER_UI_ACTION_QUEUE, 0u,
                                    (PlayerUiPrimaryAction)PLAYER_UI_QUEUE_PAGE_BACK);
                }
                nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(BUILD_QUEUE_LATER), next));
                if (nk_ui_button(ui, PLAYER_UI_ID_QUEUE_PAGE_NEXT, NK_UI_STR(BUILD_QUEUE_LATER), next)) {
                    result = action(PLAYER_UI_ACTION_QUEUE, 0u,
                                    (PlayerUiPrimaryAction)PLAYER_UI_QUEUE_PAGE_NEXT);
                }
            }
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(BUILD_BACK), 0u));
            if (nk_ui_button(ui, PLAYER_UI_ID_QUEUE_BACK, NK_UI_STR(BUILD_BACK), 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_flow_end(ui, &flow);
        }
    }
    scr_flow_end(ui, !stopped ? NK_UI_STR(BUILD_QUEUE_FOOTER_RUNNING)
                     : model->held && model->building
                         ? NK_UI_STR(BUILD_QUEUE_FOOTER_ACTIVE_PAUSE)
                         : NK_UI_STR(BUILD_QUEUE_FOOTER_PAUSED),
                 prompts, 3u);
    return result;
}
