/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Settings: one page of named rows, each with its value on the button that changes it, in
   two cards: what a game uses the next time it starts, and the launcher and sound. Its
   pages (online and developer options, third-party notices, build tools and cache,
   removing the build tools) are dialog cards like the rest of the flow. Every change is
   saved as it is made. */

#include "player_ui_nk.h"
#include "player_state.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SET_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_SETTINGS, (offset))
#define SET_ID_ROOT SET_ID(0x000001u)
#define SET_ID_TOP SET_ID(0x000010u)
#define SET_ID_BODY SET_ID(0x000020u)
#define SET_ID_COLUMN SET_ID(0x000021u)
#define SET_ID_HEAD SET_ID(0x000022u)
#define SET_ID_CARDS SET_ID(0x000023u)
#define SET_ID_GAME SET_ID(0x000024u)
#define SET_ID_LAUNCHER SET_ID(0x000025u)
#define SET_ID_ACTIONS SET_ID(0x000026u)
#define SET_ID_PROMPTS SET_ID(0x000040u)
#define SET_ID_WAVES SET_ID(0x000050u)
#define SET_ID_ROWS SET_ID(0x000100u)
#define SET_ID_ROWS_SPAN 0x100u
#define SET_ID_TEXT SET_ID(0x001000u)
#define SET_ID_TEXT_SPAN 0x1000u

static PlayerUiAction settings_op(PlayerUiSettingsOp op) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_SETTINGS;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = 0u;
    result.choice_index = -1;
    return result;
}

static const char *settings_on_off(bool on) {
    return on ? "On" : "Off";
}

/* A named row with its value on the button beside it. Returns true when the
   button is activated. */
static bool settings_row(NkUi *ui, uint32_t k, NkUiId button, const char *name,
                         const char *value, bool on, float name_width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
    NkUiId base = (NkUiId)(SET_ID_ROWS + k * 4u);
    bool pressed = false;
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return false;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(name_width), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text_role(ui, base + 2u, name, NK_UI_TYPE_BODY, theme->paper);
            nk_ui_end_container(ui);
        }
    }
    /* Left and Right stay in the row, so Right from a GAME row reaches the
       LAUNCHER AND SOUND row beside it (Controls among them), not the top bar. */
    pressed = nk_ui_button(ui, button, value,
                           NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW | NK_UI_BUTTON_ROW |
                               (on ? NK_UI_BUTTON_PRIMARY : 0u));
    nk_ui_end_container(ui);
    return pressed;
}

/* A named level with a - and a + either side of it (Game volume, Music volume). */
static void settings_level_row(NkUi *ui, uint32_t k, const char *name, int level, NkUiId down,
                               NkUiId up, NkUiId text_id, PlayerUiSettingsOp down_op,
                               PlayerUiSettingsOp up_op, float name_width,
                               PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    char line[16];
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
    NkUiId base = (NkUiId)(SET_ID_ROWS + k * 4u);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(name_width), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text_role(ui, base + 2u, name, NK_UI_TYPE_BODY, theme->paper);
            nk_ui_end_container(ui);
        }
        if (nk_ui_button(ui, down, "-", NK_UI_BUTTON_SMALL)) *result = settings_op(down_op);
        (void)snprintf(line, sizeof(line), "%d%%", level);
        cell.width = sizing_fixed(52.0f);
        cell.align_x = NK_UI_ALIGN_CENTER;
        if (nk_ui_begin_container(ui, base + 3u, &cell, NULL)) {
            nk_ui_text_role(ui, text_id, line, NK_UI_TYPE_DATA, theme->paper);
            nk_ui_end_container(ui);
        }
        if (nk_ui_button(ui, up, "+", NK_UI_BUTTON_SMALL)) *result = settings_op(up_op);
    }
    nk_ui_end_container(ui);
}

/* The game's frames come at 60 Hz (the PSP's 59.94 Hz): a display shows them evenly only when
   its rate is a whole multiple of that. Half a hertz covers the 59.94 family and the rounding
   displays report, and is far from the nearest uneven rate (75, 90, 144, 165). */
#define REFRESH_TOLERANCE_HZ 0.5
#define REFRESH_TITLE_HZ 60.0
#define REFRESH_TITLE_NTSC_HZ (60000.0 / 1001.0)

static bool refresh_is_multiple_of(double hz, double base) {
    double n = floor(hz / base + 0.5);
    return n >= 1.0 && fabs(hz - n * base) <= REFRESH_TOLERANCE_HZ;
}

size_t player_ui_report_more_count(const PlayerUiSettingsModel *model) {
    if (!model || model->report_line_count == 0u) return 0u;
    size_t shown = model->report_line_count < PLAYER_UI_REPORT_SHOWN
        ? model->report_line_count : PLAYER_UI_REPORT_SHOWN;
    /* A model that names no total (the screen snapshot fixtures) counts its lines. */
    size_t total = model->report_file_total > model->report_line_count
        ? model->report_file_total : model->report_line_count;
    return total - shown;
}

PlayerUiRefreshAdvice player_ui_refresh_advice(double refresh_hz) {
    if (!(refresh_hz > 0.0) || !isfinite(refresh_hz)) return PLAYER_UI_REFRESH_SILENT;
    if (refresh_is_multiple_of(refresh_hz, REFRESH_TITLE_HZ) ||
        refresh_is_multiple_of(refresh_hz, REFRESH_TITLE_NTSC_HZ)) {
        return PLAYER_UI_REFRESH_SILENT;
    }
    return PLAYER_UI_REFRESH_ADVISORY;
}

void player_ui_refresh_advisory_text(double refresh_hz, char *out, size_t out_size) {
    if (out == NULL || out_size == 0u) return;
    out[0] = '\0';
    if (player_ui_refresh_advice(refresh_hz) != PLAYER_UI_REFRESH_ADVISORY) return;
    if (fabs(refresh_hz - floor(refresh_hz + 0.5)) < 0.005) {
        (void)snprintf(out, out_size,
                       "Your display runs at %.0f Hz, which is not a multiple of the game's 60 Hz. "
                       "Motion can stutter slightly. For even motion, set it to 60, 120 or 180 Hz.",
                       refresh_hz);
    } else {
        (void)snprintf(out, out_size,
                       "Your display runs at %.2f Hz, which is not a multiple of the game's 60 Hz. "
                       "Motion can stutter slightly. For even motion, set it to 60, 120 or 180 Hz.",
                       refresh_hz);
    }
}

static void settings_main(NkUi *ui, const PlayerUiSettingsModel *model, float width,
                          float height, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    float column = width - 2.0f * (width < 900.0f ? NK_UI_GUTTER_NARROW : NK_UI_GUTTER);
    float card;
    float name_width;
    char line[192];
    NkUiLayout layout;
    if (column > 1040.0f) column = 1040.0f;
    card = (column - NK_UI_SPACE_4) * 0.5f;
    {
        /* The names take what the widest value button leaves, at least its
           design 190 px; at a larger text size the names wrap instead. */
        const char *values[] = {
            safe_text(model->resolution, "2x"), safe_text(model->scaling, "Sharp"),
            "Size, contrast, motion",
            "Controller and keyboard", "Check for updates", "Off", "Off: options"
        };
        float buttons = 190.0f;
        for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
            float need = nk_ui_button_width(ui, values[i], NK_UI_BUTTON_SMALL);
            if (need > buttons) buttons = need;
        }
        name_width = card - 40.0f - NK_UI_SPACE_3 - buttons;
    }
    if (name_width > 200.0f) name_width = 200.0f;
    if (name_width < 110.0f) name_width = 110.0f;
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column), sizing_fit(), NK_UI_SPACE_4);
    if (!nk_ui_begin_container(ui, SET_ID_COLUMN, &layout, NULL)) return;
    {
        NkUiLayout head = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                      NK_UI_SPACE_1);
        char advisory[192];
        player_ui_refresh_advisory_text(model->display_refresh_hz, advisory, sizeof(advisory));
        if (nk_ui_begin_container(ui, SET_ID_HEAD, &head, NULL)) {
            /* The SETTINGS tab names the page, so a short window drops the
               heading to keep every row on screen; the advisory line needs
               that room sooner. */
            if (height >= (advisory[0] != '\0' ? 780.0f : 680.0f)) {
                nk_ui_text_role(ui, SET_ID_TEXT + 0u, "SETTINGS", NK_UI_TYPE_DISPLAY,
                                theme->paper);
            }
            /* A notice (a failed save) comes first, then the page's own line. The graphics
               preset's note (#2150) is on Settings > Enhanced, with the preset's row. */
            bool has_notice = model->notice != NULL && model->notice[0] != '\0';
            nk_ui_text_role(ui, SET_ID_TEXT + 1u,
                            has_notice ? model->notice
                                       : "Changes are saved as you make them. Game settings "
                                         "apply the next time a game starts.",
                            NK_UI_TYPE_BODY_S,
                            has_notice ? theme->warning : theme->muted);
            if (advisory[0] != '\0') {
                nk_ui_text_role(ui, SET_ID_TEXT + 2u, advisory, NK_UI_TYPE_BODY_S,
                                theme->warning);
            }
            nk_ui_end_container(ui);
        }
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    if (nk_ui_begin_container(ui, SET_ID_CARDS, &layout, NULL)) {
        if (nk_ui_card_begin(ui, SET_ID_GAME, sizing_fixed(card), sizing_grow(), NK_UI_SPACE_2)) {
            nk_ui_eyebrow(ui, SET_ID_TEXT + 0x10u, "Game");
            if (settings_row(ui, 0u, PLAYER_UI_ID_SETTINGS_RESOLUTION, "Resolution",
                             safe_text(model->resolution, "2x"), false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_RESOLUTION);
            }
            if (settings_row(ui, 2u, PLAYER_UI_ID_SETTINGS_VSYNC, "VSync",
                             settings_on_off(model->vsync), model->vsync, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_VSYNC);
            }
            if (settings_row(ui, 3u, PLAYER_UI_ID_SETTINGS_FULLSCREEN, "Game fullscreen",
                             settings_on_off(model->fullscreen), model->fullscreen,
                             name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_FULLSCREEN);
            }
            if (settings_row(ui, 4u, PLAYER_UI_ID_SETTINGS_SCALING, "Scaling",
                             safe_text(model->scaling, "Sharp"), false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_SCALING);
            }
            /* The play mode (#2055): one row that opens Settings > Enhanced, where the
               library's default and each part are (#2007). */
            if (settings_row(ui, 12u, PLAYER_UI_ID_SETTINGS_ACE_OPEN, "Play mode",
                             model->enhanced_library_on ? "Enhanced: options"
                                                        : "Authentic: options",
                             model->enhanced_library_on, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_ACE);
            }
            nk_ui_end_container(ui);
        }
        if (nk_ui_card_begin(ui, SET_ID_LAUNCHER, sizing_fixed(card), sizing_grow(),
                             NK_UI_SPACE_2)) {
            nk_ui_eyebrow(ui, SET_ID_TEXT + 0x20u, "Launcher and sound");
            settings_level_row(ui, 5u, "Game volume", model->volume,
                               PLAYER_UI_ID_SETTINGS_VOLUME_DOWN, PLAYER_UI_ID_SETTINGS_VOLUME_UP,
                               SET_ID_TEXT + 0x11u, PLAYER_UI_SETTINGS_VOLUME_DOWN,
                               PLAYER_UI_SETTINGS_VOLUME_UP, name_width, result);
            if (settings_row(ui, 6u, PLAYER_UI_ID_SETTINGS_LAUNCHER_FULLSCREEN,
                             "Launcher fullscreen (F11)", settings_on_off(model->launcher_fullscreen),
                             model->launcher_fullscreen, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_LAUNCHER_FULLSCREEN);
            }
            if (settings_row(ui, 7u, PLAYER_UI_ID_SETTINGS_ACCESSIBILITY, "Accessibility",
                             "Size, contrast, motion", false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_ACCESSIBILITY);
            }
            if (settings_row(ui, 8u, PLAYER_UI_ID_SETTINGS_UI_SOUNDS, "Menu sounds",
                             settings_on_off(model->ui_sounds), model->ui_sounds, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_UI_SOUNDS);
            }
            if (settings_row(ui, 9u, PLAYER_UI_ID_SETTINGS_CONTROLLER, "Controls",
                             "Controller and keyboard", false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_CONTROLLER);
            }
            if (settings_row(ui, 10u, PLAYER_UI_ID_SETTINGS_UPDATES, "Updates",
                             "Check for updates", false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_CHECK_UPDATES);
            }
            /* Launcher music (#2207), at the card's end so the rows above keep their
               places beside the GAME rows. Its level is its own, as the menu sounds'
               is: Game volume is the game's. */
            if (settings_row(ui, 14u, PLAYER_UI_ID_SETTINGS_LAUNCHER_MUSIC, "Launcher music",
                             model->launcher_music_value != NULL
                                 ? model->launcher_music_value
                                 : settings_on_off(model->launcher_music),
                             model->launcher_music, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_LAUNCHER_MUSIC);
            }
            settings_level_row(ui, 15u, "Music volume", model->music_volume,
                               PLAYER_UI_ID_SETTINGS_MUSIC_VOLUME_DOWN,
                               PLAYER_UI_ID_SETTINGS_MUSIC_VOLUME_UP, SET_ID_TEXT + 0x12u,
                               PLAYER_UI_SETTINGS_MUSIC_VOLUME_DOWN,
                               PLAYER_UI_SETTINGS_MUSIC_VOLUME_UP, name_width, result);
            {
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                (void)snprintf(line, sizeof(line), "Controller: %s",
                               safe_text(model->controller_line, "keyboard and mouse"));
                nk_ui_text(ui, SET_ID_TEXT + 0x21u,
                           nk_ui_text_clamp(ui, line, &text, card - 40.0f, 1), &text);
                if (model->saves_folder != NULL && model->saves_folder[0] != '\0') {
                    char saves[600];
                    text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                    /* A no-break space keeps the folder on the line, so a long
                       one is cut short rather than dropped. */
                    (void)snprintf(saves, sizeof(saves), "Saves:\xC2\xA0%s",
                                   model->saves_folder);
                    nk_ui_text(ui, SET_ID_TEXT + 0x22u,
                               nk_ui_text_clamp(ui, saves, &text, card - 40.0f, 1), &text);
                }
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
    {
        /* One row at the design text size; a larger one wraps it. */
        static const struct {
            NkUiId id;
            const char *label;
            uint32_t flags;
            PlayerUiSettingsOp op;
        } actions[] = {
            {PLAYER_UI_ID_SETTINGS_DONE, "Done", NK_UI_BUTTON_PRIMARY, PLAYER_UI_SETTINGS_DONE},
            {PLAYER_UI_ID_SETTINGS_ONLINE, "Online and developer", 0u,
             PLAYER_UI_SETTINGS_OPEN_ONLINE},
            {PLAYER_UI_ID_SETTINGS_NOTICES, "Third-party notices", 0u,
             PLAYER_UI_SETTINGS_OPEN_NOTICES},
            {PLAYER_UI_ID_SETTINGS_BUILD_CACHE, "Build tools and cache", 0u,
             PLAYER_UI_SETTINGS_OPEN_BUILD_CACHE},
            {PLAYER_UI_ID_SETTINGS_FONTS, "PSP fonts", 0u, PLAYER_UI_SETTINGS_OPEN_FONTS},
            {PLAYER_UI_ID_SETTINGS_PROBLEM_REPORT, "Problem report", 0u,
             PLAYER_UI_SETTINGS_OPEN_PROBLEM_REPORT},
            {PLAYER_UI_ID_SETTINGS_OPEN_DATA_FOLDER, "Open data folder", 0u,
             PLAYER_UI_SETTINGS_OPEN_DATA_FOLDER},
            {PLAYER_UI_ID_SETTINGS_OPEN_LOGS, "Open logs", 0u, PLAYER_UI_SETTINGS_OPEN_LOGS},
            {PLAYER_UI_ID_SETTINGS_FACTORY_RESET, "Factory reset", NK_UI_BUTTON_DANGER,
             PLAYER_UI_SETTINGS_OPEN_FACTORY_RESET}
        };
        NkUiFlowRow flow;
        if (nk_ui_flow_begin(ui, &flow, SET_ID_ACTIONS, &layout, column)) {
            for (size_t i = 0u; i < sizeof(actions) / sizeof(actions[0]); ++i) {
                nk_ui_flow_item(ui, &flow,
                                nk_ui_button_width(ui, actions[i].label, actions[i].flags));
                /* Left and Right stay on this row: the cards' last rows end just above it. */
                if (nk_ui_button(ui, actions[i].id, actions[i].label,
                                 actions[i].flags | NK_UI_BUTTON_ROW)) {
                    *result = settings_op(actions[i].op);
                }
            }
            nk_ui_flow_end(ui, &flow);
        }
    }
    nk_ui_end_container(ui);
}

/* Settings > Problem report (#1849): what the bundle would hold, then one click to write
   it on this PC. Nothing is uploaded; the page says the issue tracker is public. */
static void settings_problem_report(NkUi *ui, const PlayerUiSettingsModel *model, float inner,
                                    PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    PlayerUiReportStep step = model->report_step;
    bool busy = step == PLAYER_UI_REPORT_PREVIEWING || step == PLAYER_UI_REPORT_CREATING;
    const char *lead;
    uint32_t lead_rgba = theme->muted;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "Problem report", NK_UI_TYPE_TITLE, theme->paper);
    switch (step) {
        case PLAYER_UI_REPORT_PREVIEWING:
            lead = "Listing what the report would hold…";
            break;
        case PLAYER_UI_REPORT_CREATING:
            lead = "Writing the report. Checking the build tools can take a minute…";
            break;
        case PLAYER_UI_REPORT_DONE:
            lead = "The report is written. Open the ZIP and look through it before you attach "
                   "it to an issue: issues on GitHub are public.";
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_REPORT_FAILED:
            lead = "The report could not be written.";
            lead_rgba = theme->warning;
            break;
        case PLAYER_UI_REPORT_NO_PYTHON:
            lead = "Python isn't available here, so the report can't be made from this window. "
                   "Run this command in a terminal from the Nakagawa folder instead.";
            lead_rgba = theme->warning;
            break;
        case PLAYER_UI_REPORT_PREVIEWED:
        case PLAYER_UI_REPORT_IDLE:
        default:
            lead = "Writes a ZIP on this PC with the logs and settings that help someone fix a "
                   "problem. Your user name and folders are replaced, and game files, saves and "
                   "keys are left out. Nothing is uploaded.";
            break;
    }
    nk_ui_text_role(ui, FLOW_ID_LEAD, lead, NK_UI_TYPE_BODY_S, lead_rgba);
    if (scr_wizard_box_begin(ui, 0u)) {
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
        if (step == PLAYER_UI_REPORT_NO_PYTHON) {
            nk_ui_text_role(ui, SET_ID_TEXT + 0x300u, safe_text(model->report_command, ""),
                            NK_UI_TYPE_DATA, theme->paper);
        } else if (step == PLAYER_UI_REPORT_FAILED) {
            nk_ui_text(ui, SET_ID_TEXT + 0x300u,
                       nk_ui_text_clamp(ui, safe_text(model->report_message, ""), &text,
                                        inner - 32.0f, 3), &text);
        } else if (step == PLAYER_UI_REPORT_DONE) {
            nk_ui_text_role(ui, SET_ID_TEXT + 0x300u, safe_text(model->report_path, ""),
                            NK_UI_TYPE_DATA, theme->paper);
        }
        if (model->report_line_count > 0u) {
            size_t shown = model->report_line_count < PLAYER_UI_REPORT_SHOWN
                ? model->report_line_count : PLAYER_UI_REPORT_SHOWN;
            size_t more = player_ui_report_more_count(model);
            float column = (inner - 32.0f - NK_UI_SPACE_4) / 2.0f;
            NkUiLayout cols = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                          NK_UI_SPACE_4);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x301u,
                            step == PLAYER_UI_REPORT_DONE ? "The ZIP holds:" : "The ZIP would hold:",
                            NK_UI_TYPE_BODY_S, theme->muted);
            if (nk_ui_begin_container(ui, SET_ID_TEXT + 0x310u, &cols, NULL)) {
                for (size_t c = 0u; c < 2u; ++c) {
                    NkUiLayout col = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column),
                                                 sizing_fit(), 2.0f);
                    if (!nk_ui_begin_container(ui, (NkUiId)(SET_ID_TEXT + 0x311u + (NkUiId)c),
                                               &col, NULL)) {
                        continue;
                    }
                    for (size_t r = 0u; r < 8u; ++r) {
                        size_t item = c * 8u + r;
                        if (item >= shown) break;
                        nk_ui_text(ui, (NkUiId)(SET_ID_TEXT + 0x320u + (NkUiId)item),
                                   nk_ui_text_clamp(ui, safe_text(model->report_lines[item], ""),
                                                    &text, column, 1), &text);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            if (more > 0u) {
                char more_text[48];
                (void)snprintf(more_text, sizeof(more_text), "and %zu more", more);
                nk_ui_text_role(ui, SET_ID_TEXT + 0x302u, more_text, NK_UI_TYPE_CAPTION,
                                theme->muted);
            }
        }
        nk_ui_end_container(ui);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (step == PLAYER_UI_REPORT_PREVIEWED || step == PLAYER_UI_REPORT_FAILED ||
                step == PLAYER_UI_REPORT_DONE) {
                if (nk_ui_button(ui, PLAYER_UI_ID_REPORT_CREATE,
                                 step == PLAYER_UI_REPORT_DONE ? "Create again"
                                                               : "Create the report",
                                 step == PLAYER_UI_REPORT_PREVIEWED ? NK_UI_BUTTON_PRIMARY : 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_CREATE_REPORT);
                }
            }
            if (step == PLAYER_UI_REPORT_DONE &&
                nk_ui_button(ui, PLAYER_UI_ID_REPORT_FOLDER, "Open folder", NK_UI_BUTTON_PRIMARY)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_REPORT_FOLDER);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back",
                             busy || step == PLAYER_UI_REPORT_NO_PYTHON ? NK_UI_BUTTON_PRIMARY : 0u)) {
                *result = settings_op(PLAYER_UI_SETTINGS_BACK);
            }
            nk_ui_end_container(ui);
        }
    }
}

