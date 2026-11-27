/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Setup wizard on the setup wizard state in player_state.c: model->step is a
   WizardStep, in six states (0 Welcome, 1 Online and developer options, 2 Select
   game, 3 Verify, 4 Fonts and system, 5 Ready). A first run shows four of them as
   chips (Welcome, Options, Game, Verify; PLAYER_UI_WIZARD_ROUTE_STEPS) and ends on
   the library card once the files are copied; the Fonts and Ready states remain
   only for state-only callers (docs/PLAYER_UI_DESIGN.md, First run). Every control
   returns PLAYER_UI_ACTION_WIZARD with the op main.c runs, the same handlers the
   existing renderer's wizard buttons call. */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

#define WIZ_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_WIZARD, (offset))
#define WIZ_ID_STEPS WIZ_ID(0x000001u)
#define WIZ_ID_STEPS_SPAN 0x8u
#define WIZ_ID_STEP_LABELS WIZ_ID(0x000101u)
#define WIZ_ID_STEP_LABELS_SPAN 0x8u
#define WIZ_ID_ROWS WIZ_ID(0x000200u)
#define WIZ_ID_ROWS_SPAN 0x100u
#define WIZ_ID_BUTTONS WIZ_ID(0x000030u)
#define WIZ_ID_EXTRA WIZ_ID(0x000031u)
#define WIZ_ID_TEXT WIZ_ID(0x001000u)
#define WIZ_ID_TEXT_SPAN 0x1000u
#define WIZ_ID_GUIDE WIZ_ID(0x000300u)

static PlayerUiAction wizard_op(PlayerUiWizardOp op) {
    return action(PLAYER_UI_ACTION_WIZARD, 0u, (PlayerUiPrimaryAction)op);
}

static void wizard_text(NkUi *ui, uint32_t k, const char *text, NkUiTypeRole role, uint32_t rgba) {
    if (text != NULL && text[0] != '\0') {
        nk_ui_text_role(ui, (NkUiId)(WIZ_ID_TEXT + k), text, role, rgba);
    }
}

/* The chips name the steps a first run shows (PLAYER_UI_WIZARD_ROUTE_STEPS):
   copying the files ends on the library card, so no Fonts or Ready chip
   promises a step that never comes. */
