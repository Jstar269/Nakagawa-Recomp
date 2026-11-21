/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Love All, the launcher's hidden credits page, and the overlay the hidden moment draws
   (launcher_secret.h). Project-written. */

#include "launcher_secret.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <string.h>

#define LOVE_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_ABOUT, (offset))
#define LOVE_ID_ROOT LOVE_ID(0x00A000u)
#define LOVE_ID_TOP LOVE_ID(0x00A001u)
#define LOVE_ID_LIST LOVE_ID(0x00A002u)
#define LOVE_ID_COLUMN LOVE_ID(0x00A003u)
#define LOVE_ID_HEAD LOVE_ID(0x00A004u)
#define LOVE_ID_MARK LOVE_ID(0x00A005u)
#define LOVE_ID_TITLE LOVE_ID(0x00A006u)
#define LOVE_ID_LEAD LOVE_ID(0x00A007u)
#define LOVE_ID_THANKS LOVE_ID(0x00A008u)
#define LOVE_ID_ACTIONS LOVE_ID(0x00A009u)
#define LOVE_ID_BACK LOVE_ID(0x00A00Au)
#define LOVE_ID_PROMPTS LOVE_ID(0x00A00Bu)
#define LOVE_ID_WAVES LOVE_ID(0x00A00Cu)
#define LOVE_ID_TAIL LOVE_ID(0x00A00Du)
/* One block a credit: the block, its name, its people. */
#define LOVE_ID_ROWS LOVE_ID(0x00A100u)
#define LOVE_ID_ROWS_SPAN 0x400u
#define LOVE_ROW_IDS 4u
/* The moment's overlay. */
#define LOVE_ID_MOMENT_TILE_LAYER LOVE_ID(0x00A600u)
#define LOVE_ID_MOMENT_TILE LOVE_ID(0x00A601u)
#define LOVE_ID_MOMENT_MARK LOVE_ID(0x00A602u)
#define LOVE_ID_MOMENT_CONFETTI_LAYER LOVE_ID(0x00A603u)
#define LOVE_ID_MOMENT_CONFETTI LOVE_ID(0x00A604u)
#define LOVE_ID_MOMENT_TOAST_LAYER LOVE_ID(0x00A605u)
#define LOVE_ID_MOMENT_TOAST LOVE_ID(0x00A606u)
#define LOVE_ID_MOMENT_TOAST_TEXT LOVE_ID(0x00A607u)

typedef char love_all_rows_fit_their_span[
    LOVE_ROW_IDS * 256u <= LOVE_ID_ROWS_SPAN ? 1 : -1];

/* Overlay levels: over dialogs (1) and the focus ring (2). */
#define LOVE_Z_TILE 10
#define LOVE_Z_CONFETTI 11
#define LOVE_Z_TOAST 12

NkUiId player_ui_love_all_list_id(void) {
    return LOVE_ID_LIST;
}

static void love_all_credit(NkUi *ui, size_t index, const LoveAllCredit *credit, bool first) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiId base = (NkUiId)(LOVE_ID_ROWS + (NkUiId)(index * LOVE_ROW_IDS));
    NkUiLayout block = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 2.0f);
    block.align_x = NK_UI_ALIGN_CENTER;
    if (credit->kind == LOVE_ALL_HEADING) {
        block.padding_top = first ? NK_UI_SPACE_2 : NK_UI_SPACE_7;
        block.padding_bottom = NK_UI_SPACE_2;
    }
    if (credit->kind == LOVE_ALL_DEDICATION) block.padding_top = NK_UI_SPACE_7;
    if (!nk_ui_begin_container(ui, base, &block, NULL)) return;
    switch (credit->kind) {
        case LOVE_ALL_HEADING:
            nk_ui_text_role(ui, base + 1u, nk_ui_caps(ui, credit->name), NK_UI_TYPE_EYEBROW,
                            theme->accent_dark);
            break;
        case LOVE_ALL_DEDICATION:
            nk_ui_text_role(ui, base + 1u, credit->name, NK_UI_TYPE_BODY_L, theme->paper);
            break;
        case LOVE_ALL_ENTRY:
        default:
            nk_ui_text_role(ui, base + 1u, credit->name, NK_UI_TYPE_HEADING, theme->paper);
            if (credit->people != NULL && credit->people[0] != '\0') {
                nk_ui_text_role(ui, base + 2u, credit->people, NK_UI_TYPE_BODY_S, theme->muted);
            }
            break;
    }
    nk_ui_end_container(ui);
}