/* Text only wraps at spaces, so a folder path is one word that would run out of
   the card. Each word longer than max_word bytes keeps its start and its end
   (the folder's own name) around "...", both cut at a character boundary. The
   host's display rule (the profile folder as "~") runs first, on the whole
   text: a cut made before it could keep part of the user name, which the
   draw-time rule can then no longer match. The result is never longer than the
   text, so a buffer that holds the text holds it whole. */
static void settings_shorten_words(NkUi *ui, const char *in, char *out, size_t cap,
                                   size_t max_word) {
    size_t used = 0u;
    if (cap == 0u) return;
    in = nk_ui_frame_text(ui, in);
    while (*in != '\0' && used + 1u < cap) {
        size_t word = strcspn(in, " ");
        if (word > max_word) {
            size_t head = 12u;
            size_t tail = max_word - head - 3u;
            while (head > 0u && ((unsigned char)in[head] & 0xC0u) == 0x80u) --head;
            while (tail > 0u && ((unsigned char)in[word - tail] & 0xC0u) == 0x80u) --tail;
            (void)snprintf(out + used, cap - used, "%.*s…%.*s", (int)head, in, (int)tail,
                           in + word - tail);
        } else {
            (void)snprintf(out + used, cap - used, "%.*s", (int)word, in);
        }
        used += strlen(out + used);
        in += word;
        while (*in == ' ' && used + 1u < cap) out[used++] = *in++;
        out[used] = '\0';
    }
    out[used < cap ? used : cap - 1u] = '\0';
}

/* Settings > Updates: one card that says where the check is, with the next step
   first. Nothing here reaches the network on its own: every button is a click. */
static void settings_updates(NkUi *ui, const PlayerUiSettingsModel *model, float inner,
                             PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    PlayerUiUpdateStep step = model->update_step;
    const char *version = safe_text(model->update_version, "");
    bool busy = step == PLAYER_UI_UPDATE_CHECKING || step == PLAYER_UI_UPDATE_DOWNLOADING ||
                step == PLAYER_UI_UPDATE_INSTALLING;
    char lead[192];
    char line[800]; /* a sentence around a whole folder (folder[] below) */
    char a[32];
    char b[32];
    uint32_t lead_rgba = theme->muted;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "Updates", NK_UI_TYPE_TITLE, theme->paper);
    switch (step) {
        case PLAYER_UI_UPDATE_CHECKING:
            (void)snprintf(lead, sizeof(lead), "Asking GitHub for the newest version…");
            break;
        case PLAYER_UI_UPDATE_CURRENT:
            (void)snprintf(lead, sizeof(lead), "You have the newest version.");
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_UPDATE_AVAILABLE:
            (void)snprintf(lead, sizeof(lead), "Version %s is out.", version);
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_UPDATE_DOWNLOADING:
            (void)snprintf(lead, sizeof(lead), "Downloading version %s…", version);
            break;
        case PLAYER_UI_UPDATE_DOWNLOADED:
            (void)snprintf(lead, sizeof(lead), "Version %s is downloaded and checked.", version);
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_UPDATE_INSTALLING:
            (void)snprintf(lead, sizeof(lead), "Unpacking version %s…", version);
            break;
        case PLAYER_UI_UPDATE_INSTALLED:
            (void)snprintf(lead, sizeof(lead), "Version %s is ready to start.", version);
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_UPDATE_FAILED:
            (void)snprintf(lead, sizeof(lead), "%s",
                           safe_text(model->update_error_title, "Something went wrong"));
            lead_rgba = theme->warning;
            break;
        case PLAYER_UI_UPDATE_IDLE:
        default:
            (void)snprintf(lead, sizeof(lead),
                           "Nakagawa can ask GitHub whether a newer version is out. The request "
                           "carries nothing about you, this PC or your games.");
            break;
    }
    nk_ui_text_role(ui, FLOW_ID_LEAD, lead, NK_UI_TYPE_BODY_S, lead_rgba);
    if (scr_wizard_box_begin(ui, 0u)) {
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
        if (version[0] != '\0') {
            const char *published = safe_text(model->update_published, "");
            char about[48] = "";
            if (model->update_prerelease && published[0] != '\0') {
                (void)snprintf(about, sizeof(about), " (pre-release, %s)", published);
            } else if (model->update_prerelease) {
                (void)snprintf(about, sizeof(about), " (pre-release)");
            } else if (published[0] != '\0') {
                (void)snprintf(about, sizeof(about), " (%s)", published);
            }
            (void)snprintf(line, sizeof(line), "This version: %s    Newest: %s%s",
                           safe_text(model->current_version, "unknown"), version, about);
        } else {
            (void)snprintf(line, sizeof(line), "This version: %s",
                           safe_text(model->current_version, "unknown"));
        }
        nk_ui_text_role(ui, SET_ID_TEXT + 0xD0u, line, NK_UI_TYPE_DATA, theme->paper);
        switch (step) {
            case PLAYER_UI_UPDATE_AVAILABLE:
                if (model->update_notes && model->update_notes[0]) {
                    nk_ui_text(ui, SET_ID_TEXT + 0xD1u,
                               nk_ui_text_clamp(ui, model->update_notes, &text, inner - 32.0f, 4),
                               &text);
                }
                if (model->update_page_url && model->update_page_url[0]) {
                    nk_ui_text_role(ui, SET_ID_TEXT + 0xD5u, model->update_page_url,
                                    NK_UI_TYPE_CAPTION, theme->dim);
                }
                if (!model->update_has_zip) {
                    (void)snprintf(line, sizeof(line),
                                   "This release has no Windows download. Its page on GitHub "
                                   "lists what it offers.");
                } else if (model->update_installed_by_setup) {
                    (void)snprintf(line, sizeof(line),
                                   "This copy was installed with Setup. Download the new "
                                   "Setup from the release page on GitHub and run it: it "
                                   "updates this copy and keeps your games and saves.");
                } else if (model->update_unverifiable) {
                    (void)snprintf(line, sizeof(line),
                                   "This build can't check update signatures, so it doesn't "
                                   "download updates itself. Download the new version from the "
                                   "release page on GitHub.");
                } else if (model->update_unsigned) {
                    (void)snprintf(line, sizeof(line),
                                   "This release isn't signed, so Nakagawa won't download it. "
                                   "Its page on GitHub has the download.");
                } else if (!model->update_can_download) {
                    (void)snprintf(line, sizeof(line),
                                   "Download it from the release page on GitHub.");
                } else {
                    scr_flow_bytes(a, sizeof(a), model->update_zip_bytes);
                    (void)snprintf(line, sizeof(line),
                                   "Download fetches %s (%s) and checks it against the "
                                   "release's signed SHA-256. Nothing is installed until you "
                                   "choose Install.",
                                   safe_text(model->update_zip_name, "the Windows ZIP"), a);
                }
                nk_ui_text(ui, SET_ID_TEXT + 0xD2u, line, &text);
                break;
            case PLAYER_UI_UPDATE_DOWNLOADING:
                nk_ui_progress_bar(ui, SET_ID_TEXT + 0xD3u, inner - 32.0f,
                                   model->update_zip_bytes > 0ull
                                       ? (float)((double)model->update_received_bytes /
                                                 (double)model->update_zip_bytes)
                                       : -1.0f,
                                   0.0);
                scr_flow_bytes(a, sizeof(a), model->update_received_bytes);
                scr_flow_bytes(b, sizeof(b), model->update_zip_bytes);
                (void)snprintf(line, sizeof(line), "%s of %s", a, b);
                nk_ui_text_role(ui, SET_ID_TEXT + 0xD4u, line, NK_UI_TYPE_DATA, theme->muted);
                break;
            case PLAYER_UI_UPDATE_DOWNLOADED: {
                /* Holds any folder whole (NK_MAX_PATH): a shorter buffer cut the
                   folder's own name off its end with no mark. */
                char folder[600];
                settings_shorten_words(ui, safe_text(model->update_folder, "the downloads folder"),
                                       folder, sizeof(folder), 72u);
                (void)snprintf(line, sizeof(line),
                               "Its SHA-256 matches the release's signed checksum. Saved in "
                               "%s. Install unpacks it into a new folder of its own; this "
                               "version stays as it is.", folder);
                nk_ui_text(ui, SET_ID_TEXT + 0xD2u,
                           nk_ui_text_clamp(ui, line, &text, inner - 32.0f, 4), &text);
                break;
            }
            case PLAYER_UI_UPDATE_INSTALLED: {
                char folder[600];
                settings_shorten_words(ui, safe_text(model->update_folder, "a new folder"), folder,
                                       sizeof(folder), 72u);
                (void)snprintf(line, sizeof(line),
                               "It is in %s. Close Nakagawa, then start bin\\nakagawa_player.exe "
                               "from that folder. Your library, saves and settings stay where "
                               "they are and carry over.", folder);
                nk_ui_text(ui, SET_ID_TEXT + 0xD2u,
                           nk_ui_text_clamp(ui, line, &text, inner - 32.0f, 5), &text);
                break;
            }
            case PLAYER_UI_UPDATE_FAILED:
                text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
                settings_shorten_words(ui, safe_text(model->update_error_message, ""), line,
                                       sizeof(line), 72u);
                nk_ui_text(ui, SET_ID_TEXT + 0xD2u,
                           nk_ui_text_clamp(ui, line, &text, inner - 32.0f, 5), &text);
                break;
            default:
                break;
        }
        if (!busy &&
            scr_wizard_switch(ui, PLAYER_UI_ID_UPDATE_AT_START, SET_ID_TEXT + 0xE0u,
                          "Check at start",
                          "Asks GitHub once each time Nakagawa starts. Off unless you turn it "
                          "on.",
                          model->check_updates_at_start, inner - 32.0f, NULL)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_UPDATE_AT_START);
        }
        nk_ui_end_container(ui);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            switch (step) {
                case PLAYER_UI_UPDATE_AVAILABLE:
                    if (model->update_can_download &&
                        nk_ui_button(ui, PLAYER_UI_ID_UPDATE_DOWNLOAD, "Download",
                                     NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_DOWNLOAD_UPDATE);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_PAGE, "Open release page",
                                     model->update_can_download ? 0u : NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_RELEASE_PAGE);
                    }
                    break;
                case PLAYER_UI_UPDATE_DOWNLOADING:
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_CANCEL, "Cancel", 0u)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_CANCEL_UPDATE);
                    }
                    break;
                case PLAYER_UI_UPDATE_DOWNLOADED:
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_INSTALL, "Install",
                                     NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_INSTALL_UPDATE);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_FOLDER, "Open folder", 0u)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_UPDATE_FOLDER);
                    }
                    break;
                case PLAYER_UI_UPDATE_INSTALLED:
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_FOLDER, "Open folder",
                                     NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_UPDATE_FOLDER);
                    }
                    break;
                case PLAYER_UI_UPDATE_IDLE:
                case PLAYER_UI_UPDATE_CURRENT:
                case PLAYER_UI_UPDATE_FAILED:
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_CHECK,
                                     step == PLAYER_UI_UPDATE_IDLE ? "Check for updates"
                                                                   : "Check again",
                                     NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_CHECK_UPDATES);
                    }
                    /* A refused download still has its release page, where the
                       user can download it by hand. */
                    if (step == PLAYER_UI_UPDATE_FAILED &&
                        model->update_failed_step == PLAYER_UI_UPDATE_DOWNLOADING &&
                        model->update_page_url && model->update_page_url[0] &&
                        nk_ui_button(ui, PLAYER_UI_ID_UPDATE_PAGE, "Open release page", 0u)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_RELEASE_PAGE);
                    }
                    /* A failed install leaves the checked ZIP to unpack by hand. */
                    if (step == PLAYER_UI_UPDATE_FAILED &&
                        model->update_failed_step == PLAYER_UI_UPDATE_INSTALLING &&
                        model->update_folder && model->update_folder[0] &&
                        nk_ui_button(ui, PLAYER_UI_ID_UPDATE_FOLDER, "Open folder", 0u)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_UPDATE_FOLDER);
                    }
                    break;
                default:
                    break;
            }
            if (step != PLAYER_UI_UPDATE_DOWNLOADING &&
                nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back",
                             busy ? NK_UI_BUTTON_PRIMARY : 0u)) {
                *result = settings_op(PLAYER_UI_SETTINGS_BACK);
            }
            nk_ui_end_container(ui);
        }
    }
}

/* Settings > Accessibility: the interface size, the text size, the
   high-contrast focus ring and Reduce motion. Each button steps through its
   values; Automatic follows the system, and the sentence says what the system
   asks for now. */
static void settings_accessibility(NkUi *ui, const PlayerUiSettingsModel *model, float inner,
                                   PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const char *system = safe_text(model->system_name, "your system");
    char interface_line[256];
    char contrast_line[256];
    char motion_line[256];
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "Accessibility", NK_UI_TYPE_TITLE, theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    "These change the launcher, not a running game. Automatic follows the "
                    "matching setting of your system, and changes when it does.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    (void)snprintf(interface_line, sizeof(interface_line),
                   "Grows text and controls in a large enough window. Display scale in "
                   "%s: %d%%.",
                   system, model->system_scale_pct);
    (void)snprintf(contrast_line, sizeof(contrast_line),
                   "A white ring between black lines marks the selection. High contrast "
                   "in %s: %s.",
                   system, model->system_high_contrast ? "on" : "off");
    (void)snprintf(motion_line, sizeof(motion_line),
                   "Holds the background still; pulses and sweeps become cuts. "
                   "Animations in %s: %s.",
                   system, model->system_reduced_motion ? "off" : "on");
    if (scr_wizard_box_begin(ui, 0u)) {
        const WizardRow rows[] = {
            {"Interface", safe_text(model->interface_size, "Automatic"), interface_line},
            {"Text size", safe_text(model->text_size, "Standard"),
             "Makes the launcher's text larger. Controls keep their size."},
            {"Contrast", safe_text(model->high_contrast_choice, "Automatic"), contrast_line},
            {"Motion", safe_text(model->reduce_motion_choice, "Automatic"), motion_line}
        };
        /* The four buttons line up on one column at every text size. */
        WizardColumn column = scr_wizard_column(ui, rows, sizeof(rows) / sizeof(rows[0]),
                                                inner - 32.0f, false);
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_INTERFACE_SIZE, SET_ID_TEXT + 0x300u,
                         rows[0].name, rows[0].value, rows[0].sentence, false, inner - 32.0f,
                         &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_INTERFACE_SIZE);
        }
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_TEXT_SIZE, SET_ID_TEXT + 0x30Cu,
                         rows[1].name, rows[1].value, rows[1].sentence, false, inner - 32.0f,
                         &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_TEXT_SIZE);
        }
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_HIGH_CONTRAST, SET_ID_TEXT + 0x304u,
                         rows[2].name, rows[2].value, rows[2].sentence, model->high_contrast,
                         inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_HIGH_CONTRAST);
        }
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_REDUCE_MOTION, SET_ID_TEXT + 0x308u,
                         rows[3].name, rows[3].value, rows[3].sentence, model->reduce_motion,
                         inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_REDUCE_MOTION);
        }
        nk_ui_end_container(ui);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                *result = settings_op(PLAYER_UI_SETTINGS_BACK);
            }
            nk_ui_end_container(ui);
        }
    }
}

/* Settings > Enhanced (#2055; src/rt/ace.h): the global graphics preset and the play mode above
   six category tabs, the active tab's options as one compact row each, and one panel for the
   focused option's name, value, help and caveats. The library's page sets the defaults; a
   title's page sets that game's choices, where a part on Library default follows the library.
   A row is one focus stop: Left and Right step its value, the select button takes the next
   one, and a pointer picks any value. A row that cannot apply stays in the walk, dimmed and
   inert, so the panel can say why. The rows and the two pinned controls keep the shared
   PLAYER_UI_ID_* ids; the ids below are the page's own parts. */
#define SET_ID_ENHANCED_TABS SET_ID(0x000520u)
#define SET_ID_ENHANCED_TABS_SPAN 0x10u
#define SET_ID_ENHANCED_PAGE SET_ID(0x000540u)
#define SET_ID_ENHANCED_MAIN SET_ID(0x000541u)
#define SET_ID_ENHANCED_LEFT SET_ID(0x000542u)
#define SET_ID_ENHANCED_PINNED SET_ID(0x000543u)
#define SET_ID_ENHANCED_LIST_ROW SET_ID(0x000544u)
#define SET_ID_ENHANCED_SCROLLBAR SET_ID(0x000545u)
#define SET_ID_ENHANCED_LIST SET_ID(0x000550u)       /* + the tab: one scroll position each */
#define SET_ID_ENHANCED_LIST_SPAN 0x08u
/* A part's values as pointer targets: part p's choice c. Eight choices a part, forty parts. */
#define SET_ID_ENHANCED_CHOICES(part, choice) \
    SET_ID(0x000600u + (uint32_t)(part) * 0x20u + (uint32_t)(choice) * 4u)