static void wizard_steps(NkUi *ui, int step) {
    static const char *const names[PLAYER_UI_WIZARD_ROUTE_STEPS] = {
        NK_UI_STR(WIZARD_STEP_WELCOME), NK_UI_STR(WIZARD_STEP_OPTIONS),
        NK_UI_STR(WIZARD_STEP_GAME), NK_UI_STR(WIZARD_STEP_VERIFY)
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    if (!nk_ui_begin_container(ui, WIZ_ID_STEPS, &row, NULL)) return;
    for (int i = 0; i < PLAYER_UI_WIZARD_ROUTE_STEPS; ++i) {
        bool done = i < step;
        bool current = i == step;
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
        if (done) {
            (void)snprintf(label, sizeof(label), NK_UI_STR(WIZARD_STEP_COMPLETE_FORMAT), names[i]);
        } else {
            (void)snprintf(label, sizeof(label), NK_UI_STR(WIZARD_STEP_NUMBER_FORMAT), i + 1, names[i]);
        }
        if (nk_ui_begin_container(ui, (NkUiId)(WIZ_ID_STEPS + 1u + (NkUiId)i), &chip, &style)) {
            nk_ui_text_role(ui, (NkUiId)(WIZ_ID_STEP_LABELS + (NkUiId)i),
                            nk_ui_caps(ui, label), NK_UI_TYPE_LABEL_S,
                            current ? theme->ink : done ? theme->ready : theme->muted);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

typedef struct {
    NkUiId id;
    const char *label;
    uint32_t flags;
    PlayerUiWizardOp op;
} WizardButton;

static void wizard_buttons(NkUi *ui, NkUiId row_id, const WizardButton *buttons, size_t count,
                           PlayerUiAction *result) {
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
    row.padding_top = NK_UI_SPACE_1;
    if (!nk_ui_begin_container(ui, row_id, &row, NULL)) return;
    for (size_t i = 0u; i < count; ++i) {
        if (nk_ui_button(ui, buttons[i].id, buttons[i].label, buttons[i].flags)) {
            *result = wizard_op(buttons[i].op);
        }
    }
    nk_ui_end_container(ui);
}

PlayerUiAction player_ui_screen_wizard(NkUi *ui, const PlayerUiWizardModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(WIZARD_PROMPT_MOVE)},
        {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(WIZARD_PROMPT_SELECT)},
        {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(WIZARD_BACK)}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char line[256];
    bool pc;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    pc = player_ui_host_is_pc(model->host);
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 860.0f);
    if (inner <= 0.0f) return result;
    wizard_steps(ui, model->step);
    switch (model->step) {
    case 0: {
        static const WizardButton buttons[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_GET_STARTED),
             NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_LARGE,
             PLAYER_UI_WIZARD_NEXT},
            {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_SKIP_TO_LIBRARY), NK_UI_BUTTON_LARGE,
             PLAYER_UI_WIZARD_CANCEL}
        };
        wizard_text(ui, 0u, NK_UI_STR(WIZARD_WELCOME_TITLE), NK_UI_TYPE_TITLE_XL, theme->paper);
        wizard_text(ui, 1u,
                    pc ? NK_UI_STR(WIZARD_WELCOME_INTRO) : NK_UI_STR(WIZARD_WELCOME_INTRO_DEVICE),
                    NK_UI_TYPE_BODY_L, theme->muted);
        if (scr_wizard_box_begin(ui, 0u)) {
            nk_ui_eyebrow(ui, WIZ_ID_TEXT + 2u, NK_UI_STR(WIZARD_REQUIREMENTS_HEADING));
            wizard_text(ui, 3u, NK_UI_STR(WIZARD_DISC_REQUIREMENT), NK_UI_TYPE_BODY_S,
                        theme->paper);
            wizard_text(ui, 4u,
                        pc ? NK_UI_STR(WIZARD_GRAPHICS_REQUIREMENT)
                           : NK_UI_STR(WIZARD_GRAPHICS_REQUIREMENT_DEVICE),
                        NK_UI_TYPE_BODY_S, theme->paper);
            wizard_text(ui, 5u,
                        pc ? NK_UI_STR(WIZARD_INPUT_REQUIREMENT)
                           : NK_UI_STR(WIZARD_INPUT_REQUIREMENT_DEVICE),
                        NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_end_container(ui);
        }
        wizard_text(ui, 6u,
                    pc ? NK_UI_STR(WIZARD_PRIVACY_NOTE) : NK_UI_STR(WIZARD_PRIVACY_NOTE_DEVICE),
                    NK_UI_TYPE_CAPTION, theme->muted);
        wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 2u, &result);
        break;
    }
    case 1: {
        static const WizardButton buttons[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_CONTINUE), NK_UI_BUTTON_PRIMARY,
             PLAYER_UI_WIZARD_NEXT},
            {PLAYER_UI_ID_WIZARD_SECOND, NK_UI_STR(WIZARD_SKIP), 0u, PLAYER_UI_WIZARD_SKIP_OPTIONS},
            {PLAYER_UI_ID_WIZARD_BACK, NK_UI_STR(WIZARD_BACK), 0u, PLAYER_UI_WIZARD_BACK}
        };
        wizard_text(ui, 0u, NK_UI_STR(WIZARD_OPTIONS_TITLE), NK_UI_TYPE_TITLE_XL, theme->paper);
        wizard_text(ui, 1u, SCR_ONLINE_NOT_BUILT_LINE, NK_UI_TYPE_BODY, theme->muted);
        if (scr_wizard_box_begin(ui, 0u)) {
            WizardColumn column = scr_online_developer_column(ui, model->online_mode,
                                                              model->developer_mode,
                                                              inner - 32.0f);
            if (model->online_mode &&
                scr_wizard_switch(ui, PLAYER_UI_ID_WIZARD_ONLINE, WIZ_ID_TEXT + 0x80u,
                                  NK_UI_STR(WIZARD_ONLINE_MODE), SCR_ONLINE_STORED_ON_SENTENCE, true,
                                  inner - 32.0f, &column)) {
                result = wizard_op(PLAYER_UI_WIZARD_TOGGLE_ONLINE);
            }
            if (scr_wizard_switch(ui, PLAYER_UI_ID_WIZARD_DEVELOPER, WIZ_ID_TEXT + 0x90u,
                              NK_UI_STR(WIZARD_DEVELOPER_MODE), WIZARD_DEVELOPER_SENTENCE,
                              model->developer_mode, inner - 32.0f, &column)) {
                result = wizard_op(PLAYER_UI_WIZARD_TOGGLE_DEVELOPER);
            }
            nk_ui_end_container(ui);
        }
        wizard_text(ui, 2u, NK_UI_STR(WIZARD_DEVELOPER_DEFAULT),
                    NK_UI_TYPE_CAPTION, theme->muted);
        wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        break;
    }
    case 2: {
        static const WizardButton choose[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_CHOOSE_DISC), NK_UI_BUTTON_PRIMARY,
             PLAYER_UI_WIZARD_CHOOSE_ISO},
            {PLAYER_UI_ID_WIZARD_BACK, NK_UI_STR(WIZARD_BACK), 0u, PLAYER_UI_WIZARD_BACK},
            {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL), 0u, PLAYER_UI_WIZARD_CANCEL}
        };
        static const WizardButton proceed[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_VERIFY_DISC), NK_UI_BUTTON_PRIMARY,
             PLAYER_UI_WIZARD_NEXT},
            {PLAYER_UI_ID_WIZARD_SECOND, NK_UI_STR(WIZARD_CHOOSE_ANOTHER_DISC), 0u,
             PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
            {PLAYER_UI_ID_WIZARD_BACK, NK_UI_STR(WIZARD_BACK), 0u, PLAYER_UI_WIZARD_BACK},
            {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL), 0u, PLAYER_UI_WIZARD_CANCEL}
        };
        wizard_text(ui, 0u, NK_UI_STR(WIZARD_CHOOSE_DISC_TITLE), NK_UI_TYPE_TITLE_XL, theme->paper);
        wizard_text(ui, 1u,
                    NK_UI_STR(WIZARD_CHOOSE_DISC_INTRO),
                    NK_UI_TYPE_BODY, theme->muted);
        /* Chosen only while the pick stands: a refused pick or a cancelled
           conversion clears iso_selected but leaves the last disc's path. */
        bool chosen = model->has_iso && model->iso_selected;
        if (scr_wizard_box_begin(ui, chosen ? theme->ready : 0u)) {
            if (chosen) {
                nk_ui_status_pill(ui, WIZ_ID_TEXT + 2u, FLOW_MARK_DOT, NK_UI_STR(WIZARD_DISC_CHOSEN),
                                  theme->ready);
                wizard_text(ui, 3u, model->iso_detail, NK_UI_TYPE_DATA, theme->muted);
                {
                    NkUiTextStyle path = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
                    nk_ui_text(ui, WIZ_ID_TEXT + 4u,
                               nk_ui_text_clamp(ui, safe_text(model->iso_path, ""), &path,
                                                inner - 32.0f, 2), &path);
                }
                wizard_text(ui, 5u, safe_text(model->status_message, NK_UI_STR(WIZARD_READY_TO_VERIFY)),
                            NK_UI_TYPE_BODY_S, theme->ready);
            } else {
                nk_ui_status_pill(ui, WIZ_ID_TEXT + 2u, FLOW_MARK_TRIANGLE,
                                  NK_UI_STR(WIZARD_DISC_NOT_CHOSEN),
                                  theme->warning);
                if (model->status_message != NULL && model->status_message[0] != '\0') {
                    wizard_text(ui, 5u, model->status_message,
                                NK_UI_TYPE_BODY_S, theme->warning);
                } else {
                    wizard_text(ui, 5u,
                                pc ? NK_UI_STR(WIZARD_DROP_DISC_HINT)
                                   : NK_UI_STR(WIZARD_DROP_DISC_HINT_DEVICE),
                                NK_UI_TYPE_BODY_S, theme->muted);
                }
            }
            nk_ui_end_container(ui);
        }
        if (model->iso_selected) {
            wizard_buttons(ui, WIZ_ID_BUTTONS, proceed, 4u, &result);
        } else {
            wizard_buttons(ui, WIZ_ID_BUTTONS, choose, 3u, &result);
        }
        break;
    }
    case 3: {
        bool blocked = model->experimental && model->blocked_reason != NULL &&
                       model->blocked_reason[0] != '\0';
        /* Encrypted program files the user can supply (#308). */
        bool guided = blocked && scr_input_guide_present(&model->input_guide);
        const char *badge = guided ? NK_UI_STR(WIZARD_DISC_BADGE_NEEDS_FILES)
            : blocked ? NK_UI_STR(WIZARD_DISC_BADGE_BLOCKED)
            : model->recorded ? NK_UI_STR(WIZARD_DISC_BADGE_RECORDED)
            : (model->experimental ? NK_UI_STR(WIZARD_DISC_BADGE_EXPERIMENTAL)
               : (model->supported ? NK_UI_STR(WIZARD_DISC_BADGE_KNOWN)
                                   : NK_UI_STR(WIZARD_DISC_BADGE_UNKNOWN)));
        uint32_t tone = guided ? theme->warning : blocked ? theme->error
            : model->recorded ? theme->ready
            : (model->experimental ? theme->warning
               : (model->supported ? theme->ready : theme->error));
        const char *note;
        uint32_t note_tone = theme->muted;
        if (model->extracting) {
            badge = model->converting_package ? NK_UI_STR(WIZARD_DISC_BADGE_CONVERTING)
                                              : NK_UI_STR(WIZARD_DISC_BADGE_COPYING);
            tone = theme->warning;
        } else if (model->extraction_complete) {
            badge = NK_UI_STR(WIZARD_DISC_BADGE_STAGED);
        } else if (model->extraction_failed) {
            badge = NK_UI_STR(WIZARD_DISC_BADGE_COPY_FAILED);
            tone = theme->error;
        }
        if (model->extracting && model->converting_package) {
            note = NK_UI_STR(WIZARD_DISC_CONVERTING_NOTE);
        } else if (model->extracting) {
            note = NK_UI_STR(WIZARD_DISC_COPYING_NOTE);
        } else if (model->extraction_failed && model->extraction_error != NULL &&
                   model->extraction_error[0] != '\0') {
            /* Why the copy failed and what to do next, in place of the catalog note. */
            note = model->extraction_error;
            note_tone = theme->error;
        } else if (guided) {
            note = NK_UI_STR(WIZARD_DISC_NEEDS_FILES_NOTE);
            note_tone = theme->warning;
        } else if (blocked) {
            note = NK_UI_STR(WIZARD_DISC_BLOCKED_NOTE);
            note_tone = theme->error;
        } else if (model->recorded) {
            note = NK_UI_STR(WIZARD_DISC_RECORDED_NOTE);
        } else if (model->experimental) {
            note = NK_UI_STR(WIZARD_DISC_EXPERIMENTAL_NOTE);
            note_tone = theme->warning;
        } else if (model->supported) {
            note = NK_UI_STR(WIZARD_DISC_KNOWN_NOTE);
        } else {
            note = NK_UI_STR(WIZARD_DISC_UNKNOWN_NOTE);
            note_tone = theme->warning;
        }
        wizard_text(ui, 0u, model->converting_package ? NK_UI_STR(WIZARD_DISC_CONVERTING_TITLE)
                            : (model->extracting ? NK_UI_STR(WIZARD_DISC_COPYING_TITLE)
                                                 : NK_UI_STR(WIZARD_VERIFY_TITLE)),
                    NK_UI_TYPE_TITLE_XL, theme->paper);
        if (scr_wizard_box_begin(ui, tone)) {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_2);
            if (nk_ui_begin_container(ui, WIZ_ID_TEXT + 0x20u, &row, NULL)) {
                nk_ui_status_pill(ui, WIZ_ID_TEXT + 0x21u, NULL, badge, tone);
                nk_ui_tag(ui, WIZ_ID_TEXT + 0x22u,
                          safe_text(model->disc_id, NK_UI_STR(WIZARD_DISC_ID_UNKNOWN)));
                nk_ui_end_container(ui);
            }
            /* A guided step on a short window gives the name's line to the
               files; the disc ID chip above still says which disc it is. */
            if (!guided || model->logical_height >= 680.0f) {
                wizard_text(ui, 1u, safe_text(model->title_name, NK_UI_STR(WIZARD_TITLE_FALLBACK)),
                            NK_UI_TYPE_TITLE, theme->paper);
            }
            /* A guided step says it in the guide itself. */
            if (!guided) wizard_text(ui, 2u, note, NK_UI_TYPE_BODY_S, note_tone);
            if (blocked && !model->extracting && !guided) {
                wizard_text(ui, 6u, model->blocked_reason, NK_UI_TYPE_BODY_S, theme->paper);
            }
            if (model->extracting && model->converting_package) {
                (void)snprintf(line, sizeof(line), NK_UI_STR(WIZARD_DISC_CONVERSION_PROGRESS_FORMAT),
                               model->extraction_percent);
            } else if (model->extracting) {
                (void)snprintf(line, sizeof(line), NK_UI_STR(WIZARD_DISC_COPY_PROGRESS_FORMAT),
                               model->files_extracted, model->total_files,
                               model->extraction_percent);
            }
            if (model->extracting) {
                wizard_text(ui, 3u, line, NK_UI_TYPE_DATA, theme->paper);
                nk_ui_progress_bar(ui, WIZ_ID_TEXT + 0x30u, inner - 32.0f,
                                   (float)model->extraction_percent / 100.0f, 0.0);
                {
                    NkUiTextStyle file = nk_ui_type(NK_UI_TYPE_DATA, theme->dim);
                    nk_ui_text(ui, WIZ_ID_TEXT + 4u,
                               nk_ui_text_clamp(ui, safe_text(model->current_file,
                                                              NK_UI_STR(WIZARD_DISC_FILE_FALLBACK)),
                                                &file, inner - 32.0f, 1), &file);
                }
            } else if (guided) {
                /* The files take the room the check rows had; the rows come
                   back once Check again lets the disc in. */
                scr_input_guide_box(ui, WIZ_ID_GUIDE, &model->input_guide, inner - 32.0f,
                                model->logical_height >= 800.0f ? PLAYER_UI_INPUT_FILES
                                : model->logical_height >= 680.0f ? 2u : 1u);
            } else {
                size_t rows = model->check_count < PLAYER_UI_WIZARD_CHECKS
                    ? model->check_count : PLAYER_UI_WIZARD_CHECKS;
                float status_w = scr_check_column_width(ui, model->checks, rows, true,
                                                    NK_UI_TYPE_LABEL_S, "", 110.0f);
                float name_w = scr_check_column_width(ui, model->checks, rows, false,
                                                  NK_UI_TYPE_DATA, "", 150.0f);
                for (size_t i = 0u; i < rows; ++i) {
                    const PlayerUiCheckRow *check = &model->checks[i];
                    NkUiLayout crow = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                                  NK_UI_SPACE_3);
                    NkUiId base = (NkUiId)(WIZ_ID_ROWS + (NkUiId)i * 8u);
                    if (nk_ui_begin_container(ui, base, &crow, NULL)) {
                        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(status_w),
                                                      sizing_fit(), 0.0f);
                        NkUiTextStyle msg = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
                            nk_ui_text_role(ui, base + 4u, nk_ui_caps(ui, safe_text(check->status, "")),
                                            NK_UI_TYPE_LABEL_S, check->rgba);
                            nk_ui_end_container(ui);
                        }
                        cell.width = sizing_fixed(name_w);
                        if (nk_ui_begin_container(ui, base + 2u, &cell, NULL)) {
                            nk_ui_text_role(ui, base + 5u, safe_text(check->name, ""),
                                            NK_UI_TYPE_DATA, theme->paper);
                            nk_ui_end_container(ui);
                        }
                        nk_ui_text(ui, base + 3u,
                                   nk_ui_text_clamp(ui, safe_text(check->message, ""), &msg,
                                                    inner - 32.0f - status_w - name_w -
                                                        2.0f * NK_UI_SPACE_3, 1), &msg);
                        nk_ui_end_container(ui);
                    }
                }
            }
            nk_ui_end_container(ui);
        }
        if (model->extracting) {
            static const WizardButton copying[] = {
                {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL_COPY), NK_UI_BUTTON_DANGER,
                 PLAYER_UI_WIZARD_CANCEL}
            };
            static const WizardButton converting[] = {
                {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL_CONVERSION), NK_UI_BUTTON_DANGER,
                 PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS,
                           model->converting_package ? converting : copying, 1u, &result);
        } else if (guided) {
            /* Check again reads the same disc again once the files are in
               the folder; nothing is imported again (#308). */
            WizardButton buttons[4] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_CHECK_AGAIN), NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_CHECK_AGAIN},
                {PLAYER_UI_ID_WIZARD_OPEN_FOLDER, NK_UI_STR(WIZARD_OPEN_FOLDER), 0u,
                 PLAYER_UI_WIZARD_OPEN_INPUT_FOLDER},
                {PLAYER_UI_ID_WIZARD_SECOND, NK_UI_STR(WIZARD_CHOOSE_ANOTHER_DISC), 0u,
                 PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
                {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL), 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            if (model->input_guide.folder != NULL && model->input_guide.folder[0] != '\0') {
                wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 4u, &result);
            } else {
                buttons[1] = buttons[2];
                buttons[2] = buttons[3];
                wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
            }
        } else if (blocked) {
            /* Adding it would only fail later, at build time (#1280). */
            static const WizardButton buttons[] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_CHOOSE_ANOTHER_DISC), NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
                {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL), 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 2u, &result);
        } else if (model->experimental) {
            static const WizardButton buttons[] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_ADD_TO_LIBRARY), NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_ADD_EXPERIMENTAL},
                {PLAYER_UI_ID_WIZARD_SECOND, NK_UI_STR(WIZARD_CHOOSE_ANOTHER_DISC), 0u,
                 PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
                {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL), 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        } else if (model->supported) {
            WizardButton buttons[3] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_COPY_FILES_AND_CONTINUE),
                 NK_UI_BUTTON_PRIMARY, PLAYER_UI_WIZARD_NEXT},
                {PLAYER_UI_ID_WIZARD_BACK, NK_UI_STR(WIZARD_BACK), 0u, PLAYER_UI_WIZARD_BACK},
                {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL), 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            if (model->extraction_complete) buttons[0].label = NK_UI_STR(WIZARD_CONTINUE);
            else if (model->extraction_failed) buttons[0].label = NK_UI_STR(WIZARD_RETRY_COPY);
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        } else {
            static const WizardButton buttons[] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_CHOOSE_ANOTHER_DISC), NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
                {PLAYER_UI_ID_WIZARD_BACK, NK_UI_STR(WIZARD_BACK), 0u, PLAYER_UI_WIZARD_BACK},
                {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL), 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        }
        break;
    }
    case 4: {
        static const WizardButton buttons[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_ACCEPT_AND_CONTINUE), NK_UI_BUTTON_PRIMARY,
             PLAYER_UI_WIZARD_ACCEPT_FONTS},
            {PLAYER_UI_ID_WIZARD_SECOND, NK_UI_STR(WIZARD_CHANGE_RESOLUTION), 0u,
             PLAYER_UI_WIZARD_CYCLE_RESOLUTION},
            {PLAYER_UI_ID_WIZARD_BACK, NK_UI_STR(WIZARD_BACK), 0u, PLAYER_UI_WIZARD_BACK},
            {PLAYER_UI_ID_WIZARD_CANCEL, NK_UI_STR(WIZARD_CANCEL), 0u, PLAYER_UI_WIZARD_CANCEL}
        };
        static const WizardButton font_buttons[] = {
            {PLAYER_UI_ID_WIZARD_PSP_FONTS, NK_UI_STR(WIZARD_GET_PSP_FONTS), NK_UI_BUTTON_SMALL,
             PLAYER_UI_WIZARD_PSP_FONTS},
            {PLAYER_UI_ID_WIZARD_IMPORT_FONTS, NK_UI_STR(WIZARD_CHOOSE_FONTS), NK_UI_BUTTON_SMALL,
             PLAYER_UI_WIZARD_IMPORT_FONTS},
            {PLAYER_UI_ID_WIZARD_REMOVE_FONTS, NK_UI_STR(WIZARD_REMOVE_IMPORTED_FONTS), NK_UI_BUTTON_SMALL,
             PLAYER_UI_WIZARD_REMOVE_FONTS}
        };
        wizard_text(ui, 0u, NK_UI_STR(WIZARD_FONTS_TITLE), NK_UI_TYPE_TITLE_XL, theme->paper);
        if (scr_wizard_box_begin(ui, 0u)) {
            wizard_text(ui, 1u, model->resolution_line, NK_UI_TYPE_BODY, theme->paper);
            wizard_text(ui, 2u, model->controller_line, NK_UI_TYPE_BODY_S, theme->ready);
            wizard_text(ui, 6u, model->font_summary, NK_UI_TYPE_BODY_S,
                        model->fonts_all_own ? theme->ready : theme->warning);
            nk_ui_eyebrow(ui, WIZ_ID_TEXT + 3u, NK_UI_STR(WIZARD_FONT_SLOT_HEADING));
            {
                NkUiLayout cols = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_4);
                if (nk_ui_begin_container(ui, WIZ_ID_TEXT + 0x40u, &cols, NULL)) {
                    float column = (inner - 32.0f - 2.0f * NK_UI_SPACE_4) / 3.0f;
                    for (int c = 0; c < 3; ++c) {
                        NkUiLayout col = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column),
                                                     sizing_fit(), 2.0f);
                        if (!nk_ui_begin_container(ui, (NkUiId)(WIZ_ID_TEXT + 0x41u + (NkUiId)c),
                                                   &col, NULL)) {
                            continue;
                        }
                        for (size_t r = 0u; r < 6u; ++r) {
                            size_t item = (size_t)c * 6u + r;
                            NkUiTextStyle font = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                            if (item >= model->font_line_count) break;
                            nk_ui_text(ui, (NkUiId)(WIZ_ID_TEXT + 0x60u + (NkUiId)item),
                                       nk_ui_text_clamp(ui, safe_text(model->font_lines[item], ""),
                                                        &font, column, 1), &font);
                        }
                        nk_ui_end_container(ui);
                    }
                    nk_ui_end_container(ui);
                }
            }
            wizard_text(ui, 4u,
                        safe_text(model->font_message,
                                  NK_UI_STR(WIZARD_PSP_FONT_IMPORT_HELP)),
                        NK_UI_TYPE_CAPTION,
                        model->font_message != NULL && model->font_message[0] != '\0'
                            ? note_rgba(theme, model->font_message_kind) : theme->muted);
            wizard_buttons(ui, WIZ_ID_EXTRA, font_buttons, 3u, &result);
            nk_ui_end_container(ui);
        }
        wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 4u, &result);
        break;
    }
    case 5:
    default: {
        WizardButton buttons[3] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, NK_UI_STR(WIZARD_PLAY),
             NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_LARGE, PLAYER_UI_WIZARD_FINISH},
            {PLAYER_UI_ID_WIZARD_SECOND, NK_UI_STR(WIZARD_GO_TO_LIBRARY), NK_UI_BUTTON_LARGE,
             PLAYER_UI_WIZARD_GO_TO_LIBRARY},
            {PLAYER_UI_ID_WIZARD_BACK, NK_UI_STR(WIZARD_BACK), NK_UI_BUTTON_LARGE, PLAYER_UI_WIZARD_BACK}
        };
        if (!model->runtime_ready) buttons[0].label = NK_UI_STR(WIZARD_SAVE_TO_LIBRARY);
        wizard_text(ui, 0u, model->runtime_ready ? NK_UI_STR(WIZARD_READY_TO_PLAY)
                                                 : NK_UI_STR(WIZARD_FILES_STAGED_TITLE),
                    NK_UI_TYPE_TITLE_XL, theme->paper);
        if (scr_wizard_box_begin(ui, model->runtime_ready ? theme->ready : theme->warning)) {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_2);
            if (nk_ui_begin_container(ui, WIZ_ID_TEXT + 0x20u, &row, NULL)) {
                nk_ui_status_pill(ui, WIZ_ID_TEXT + 0x21u,
                                  model->runtime_ready ? FLOW_MARK_DOT : FLOW_MARK_TRIANGLE,
                                  model->runtime_ready ? NK_UI_STR(WIZARD_READY_TO_PLAY)
                                                       : NK_UI_STR(WIZARD_DISC_BADGE_STAGED),
                                  model->runtime_ready ? theme->ready : theme->warning);
                if (model->disc_id != NULL && model->disc_id[0] != '\0') {
                    nk_ui_tag(ui, WIZ_ID_TEXT + 0x22u, model->disc_id);
                }
                nk_ui_end_container(ui);
            }
            wizard_text(ui, 1u, safe_text(model->title_name, NK_UI_STR(WIZARD_TITLE_FALLBACK)),
                        NK_UI_TYPE_TITLE, theme->paper);
            wizard_text(ui, 2u, model->summary_line, NK_UI_TYPE_DATA, theme->muted);
            wizard_text(ui, 3u,
                        model->runtime_ready
                            ? NK_UI_STR(WIZARD_READY_NOTE)
                            : NK_UI_STR(WIZARD_FILES_STAGED_NOTE),
                        NK_UI_TYPE_BODY_S, theme->muted);
            nk_ui_end_container(ui);
        }
        wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        break;
    }
    }
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}