PlayerUiAction player_ui_screen_love_all(NkUi *ui, const PlayerUiLoveAllModel *model) {
    const NkUiTheme *theme;
    NkUiLayout layout;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float width;
    float height;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 720.0f;
    if (model->scroll_set) nk_ui_scroll_to(ui, LOVE_ID_LIST, model->scroll_offset);

    (void)nk_ui_waves_element(ui, LOVE_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, LOVE_ID_ROOT, &layout, NULL)) return result;
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, LOVE_ID_TOP, PLAYER_UI_ROUTE_ABOUT);
        if (tab != PLAYER_UI_ACTION_NONE) result = action(tab, 0u, PLAYER_UI_PRIMARY_NONE);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.padding_top = NK_UI_SPACE_6;
    layout.padding_left = layout.padding_right = NK_UI_GUTTER;
    layout.scroll = SCR_PAGE_BODY_SCROLLS;
    if (nk_ui_begin_container(ui, LOVE_ID_LIST, &layout, NULL)) {
        float column = width - 2.0f * NK_UI_GUTTER;
        if (column > 760.0f) column = 760.0f;
        layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column), sizing_fit(),
                             NK_UI_SPACE_3);
        layout.align_x = NK_UI_ALIGN_CENTER;
        layout.padding_bottom = NK_UI_SPACE_8;
        if (nk_ui_begin_container(ui, LOVE_ID_COLUMN, &layout, NULL)) {
            NkUiLayout head = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                          NK_UI_SPACE_3);
            head.align_x = NK_UI_ALIGN_CENTER;
            head.padding_bottom = NK_UI_SPACE_4;
            if (nk_ui_begin_container(ui, LOVE_ID_HEAD, &head, NULL)) {
                nk_ui_shapes(ui, LOVE_ID_MARK, sizing_fixed(64.0f), sizing_fixed(64.0f),
                             nk_ui_art_n1_mark());
                nk_ui_text_role(ui, LOVE_ID_TITLE, "LOVE ALL", NK_UI_TYPE_DISPLAY_XL,
                                theme->paper);
                nk_ui_text_role(ui, LOVE_ID_LEAD,
                                "Nothing all, and thanks to everyone: the projects Nakagawa "
                                "builds on, the people who made them, and you.",
                                NK_UI_TYPE_BODY_L, theme->muted);
                nk_ui_end_container(ui);
            }
            for (size_t i = 0u; i < g_love_all_credit_count && i < 256u; ++i) {
                love_all_credit(ui, i, &g_love_all_credits[i], i == 0u);
            }
            nk_ui_text_role(ui, LOVE_ID_THANKS,
                            "Thank you to everyone who reports a problem, tests a build, "
                            "or plays.",
                            NK_UI_TYPE_BODY, theme->muted);
            nk_ui_text_role(ui, LOVE_ID_TAIL, "The full list is in CREDITS.md and NOTICE.md.",
                            NK_UI_TYPE_FOOTNOTE, theme->dim);
            {
                NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), 0.0f);
                row.padding_top = NK_UI_SPACE_4;
                if (nk_ui_begin_container(ui, LOVE_ID_ACTIONS, &row, NULL)) {
                    if (nk_ui_button(ui, PLAYER_UI_ID_LOVE_ALL_BACK, "Back",
                                     NK_UI_BUTTON_PRIMARY)) {
                        result = action(PLAYER_UI_ACTION_BACK, 0u, PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    {
        static const NkUiPromptItem prompts[] = {
            {NK_UI_PROMPT_ACT_MOVE, "Scroll"},
            {NK_UI_PROMPT_ACT_BACK, "Back"}
        };
        nk_ui_prompt_bar(ui, LOVE_ID_PROMPTS, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    }
    nk_ui_end_container(ui);
    return result;
}

static uint32_t love_alpha(uint32_t rgba, float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return (rgba & 0xFFFFFF00u) | (uint32_t)((float)(rgba & 0xFFu) * alpha + 0.5f);
}

static void love_moment_tile(NkUi *ui, LauncherMomentShapes *shapes,
                             const LauncherMomentFrame *frame, float width, float height) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const NkUiShapeArt *mark = nk_ui_art_n1_mark();
    /* The tile shows while the wave is up and leaves as it settles. */
    float alpha = frame->ripple_strength * 2.5f;
    NkUiLayout layer = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(width), sizing_fixed(height),
                                   0.0f);
    NkUiLayout tile = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(248.0f), sizing_fixed(248.0f),
                                  0.0f);
    NkUiStyle style;
    NkUiShapeArt art;
    size_t count = launcher_ripple_shapes(frame->ripple_t, frame->ripple_strength, shapes->ripple,
                                          LAUNCHER_RIPPLE_SHAPES_MAX);
    if (alpha > 1.0f) alpha = 1.0f;
    if (count == 0u) return;
    layer.align_x = NK_UI_ALIGN_CENTER;
    layer.align_y = NK_UI_ALIGN_CENTER;
    tile.align_x = NK_UI_ALIGN_CENTER;
    tile.align_y = NK_UI_ALIGN_CENTER;
    memset(&style, 0, sizeof(style));
    style.fill_rgba = love_alpha(theme->surface_raised, 0.94f * alpha);
    style.border_rgba = love_alpha(theme->accent_dark, alpha);
    style.border_width = NK_UI_BORDER_HAIRLINE;
    style.corner_radius = NK_UI_RADIUS_DIALOG * 2.0f;
    /* The wave lifts the strips above and below the mark's own box: the art's view box has
       room for it, so the mark is drawn at the same size whatever the wave does. */
    art = *mark;
    art.view_y -= 14.0f;
    art.view_h += 28.0f;
    art.shapes = shapes->ripple;
    art.count = count;
    for (size_t i = 0u; i < count; ++i) {
        shapes->ripple[i].rgba = love_alpha(shapes->ripple[i].rgba, alpha);
    }
    if (!nk_ui_begin_overlay(ui, LOVE_ID_MOMENT_TILE_LAYER, &layer, NULL, LOVE_Z_TILE)) return;
    if (nk_ui_begin_container(ui, LOVE_ID_MOMENT_TILE, &tile, &style)) {
        nk_ui_shapes(ui, LOVE_ID_MOMENT_MARK, sizing_fixed(176.0f), sizing_fixed(200.0f), &art);
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

static void love_moment_confetti(NkUi *ui, LauncherMomentShapes *shapes,
                                 const LauncherMomentFrame *frame, float width, float height) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const uint32_t colors[] = {theme->accent_dark, theme->ready, theme->warning, theme->info,
                               theme->error, theme->paper};
    NkUiLayout layer = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(width), sizing_fixed(height),
                                   0.0f);
    NkUiShapeArt art;
    size_t count = launcher_confetti_shapes(frame->confetti_t, width, height, colors,
                                            sizeof(colors) / sizeof(colors[0]), shapes->confetti,
                                            LAUNCHER_CONFETTI_PIECES);
    if (count == 0u) return;
    memset(&art, 0, sizeof(art));
    art.view_w = width;
    art.view_h = height;
    art.shapes = shapes->confetti;
    art.count = count;
    if (!nk_ui_begin_overlay(ui, LOVE_ID_MOMENT_CONFETTI_LAYER, &layer, NULL, LOVE_Z_CONFETTI)) {
        return;
    }
    nk_ui_shapes(ui, LOVE_ID_MOMENT_CONFETTI, sizing_fixed(width), sizing_fixed(height), &art);
    nk_ui_end_container(ui);
}