#define SET_ID_ENHANCED_DETAIL PLAYER_UI_ID_ENHANCED_DETAIL     /* SET_ID(0x000B00u) */
#define SET_ID_ENHANCED_DETAIL_BODY PLAYER_UI_ID_ENHANCED_DETAIL_BODY
#define SET_ID_ENHANCED_DETAIL_TRACK SET_ID(0x000B02u)
typedef char settings_enhanced_detail_ids_are_the_screens[
    PLAYER_UI_ID_ENHANCED_DETAIL == SET_ID(0x000B00u) &&
    PLAYER_UI_ID_ENHANCED_DETAIL_BODY == SET_ID(0x000B01u) ? 1 : -1];
#define SET_ID_ENHANCED_DIVIDER SET_ID(0x000D00u)    /* + the row's place in its list */
#define SET_ID_ENHANCED_DIVIDER_SPAN 0x40u
typedef char settings_enhanced_tabs_have_lists[
    PLAYER_UI_ENHANCED_GROUP_COUNT <= SET_ID_ENHANCED_LIST_SPAN ? 1 : -1];
typedef char settings_enhanced_choice_ids_end_before_the_panel[
    0x000600u + PLAYER_UI_ENHANCED_ROWS * 0x20u <= 0x000B00u ? 1 : -1];
typedef char settings_enhanced_rows_have_dividers[
    PLAYER_UI_ENHANCED_ROWS + 3 <= SET_ID_ENHANCED_DIVIDER_SPAN ? 1 : -1];

#define ENHANCED_BOX_PAD 8.0f        /* inside the list box */
#define ENHANCED_BOX_TOP 12.0f       /* above its first row: room for that row's focus ring */
#define ENHANCED_BOX_GAP 2.0f        /* between the pinned rows, the tabs and the list */
#define ENHANCED_PAGE_GAP 14.0f      /* between the page's parts, as in the card */
/* Inside the scrolling list, around its rows: room for a row's focus ring, the high-contrast
   one included, which the list would clip. */
#define ENHANCED_LIST_PAD 8.0f

#define ENHANCED_SCROLLBAR_ROOM 8.0f /* right of the list, kept on every tab so rows line up */
#define ENHANCED_TABS_HEIGHT 44.0f
#define ENHANCED_STEP_ARROW 28.0f
#define ENHANCED_CHIP_GAP 4.0f

/* A row and its value box keep the design system's target sizes (44 and 34 px) and grow with
   the text size. */
static float settings_enhanced_row_height(const NkUi *ui) {
    NkUiTextStyle name = nk_ui_type(NK_UI_TYPE_BODY, 0u);
    return nk_ui_line_box(ui, NK_UI_CONTROL_HEIGHT, &name);
}

static float settings_enhanced_value_height(const NkUi *ui) {
    NkUiTextStyle value = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
    return nk_ui_line_box(ui, NK_UI_CONTROL_HEIGHT_SMALL, &value);
}

static float settings_enhanced_chip_width(const NkUi *ui, const char *label) {
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
    float width = ceilf(nk_ui_text_width(ui, label, &text)) + 2.0f * NK_UI_SPACE_3;
    return width < NK_UI_CONTROL_HEIGHT ? NK_UI_CONTROL_HEIGHT : width;
}

/* How wide a "< value >" box is for the widest of these values, so stepping never resizes it. */
static float settings_enhanced_stepper_width(const NkUi *ui, const char *const *values, int count,
                                             const char *extra) {
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
    float widest = extra != NULL ? nk_ui_text_width(ui, extra, &text) : 0.0f;
    for (int i = 0; i < count; ++i) {
        float width = values[i] != NULL ? nk_ui_text_width(ui, values[i], &text) : 0.0f;
        if (width > widest) widest = width;
    }
    return ceilf(widest) + 2.0f * ENHANCED_STEP_ARROW + 2.0f * NK_UI_SPACE_2;
}

/* Opens one option row, the focus stop: its name, and its tag beside the name, on the left. The
   caller adds the value on the right and closes the row. A blank row is in the list and in the
   walk with nothing drawn in it (settings_enhanced_row_blank). */
static bool settings_enhanced_row_begin(NkUi *ui, NkUiId id, const char *name, const char *tag,
                                        bool dimmed, uint32_t flags, float name_limit,
                                        NkUiId up, NkUiId down, bool blank) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool focused = nk_ui_button_focused(ui, id);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(),
                                 sizing_fixed(settings_enhanced_row_height(ui)), NK_UI_SPACE_3);
    NkUiStyle style;
    NkUiTextStyle name_style = nk_ui_type(NK_UI_TYPE_BODY, dimmed ? theme->muted : theme->paper);
    memset(&style, 0, sizeof(style));
    row.align_y = NK_UI_ALIGN_CENTER;
    row.padding_left = row.padding_right = NK_UI_SPACE_3;
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    style.fill_rgba = nk_ui_pressed(ui, id) ? theme->pressed
                      : focused || nk_ui_hovered(ui, id) ? theme->surface_high : 0u;
    name_style.wrap = false;
    if (name_limit < 40.0f) name_limit = 40.0f;
    nk_ui_register_button_between(ui, id, flags, up, down);
    if (!nk_ui_begin_container(ui, id, &row, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    if (!blank) {
        nk_ui_text(ui, nk_ui_child_id(id, 2u),
                   nk_ui_text_clamp(ui, safe_text(name, ""), &name_style, name_limit, 1),
                   &name_style);
        if (tag != NULL) nk_ui_tag(ui, nk_ui_child_id(id, 3u), tag);
    }
    nk_ui_spacer(ui, nk_ui_child_id(id, 1u), sizing_grow(), sizing_fixed(1.0f));
    return true;
}

/* One arrow of a "< value >" box: a pointer target when the row can change. */
static bool settings_enhanced_arrow(NkUi *ui, NkUiId id, const char *mark, bool live) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(ENHANCED_STEP_ARROW),
                                  sizing_grow(), 0.0f);
    NkUiStyle style;
    bool hovered = live && nk_ui_hovered(ui, id);
    bool clicked = false;
    memset(&style, 0, sizeof(style));
    cell.align_x = NK_UI_ALIGN_CENTER;
    cell.align_y = NK_UI_ALIGN_CENTER;
    style.corner_radius = NK_UI_RADIUS_CONTROL - 1.0f;
    if (live) {
        nk_ui_register_button(ui, id, NK_UI_BUTTON_POINTER_ONLY);
        style.fill_rgba = nk_ui_pressed(ui, id) ? theme->pressed
                          : hovered ? theme->surface_high : 0u;
    }
    if (nk_ui_begin_container(ui, id, &cell, &style)) {
        nk_ui_text_role(ui, nk_ui_child_id(id, 1u), mark, NK_UI_TYPE_LABEL,
                        !live ? theme->dim : hovered ? theme->paper : theme->muted);
        nk_ui_end_container(ui);
    }
    if (live) clicked = nk_ui_take_activation(ui, id);
    return clicked;
}

/* A row's value as "< value >" in its own box, the row's part 4. Returns -1 or 1 when the
   pointer clicked an arrow; the keys go to the row. */
static int settings_enhanced_stepper(NkUi *ui, NkUiId row_id, const char *value, float width,
                                     bool dimmed, bool value_muted) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiId prev = nk_ui_child_id(row_id, 5u);
    NkUiId next = nk_ui_child_id(row_id, 6u);
    bool row_focused = nk_ui_button_focused(ui, row_id) || nk_ui_button_focused(ui, prev) ||
                       nk_ui_button_focused(ui, next);
    NkUiLayout box = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(width),
                                 sizing_fixed(settings_enhanced_value_height(ui)), 0.0f);
    NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    NkUiStyle style;
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL,
                                    dimmed || value_muted ? theme->muted : theme->paper);
    int step = 0;
    memset(&style, 0, sizeof(style));
    box.align_y = NK_UI_ALIGN_CENTER;
    cell.align_x = NK_UI_ALIGN_CENTER;
    cell.align_y = NK_UI_ALIGN_CENTER;
    style.fill_rgba = theme->surface;
    style.border_rgba = row_focused && !dimmed ? theme->accent_dark : theme->line_strong;
    style.border_width = NK_UI_BORDER_HAIRLINE;
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    if (!nk_ui_begin_container(ui, nk_ui_child_id(row_id, 4u), &box, &style)) return 0;
    if (settings_enhanced_arrow(ui, prev, "<", !dimmed)) step = -1;
    if (nk_ui_begin_container(ui, nk_ui_child_id(row_id, 7u), &cell, NULL)) {
        nk_ui_text(ui, nk_ui_child_id(row_id, 8u),
                   nk_ui_text_clamp(ui, safe_text(value, ""), &text,
                                    width - 2.0f * ENHANCED_STEP_ARROW - NK_UI_SPACE_2, 1),
                   &text);
        nk_ui_end_container(ui);
    }
    if (settings_enhanced_arrow(ui, next, ">", !dimmed)) step = 1;
    nk_ui_end_container(ui);
    /* The keys stay with the row: a clicked arrow hands focus back to it. */
    if (nk_ui_button_focused(ui, prev) || nk_ui_button_focused(ui, next)) nk_ui_focus(ui, row_id);
    return step;
}

/* What the keys did to the row `id`: -1 or 1 for Left and Right on the focused row, and
   whether the select button, or a click on the row itself, asked for the next value. A press
   is the row's when the row had the focus as it was made (nk_ui_take_activation), so a row
   takes its press even when a move queued after it has taken the focus on in the same
   frame. */
static int settings_enhanced_row_keys(NkUi *ui, NkUiId id, bool *activated) {
    NkUiNavDir direction = NK_UI_DIR_NONE;
    *activated = false;
    if (nk_ui_button_focused(ui, id) && nk_ui_take_event(ui, NK_UI_EV_NAV, &direction)) {
        return direction == NK_UI_DIR_LEFT ? -1 : direction == NK_UI_DIR_RIGHT ? 1 : 0;
    }
    *activated = nk_ui_take_activation(ui, id);
    return 0;
}

/* A pinned row or Frame pacing: a name and "< value >". Sets *step to -1 or 1 for Left, Right
   or a clicked arrow, and *activated for the select button; returns whether it has focus. */
static bool settings_enhanced_step_row(NkUi *ui, NkUiId id, const char *name, const char *value,
                                       float width, float box_width, NkUiId up, NkUiId down,
                                       bool blank, int *step, bool *activated) {
    bool focused;
    int keys;
    *step = 0;
    *activated = false;
    if (!settings_enhanced_row_begin(ui, id, name, NULL, false, NK_UI_BUTTON_OWNS_HORIZONTAL,
                                     width - box_width - 3.0f * NK_UI_SPACE_3, up, down,
                                     blank)) {
        return false;
    }
    if (!blank) *step = settings_enhanced_stepper(ui, id, value, box_width, false, false);
    nk_ui_end_container(ui);
    focused = nk_ui_button_focused(ui, id);
    keys = settings_enhanced_row_keys(ui, id, activated);
    if (keys != 0) *step = keys;
    return focused;
}

/* One value of a part, in its own box. A pointer picks it; the keys go to its row. `on` is
   whether the row's value is other than Off: the accent marks a value that is on or chosen
   (16x, On), and a selected Off is marked as the library's value is, by the raised box with
   an edge that stands 3:1 or more from the list and from the box (selected_neutral_edge),
   and by primary text, so a tab of options that are off is not a column of accent boxes. A
   dimmed row's mark has a weaker edge (selected_neutral_edge_dimmed, still 3:1 from the
   list), so that an inert row does not look as live as the rows around it. */
static bool settings_enhanced_chip(NkUi *ui, NkUiId id, const char *label, bool selected,
                                   bool inherited, bool dimmed, bool on) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool live = !dimmed;
    bool hovered = live && nk_ui_hovered(ui, id);
    bool lit = selected && on && !inherited && !dimmed;
    NkUiLayout chip = layout_make(NK_UI_LAYOUT_ROW,
                                  sizing_fixed(settings_enhanced_chip_width(ui, label)),
                                  sizing_fixed(settings_enhanced_value_height(ui)), 0.0f);
    NkUiStyle style;
    /* Ink on the accent fill, as a primary button; a selected Off, the library's value and a
       dimmed row's value are marked by a raised box and never by the accent. */
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL,
                                    lit ? theme->ink
                                    : selected || hovered ? theme->paper
                                    : dimmed ? theme->dim : theme->muted);
    memset(&style, 0, sizeof(style));
    chip.align_x = NK_UI_ALIGN_CENTER;
    chip.align_y = NK_UI_ALIGN_CENTER;
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    style.fill_rgba = lit ? (live && nk_ui_pressed(ui, id) ? theme->accent_pressed
                             : hovered ? theme->accent_hover : theme->accent_dark)
                      : live && nk_ui_pressed(ui, id) ? theme->pressed
                      : selected || hovered ? theme->surface_high : theme->surface;
    style.border_rgba = lit ? style.fill_rgba
                        : !selected ? theme->line
                        : dimmed ? theme->selected_neutral_edge_dimmed
                                 : theme->selected_neutral_edge;
    style.border_width = NK_UI_BORDER_HAIRLINE;
    if (selected && dimmed) text.rgba = theme->muted;
    if (selected && inherited && !dimmed) text.rgba = theme->muted;
    if (live) nk_ui_register_button(ui, id, NK_UI_BUTTON_POINTER_ONLY);
    if (nk_ui_begin_container(ui, id, &chip, &style)) {
        nk_ui_text(ui, nk_ui_child_id(id, 1u), label, &text);
        nk_ui_end_container(ui);
    }
    return live && nk_ui_take_activation(ui, id);
}

static void settings_enhanced_choice_action(PlayerUiAction *result, int part, bool title,
                                            int choice) {
    *result = settings_op(PLAYER_UI_SETTINGS_SET_ENHANCED_PART);
    result->title_index = (size_t)part;
    result->choice_index = title ? choice - 1 : choice;
}

/* A part's row: its name, its tag, and every value it can take, the current one marked. When
   the values do not fit beside the name (a larger text size) the row shows "< value >"
   instead. Returns whether the row has focus. */
static bool settings_enhanced_part_row(NkUi *ui, const PlayerUiSettingsModel *model,
                                       const PlayerUiEnhancedRow *row, float width, NkUiId up,
                                       bool blank, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const bool title = model->enhanced_title != NULL;
    const int choice_count = row->label_count + (title ? 1 : 0);
    const NkUiId id = PLAYER_UI_ID_ENHANCED_PART(row->part);
    const char *labels[8];
    NkUiTextStyle name_style = nk_ui_type(NK_UI_TYPE_BODY, theme->paper);
    NkUiTextStyle tag_style = nk_ui_type(NK_UI_TYPE_LABEL_S, theme->muted);
    float inner = width - 2.0f * NK_UI_SPACE_3;
    float choices_width = 0.0f;
    float tag_width = row->tag != NULL
        ? ceilf(nk_ui_text_width(ui, nk_ui_caps(ui, row->tag), &tag_style)) + 20.0f + NK_UI_SPACE_3
        : 0.0f;
    float name_width = ceilf(nk_ui_text_width(ui, safe_text(row->name, ""), &name_style));
    int selected = row->selected_index;
    int step = 0;
    int picked = -1;
    bool focused = false;
    bool activated = false;
    bool compact;
    if (choice_count <= 0 || choice_count > (int)(sizeof(labels) / sizeof(labels[0]))) {
        return false;
    }
    if (selected < 0 || selected >= choice_count) selected = 0;
    for (int choice = 0; choice < choice_count; ++choice) {
        labels[choice] = title && choice == 0 ? safe_text(row->library_value, "Library")
                                              : safe_text(row->labels[choice - (title ? 1 : 0)],
                                                          "Off");
        choices_width += settings_enhanced_chip_width(ui, labels[choice]);
    }
    choices_width += ENHANCED_CHIP_GAP * (float)(choice_count - 1);
    compact = name_width + NK_UI_SPACE_3 + tag_width + choices_width > inner;
    if (compact) {
        choices_width = settings_enhanced_stepper_width(ui, labels, choice_count, NULL);
        if (choices_width > inner * 0.6f) choices_width = inner * 0.6f;
    }
    if (!settings_enhanced_row_begin(ui, id, row->name, row->tag, row->disabled,
                                     NK_UI_BUTTON_OWNS_HORIZONTAL,
                                     inner - NK_UI_SPACE_3 - tag_width - choices_width, up, 0u,
                                     blank)) {
        return false;
    }
    if (blank) {
        /* nothing: see settings_enhanced_row_blank */
    } else if (compact) {
        step = settings_enhanced_stepper(ui, id, labels[selected], choices_width, row->disabled,
                                         title && selected == 0);
    } else {
        NkUiLayout choices = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         ENHANCED_CHIP_GAP);
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 4u), &choices, NULL)) {
            for (int choice = 0; choice < choice_count; ++choice) {
                NkUiId chip = SET_ID_ENHANCED_CHOICES(row->part, choice);
                if (settings_enhanced_chip(ui, chip, labels[choice], choice == selected,
                                           title && choice == 0, row->disabled, row->on)) {
                    picked = choice;
                }
                /* The keys stay with the row: a clicked value hands focus back to it. */
                if (nk_ui_button_focused(ui, chip)) nk_ui_focus(ui, id);
            }
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
    focused = nk_ui_button_focused(ui, id);
    {
        int keys = settings_enhanced_row_keys(ui, id, &activated);
        if (keys != 0) step = keys;
    }
    if (row->disabled) return focused;
    if (picked >= 0) {
        settings_enhanced_choice_action(result, row->part, title, picked);
    } else if (step != 0) {
        settings_enhanced_choice_action(result, row->part, title,
                                        (selected + step + choice_count) % choice_count);
    } else if (activated) {
        *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_ENHANCED_PART);
        result->title_index = (size_t)row->part;
    }
    return focused;
}

/* A game's texture pack rows: the name, what is chosen, and the action in its own box. */
static bool settings_enhanced_action_row(NkUi *ui, NkUiId id, const char *name,
                                         const char *value, const char *action, float width,
                                         NkUiId up, bool blank, PlayerUiAction *result,
                                         PlayerUiSettingsOp op) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle name_style = nk_ui_type(NK_UI_TYPE_BODY, theme->paper);
    NkUiTextStyle value_text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
    NkUiTextStyle action_text = nk_ui_type(NK_UI_TYPE_LABEL, theme->paper);
    NkUiLayout button = layout_make(NK_UI_LAYOUT_ROW,
                                    sizing_fixed(settings_enhanced_chip_width(ui, action)),
                                    sizing_fixed(settings_enhanced_value_height(ui)), 0.0f);
    NkUiStyle button_style;
    float inner = width - 2.0f * NK_UI_SPACE_3;
    float name_width = ceilf(nk_ui_text_width(ui, name, &name_style));
    float value_room = inner - name_width - settings_enhanced_chip_width(ui, action) -
                       2.0f * NK_UI_SPACE_3;
    bool focused;
    memset(&button_style, 0, sizeof(button_style));
    value_text.wrap = false;
    button.align_x = NK_UI_ALIGN_CENTER;
    button.align_y = NK_UI_ALIGN_CENTER;
    if (!settings_enhanced_row_begin(ui, id, name, NULL, false, 0u, name_width + 1.0f, up, 0u,
                                     blank)) {
        return false;
    }
    focused = nk_ui_button_focused(ui, id);
    if (blank) {
        nk_ui_end_container(ui);
        if (nk_ui_take_activation(ui, id)) *result = settings_op(op);
        return focused;
    }
    if (value != NULL && value[0] != '\0' && value_room >= 60.0f) {
        /* The whole value is in the panel; here it is cut to the room the row has. */
        nk_ui_text(ui, nk_ui_child_id(id, 4u),
                   nk_ui_text_clamp(ui, value, &value_text, value_room, 1), &value_text);
    }
    button_style.fill_rgba = theme->surface;
    button_style.border_rgba = focused ? theme->accent_dark : theme->line_strong;
    button_style.border_width = NK_UI_BORDER_HAIRLINE;
    button_style.corner_radius = NK_UI_RADIUS_CONTROL;
    if (nk_ui_begin_container(ui, nk_ui_child_id(id, 5u), &button, &button_style)) {
        nk_ui_text(ui, nk_ui_child_id(id, 6u), action, &action_text);
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
    if (nk_ui_take_activation(ui, id)) *result = settings_op(op);
    return focused;
}

/* What the panel says about one control. */
typedef struct {
    bool focused;          /* it is the focused control, not the tab's first one */
    const char *name;
    const char *value;
    const char *sentence;
    const char *note;      /* why the row is dimmed, or the preset's note; NULL for none */
    const char *tag;       /* the row's tag ("Ace only"); NULL for none */
    const char *scope;     /* on a game's page, whose value it is; NULL for none */
    bool value_muted;      /* the library's value on a game's page, or a dimmed row's */
} SettingsEnhancedDetail;

/* A thin thumb beside a list (or the panel's column) that scrolls: where the window on its
   content is. `id` is the track's, the thumb's ids its children. */
static void settings_enhanced_scrollbar(NkUi *ui, NkUiId id, NkUiId list) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout track = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(ENHANCED_SCROLLBAR_ROOM),
                                   sizing_grow(), 0.0f);
    NkUiScrollState state;
    track.align_x = NK_UI_ALIGN_END;
    if (!nk_ui_begin_container(ui, id, &track, NULL)) return;
    if (nk_ui_scroll_state(ui, list, &state) && state.max_offset > 0.5f &&
        state.viewport > 1.0f) {
        float content = state.max_offset + state.viewport;
        float thumb = state.viewport * state.viewport / content;
        float top;
        NkUiStyle fill;
        NkUiLayout bar;
        if (thumb < 24.0f) thumb = 24.0f;
        if (thumb > state.viewport) thumb = state.viewport;
        top = (state.viewport - thumb) * (state.offset / state.max_offset);
        if (top < 0.0f) top = 0.0f;
        if (top > state.viewport - thumb) top = state.viewport - thumb;
        memset(&fill, 0, sizeof(fill));
        fill.fill_rgba = theme->line_strong;
        fill.corner_radius = 2.0f;
        bar = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(4.0f), sizing_fixed(thumb), 0.0f);
        nk_ui_spacer(ui, nk_ui_child_id(id, 1u), sizing_fixed(4.0f), sizing_fixed(top));
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 2u), &bar, &fill)) {
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* What the panel showed last, so that another option's text starts at the panel's top: the
   control with focus and the option's name (a name longer than the copy is told apart by its
   length and its first bytes). The page is drawn from one thread. */
static struct {
    const NkUi *ui;
    NkUiId focused;
    size_t name_length;
    char name[128];
} s_enhanced_detail;

/* Records what the panel shows this frame; true when that is not what it showed last. */
static bool settings_enhanced_detail_changed(NkUi *ui, const SettingsEnhancedDetail *detail) {
    const char *name = safe_text(detail->name, "");
    const size_t length = strlen(name);
    const NkUiId focused = nk_ui_focused(ui);
    const bool changed = s_enhanced_detail.ui != ui || s_enhanced_detail.focused != focused ||
                         s_enhanced_detail.name_length != length ||
                         strncmp(s_enhanced_detail.name, name, sizeof(s_enhanced_detail.name) - 1u) != 0;
    if (changed) {
        s_enhanced_detail.ui = ui;
        s_enhanced_detail.focused = focused;
        s_enhanced_detail.name_length = length;
        snprintf(s_enhanced_detail.name, sizeof(s_enhanced_detail.name), "%s", name);
    }
    return changed;
}

/* Where the panel's column is scrolled this frame. A new option shows from the top. Page Up
   and Page Down (the pad's triggers) move it a column less one line, as on the developer log,
   so a line carries over; the wheel and a drag scroll it as any scroll container. Focus stays
   on the row: the panel is read, never focused. */
static void settings_enhanced_detail_scroll(NkUi *ui, const SettingsEnhancedDetail *detail,
                                            float line) {
    const bool fresh = settings_enhanced_detail_changed(ui, detail);
    NkUiScrollState state;
    NkUiNavDir page;
    bool known;
    float offset;
    float start;
    known = nk_ui_scroll_state(ui, SET_ID_ENHANCED_DETAIL_BODY, &state);
    offset = fresh || !known ? 0.0f : state.offset;
    start = offset;
    while (nk_ui_take_event(ui, NK_UI_EV_PAGE, &page)) {
        float step = known && state.viewport > 2.0f * line ? state.viewport - line : line;
        offset += page == NK_UI_DIR_RIGHT ? step : -step;
    }
    if (known && offset > state.max_offset) offset = state.max_offset;
    if (offset < 0.0f) offset = 0.0f;
    if (fresh || fabsf(offset - start) >= 0.5f) {
        nk_ui_scroll_to(ui, SET_ID_ENHANCED_DETAIL_BODY, offset);
    }
}

/* The panel beside the list: the option's name, value and whole description. It is exactly as
   tall as the list box beside it (a percentage of the row they share, so its text never makes
   the row taller), and its text is a column inside it that scrolls where it is taller than
   that, with a thumb at the panel's right edge, as the list has. Every sentence wraps whole at
   its words, and a word wider than the column (a texture pack folder's name, a long compound)
   is broken across lines inside it (nk_ui_text_break), so nothing is cut or drawn past the
   panel. */
static void settings_enhanced_details(NkUi *ui, const SettingsEnhancedDetail *detail,
                                      float width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout panel = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(width), sizing_grow(),
                                   NK_UI_SPACE_1);
    NkUiStyle style = scr_flow_box_style(theme, theme->surface);
    NkUiTextStyle name = nk_ui_type(NK_UI_TYPE_HEADING, theme->paper);
    NkUiTextStyle value = nk_ui_type(NK_UI_TYPE_BODY,
                                     detail->value_muted ? theme->muted : theme->paper);
    NkUiTextStyle sentence = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
    /* The text's column: the panel less its padding, the gap and the thumb's track. The
       thumb sits in the panel's right padding, where the list's sits in its box's. */
    const float column = width - NK_UI_SPACE_4 - ENHANCED_BOX_PAD - NK_UI_SPACE_1 -
                         ENHANCED_SCROLLBAR_ROOM;
    NkUiLayout body = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column), sizing_grow(),
                                  NK_UI_SPACE_3);
    /* As tall as the row it shares with the list box, which only that box sizes. */
    panel.height.kind = NK_UI_SIZE_PERCENT;
    panel.height.value = 1.0f;
    panel.padding_left = NK_UI_SPACE_4;
    panel.padding_right = ENHANCED_BOX_PAD;
    panel.padding_top = panel.padding_bottom = NK_UI_SPACE_4;
    body.scroll = true;
    settings_enhanced_detail_scroll(ui, detail, nk_ui_line_height(ui, &sentence));
    if (!nk_ui_begin_container(ui, SET_ID_ENHANCED_DETAIL, &panel, &style)) return;
    if (nk_ui_begin_container(ui, SET_ID_ENHANCED_DETAIL_BODY, &body, NULL)) {
        if (detail->focused) nk_ui_eyebrow(ui, SET_ID_TEXT + 0x701u, "Focused option");
        nk_ui_text(ui, SET_ID_TEXT + 0x702u,
                   nk_ui_text_break(ui, safe_text(detail->name, "Enhanced"), &name, column),
                   &name);
        if (detail->value != NULL && detail->value[0] != '\0') {
            nk_ui_text(ui, SET_ID_TEXT + 0x703u,
                       nk_ui_text_break(ui, detail->value, &value, column), &value);
        }
        if (detail->tag != NULL || detail->scope != NULL) {
            /* The tags wrap onto a second row where both do not fit (a larger text size). */
            const char *tags[2] = {detail->tag, detail->scope};
            NkUiTextStyle label = nk_ui_type(NK_UI_TYPE_LABEL_S, 0u);
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_2);
            NkUiFlowRow flow;
            if (nk_ui_flow_begin(ui, &flow, SET_ID_TEXT + 0x708u, &row, column)) {
                for (int i = 0; i < 2; ++i) {
                    if (tags[i] == NULL) continue;
                    /* nk_ui_tag: its caps and 10 px either side. */
                    nk_ui_flow_item(ui, &flow,
                                    ceilf(nk_ui_text_width(ui, nk_ui_caps(ui, tags[i]), &label)) +
                                        20.0f);
                    nk_ui_tag(ui, SET_ID_TEXT + 0x704u + (uint32_t)i, tags[i]);
                }
                nk_ui_flow_end(ui, &flow);
            }
        }
        if (detail->sentence != NULL && detail->sentence[0] != '\0') {
            nk_ui_text(ui, SET_ID_TEXT + 0x706u,
                       nk_ui_text_break(ui, detail->sentence, &sentence, column), &sentence);
        }
        if (detail->note != NULL && detail->note[0] != '\0') {
            nk_ui_text(ui, SET_ID_TEXT + 0x707u,
                       nk_ui_text_break(ui, detail->note, &sentence, column), &sentence);
        }
        nk_ui_end_container(ui);
    }
    settings_enhanced_scrollbar(ui, SET_ID_ENHANCED_DETAIL_TRACK, SET_ID_ENHANCED_DETAIL_BODY);
    nk_ui_end_container(ui);
}

static void settings_enhanced_row_detail(SettingsEnhancedDetail *detail,
                                         const PlayerUiEnhancedRow *row, bool title,
                                         bool focused) {
    memset(detail, 0, sizeof(*detail));
    detail->focused = focused;
    detail->name = row->name;
    detail->value = row->value;
    detail->sentence = row->sentence;
    detail->note = row->disabled ? row->disabled_reason : NULL;
    detail->tag = row->tag;
    /* Whose value it is; a value the page forces (a dimmed row's Off) is nobody's choice. */
    detail->scope = !title ? NULL
                    : row->inherited ? "Library default"
                    : row->disabled ? NULL : "This game";
    detail->value_muted = row->inherited || row->disabled;
}

/* The row of which list Settings > Enhanced last kept its padding for, and for how many
   frames more: the frame focus enters a row and the frames after it in which the focus graph
   brings it into view again (NK_UI_FOCUS_REVEAL_FRAMES in nk_ui_focus.c). The page is drawn
   from one thread; a frame with no row focused clears it, so a new NkUi starts afresh. */
#define ENHANCED_PAD_FRAMES 3
static struct {
    const NkUi *ui;
    NkUiId list;
    NkUiId row;
    int frames;
} s_enhanced_pad;

/* How tall `text` is wrapped in `width`, for the room the page keeps for its list:
   nk_ui_text_clamp wraps as the layout does, so the first line count at which it gives the
   whole text back is the text's. The whole text is what a clamp to the most lines returns: an
   ellipsis the text has of its own (a game's title can) is not a cut. */
static float settings_enhanced_text_height(NkUi *ui, const char *text, NkUiTypeRole role,
                                           float width) {
    enum { MOST_LINES = 12 };
    NkUiTextStyle style = nk_ui_type(role, 0u);
    const char *whole;
    int lines = 1;
    if (text == NULL || text[0] == '\0') return 0.0f;
    whole = nk_ui_text_clamp(ui, text, &style, width, MOST_LINES);
    while (lines < MOST_LINES &&
           strcmp(nk_ui_text_clamp(ui, text, &style, width, lines), whole) != 0) {
        ++lines;
    }
    return (float)lines * nk_ui_line_height(ui, &style);
}

/* The rows a list at rest shows whole, and whether row `place` is the one just above or just
   below them. The list's padding, there for the focus ring, shows a few pixels of that row: the
   edge of its value boxes, which reads as a drawing fault. So while the list rests on a row
   boundary that row is drawn blank, unless it has focus (it is about to scroll in). A list the
   pointer has scrolled between two rows draws every row. */
typedef struct {
    bool at_rest;
    int first;      /* the first whole row */
    int count;      /* how many whole rows */
} SettingsEnhancedWindow;

static SettingsEnhancedWindow settings_enhanced_window(NkUi *ui, NkUiId list, float pitch,
                                                       int count) {
    SettingsEnhancedWindow window = {true, 0, count};
    NkUiScrollState state;
    if (nk_ui_scroll_state(ui, list, &state) && state.max_offset > 0.5f) {
        float rows = state.offset / pitch;
        window.first = (int)floorf(rows + 0.5f);
        window.at_rest = fabsf(rows - (float)window.first) * pitch < 0.75f;
    }
    return window;
}

static bool settings_enhanced_row_blank(NkUi *ui, const SettingsEnhancedWindow *window,
                                        int place, NkUiId id) {
    return window->at_rest && !nk_ui_button_focused(ui, id) &&
           (place == window->first - 1 || place == window->first + window->count);
}

const char *player_ui_enhanced_group_label(PlayerUiEnhancedGroup group) {
    switch (group) {
    case PLAYER_UI_ENHANCED_GROUP_PICTURE: return "Picture";
    case PLAYER_UI_ENHANCED_GROUP_MOTION: return "Motion";
    case PLAYER_UI_ENHANCED_GROUP_SOUND: return "Sound";
    case PLAYER_UI_ENHANCED_GROUP_CONTROLS: return "Controls";
    case PLAYER_UI_ENHANCED_GROUP_ON_SCREEN: return "On-screen";
    case PLAYER_UI_ENHANCED_GROUP_ADVANCED: return "Advanced";
    default: return "";
    }
}