static void love_moment_toast(NkUi *ui, const LauncherMomentFrame *frame, const char *toast,
                              float width, float height) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layer = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(width), sizing_fixed(height),
                                   0.0f);
    NkUiLayout pill = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), 0.0f);
    NkUiStyle style;
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_TITLE, love_alpha(theme->paper, frame->toast_alpha));
    layer.align_x = NK_UI_ALIGN_CENTER;
    layer.align_y = NK_UI_ALIGN_END;
    layer.padding_bottom = NK_UI_PROMPT_BAR_HEIGHT + NK_UI_SPACE_7 + frame->toast_rise;
    pill.padding_left = pill.padding_right = NK_UI_SPACE_7;
    pill.padding_top = pill.padding_bottom = NK_UI_SPACE_4;
    memset(&style, 0, sizeof(style));
    style.fill_rgba = love_alpha(theme->surface_high, frame->toast_alpha);
    style.border_rgba = love_alpha(theme->accent_dark, frame->toast_alpha);
    style.border_width = NK_UI_BORDER_HAIRLINE;
    style.corner_radius = NK_UI_RADIUS_PILL;
    if (!nk_ui_begin_overlay(ui, LOVE_ID_MOMENT_TOAST_LAYER, &layer, NULL, LOVE_Z_TOAST)) return;
    if (nk_ui_begin_container(ui, LOVE_ID_MOMENT_TOAST, &pill, &style)) {
        nk_ui_text(ui, LOVE_ID_MOMENT_TOAST_TEXT, toast, &text);
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

void launcher_moment_draw(NkUi *ui, LauncherMomentShapes *shapes, const LauncherMomentFrame *frame,
                          const char *toast, float width, float height) {
    if (ui == NULL || shapes == NULL || frame == NULL || !frame->active || !(width > 0.0f) ||
        !(height > 0.0f)) {
        return;
    }
    if (frame->ripple) love_moment_tile(ui, shapes, frame, width, height);
    if (frame->confetti) love_moment_confetti(ui, shapes, frame, width, height);
    if (frame->toast && toast != NULL && toast[0] != '\0' && frame->toast_alpha > 0.0f) {
        love_moment_toast(ui, frame, toast, width, height);
    }
}