static void settings_ace(NkUi *ui, const PlayerUiSettingsModel *model, float inner, float height,
                         PlayerUiAction *result) {
    static const NkUiId tab_ids[PLAYER_UI_ENHANCED_GROUP_COUNT] = {
        SET_ID(0x000500u), SET_ID(0x000504u), SET_ID(0x000508u), SET_ID(0x00050Cu),
        SET_ID(0x000510u), SET_ID(0x000514u)
    };
    static const char *const preset_labels[] = {
        "Custom", "Low", "Medium", "High", "Ultra", "Handheld"
    };
    static const char *const pacing_values[] = {"PSP timing", "No extra cap"};
    const NkUiTheme *theme = nk_ui_theme(ui);
    const bool title = model->enhanced_title != NULL;
    const bool has_notice = model->notice != NULL && model->notice[0] != '\0';
    const float row_height = settings_enhanced_row_height(ui);
    const float pitch = row_height + 1.0f;
    float detail_width;
    float left_width;
    float row_width;
    NkUiSection active_tabs[PLAYER_UI_ENHANCED_GROUP_COUNT];
    SettingsEnhancedDetail detail;
    SettingsEnhancedDetail first_detail;
    SettingsEnhancedWindow window;
    const char *lead;
    char lead_text[256];
    int active_group = model->enhanced_tab;
    int preset_index = 0;
    int mode_index;
    int pacing_index = strcmp(safe_text(model->pacing, pacing_values[0]), pacing_values[1]) == 0;
    int tab_rows[PLAYER_UI_ENHANCED_GROUP_COUNT] = {0};
    int most_rows = 0;
    int visible_rows;
    int listed = 0;
    int focused_place = -1;
    float box_width;
    float page_room;
    bool have_detail = false;
    bool have_first = false;
    NkUiId active_tab_id;
    NkUiId list_id;
    NkUiId first_row = 0u;
    NkUiId last_row = 0u;
    NkUiLayout page;
    if (active_group < 0 || active_group >= PLAYER_UI_ENHANCED_GROUP_COUNT) {
        active_group = PLAYER_UI_ENHANCED_GROUP_PICTURE;
    }
    /* The panel has a third of the width, and less where the six tabs need more than the rest
       (a larger text size): the strip is never cut. Rows whose values no longer fit beside the
       name show "< value >" instead. */
    detail_width = floorf(inner * 0.34f);
    {
        NkUiTextStyle tab = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
        float strip = 2.0f * ENHANCED_BOX_PAD + ENHANCED_LIST_PAD + NK_UI_SPACE_2;
        tab.letter_spacing = 1.2f; /* as nk_ui_section_tabs draws its labels */
        for (int i = 0; i < PLAYER_UI_ENHANCED_GROUP_COUNT; ++i) {
            const char *label = player_ui_enhanced_group_label((PlayerUiEnhancedGroup)i);
            strip += ceilf(nk_ui_text_width(ui, nk_ui_caps(ui, label), &tab)) +
                     2.0f * NK_UI_SPACE_3 + (i > 0 ? NK_UI_SPACE_1 : 0.0f);
        }
        if (inner - detail_width - NK_UI_SPACE_4 < strip) {
            detail_width = floorf(inner - NK_UI_SPACE_4 - strip);
        }
        if (detail_width < floorf(inner * 0.24f)) detail_width = floorf(inner * 0.24f);
    }
    left_width = inner - detail_width - NK_UI_SPACE_4;
    row_width = left_width - 2.0f * ENHANCED_BOX_PAD - 2.0f * ENHANCED_LIST_PAD -
                ENHANCED_SCROLLBAR_ROOM;
    active_tab_id = tab_ids[active_group];
    list_id = SET_ID_ENHANCED_LIST + (uint32_t)active_group;
    memset(&detail, 0, sizeof(detail));
    memset(&first_detail, 0, sizeof(first_detail));
    if (title) {
        (void)snprintf(lead_text, sizeof(lead_text),
                       "For %s alone. Library follows Settings > Enhanced.",
                       model->enhanced_title);
        lead = nk_ui_frame_text(ui, lead_text);
    } else {
        lead = "Enhanced adds modern touches that work for every game, such as sharper "
               "textures and crisp system text. The games themselves run as released.";
    }
    if (model->preset != NULL) {
        for (int i = 0; i < (int)(sizeof(preset_labels) / sizeof(preset_labels[0])); ++i) {
            if (strcmp(model->preset, preset_labels[i]) == 0) preset_index = i;
        }
    }
    /* -1: the mode is none of the list's, a saved Ace whose package has gone. It sits where
       Ace would, after the last mode: Right goes round to the first, Left to the last, as the
       select button steps it (player_app_cycle_title_play_mode). */
    mode_index = -1;
    for (int i = 0; i < model->enhanced_mode_count; ++i) {
        if (strcmp(safe_text(model->enhanced_mode, ""),
                   safe_text(model->enhanced_modes[i], "")) == 0) mode_index = i;
    }
    for (int i = 0; i < model->enhanced_row_count && i < PLAYER_UI_ENHANCED_ROWS; ++i) {
        int group = (int)model->enhanced_rows[i].group;
        if (group >= 0 && group < PLAYER_UI_ENHANCED_GROUP_COUNT) ++tab_rows[group];
        if (group == active_group && first_row == 0u) {
            first_row = PLAYER_UI_ID_ENHANCED_PART(model->enhanced_rows[i].part);
        }
    }
    /* The row a Down move from the tab strip lands on: the list's first, as its parts come
       before Frame pacing and the texture pack rows. */
    if (first_row == 0u && active_group == PLAYER_UI_ENHANCED_GROUP_MOTION) {
        first_row = PLAYER_UI_ID_SETTINGS_PACING;
    }
    if (first_row == 0u && active_group == PLAYER_UI_ENHANCED_GROUP_PICTURE &&
        model->texpack_row) {
        first_row = PLAYER_UI_ID_SETTINGS_TEXPACK_CHOOSE;
    }
    ++tab_rows[PLAYER_UI_ENHANCED_GROUP_MOTION];
    if (model->texpack_row) {
        tab_rows[PLAYER_UI_ENHANCED_GROUP_PICTURE] += model->texpack_chosen ? 2 : 1;
    }
    for (int i = 0; i < PLAYER_UI_ENHANCED_GROUP_COUNT; ++i) {
        if (tab_rows[i] > most_rows) most_rows = tab_rows[i];
    }
    /* One width for the three "< value >" boxes that are always that: the widest value. */
    box_width = settings_enhanced_stepper_width(
        ui, preset_labels, (int)(sizeof(preset_labels) / sizeof(preset_labels[0])),
        pacing_values[1]);
    {
        float modes = settings_enhanced_stepper_width(ui, model->enhanced_modes,
                                                      model->enhanced_mode_count, NULL);
        if (modes > box_width) box_width = modes;
        if (box_width < 220.0f) box_width = 220.0f;
        if (box_width > row_width * 0.6f) box_width = row_width * 0.6f;
    }
    /* The page fits the room between the bars: its list shows as many whole rows as the rest
       of the page leaves there, the same number on every tab so that changing tabs moves
       nothing, and never more than the longest tab has. A longer tab scrolls inside the list,
       so the panel and Back stay on screen. Where fewer than two rows would be left (a
       larger text size in a small window) the list keeps two and the card scrolls as a whole
       instead. */
    page_room = height - NK_UI_TOP_BAR_HEIGHT - NK_UI_PROMPT_BAR_HEIGHT - 2.0f * NK_UI_SPACE_4 -
                48.0f;
    {
        NkUiTextStyle eyebrow = nk_ui_type(NK_UI_TYPE_EYEBROW, 0u);
        NkUiTextStyle heading = nk_ui_type(NK_UI_TYPE_TITLE, 0u);
        NkUiTextStyle label = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
        /* Everything on the page but the list's rows: the heading, the box around the pinned
           rows, the tabs and the list's own padding, the notice, and Back. */
        float fixed = nk_ui_line_height(ui, &eyebrow) + nk_ui_line_height(ui, &heading) +
                      settings_enhanced_text_height(ui, lead, NK_UI_TYPE_BODY_S, inner) +
                      (has_notice ? settings_enhanced_text_height(ui, model->notice,
                                                                  NK_UI_TYPE_CAPTION, inner) +
                                    ENHANCED_PAGE_GAP : 0.0f) +
                      4.0f * ENHANCED_PAGE_GAP + ENHANCED_BOX_TOP + ENHANCED_BOX_PAD +
                      2.0f * row_height + ENHANCED_TABS_HEIGHT + 1.0f + 4.0f * ENHANCED_BOX_GAP +
                      2.0f * ENHANCED_LIST_PAD +
                      nk_ui_line_box(ui, NK_UI_CONTROL_HEIGHT, &label);
        /* The last row has no divider under it. */
        visible_rows = (int)floorf((page_room - fixed + 1.0f) / pitch);
        if (visible_rows > most_rows) visible_rows = most_rows;
        if (visible_rows < 2) visible_rows = most_rows < 2 ? most_rows : 2;
    }
    page = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), ENHANCED_PAGE_GAP);
    if (!nk_ui_begin_container(ui, SET_ID_ENHANCED_PAGE, &page, NULL)) return;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, title ? "Game" : "Settings", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "Enhanced", NK_UI_TYPE_TITLE, theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD, lead, NK_UI_TYPE_BODY_S, theme->muted);
    {
        NkUiLayout main = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                      NK_UI_SPACE_4);
        NkUiLayout left = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(left_width),
                                      sizing_grow(), ENHANCED_BOX_GAP);
        NkUiLayout pinned = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                        ENHANCED_BOX_GAP);
        NkUiStyle box = scr_flow_box_style(theme, theme->surface);
        left.padding_left = left.padding_right = ENHANCED_BOX_PAD;
        left.padding_top = ENHANCED_BOX_TOP;
        left.padding_bottom = ENHANCED_BOX_PAD;
        pinned.padding_left = ENHANCED_LIST_PAD;
        pinned.padding_right = ENHANCED_LIST_PAD + ENHANCED_SCROLLBAR_ROOM;
        if (!nk_ui_begin_container(ui, SET_ID_ENHANCED_MAIN, &main, NULL)) {
            nk_ui_end_container(ui);
            return;
        }
        if (nk_ui_begin_container(ui, SET_ID_ENHANCED_LEFT, &left, &box)) {
            if (nk_ui_begin_container(ui, SET_ID_ENHANCED_PINNED, &pinned, NULL)) {
                int step = 0;
                bool activated = false;
                if (settings_enhanced_step_row(ui, PLAYER_UI_ID_SETTINGS_PRESET,
                                               "Graphics preset", preset_labels[preset_index],
                                               row_width, box_width, 0u, 0u, false, &step,
                                               &activated)) {
                    detail.focused = true;
                    detail.name = "Graphics preset";
                    detail.value = preset_labels[preset_index];
                    detail.sentence = "Library-wide for Enhanced and Ace; Authentic ignores it, "
                                      "and each game's part choices win.";
                    detail.note = model->preset_notice;
                    have_detail = true;
                }
                if (step != 0 || activated) {
                    *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_PRESET);
                    result->choice_index = step < 0 ? -1 : 1;
                }
                /* Up and Down between the play mode and the list go by the active tab, which
                   may sit far to one side of these full-width rows. */
                if (settings_enhanced_step_row(ui, PLAYER_UI_ID_SETTINGS_ACE_MODE, "Play mode",
                                               safe_text(model->enhanced_mode, "Authentic"),
                                               row_width, box_width, 0u, active_tab_id, false,
                                               &step, &activated)) {
                    detail.focused = true;
                    detail.name = "Play mode";
                    detail.value = safe_text(model->enhanced_mode, "Authentic");
                    detail.sentence = model->enhanced_mode_sentence;
                    have_detail = true;
                }
                if (step != 0 && model->enhanced_mode_count > 0) {
                    *result = settings_op(PLAYER_UI_SETTINGS_SET_PLAY_MODE);
                    result->choice_index =
                        mode_index < 0 ? (step > 0 ? 0 : model->enhanced_mode_count - 1)
                                       : (mode_index + step + model->enhanced_mode_count) %
                                             model->enhanced_mode_count;
                } else if (activated) {
                    *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_PLAY_MODE);
                }
                nk_ui_end_container(ui);
            }
            for (int i = 0; i < PLAYER_UI_ENHANCED_GROUP_COUNT; ++i) {
                active_tabs[i].label = player_ui_enhanced_group_label((PlayerUiEnhancedGroup)i);
                active_tabs[i].active = i == active_group;
                active_tabs[i].id = tab_ids[i];
                /* Down from the strip enters the list at its top. By position alone a short
                   list loses to Back: a tab sits over Back, far from the middle of a
                   full-width row (two rows at Larger text; review of #2644, N1). A named
                   neighbour is taken whether or not the list has it scrolled out of view. */
                active_tabs[i].down = first_row;
            }
            {
                /* The strip starts where the rows do. */
                NkUiLayout strip = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                               0.0f);
                int chosen = -1;
                strip.padding_left = ENHANCED_LIST_PAD;
                if (nk_ui_begin_container(ui, SET_ID_ENHANCED_TABS + 2u, &strip, NULL)) {
                    chosen = nk_ui_section_tabs(ui, SET_ID_ENHANCED_TABS, active_tabs,
                                                PLAYER_UI_ENHANCED_GROUP_COUNT,
                                                ENHANCED_TABS_HEIGHT);
                    nk_ui_end_container(ui);
                }
                nk_ui_divider(ui, SET_ID_ENHANCED_TABS + 1u, true);
                if (chosen >= 0 && chosen < PLAYER_UI_ENHANCED_GROUP_COUNT) {
                    if (model->enhanced_tab_chosen != NULL) *model->enhanced_tab_chosen = chosen;
                    nk_ui_focus(ui, active_tabs[chosen].id);
                }
            }
            {
                NkUiLayout list_row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                                  0.0f);
                NkUiLayout list = layout_make(
                    NK_UI_LAYOUT_COLUMN, sizing_grow(),
                    sizing_fixed(2.0f * ENHANCED_LIST_PAD + (float)visible_rows * pitch - 1.0f),
                    0.0f);
                list.padding_left = list.padding_right = ENHANCED_LIST_PAD;
                list.padding_top = ENHANCED_LIST_PAD;
                list.padding_bottom = ENHANCED_LIST_PAD;
                list.scroll = true;
                window = settings_enhanced_window(ui, list_id, pitch, visible_rows);
                if (nk_ui_begin_container(ui, SET_ID_ENHANCED_LIST_ROW, &list_row, NULL)) {
                    if (nk_ui_begin_container(ui, list_id, &list, NULL)) {
                        for (int i = 0; i < model->enhanced_row_count &&
                                        i < PLAYER_UI_ENHANCED_ROWS; ++i) {
                            const PlayerUiEnhancedRow *row = &model->enhanced_rows[i];
                            bool focused;
                            if ((int)row->group != active_group) continue;
                            if (listed > 0) {
                                nk_ui_divider(ui, SET_ID_ENHANCED_DIVIDER + (uint32_t)listed,
                                              true);
                            }
                            focused = settings_enhanced_part_row(
                                ui, model, row, row_width, listed == 0 ? active_tab_id : 0u,
                                settings_enhanced_row_blank(
                                    ui, &window, listed, PLAYER_UI_ID_ENHANCED_PART(row->part)),
                                result);
                            if (focused) {
                                settings_enhanced_row_detail(&detail, row, title, true);
                                have_detail = true;
                                focused_place = listed;
                            }
                            if (!have_first) {
                                settings_enhanced_row_detail(&first_detail, row, title, false);
                                have_first = true;
                            }
                            last_row = PLAYER_UI_ID_ENHANCED_PART(row->part);
                            ++listed;
                        }
                        if (active_group == PLAYER_UI_ENHANCED_GROUP_MOTION) {
                            int step = 0;
                            bool activated = false;
                            /* The library's frame pacing (fps_cap): one value for every game. */
                            if (listed > 0) {
                                nk_ui_divider(ui, SET_ID_ENHANCED_DIVIDER + (uint32_t)listed,
                                              true);
                            }
                            if (settings_enhanced_step_row(
                                    ui, PLAYER_UI_ID_SETTINGS_PACING, "Frame pacing",
                                    pacing_values[pacing_index], row_width, box_width,
                                    listed == 0 ? active_tab_id : 0u, 0u,
                                    settings_enhanced_row_blank(ui, &window, listed,
                                                                PLAYER_UI_ID_SETTINGS_PACING),
                                    &step, &activated)) {
                                detail.focused = true;
                                detail.name = "Frame pacing";
                                detail.value = pacing_values[pacing_index];
                                detail.sentence = model->pacing_sentence;
                                /* fps_cap is the library's: a game's page says so. */
                                detail.scope = title ? "All games" : NULL;
                                have_detail = true;
                                focused_place = listed;
                            }
                            if (step != 0 || activated) {
                                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_PACING);
                            }
                            last_row = PLAYER_UI_ID_SETTINGS_PACING;
                            ++listed;
                        }
                        if (active_group == PLAYER_UI_ENHANCED_GROUP_PICTURE &&
                            model->texpack_row) {
                            if (listed > 0) {
                                nk_ui_divider(ui, SET_ID_ENHANCED_DIVIDER + (uint32_t)listed,
                                              true);
                            }
                            if (settings_enhanced_action_row(
                                    ui, PLAYER_UI_ID_SETTINGS_TEXPACK_CHOOSE,
                                    "Texture pack folder", model->texpack_value, "Choose",
                                    row_width, listed == 0 ? active_tab_id : 0u,
                                    settings_enhanced_row_blank(
                                        ui, &window, listed,
                                        PLAYER_UI_ID_SETTINGS_TEXPACK_CHOOSE),
                                    result, PLAYER_UI_SETTINGS_CHOOSE_TEXPACK)) {
                                detail.focused = true;
                                detail.name = "Texture pack folder";
                                detail.value = safe_text(model->texpack_value, "None");
                                detail.sentence = model->texpack_sentence;
                                detail.scope = "This game";
                                have_detail = true;
                                focused_place = listed;
                            }
                            last_row = PLAYER_UI_ID_SETTINGS_TEXPACK_CHOOSE;
                            ++listed;
                            if (model->texpack_chosen) {
                                nk_ui_divider(ui, SET_ID_ENHANCED_DIVIDER + (uint32_t)listed,
                                              true);
                                if (settings_enhanced_action_row(
                                        ui, PLAYER_UI_ID_SETTINGS_TEXPACK_REMOVE,
                                        "Remove texture pack", NULL, "Remove", row_width, 0u,
                                        settings_enhanced_row_blank(
                                            ui, &window, listed,
                                            PLAYER_UI_ID_SETTINGS_TEXPACK_REMOVE),
                                        result, PLAYER_UI_SETTINGS_REMOVE_TEXPACK)) {
                                    detail.focused = true;
                                    detail.name = "Remove texture pack";
                                    detail.value = safe_text(model->texpack_value, "None");
                                    detail.sentence = "Forgets the folder for this game. The "
                                                      "folder is not deleted.";
                                    detail.scope = "This game";
                                    have_detail = true;
                                    focused_place = listed;
                                }
                                last_row = PLAYER_UI_ID_SETTINGS_TEXPACK_REMOVE;
                                ++listed;
                            }
                        }
                        /* A short tab keeps the list's height. */
                        if (listed < visible_rows) {
                            nk_ui_spacer(ui, nk_ui_child_id(list_id, 1u), sizing_fixed(1.0f),
                                         sizing_fixed((float)(visible_rows - listed) * pitch));
                        }
                        nk_ui_end_container(ui);
                    }
                    settings_enhanced_scrollbar(ui, SET_ID_ENHANCED_SCROLLBAR, list_id);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
        /* The focused row scrolls into view flush with the list's edge, which would cut its
           focus ring there: keep the list's own padding between them. Only in the frames
           just after focus enters the row, as the focus graph brings it into view: after
           that the list stays where the wheel or a drag puts it, focused row or not. */
        if (focused_place < 0) {
            s_enhanced_pad.row = 0u;
        } else {
            NkUiScrollState state;
            const NkUiId row = nk_ui_focused(ui);
            memset(&state, 0, sizeof(state));
            if (s_enhanced_pad.ui != ui || s_enhanced_pad.list != list_id ||
                s_enhanced_pad.row != row) {
                s_enhanced_pad.ui = ui;
                s_enhanced_pad.list = list_id;
                s_enhanced_pad.row = row;
                s_enhanced_pad.frames = ENHANCED_PAD_FRAMES;
            }
            /* A frame before the list's first layout knows no offset and does not count. */
            if (s_enhanced_pad.frames > 0 && nk_ui_scroll_state(ui, list_id, &state)) {
                --s_enhanced_pad.frames;
            } else {
                state.max_offset = 0.0f;
            }
            if (state.max_offset > 0.5f) {
                float top = ENHANCED_LIST_PAD + (float)focused_place * pitch;
                float want = state.offset;
                if (top - want < ENHANCED_LIST_PAD) want = top - ENHANCED_LIST_PAD;
                if (top + row_height - want > state.viewport - ENHANCED_LIST_PAD) {
                    want = top + row_height + ENHANCED_LIST_PAD - state.viewport;
                }
                if (want > state.max_offset) want = state.max_offset;
                if (want < 0.0f) want = 0.0f;
                if (fabsf(want - state.offset) > 0.5f) nk_ui_scroll_to(ui, list_id, want);
            }
        }
        /* With a tab or Back focused the panel shows the tab's first option. */
        settings_enhanced_details(ui, have_detail ? &detail : &first_detail, detail_width);
        nk_ui_end_container(ui);
    }
    if (has_notice) {
        nk_ui_text_role(ui, SET_ID_TEXT + 0x710u, model->notice, NK_UI_TYPE_CAPTION,
                        theme->warning);
    }
    {
        NkUiLayout actions = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &actions, NULL)) {
            /* Up from Back enters the list at its end, whatever tab sits above the button. */
            if (nk_ui_button_between(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back",
                                     NK_UI_BUTTON_PRIMARY, last_row, 0u)) {
                *result = settings_op(PLAYER_UI_SETTINGS_BACK);
            }
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* The Online play server picker (#1139): one row per server, the whole row its control: the
   server's name and host on the left, where it comes from and Current on the one in use as
   chips at the right edge. The list scrolls inside the card, beside a thin bar that shows where
   it is, so the heading, the intro and Back stay put. */
#define ONLINE_PLAY_ROW_HEIGHT NK_UI_CONTROL_HEIGHT /* a row's least height: ACCESSIBILITY.md's target */
#define ONLINE_PLAY_CHIP_GAP 6.0f
/* PPSSPP's list's heading: the launcher's gap between blocks above it, the heading-to-text
   gap under it (docs/PLAYER_UI_DESIGN.md spacing scale). */
#define ONLINE_PLAY_SECTION_GAP NK_UI_SPACE_4
#define ONLINE_PLAY_HEADING_GAP NK_UI_SPACE_2
#define ONLINE_PLAY_CHECK "\xE2\x9C\x93"
#define SET_ID_ONLINE_PLAY_LIST_ROW (SET_ID_TEXT + 0x7E0u)
#define SET_ID_ONLINE_PLAY_LIST (SET_ID_TEXT + 0x7E1u)
#define SET_ID_ONLINE_PLAY_TRACK (SET_ID_TEXT + 0x7E2u)
#define SET_ID_ONLINE_PLAY_BOX (SET_ID_TEXT + 0x7E3u)
#define SET_ID_ONLINE_PLAY_HEADING (SET_ID_TEXT + 0x7E4u) /* + 2: PPSSPP's list's heading */
#define SET_ID_ONLINE_PLAY_OWN_DIVIDER (SET_ID_TEXT + 0x7F7u)
#define SET_ID_ONLINE_PLAY_OWN_LINE (SET_ID_TEXT + 0x7E8u)
#define SET_ID_ONLINE_PLAY_OFF_LINE (SET_ID_TEXT + 0x7E9u)

/* How a run of rows lays out: the host beside the name where every row of the run has room
   for both and its chips, else under the name; and where the chips would leave the words less
   than half the row, the chips under the words. */
typedef struct {
    bool beside;
    bool chips_under;
} OnlinePlayRowLayout;

static float settings_online_play_row_height(const NkUi *ui) {
    NkUiTextStyle name = nk_ui_type(NK_UI_TYPE_BODY, 0u);
    return nk_ui_line_box(ui, ONLINE_PLAY_ROW_HEIGHT, &name);
}

/* How wide a row's chips are: `chip`'s (none on the picker today) and Current's. */
static float settings_online_play_chips_width(NkUi *ui, const char *chip, bool current) {
    NkUiTextStyle label = nk_ui_type(NK_UI_TYPE_LABEL_S, 0u);
    float width = 0.0f;
    if (chip != NULL && chip[0] != '\0') {
        width += ceilf(nk_ui_text_width(ui, nk_ui_caps(ui, chip), &label)) + 2.0f * 10.0f;
    }
    if (current) {
        if (width > 0.0f) width += ONLINE_PLAY_CHIP_GAP;
        width += ceilf(nk_ui_text_width(ui, ONLINE_PLAY_CHECK, &label)) + 6.0f +
                 ceilf(nk_ui_text_width(ui, nk_ui_caps(ui, "Current"), &label)) + 2.0f * 10.0f;
    }
    return width;
}

/* A row's mono line: the host, and for a list entry where it is. */
static const char *settings_online_play_host_line(NkUi *ui, const PlayerUiOnlineServer *row) {
    char line[NK_ADHOC_SERVER_MAX_LEN + 160u];
    const char *host = safe_text(row->host, "");
    const char *where = safe_text(row->line, "");
    if (!row->listed || where[0] == '\0') return host;
    (void)snprintf(line, sizeof(line), "%s \xC2\xB7 %s", host, where);
    return nk_ui_frame_text(ui, line);
}

/* Whether one row has room for its name, its host and its chips side by side. */
static bool settings_online_play_fits_beside(NkUi *ui, const char *name, const char *host,
                                             const char *chip, bool current, float width) {
    NkUiTextStyle name_style = nk_ui_type(NK_UI_TYPE_BODY, 0u);
    NkUiTextStyle host_style = nk_ui_type(NK_UI_TYPE_DATA, 0u);
    float need = 2.0f * NK_UI_SPACE_3 + ceilf(nk_ui_text_width(ui, name, &name_style)) +
                 NK_UI_SPACE_3 + ceilf(nk_ui_text_width(ui, host, &host_style)) +
                 NK_UI_SPACE_3 + settings_online_play_chips_width(ui, chip, current);
    return need <= width;
}

static OnlinePlayRowLayout settings_online_play_layout(NkUi *ui, bool beside, const char *chip,
                                                       float width) {
    OnlinePlayRowLayout layout = {beside, false};
    float words = width - 2.0f * NK_UI_SPACE_3 - NK_UI_SPACE_3 -
                  settings_online_play_chips_width(ui, chip, true);
    layout.chips_under = !beside && words < 0.5f * (width - 2.0f * NK_UI_SPACE_3);
    return layout;
}

/* Whether the list showed `id` only in part in the last layout, whether or not it has focus;
   *height is its height then. */
static bool settings_online_play_part_shown(NkUi *ui, NkUiId id, float *height) {
    NkUiScrollState state;
    NkUiRect view;
    NkUiRect rect;
    if (!nk_ui_scroll_state(ui, SET_ID_ONLINE_PLAY_LIST, &state) || state.max_offset <= 0.5f ||
        !nk_ui_element_rect(ui, SET_ID_ONLINE_PLAY_LIST, &view) ||
        !nk_ui_element_rect(ui, id, &rect)) {
        return false;
    }
    *height = rect.h;
    return rect.y < view.y - 0.5f || rect.y + rect.h > view.y + view.h + 0.5f;
}

/* Whether `id` is drawn blank at its height (*height), so no line is ever cut at the list's
   edge: the list showed it only in part. A row with focus is drawn: it is being brought into
   view. */
static bool settings_online_play_cut(NkUi *ui, NkUiId id, float *height) {
    return !nk_ui_button_focused(ui, id) && settings_online_play_part_shown(ui, id, height);
}

/* A click on what shows of a row the list cuts (the strip at its edge, drawn blank) focuses the
   row, which then comes into view, and opens nothing: the player has not seen what it names.
   The press already focuses it, so the test is the row's place in the last layout, not whether
   it was drawn blank; a key or a button press (no pointer over the row) is not refused. */
static bool settings_online_play_take(NkUi *ui, NkUiId id) {
    float height = 0.0f;
    const bool hidden = nk_ui_hovered(ui, id) && settings_online_play_part_shown(ui, id, &height);
    return nk_ui_take_activation(ui, id) && !hidden;
}

/* One server: the row is the button. `host` is the mono line (the format to type, muted, on
   the own server's row until one is in use); `chip` says where the server comes from; `note`,
   where not NULL, is a dim line under the name and host inside the row, so the row's focus
   ring goes round it too. Returns true when the row is activated. */
static bool settings_online_play_row(NkUi *ui, NkUiId id, const char *name, const char *host,
                                     const char *note, const char *chip, bool current,
                                     float width, OnlinePlayRowLayout layout, NkUiId up,
                                     NkUiId down) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const bool focused = nk_ui_button_focused(ui, id);
    const float chips = settings_online_play_chips_width(ui, chip, current);
    const float inside = width - 2.0f * NK_UI_SPACE_3;
    const float words = layout.chips_under || chips <= 0.0f ? inside
                                                             : inside - NK_UI_SPACE_3 - chips;
    NkUiTextStyle name_style = nk_ui_type(NK_UI_TYPE_BODY, theme->paper);
    NkUiTextStyle host_style = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
    float blank_height = 0.0f;
    const bool blank = settings_online_play_cut(ui, id, &blank_height);
    const bool has_note = note != NULL && note[0] != '\0';
    /* The name, host and chips on the row's top line (or stacked), and under them, across
       the row's whole width, the note: all inside the row, so its focus ring goes round all. */
    NkUiLayout top = layout_make(layout.chips_under ? NK_UI_LAYOUT_COLUMN : NK_UI_LAYOUT_ROW,
                                 has_note ? sizing_grow() : sizing_fixed(width),
                                 blank ? sizing_fixed(blank_height) : sizing_fit(),
                                 layout.chips_under ? NK_UI_SPACE_1 : NK_UI_SPACE_3);
    NkUiLayout row = has_note ? layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(width),
                                            blank ? sizing_fixed(blank_height) : sizing_fit(),
                                            2.0f)
                              : top;
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    if (!blank) row.height.min = settings_online_play_row_height(ui);
    row.align_y = NK_UI_ALIGN_CENTER;
    top.align_y = NK_UI_ALIGN_CENTER;
    row.padding_left = row.padding_right = NK_UI_SPACE_3;
    /* As deep as a focus ring reaches outside its row, so the ring of the row above or
       below (past the 1 px divider) never crosses this row's lines. */
    row.padding_top = row.padding_bottom = NK_UI_FOCUS_RING_GAP + NK_UI_FOCUS_RING_WIDTH;
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    if (!blank) {
        style.fill_rgba = nk_ui_pressed(ui, id) ? theme->pressed
                          : focused || nk_ui_hovered(ui, id) ? theme->surface_high : 0u;
    }
    nk_ui_register_button_between(ui, id, 0u, up, down);
    if (!nk_ui_begin_container(ui, id, &row, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    if (!blank) {
        NkUiLayout text = layout_make(layout.beside ? NK_UI_LAYOUT_ROW : NK_UI_LAYOUT_COLUMN,
                                      layout.beside ? sizing_grow() : sizing_fixed(words),
                                      sizing_fit(), layout.beside ? NK_UI_SPACE_3 : 0.0f);
        const bool top_open =
            has_note && nk_ui_begin_container(ui, nk_ui_child_id(id, 7u), &top, NULL);
        text.align_y = NK_UI_ALIGN_CENTER;
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 1u), &text, NULL)) {
            nk_ui_text(ui, nk_ui_child_id(id, 2u),
                       nk_ui_text_break(ui, name, &name_style, words), &name_style);
            if (host[0] != '\0') {
                nk_ui_text(ui, nk_ui_child_id(id, 3u),
                           nk_ui_text_break(ui, host, &host_style, words), &host_style);
            }
            nk_ui_end_container(ui);
        }
        if (chips > 0.0f) {
            NkUiLayout tags = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                          ONLINE_PLAY_CHIP_GAP);
            tags.align_y = NK_UI_ALIGN_CENTER;
            if (nk_ui_begin_container(ui, nk_ui_child_id(id, 4u), &tags, NULL)) {
                if (chip != NULL && chip[0] != '\0') nk_ui_tag(ui, nk_ui_child_id(id, 5u), chip);
                if (current) {
                    nk_ui_status_pill(ui, nk_ui_child_id(id, 6u), ONLINE_PLAY_CHECK, "Current",
                                      theme->ready);
                }
                nk_ui_end_container(ui);
            }
        }
        if (top_open) {
            NkUiTextStyle note_style = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
            nk_ui_end_container(ui);
            nk_ui_text(ui, nk_ui_child_id(id, 8u),
                       nk_ui_text_break(ui, note, &note_style, inside), &note_style);
        }
    }
    nk_ui_end_container(ui);
    return settings_online_play_take(ui, id);
}

/* The own server's Port offset button, beside its row; while the list cuts it, a blank of its
   size that keeps its place in the focus walk. */
static bool settings_online_play_port(NkUi *ui, const char *label, NkUiId up, NkUiId down) {
    const NkUiId id = PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_PORT;
    float height = 0.0f;
    NkUiRect rect;
    if (settings_online_play_cut(ui, id, &height) && nk_ui_element_rect(ui, id, &rect)) {
        NkUiLayout blank = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(rect.w),
                                       sizing_fixed(height), 0.0f);
        nk_ui_register_button_between(ui, id, 0u, up, down);
        if (nk_ui_begin_container(ui, id, &blank, NULL)) nk_ui_end_container(ui);
        return settings_online_play_take(ui, id);
    }
    if (nk_ui_hovered(ui, id) && settings_online_play_part_shown(ui, id, &height)) {
        /* Focused by the press on its cut strip: drawn, coming into view, not yet opened. */
        (void)nk_ui_button_between(ui, id, label, 0u, up, down);
        return false;
    }
    return nk_ui_button_between(ui, id, label, 0u, up, down);
}

/* Where Up and Down from `id` land in the picker's walk `order` (shipped servers, the own
   server, the list's): its neighbours, named, because a move by position skips a row the list
   has scrolled out of view. 0 past either end: by position (Back below). */
static void settings_online_play_links(const NkUiId *order, size_t count, NkUiId id, NkUiId *up,
                                       NkUiId *down) {
    *up = 0u;
    *down = 0u;
    for (size_t k = 0u; k < count; ++k) {
        if (order[k] != id) continue;
        if (k > 0u) *up = order[k - 1u];
        if (k + 1u < count) *down = order[k + 1u];
        return;
    }
}

/* A section heading in the list, or a line of text under one; blank while the list cuts it.
   `top` and `bottom` are its padding; a line a row follows keeps 6 px under it, clear of the
   row's focus ring. */
static void settings_online_play_text(NkUi *ui, NkUiId id, const char *text, NkUiTypeRole role,
                                      uint32_t rgba, float top, float bottom) {
    float height = 0.0f;
    NkUiLayout line = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), 0.0f);
    if (settings_online_play_cut(ui, id, &height)) {
        nk_ui_spacer(ui, id, sizing_grow(), sizing_fixed(height));
        return;
    }
    line.padding_left = line.padding_right = NK_UI_SPACE_3;
    line.padding_top = top;
    line.padding_bottom = bottom;
    if (nk_ui_begin_container(ui, id, &line, NULL)) {
        nk_ui_text_role(ui, nk_ui_child_id(id, 1u),
                        role == NK_UI_TYPE_EYEBROW ? nk_ui_caps(ui, text) : text, role, rgba);
        nk_ui_end_container(ui);
    }
}

/* The picker control focus last moved to. Focus brings it to the list's edge, where the list
   would cut its focus ring; for the frames just after a move it is brought in by the list's
   padding as well, and never later, so the wheel and a drag still scroll freely. Where the
   list cannot hold two rows (a larger text size in a small window) the card scrolls instead. */
static struct {
    const NkUi *ui;
    NkUiId focused;
    int frames;
} s_online_play_row;

static void settings_online_play_reveal_row(NkUi *ui, size_t count) {
    const NkUiId focused = nk_ui_focused(ui);
    NkUiId scroller = SET_ID_ONLINE_PLAY_LIST;
    float pad = ENHANCED_LIST_PAD;
    NkUiRect view;
    NkUiRect rect;
    NkUiRect first;
    NkUiScrollState state;
    float offset;
    float top;
    if (s_online_play_row.ui != ui || s_online_play_row.focused != focused) {
        s_online_play_row.ui = ui;
        s_online_play_row.focused = focused;
        s_online_play_row.frames = 3;
    }
    if (s_online_play_row.frames <= 0) return;
    --s_online_play_row.frames;
    if (!(focused >= PLAYER_UI_ID_ONLINE_PLAY_SERVER_BASE &&
          focused < PLAYER_UI_ID_ONLINE_PLAY_SERVER_BASE + (NkUiId)count) &&
        focused != PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_SERVER &&
        focused != PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_PORT) {
        return;
    }
    if (!nk_ui_scroll_state(ui, scroller, &state) || state.max_offset <= 0.5f) {
        scroller = FLOW_ID_CARD;
        pad = 0.0f;
    }
    if (!nk_ui_element_rect(ui, focused, &rect) || !nk_ui_element_rect(ui, scroller, &view) ||
        !nk_ui_scroll_state(ui, scroller, &state)) {
        return;
    }
    offset = state.offset;
    if (scroller == SET_ID_ONLINE_PLAY_LIST &&
        nk_ui_element_rect(ui, PLAYER_UI_ID_ONLINE_PLAY_SERVER_BASE, &first)) {
        /* Where the row lies in the list's content, measured from the list's first row (its
           top is the list's padding): both rects come from the same layout, which may have
           been laid out at another offset than the one now asked for (the focus graph moves
           the list too), so the offset now asked for is only the starting point. */
        const float row_top = pad + (rect.y - first.y);
        const float lowest = row_top + rect.h - (view.h - pad);
        const float highest = row_top - pad;
        if (offset < lowest) offset = lowest;
        if (offset > highest) offset = highest;
    } else {
        if (rect.y + rect.h > view.y + view.h - pad) {
            offset += rect.y + rect.h - (view.y + view.h - pad);
        }
        top = rect.y - (offset - state.offset);
        if (top < view.y + pad) offset -= view.y + pad - top;
    }
    if (offset > state.max_offset) offset = state.max_offset;
    if (offset < 0.0f) offset = 0.0f;
    if (fabsf(offset - state.offset) >= 0.5f) nk_ui_scroll_to(ui, scroller, offset);
}

/* At rest the list's top edge lies on an item's top: an item the edge would cut is drawn blank
   (settings_online_play_cut), which would leave a gap there. Once the wheel, a drag or focus
   has moved the list, it is moved on to the nearest item boundary: up to show the cut item
   whole, unless that would push the focused control past the foot, else down past it. Scrolled
   to its end it stays there, where the last item rests whole on the foot. `items` are the ids
   of everything the list holds, in any order. */
static void settings_online_play_snap(NkUi *ui, const NkUiId *items, size_t count) {
    const NkUiId focused = nk_ui_focused(ui);
    NkUiScrollState state;
    NkUiRect view;
    NkUiRect rect;
    NkUiRect mark;
    float top;
    float offset;
    if (s_online_play_row.frames > 0 || nk_ui_scroll_in_motion(ui) ||
        !nk_ui_scroll_state(ui, SET_ID_ONLINE_PLAY_LIST, &state) || state.max_offset <= 0.5f ||
        state.offset >= state.max_offset - 0.5f ||
        !nk_ui_element_rect(ui, SET_ID_ONLINE_PLAY_LIST, &view)) {
        return; /* at the foot the last item rests on the edge: nothing is cut there */
    }
    top = view.y + ENHANCED_LIST_PAD;
    for (size_t i = 0u; i < count; ++i) {
        if (!nk_ui_element_rect(ui, items[i], &rect) || !(rect.y < top - 0.5f) ||
            !(rect.y + rect.h > top + 0.5f)) {
            continue;
        }
        offset = state.offset - (top - rect.y);
        if (nk_ui_element_rect(ui, focused, &mark) && mark.y >= view.y && mark.y < view.y + view.h &&
            mark.y + mark.h + (top - rect.y) > view.y + view.h - ENHANCED_LIST_PAD) {
            /* Never down past an item that holds the focused control (a tall own row with
               Port offset under it): focus put it in view, and there it stays. */
            if (mark.y >= rect.y - 0.5f && mark.y + mark.h <= rect.y + rect.h + 0.5f) return;
            offset = state.offset + (rect.y + rect.h - top);
        }
        if (offset > state.max_offset) offset = state.max_offset;
        if (offset < 0.0f) offset = 0.0f;
        if (fabsf(offset - state.offset) >= 0.5f) nk_ui_scroll_to(ui, SET_ID_ONLINE_PLAY_LIST, offset);
        return;
    }
}

/* The Online play page's lines above its buttons change after a press (New console ID adds a
   notice), which moves the focused button down after focus brought it into view; at a larger
   text size that is past the card's edge. When those lines change, focus is set again on the
   control that has it, which brings it into view where the new layout puts it. */
static struct {
    const NkUi *ui;
    char lines[2048]; /* the lines last drawn, each ended by a 0x1F byte */
} s_online_play_page;

static void settings_online_play_keep_focus_in_view(NkUi *ui, const char *const *lines,
                                                    size_t count) {
    char now[sizeof(s_online_play_page.lines)];
    size_t used = 0u;
    NkUiId focused;
    now[0] = '\0';
    for (size_t i = 0u; i < count && used < sizeof(now); ++i) {
        int wrote = snprintf(now + used, sizeof(now) - used, "%s\x1F", safe_text(lines[i], ""));
        if (wrote < 0) break;
        used += (size_t)wrote;
    }
    if (s_online_play_page.ui == ui && strcmp(s_online_play_page.lines, now) == 0) return;
    focused = nk_ui_focused(ui);
    if (s_online_play_page.ui == ui && focused != 0u) nk_ui_focus(ui, focused);
    s_online_play_page.ui = ui;
    memcpy(s_online_play_page.lines, now, sizeof(now));
}

/* The consent card is taller than the window at the smallest window or a larger text size, and
   its focus starts on its buttons at the foot. Page Up and Page Down (the pad's triggers) scroll
   it a view less one line, so its first lines and the server's name can be read by keyboard or
   controller before choosing; the wheel, a drag and touch scroll it as any card does. Returns
   whether the card scrolls, so it can say how. */
static bool settings_online_play_card_scroll(NkUi *ui) {
    NkUiTextStyle body = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    float line = nk_ui_line_height(ui, &body);
    NkUiScrollState state;
    NkUiNavDir page;
    const bool known = nk_ui_scroll_state(ui, FLOW_ID_CARD, &state);
    float offset = known ? state.offset : 0.0f;
    const float start = offset;
    while (nk_ui_take_event(ui, NK_UI_EV_PAGE, &page)) {
        float step = known && state.viewport > 2.0f * line ? state.viewport - line : line;
        offset += page == NK_UI_DIR_RIGHT ? step : -step;
    }
    if (known && offset > state.max_offset) offset = state.max_offset;
    if (offset < 0.0f) offset = 0.0f;
    if (fabsf(offset - start) >= 0.5f) nk_ui_scroll_to(ui, FLOW_ID_CARD, offset);
    return known && state.max_offset > 0.5f;
}

static void settings_dialog(NkUi *ui, const PlayerUiSettingsModel *model, float width,
                            float height, PlayerUiAction *result) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Back to settings"}
    };
    static const NkUiPromptItem enhanced_prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_TAB_PREV, "Prev tab"},
        {NK_UI_PROMPT_ACT_TAB_NEXT, "Next tab"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Change value"}, {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    float inner = scr_flow_begin(ui, width, height,
                                 model->page == PLAYER_UI_SETTINGS_PAGE_ACE ? 1120.0f
                                 /* The picker: its hint on one line under the intro. */
                                 : model->page == PLAYER_UI_SETTINGS_PAGE_ONLINE_PLAY_SERVERS
                                     ? 840.0f
                                     : 760.0f);
    if (inner <= 0.0f) return;
    switch (model->page) {
    case PLAYER_UI_SETTINGS_PAGE_ONLINE:
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Online and developer options", NK_UI_TYPE_TITLE,
                        theme->paper);
        /* Online mode has nothing to switch yet: one plain line says so rather
           than a switch that does nothing (#2007). The stored setting stays. */
        nk_ui_text_role(ui, FLOW_ID_LEAD, SCR_ONLINE_NOT_BUILT_LINE, NK_UI_TYPE_BODY_S,
                        theme->warning);
        if (scr_wizard_box_begin(ui, 0u)) {
            /* Discord status shows when this build can show it, or while it is stored On
               (so it can be turned off), as the Online mode row does (#2007). */
            bool discord_row = model->discord_available || model->discord_presence;
            const char *discord_sentence = model->discord_available
                ? SCR_DISCORD_SENTENCE : SCR_DISCORD_STORED_ON_SENTENCE;
            /* One button column for every row the page shows, so a sentence of any of them
               that would be cut makes them all stack. The Nickname button's value is clamped
               to half the column, so only its name counts for the column's width. */
            static const char k_cover_art_sentence[] =
                "Fetch Redump-matched box art from thumbnails.libretro.com to cache; "
                "unmatched discs keep disc art.";
            /* Cheats (#2639): the user's own CWCheat file; the game marks a run that applies one. */
            static const char k_cheats_sentence[] =
                "Apply your CWCheat file, cheats/<disc ID>.ini in the data folder. "
                "The game shows a marker while any cheat is on.";
            WizardRow rows[7];
            size_t row_count = 0u;
            if (model->online_mode) {
                rows[row_count++] = (WizardRow){"Online mode", "On",
                                                SCR_ONLINE_STORED_ON_SENTENCE};
            }
            rows[row_count++] = (WizardRow){"Developer mode",
                                            model->developer_mode ? "On" : "Off",
                                            WIZARD_DEVELOPER_SENTENCE};
            if (discord_row) {
                rows[row_count++] = (WizardRow){"Discord status",
                                                model->discord_presence ? "On" : "Off",
                                                discord_sentence};
            }
            rows[row_count++] = (WizardRow){"Nickname", "", SCR_NICKNAME_SENTENCE};
            rows[row_count++] = (WizardRow){"Online play",
                                            safe_text(model->online_play_row_value, "Off"),
                                            SCR_ONLINE_PLAY_ROW_SENTENCE};
            rows[row_count++] = (WizardRow){"Download cover art",
                                            settings_on_off(model->download_cover_art),
                                            k_cover_art_sentence};
            rows[row_count++] = (WizardRow){"Cheats", settings_on_off(model->cheats),
                                            k_cheats_sentence};
            WizardColumn column = scr_wizard_column(ui, rows, row_count, inner - 32.0f, false);
            if (model->online_mode &&
                scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_ONLINE_MODE, SET_ID_TEXT + 0x80u,
                                  "Online mode", SCR_ONLINE_STORED_ON_SENTENCE, true,
                                  inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ONLINE);
            }
            if (scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_DEVELOPER, SET_ID_TEXT + 0x90u,
                              "Developer mode", WIZARD_DEVELOPER_SENTENCE,
                              model->developer_mode, inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_DEVELOPER);
            }
            if (discord_row &&
                scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_DISCORD, SET_ID_TEXT + 0xB0u,
                                  "Discord status", discord_sentence, model->discord_presence,
                                  inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_DISCORD);
            }
            {
                /* The console nickname games read (#2197); its button opens the nickname
                   editor. A long one is shortened here. */
                NkUiTextStyle value = nk_ui_type(NK_UI_TYPE_BODY, theme->paper);
                if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_NICKNAME, SET_ID_TEXT + 0xB4u,
                                     "Nickname",
                                     nk_ui_text_clamp(ui, safe_text(model->nickname,
                                                                    NK_NICKNAME_DEFAULT),
                                                      &value, column.cell * 0.5f, 1),
                                     SCR_NICKNAME_SENTENCE,
                                     false, inner - 32.0f, &column)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_EDIT_NICKNAME);
                }
            }
            if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_ONLINE_PLAY, SET_ID_TEXT + 0xA8u,
                                 "Online play",
                                 safe_text(model->online_play_row_value, "Off"),
                                 SCR_ONLINE_PLAY_ROW_SENTENCE,
                                 model->online_play, inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_ONLINE_PLAY);
            }
            if (scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_COVER_ART, SET_ID_TEXT + 0xB8u,
                                  "Download cover art", k_cover_art_sentence,
                                  model->download_cover_art, inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_COVER_ART);
            }
            if (scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_CHEATS, SET_ID_TEXT + 0xBCu,
                                  "Cheats", k_cheats_sentence, model->cheats, inner - 32.0f,
                                  &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_CHEATS);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                if (model->developer_mode &&
                    nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_DEVELOPER_LOG,
                                 "Open developer log (F12)", 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_OPEN_DEVELOPER_LOG);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_ONLINE_PLAY: {
        /* Online play (#1139): the switch, the server, the console ID and the port offset. The
           switch never turns it on by itself: Off opens the consent card for the server shown. */
        char console_line[160];
        const char *switch_sentence = model->online_play ? SCR_ONLINE_PLAY_ON_SENTENCE
                                                         : SCR_ONLINE_PLAY_OFF_SENTENCE;
        const char *server = safe_text(model->online_play_server, NK_ADHOC_DEFAULT_SERVER);
        /* No list names the server (typed in, or from PPSSPP's list while it is not loaded). */
        const char *server_name = safe_text(model->online_play_server_label, "Other server");
        char server_label[NK_ADHOC_SERVER_MAX_LEN + 16u];
        char server_sentence[NK_ADHOC_SERVER_MAX_LEN + 192u];
        bool server_fits;
        if (model->online_play_console_id_shown) {
            snprintf(console_line, sizeof(console_line),
                     "Console ID: %s. Nakagawa made it at random for this PC's games. It is not "
                     "your PC's hardware address.",
                     safe_text(model->online_play_console_id, ""));
        } else {
            snprintf(console_line, sizeof(console_line), "%s",
                     "Console ID: none yet. Nakagawa makes a random one the first time a game "
                     "asks for it. It is not your PC's hardware address.");
        }
        /* The host is never cut: on the button while "Server: host" fits the row, else the
           button reads "Server: Choose" and the host leads its sentence, which wraps whole. */
        snprintf(server_label, sizeof(server_label), "Server: %s", server);
        server_fits = nk_ui_button_width(ui, server_label, 0u) <= inner - 32.0f;
        if (server_fits) {
            snprintf(server_sentence, sizeof(server_sentence),
                     "%s. Choose to pick another.",
                     server_name);
        } else {
            snprintf(server_sentence, sizeof(server_sentence),
                     "%s: %s. Choose to pick another.",
                     server, server_name);
        }
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Online play", NK_UI_TYPE_TITLE, theme->paper);
        {
            const char *lines[] = {console_line, model->online_play_console_note,
                                   model->online_play_status, server_sentence};
            settings_online_play_keep_focus_in_view(ui, lines, sizeof(lines) / sizeof(lines[0]));
        }
        nk_ui_text_role(ui, FLOW_ID_LEAD, SCR_ONLINE_PLAY_LEAD, NK_UI_TYPE_BODY_S, theme->muted);
        if (scr_wizard_box_begin(ui, 0u)) {
            WizardRow rows[3] = {
                {"Online play", model->online_play ? "On" : "Off", switch_sentence},
                {"Server", server_fits ? server : "Choose", server_sentence},
                {"Port offset", safe_text(model->online_play_port_offset, "10000"),
                 SCR_ONLINE_PLAY_PORT_SENTENCE}
            };
            /* A sentence two lines cannot hold moves every sentence under its button, at any
               text size; so does a host too long for the button, whose sentence then breaks
               inside the host where it is wider than the row (nk_ui_text_break). */
            WizardColumn column = scr_wizard_column(ui, rows, 3u, inner - 32.0f, true);
            const NkUiTextStyle sentence_style = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
            if (!server_fits) {
                column.stacked = true;
                rows[1].sentence = nk_ui_text_break(ui, server_sentence, &sentence_style,
                                                    inner - 32.0f);
            }
            if (scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_TOGGLE,
                                  SET_ID_TEXT + 0x600u, "Online play", switch_sentence,
                                  model->online_play, inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ONLINE_PLAY);
            }
            if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_PRESET,
                                 SET_ID_TEXT + 0x604u, "Server", rows[1].value,
                                 rows[1].sentence, false, inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_ONLINE_PLAY_SERVERS);
            }
            if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_PORT,
                                 SET_ID_TEXT + 0x610u, "Port offset",
                                 safe_text(model->online_play_port_offset, "10000"),
                                 SCR_ONLINE_PLAY_PORT_SENTENCE, false, inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_EDIT_ONLINE_PLAY_PORT);
            }
            nk_ui_text_role(ui, SET_ID_TEXT + 0x614u, console_line, NK_UI_TYPE_BODY_S,
                            theme->muted);
            if (model->online_play_console_note && model->online_play_console_note[0]) {
                nk_ui_text_role(ui, SET_ID_TEXT + 0x615u, model->online_play_console_note,
                                NK_UI_TYPE_BODY_S, theme->paper);
            }
            nk_ui_text_role(ui, SET_ID_TEXT + 0x60Cu,
                            safe_text(model->online_play_status,
                                      "Online play is off. Nothing connects."),
                            NK_UI_TYPE_BODY_S,
                            model->online_play ? theme->ready : theme->muted);
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                /* A game keeps the console ID it started with, so the button waits for it to
                   close; with none yet there is nothing to replace. */
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_CONSOLE_ID, "New console ID",
                                 model->online_play_console_id_shown &&
                                         !model->online_play_console_id_locked
                                     ? 0u : NK_UI_BUTTON_DISABLED)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_NEW_CONSOLE_ID);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    }
    case PLAYER_UI_SETTINGS_PAGE_ONLINE_PLAY_CARD: {
        /* The consent card (owner's wording of 2026-11-19 and 2026-11-20): it names the exact
           server each time, and it is the only way Online play turns on. */
        char server_line[NK_ADHOC_SERVER_MAX_LEN + 96u];
        char nickname_line[160];
        const char *server = safe_text(model->online_play_card_server, NK_ADHOC_DEFAULT_SERVER);
        const bool scrolls = settings_online_play_card_scroll(ui);
        const NkUiTextStyle eyebrow_style = nk_ui_type(NK_UI_TYPE_EYEBROW, 0u);
        const NkUiTextStyle line_style = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
        {
            /* Exactly what the runtime contacts: the host and both ports, as the model read
               them with the runtime's own parser (player_ui_nk.c), the defaults written out. */
            const char *host = safe_text(model->online_play_card_host, "");
            if (host[0] != '\0' && model->online_play_card_lobby_port != 0u &&
                model->online_play_card_data_port != 0u) {
                snprintf(server_line, sizeof(server_line),
                         "To do this, Nakagawa connects to %s on ports %u and %u. This is a "
                         "community server. Nakagawa does not run it.",
                         host, model->online_play_card_lobby_port,
                         model->online_play_card_data_port);
            } else {
                snprintf(server_line, sizeof(server_line),
                         "To do this, Nakagawa connects to %s. This is a community server. "
                         "Nakagawa does not run it.", server);
            }
        }
        /* The login sends the console nickname, the registry's owner_name: this setting unless
           a game saved its own there (hle_network.c adhoc_fill_nickname). */
        snprintf(nickname_line, sizeof(nickname_line),
                 "- Your Nickname: %s, or the one a game saved instead.",
                 safe_text(model->nickname, NK_NICKNAME_DEFAULT));
        /* The eyebrow names the server where it fits on its one line; a host too long for it
           (a player's own) is named whole in the card's first line, broken inside the host
           where it is wider than the box. */
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND,
                         nk_ui_text_width(ui, nk_ui_caps(ui, server), &eyebrow_style) +
                                 2.0f * NK_UI_SPACE_5 <= inner
                             ? server : "Online play",
                         theme->info);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Online play", NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Online play lets this game find other players over the internet, the "
                        "same way it finds players nearby over local wireless.",
                        NK_UI_TYPE_BODY_S, theme->paper);
        if (scr_wizard_box_begin(ui, 0u)) {
            nk_ui_text_role(ui, SET_ID_TEXT + 0x780u,
                            nk_ui_text_break(ui, server_line, &line_style, inner - 32.0f),
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x781u, "What Nakagawa sends to that server",
                            NK_UI_TYPE_BODY, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x782u, nickname_line, NK_UI_TYPE_BODY_S,
                            theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x783u,
                            "- This game's ID: the ID of the game you start, for example ULUS12345",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x78Eu,
                            "- The name of the room the game makes or joins.", NK_UI_TYPE_BODY_S,
                            theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x784u,
                            "- A random console ID that Nakagawa made for you. It is not your "
                            "PC's hardware address.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x785u, "- The game's own multiplayer messages.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x786u, "Who can see what", NK_UI_TYPE_BODY,
                            theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x787u,
                            "- The people who run the server can see your IP address.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x788u,
                            "- Other players in your game may also see your IP address. This "
                            "depends on the server.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x789u,
                            "- Your Nickname, the game and your room name may appear on the "
                            "server's public status page.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x78Au,
                            "- The connection is not encrypted. Others on the network path could "
                            "read it.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x78Fu,
                            "While Online play is on, opening the server list also downloads the "
                            "current list from metadata.ppsspp.org.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x78Bu,
                            "Nakagawa never sends your saves, your game files, your PC's user "
                            "name or anything else from your PC.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x78Cu,
                            "Online play is off until you choose TURN ON. You can turn it off "
                            "again at any time in Settings > Online and developer > Online play. "
                            "Turning it off ends the connection.",
                            NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_end_container(ui);
        }
        if (scrolls) {
            nk_ui_text_role(ui, SET_ID_TEXT + 0x78Du,
                            "Page Up and Page Down, or the triggers, scroll this card.",
                            NK_UI_TYPE_CAPTION, theme->muted);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_ONLINE_PLAY_TURN_ON, "Turn on",
                                 NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_ONLINE_PLAY_TURN_ON);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_ONLINE_PLAY_NOT_NOW, "Not now", 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_ONLINE_PLAY_NOT_NOW);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_ONLINE_PLAY_CHANGE_SERVER, "Change server",
                                 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_ONLINE_PLAY_CHANGE_SERVER);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    }
    case PLAYER_UI_SETTINGS_PAGE_ONLINE_PLAY_SERVERS: {
        /* The server picker: the shipped servers, the player's own, and with Online play on
           PPSSPP's list. Choosing one opens the consent card for it; nothing here connects. */
        size_t count = model->online_play_servers ? model->online_play_server_count : 0u;
        const char *server = safe_text(model->online_play_server, "");
        /* Where PPSSPP's list came from is said once, in its heading, not on each row. */
        const char *list_heading = model->online_play_list_live ? "PPSSPP's list"
                                                                : "PPSSPP's list (shipped copy)";
        /* The list's panel keeps little padding of its own: the list's, there for the focus
           ring, is the rest. */
        const float row_width = inner - 16.0f - ENHANCED_SCROLLBAR_ROOM -
                                2.0f * ENHANCED_LIST_PAD;
        NkUiLayout box_layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                            0.0f);
        NkUiStyle box_style = scr_flow_box_style(theme, theme->surface);
        const NkUiTextStyle eyebrow = nk_ui_type(NK_UI_TYPE_EYEBROW, 0u);
        const NkUiTextStyle heading = nk_ui_type(NK_UI_TYPE_TITLE, 0u);
        const NkUiTextStyle label = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
        const NkUiTextStyle host_line_style = nk_ui_type(NK_UI_TYPE_DATA, 0u);
        char port_label[48];
        bool shipped_beside = true;
        bool listed_beside = true;
        bool own_current;
        const char *own_name;
        bool any_listed = false;
        OnlinePlayRowLayout shipped;
        OnlinePlayRowLayout from_list;
        OnlinePlayRowLayout own;
        NkUiId order[PLAYER_UI_ONLINE_PLAY_SERVERS_MAX + 1u];
        size_t order_count = 0u;
        NkUiId up = 0u;
        NkUiId down = 0u;
        float own_width;
        float port_width;
        /* The list holds what the card leaves between the bars once the heading, the intro and
           its hint, the box's padding and Back are placed, so the card itself does not scroll;
           where that is less than two rows (a larger text size in a small window), the list
           takes its whole height and the card scrolls instead. */
        float list_room = height - NK_UI_TOP_BAR_HEIGHT - NK_UI_PROMPT_BAR_HEIGHT -
                          2.0f * NK_UI_SPACE_4 - NK_UI_DIALOG_PAD_TOP - NK_UI_DIALOG_PAD_BOTTOM -
                          nk_ui_line_height(ui, &eyebrow) - nk_ui_line_height(ui, &heading) -
                          settings_enhanced_text_height(ui, SCR_ONLINE_PLAY_SERVERS_LEAD,
                                                        NK_UI_TYPE_BODY_S, inner) -
                          settings_enhanced_text_height(ui, SCR_ONLINE_PLAY_SERVERS_HINT,
                                                        NK_UI_TYPE_BODY_S, inner) -
                          4.0f * NK_UI_DIALOG_GAP - 12.0f - NK_UI_SPACE_1 -
                          nk_ui_line_box(ui, NK_UI_CONTROL_HEIGHT, &label) - 1.0f;
        if (list_room < 2.0f * ENHANCED_LIST_PAD + 2.0f * settings_online_play_row_height(ui) +
                            nk_ui_line_height(ui, &eyebrow)) {
            list_room = 0.0f;
        }
        if (count > PLAYER_UI_ONLINE_PLAY_SERVERS_MAX) count = PLAYER_UI_ONLINE_PLAY_SERVERS_MAX;
        own_current = model->online_play && server[0] != '\0';
        for (size_t i = 0u; i < count; ++i) {
            const PlayerUiOnlineServer *row = &model->online_play_servers[i];
            const char *host = safe_text(row->host, "");
            if (row->listed && !model->online_play) continue;
            if (strcmp(host, server) == 0) own_current = false;
            if (row->listed) {
                any_listed = true;
                listed_beside = listed_beside &&
                                settings_online_play_fits_beside(
                                    ui, safe_text(row->name, ""),
                                    settings_online_play_host_line(ui, row), NULL,
                                    model->online_play && strcmp(host, server) == 0, row_width);
            } else {
                shipped_beside = shipped_beside &&
                                 settings_online_play_fits_beside(
                                     ui, safe_text(row->name, ""), host, NULL,
                                     model->online_play && strcmp(host, server) == 0, row_width);
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0u; i < count; ++i) {
                if (model->online_play_servers[i].listed != (pass == 1)) continue;
                if (pass == 1 && !model->online_play) continue;
                order[order_count++] = (NkUiId)(PLAYER_UI_ID_ONLINE_PLAY_SERVER_BASE + i);
            }
            if (pass == 0) order[order_count++] = PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_SERVER;
        }
        shipped = settings_online_play_layout(ui, shipped_beside, NULL, row_width);
        from_list = settings_online_play_layout(ui, listed_beside, NULL, row_width);
        (void)snprintf(port_label, sizeof(port_label), "Port offset: %s",
                       safe_text(model->online_play_port_offset, "10000"));
        port_width = nk_ui_button_width(ui, port_label, 0u);
        own_width = row_width - NK_UI_SPACE_3 - port_width;
        /* The own row is "Other server" whether it is empty, holds a server typed in, or holds a
           consented server no shown list names (a list entry while the list is not loaded: it
           lasts one session); Current alone says it is the one in use (owner, 2026-11-20). */
        own_name = "Other server";
        /* The port offset beside the own server's row while that leaves the row half the
           width and its host whole on one line, else under it: a long typed host (up to 127
           characters) takes the whole width, so the row stays short enough for the list. */
        if (own_width < 0.5f * row_width ||
            2.0f * NK_UI_SPACE_3 + NK_UI_SPACE_3 +
                    ceilf(nk_ui_text_width(ui, own_current ? server : SCR_ONLINE_PLAY_OWN_FORMAT,
                                           &host_line_style)) +
                    settings_online_play_chips_width(ui, NULL, own_current) >
                own_width) {
            own_width = row_width;
        }
        own = settings_online_play_layout(
            ui, settings_online_play_fits_beside(ui, own_name,
                                                 own_current ? server : SCR_ONLINE_PLAY_OWN_FORMAT,
                                                 NULL, own_current, own_width),
            NULL, own_width);
        box_layout.padding_top = box_layout.padding_bottom = 6.0f;
        box_layout.padding_left = box_layout.padding_right = 8.0f;
        settings_online_play_reveal_row(ui, count);
        {
            NkUiId items[PLAYER_UI_ONLINE_PLAY_SERVERS_MAX + 12u];
            size_t item_count = 0u;
            items[item_count++] = SET_ID_ONLINE_PLAY_OWN_LINE;
            if (model->online_play) {
                items[item_count++] = SET_ID_ONLINE_PLAY_HEADING + 2u;
                items[item_count++] = SET_ID_TEXT + 0x7F4u;
                if (any_listed) items[item_count++] = SET_ID_TEXT + 0x7F5u;
            } else {
                items[item_count++] = SET_ID_ONLINE_PLAY_OFF_LINE;
            }
            for (size_t i = 0u; i < order_count; ++i) {
                if (order[i] != PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_SERVER) {
                    items[item_count++] = order[i];
                }
            }
            settings_online_play_snap(ui, items, item_count);
        }
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Online play", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Choose a server", NK_UI_TYPE_TITLE, theme->paper);
        /* The intro, and its hint on a line of its own under it, in one block. */
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        SCR_ONLINE_PLAY_SERVERS_LEAD "\n" SCR_ONLINE_PLAY_SERVERS_HINT,
                        NK_UI_TYPE_BODY_S, theme->muted);
        if (nk_ui_begin_container(ui, SET_ID_ONLINE_PLAY_BOX, &box_layout, &box_style)) {
            NkUiLayout list_row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              0.0f);
            NkUiLayout list = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 0.0f);
            list.padding_left = list.padding_right = ENHANCED_LIST_PAD;
            list.padding_top = list.padding_bottom = ENHANCED_LIST_PAD;
            if (list_room > 0.0f) {
                list.height.max = list_room;
                list.scroll = true;
            }
            if (nk_ui_begin_container(ui, SET_ID_ONLINE_PLAY_LIST_ROW, &list_row, NULL)) {
                if (nk_ui_begin_container(ui, SET_ID_ONLINE_PLAY_LIST, &list, NULL)) {
                    bool first_in_section = true;
                    /* No row carries a tag that every row near it would carry too: the
                       shipped servers come first, with no heading or chip, and PPSSPP's list
                       says once, in its heading, where it came from. Only Current marks a row. */
                    for (int pass = 0; pass < 2; ++pass) {
                        /* The shipped servers first; PPSSPP's list after the own server. */
                        if (pass == 1) {
                            if (!model->online_play) break;
                            const bool has_status = model->online_play_list_status &&
                                                    model->online_play_list_status[0] != '\0';
                            /* A new section: the launcher's gap between blocks (16 px,
                               docs/PLAYER_UI_DESIGN.md) above the heading, and its 8 px
                               heading-to-text gap under it; whichever line the first row
                               follows keeps 8 px, clear of the row's 6 px focus ring (owner
                               look note, 2026-11-20). */
                            settings_online_play_text(ui, SET_ID_ONLINE_PLAY_HEADING + 2u,
                                                      list_heading, NK_UI_TYPE_EYEBROW,
                                                      theme->muted, ONLINE_PLAY_SECTION_GAP,
                                                      ONLINE_PLAY_HEADING_GAP);
                            if (has_status) {
                                settings_online_play_text(ui, SET_ID_TEXT + 0x7F4u,
                                                          model->online_play_list_status,
                                                          NK_UI_TYPE_BODY_S, theme->muted, 0.0f,
                                                          ONLINE_PLAY_HEADING_GAP);
                            }
                            first_in_section = true;
                        }
                        for (size_t i = 0u; i < count; ++i) {
                            const PlayerUiOnlineServer *row = &model->online_play_servers[i];
                            const char *host = safe_text(row->host, "");
                            const NkUiId id = (NkUiId)(PLAYER_UI_ID_ONLINE_PLAY_SERVER_BASE + i);
                            if (row->listed != (pass == 1)) continue;
                            if (!first_in_section) {
                                nk_ui_divider(ui, (NkUiId)(SET_ID_TEXT + 0x800u + 8u * (NkUiId)i),
                                              true);
                            }
                            first_in_section = false;
                            settings_online_play_links(order, order_count, id, &up, &down);
                            if (settings_online_play_row(
                                    ui, id, safe_text(row->name, ""),
                                    settings_online_play_host_line(ui, row), NULL, NULL,
                                    model->online_play && strcmp(host, server) == 0, row_width,
                                    pass == 1 ? from_list : shipped, up, down)) {
                                *result = settings_op(PLAYER_UI_SETTINGS_PICK_ONLINE_PLAY_SERVER);
                                result->choice_index = (int)i;
                            }
                        }
                        if (pass == 0) {
                            NkUiLayout own_line = layout_make(
                                own_width < row_width ? NK_UI_LAYOUT_ROW : NK_UI_LAYOUT_COLUMN,
                                sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
                            own_line.align_y = NK_UI_ALIGN_CENTER;
                            /* No heading: the row is named Other server already, and its note
                               is its second line, inside the row and its focus ring. */
                            nk_ui_divider(ui, SET_ID_ONLINE_PLAY_OWN_DIVIDER, true);
                            settings_online_play_links(order, order_count,
                                                       PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_SERVER,
                                                       &up, &down);
                            if (nk_ui_begin_container(ui, SET_ID_ONLINE_PLAY_OWN_LINE, &own_line,
                                                      NULL)) {
                                if (settings_online_play_row(
                                        ui, PLAYER_UI_ID_SETTINGS_ONLINE_PLAY_SERVER, own_name,
                                        own_current ? server : SCR_ONLINE_PLAY_OWN_FORMAT,
                                        SCR_ONLINE_PLAY_OWN_SENTENCE, NULL, own_current,
                                        own_width, own, up, down)) {
                                    *result = settings_op(PLAYER_UI_SETTINGS_EDIT_ONLINE_PLAY_SERVER);
                                }
                                if (settings_online_play_port(ui, port_label, up, down)) {
                                    *result = settings_op(PLAYER_UI_SETTINGS_EDIT_ONLINE_PLAY_PORT);
                                }
                                nk_ui_end_container(ui);
                            }
                        }
                    }
                    if (model->online_play && any_listed) {
                        /* Clear of the last row's focus ring. */
                        settings_online_play_text(ui, SET_ID_TEXT + 0x7F5u,
                                                  SCR_ONLINE_PLAY_RELAY_ONLY_CAPTION,
                                                  NK_UI_TYPE_CAPTION, theme->muted,
                                                  NK_UI_FOCUS_RING_GAP + NK_UI_FOCUS_RING_WIDTH,
                                                  0.0f);
                    } else if (!model->online_play) {
                        settings_online_play_text(ui, SET_ID_ONLINE_PLAY_OFF_LINE,
                                                  SCR_ONLINE_PLAY_LIST_OFF_LINE, NK_UI_TYPE_CAPTION,
                                                  theme->muted, NK_UI_SPACE_2, 0.0f);
                    }
                    nk_ui_end_container(ui);
                }
                settings_enhanced_scrollbar(ui, SET_ID_ONLINE_PLAY_TRACK, SET_ID_ONLINE_PLAY_LIST);
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                /* Up from Back enters the list at its end, scrolled out of view or not. */
                if (nk_ui_button_between(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back",
                                         NK_UI_BUTTON_PRIMARY,
                                         order_count > 0u ? order[order_count - 1u] : 0u, 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                /* The list downloads only with Online play on. */
                if (model->online_play &&
                    nk_ui_button(ui, PLAYER_UI_ID_ONLINE_PLAY_REFRESH, "Refresh list",
                                 model->online_play_list_busy ? NK_UI_BUTTON_DISABLED : 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_REFRESH_ONLINE_PLAY_SERVERS);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    }
    case PLAYER_UI_SETTINGS_PAGE_ACCESSIBILITY:
        settings_accessibility(ui, model, inner, result);
        break;
    case PLAYER_UI_SETTINGS_PAGE_ACE:
        settings_ace(ui, model, inner, height, result);
        break;
    case PLAYER_UI_SETTINGS_PAGE_NOTICES:
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Third-party notices", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Build tools and their licences", NK_UI_TYPE_TITLE,
                        theme->paper);
        if (scr_wizard_box_begin(ui, 0u)) {
            NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
            for (size_t i = 0u; i < model->tool_count && i < PLAYER_UI_SETTINGS_TOOLS; ++i) {
                nk_ui_text(ui, (NkUiId)(SET_ID_TEXT + 0xA0u + (NkUiId)i),
                           nk_ui_text_clamp(ui, safe_text(model->tools[i], ""), &data,
                                            inner - 32.0f, 1), &data);
            }
            if (model->tool_count == 0u) {
                nk_ui_text_role(ui, SET_ID_TEXT + 0x9Fu,
                                "No build tools have been downloaded on this PC yet.",
                                NK_UI_TYPE_BODY_S, theme->muted);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Nakagawa Recomp is free software under the GNU GPL version 3. Its own "
                        "code also allows later versions; see LICENSE and NOTICE.md. Not "
                        "affiliated with or endorsed by Sony Interactive Entertainment; "
                        "PlayStation and PSP are its trademarks.",
                        NK_UI_TYPE_CAPTION, theme->muted);
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                if (model->tool_count > 0u &&
                    nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_LICENSE_TEXTS, "Open licence texts",
                                 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_OPEN_LICENSE_TEXTS);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE:
        /* #1306: the shared library switch, its clear button and tool removal. */
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Build tools and cache", NK_UI_TYPE_TITLE,
                        theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "A game disc carries system libraries that other games often share. "
                        "The compiled copies stay in the app-data folder on this PC and are "
                        "never uploaded or shared.",
                        NK_UI_TYPE_BODY_S, theme->muted);
        if (scr_wizard_box_begin(ui, 0u)) {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            const WizardRow rows[] = {
                {"Share libraries", settings_on_off(model->share_system_libraries),
                 "Builds keep each compiled library and reuse it for another game "
                 "with the same one, so those builds finish sooner."},
                {"Build speed", safe_text(model->build_speed, "Balanced"),
                 safe_text(model->build_speed_sentence, "")}
            };
            /* The two buttons line up on one column at every text size. */
            WizardColumn column = scr_wizard_column(ui, rows, sizeof(rows) / sizeof(rows[0]),
                                                    inner - 32.0f, false);
            if (scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_SHARE_LIBRARIES, SET_ID_TEXT + 0xC0u,
                              rows[0].name, rows[0].sentence,
                              model->share_system_libraries, inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_SHARE_LIBRARIES);
            }
            /* How much of the PC a game build takes: Background keeps a game smooth. */
            if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_BUILD_SPEED, SET_ID_TEXT + 0xC4u,
                                 rows[1].name, rows[1].value, rows[1].sentence, false,
                                 inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_BUILD_SPEED);
            }
            if (nk_ui_begin_container(ui, SET_ID_TEXT + 0xC8u, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_CLEAR_LIBRARIES,
                                 "Clear shared library cache", NK_UI_BUTTON_SMALL)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_CLEAR_SHARED_LIBRARIES);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_REMOVE_TOOLS, "Remove build tools",
                                 NK_UI_BUTTON_SMALL)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_OPEN_REMOVE_TOOLS);
                }
                nk_ui_end_container(ui);
            }
            {
                char cache_line[96];
                NkUiLayout cache_row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                                   NK_UI_SPACE_3);
                cache_row.align_y = NK_UI_ALIGN_CENTER;
                (void)snprintf(cache_line, sizeof(cache_line), "Cover art cache: %s",
                               safe_text(model->cover_art_cache_size, "0 KB"));
                if (nk_ui_begin_container(ui, SET_ID_TEXT + 0xCDu, &cache_row, NULL)) {
                    NkUiTextStyle label_style = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
                    nk_ui_text(ui, SET_ID_TEXT + 0xCEu, nk_ui_frame_text(ui, cache_line),
                               &label_style);
                    nk_ui_spacer(ui, SET_ID_TEXT + 0xCFu, sizing_grow(), sizing_fixed(1.0f));
                    if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_REFRESH_COVER_ART,
                                     "Refresh cover art", NK_UI_BUTTON_SMALL)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_REFRESH_COVER_ART);
                    }
                    nk_ui_end_container(ui);
                }
            }
            if (model->notice != NULL && model->notice[0] != '\0') {
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->warning);
                nk_ui_text(ui, SET_ID_TEXT + 0xCCu,
                           nk_ui_text_clamp(ui, model->notice, &text, inner - 32.0f, 2), &text);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_UPDATES:
        settings_updates(ui, model, inner, result);
        break;
    case PLAYER_UI_SETTINGS_PAGE_PROBLEM_REPORT:
        settings_problem_report(ui, model, inner, result);
        break;
    case PLAYER_UI_SETTINGS_PAGE_FONTS:
        /* Walkthrough blocker: the wizard's Fonts step is not on the Add to
           library path, so the import lives here, with each slot's status. */
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "PSP fonts", NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        /* #2005: what the stand-in fonts change, as font/README.md
                           describes them: letter shapes, Latin line widths within 3 %, and the
                           characters they lack. The Latin stand-ins have every character
                           of the PSP's Latin fonts, Cyrillic included (--latin-cyrillic-text
                           in mk/housekeeping.mk); the Japanese one lacks about 2,200 of the
                           PSP font's characters, all outside code page 932, and the Korean
                           one 512, none of them Hangul. */
                        "Optional. Without your PSP's own fonts, every game still runs and "
                        "shows its text in open stand-in fonts. They look different: the "
                        "letters are shaped differently, English lines come out a little wider "
                        "or narrower, and some rarely used characters of the PSP's Japanese "
                        "and Korean fonts are missing. For the PSP's own look, "
                        "connect your PSP by USB and choose Get fonts from my PSP, or copy the "
                        ".pgf files from flash0:/font on your PSP to a folder on this PC "
                        "(docs/SETUP.md, \"Copying the fonts from your PSP\") and choose them "
                        "here. The import reads ltn0.pgf to ltn15.pgf, jpn0.pgf and kr0.pgf "
                        "from that folder.",
                        NK_UI_TYPE_BODY_S, theme->muted);
        if (model->font_summary != NULL && model->font_summary[0] != '\0') {
            nk_ui_text_role(ui, SET_ID_TEXT + 0xDDu, model->font_summary, NK_UI_TYPE_BODY_S,
                            model->fonts_all_own ? theme->ready : theme->warning);
        }
        if (scr_wizard_box_begin(ui, 0u)) {
            NkUiLayout cols = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                          NK_UI_SPACE_4);
            if (nk_ui_begin_container(ui, SET_ID_TEXT + 0xD0u, &cols, NULL)) {
                float column = (inner - 32.0f - 2.0f * NK_UI_SPACE_4) / 3.0f;
                for (int c = 0; c < 3; ++c) {
                    NkUiLayout col = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column),
                                                 sizing_fit(), 2.0f);
                    if (!nk_ui_begin_container(ui, (NkUiId)(SET_ID_TEXT + 0xD1u + (NkUiId)c),
                                               &col, NULL)) {
                        continue;
                    }
                    for (size_t r = 0u; r < 6u; ++r) {
                        size_t item = (size_t)c * 6u + r;
                        NkUiTextStyle font = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                        if (item >= model->font_line_count) break;
                        nk_ui_text(ui, (NkUiId)(SET_ID_TEXT + 0xE0u + (NkUiId)item),
                                   nk_ui_text_clamp(ui, safe_text(model->font_lines[item], ""),
                                                    &font, column, 1), &font);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            if (model->font_message != NULL && model->font_message[0] != '\0') {
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_CAPTION,
                                                note_rgba(theme, model->font_message_kind));
                nk_ui_text(ui, SET_ID_TEXT + 0xDCu,
                           nk_ui_text_clamp(ui, model->font_message, &text, inner - 32.0f, 2),
                           &text);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_PSP_FONTS, "Get fonts from my PSP",
                                 NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_PSP_FONTS);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_IMPORT_FONTS, "Choose font files",
                                 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_IMPORT_FONTS);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_REMOVE_FONTS, "Remove imported fonts",
                                 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_REMOVE_FONTS);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_FACTORY_RESET:
        /* Factory reset: back to a first run, with the user data moved
           aside (player_app_factory_reset), never deleted. */
        scr_flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Factory reset", theme->warning);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Reset Nakagawa Recomp to a first run?",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Your library, saves, imported PSP fonts, keys, settings, built games "
                        "and downloaded build tools move into backup folders. Nakagawa Recomp "
                        "then starts again with the setup it shows the first time. Nothing is "
                        "deleted, and your disc images and logs stay where they are.",
                        NK_UI_TYPE_BODY, theme->muted);
        if (scr_wizard_box_begin(ui, 0u)) {
            NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x200u,
                            model->reset_folder_count > 0u
                                ? "These folders get \"-backup-\" and today's date and time "
                                  "added to their names:"
                                : "Nakagawa Recomp has no user data on this PC yet.",
                            NK_UI_TYPE_BODY_S, theme->muted);
            for (size_t i = 0u; i < model->reset_folder_count && i < PLAYER_UI_RESET_FOLDERS;
                 ++i) {
                /* The folders differ at their ends (config, data, cache,
                   saves), so a long one keeps its end and loses its middle. */
                char folder[600];
                settings_shorten_words(ui, safe_text(model->reset_folders[i], ""), folder,
                                       sizeof(folder), 72u);
                nk_ui_text(ui, (NkUiId)(SET_ID_TEXT + 0x201u + (NkUiId)i),
                           nk_ui_text_clamp(ui, folder, &data, inner - 32.0f, 1), &data);
            }
            if (model->reset_folder_count > 0u) {
                nk_ui_text_role(ui, SET_ID_TEXT + 0x20Fu,
                                "To undo it, close Nakagawa Recomp and give each folder its "
                                "old name back.",
                                NK_UI_TYPE_CAPTION, theme->muted);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Keep everything",
                                 NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                if (model->reset_folder_count > 0u &&
                    nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_CONFIRM_RESET, "Reset",
                                 NK_UI_BUTTON_DANGER)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_FACTORY_RESET);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_REMOVE_TOOLS:
    default:
        scr_flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Remove build tools", theme->warning);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Remove the downloaded build tools?",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "This removes the build tools Nakagawa downloaded. Your library, disc "
                        "images, saves and built games are kept. Nakagawa asks before it "
                        "downloads them again, the next time you build a game.",
                        NK_UI_TYPE_BODY, theme->muted);
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Keep them",
                                 NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_KEEP_TOOLS);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_CONFIRM_REMOVE, "Remove tools",
                                 NK_UI_BUTTON_DANGER)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_REMOVE_TOOLS);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    }
    if (model->page == PLAYER_UI_SETTINGS_PAGE_ACE) {
        scr_flow_end(ui, "", enhanced_prompts,
                     sizeof(enhanced_prompts) / sizeof(enhanced_prompts[0]));
    } else {
        scr_flow_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    }
}

PlayerUiAction player_ui_screen_settings(NkUi *ui, const PlayerUiSettingsModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Change"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    NkUiLayout layout;
    float width;
    float height;
    if (ui == NULL || model == NULL) return result;
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    if (model->page != PLAYER_UI_SETTINGS_PAGE_MAIN) {
        settings_dialog(ui, model, width, height, &result);
        return result;
    }
    (void)nk_ui_waves_element(ui, SET_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, SET_ID_ROOT, &layout, NULL)) return result;
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, SET_ID_TOP, PLAYER_UI_ROUTE_SETTINGS);
        if (tab != PLAYER_UI_ACTION_NONE) result = action(tab, 0u, PLAYER_UI_PRIMARY_NONE);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.padding_top = height >= 680.0f ? NK_UI_SPACE_7 : NK_UI_SPACE_4;
    layout.scroll = SCR_PAGE_BODY_SCROLLS;
    if (nk_ui_begin_container(ui, SET_ID_BODY, &layout, NULL)) {
        settings_main(ui, model, width, height, &result);
        nk_ui_end_container(ui);
    }
    nk_ui_prompt_bar(ui, SET_ID_PROMPTS, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    nk_ui_end_container(ui);
    return result;
}
