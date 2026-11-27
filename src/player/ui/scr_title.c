/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Title detail: the hero art and package status on the left; the name, status
   chips, actions and compatibility card on the right; the prompt bar below.
   Layout follows the design system (docs/PLAYER_UI_DESIGN.md). */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"
#ifdef NK_PLAYER_UI_REGRESSION_TEST
#include "nk_ui_draw.h"
#endif

#include <stdio.h>
#include <string.h>

#define TITLE_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_TITLE, (offset))
#define TITLE_ID_ROOT TITLE_ID(0x000001u)
#define TITLE_ID_TOP TITLE_ID(0x000010u)
#define TITLE_ID_BODY TITLE_ID(0x000020u)
#define TITLE_ID_ASIDE TITLE_ID(0x000021u)
#define TITLE_ID_MAIN TITLE_ID(0x000022u)
#define TITLE_ID_HERO TITLE_ID(0x000023u)
#define TITLE_ID_STATUS_CARD TITLE_ID(0x000009u)
#define TITLE_ID_STATUS_HEAD TITLE_ID(0x000024u)
#define TITLE_ID_STATUS_EYEBROW TITLE_ID(0x000025u)
#define TITLE_ID_STATUS_PILL TITLE_ID(0x000026u)
#define TITLE_ID_STATUS_DETAIL TITLE_ID(0x00000Au)
#define TITLE_ID_NOTE TITLE_ID(0x00000Bu)
#define TITLE_ID_NOTE_TEXT TITLE_ID(0x00000Cu)
#define TITLE_ID_CRUMB TITLE_ID(0x000002u)
#define TITLE_ID_NAME TITLE_ID(0x000003u)
#define TITLE_ID_DISC TITLE_ID(0x000004u)
#define TITLE_ID_CHIPS TITLE_ID(0x000027u)
#define TITLE_ID_CHIP_STATUS TITLE_ID(0x000028u)
#define TITLE_ID_CHIP_BADGE TITLE_ID(0x000029u)
#define TITLE_ID_ACTIONS TITLE_ID(0x00002Au)
#define TITLE_ID_BUILD_PREFLIGHT TITLE_ID(0x000030u)
#define TITLE_ID_BUILD_FOLDER TITLE_ID(0x000031u)
#define TITLE_ID_INPUT_ACTIONS TITLE_ID(0x000032u)
#define TITLE_ID_INPUT_GUIDE TITLE_ID(0x000100u)
#define TITLE_ID_STALL_NOTICE TITLE_ID(0x000035u)
#define TITLE_ID_STALL_EYEBROW TITLE_ID(0x000036u)
#define TITLE_ID_STALL_TEXT TITLE_ID(0x000037u)
#define TITLE_ID_STALL_ACTIONS TITLE_ID(0x000038u)
#define TITLE_ID_RENDER_NOTICE TITLE_ID(0x000070u)
#define TITLE_ID_RENDER_HEAD TITLE_ID(0x000071u)
#define TITLE_ID_RENDER_EYEBROW TITLE_ID(0x000072u)
#define TITLE_ID_RENDER_PILL TITLE_ID(0x000073u)
#define TITLE_ID_RENDER_TEXT TITLE_ID(0x000074u)
#define TITLE_ID_FONT_NOTE TITLE_ID(0x000078u)
#define TITLE_ID_FONT_EYEBROW TITLE_ID(0x000079u)
#define TITLE_ID_FONT_TEXT TITLE_ID(0x00007Au)
#define TITLE_ID_FONT_ACTIONS TITLE_ID(0x00007Bu)
#define TITLE_ID_START_CARD TITLE_ID(0x000039u)
#define TITLE_ID_START_NAME TITLE_ID(0x00003Au)
#define TITLE_ID_START_TEXT TITLE_ID(0x00003Bu)
#define TITLE_ID_START_BAR TITLE_ID(0x00003Cu)
#define TITLE_ID_ACHIEVEMENTS TITLE_ID(0x000080u)
#define TITLE_ID_ACHIEVEMENTS_HEAD TITLE_ID(0x000081u)
#define TITLE_ID_ACHIEVEMENTS_STATUS TITLE_ID(0x000082u)
#define TITLE_ID_ACHIEVEMENTS_PROGRESS TITLE_ID(0x000083u)
#define TITLE_ID_ACHIEVEMENTS_COUNT TITLE_ID(0x000084u)
#define TITLE_ID_ACHIEVEMENTS_POINTS TITLE_ID(0x000085u)
#define TITLE_ID_ACHIEVEMENTS_BAR TITLE_ID(0x000086u)
#define TITLE_ID_ACHIEVEMENTS_NOTICE TITLE_ID(0x000087u)
#define TITLE_ID_ACHIEVEMENTS_NOTICE_SLOT TITLE_ID(0x000088u)
#define TITLE_ID_ACHIEVEMENTS_NOTICE_ICON TITLE_ID(0x000089u) /* the slot's art: slot + 1 */
#define TITLE_ID_ACHIEVEMENTS_NOTICE_TEXT TITLE_ID(0x00008Au)
#define TITLE_ID_ACHIEVEMENTS_NOTICE_EYEBROW TITLE_ID(0x00008Bu)
#define TITLE_ID_ACHIEVEMENTS_NOTICE_REASON TITLE_ID(0x00008Cu)
#define TITLE_ID_ACHIEVEMENTS_LIST TITLE_ID(0x00008Du)
#define TITLE_ID_ACHIEVEMENTS_SPACER TITLE_ID(0x00008Eu)
#define TITLE_ID_ACHIEVEMENTS_MORE TITLE_ID(0x0000D0u)
/* One achievement row's parts, TITLE_ID_ACH_ROW_PARTS to a row: + 0 the row, + 1 its icon
   slot, + 2 the slot's picture or glyph, + 3 its text column, + 4 its title, + 5 its
   description, + 6 its points pill, + 7 the pill's text. */
#define TITLE_ID_ACH_ROW TITLE_ID(0x001000u)
#define TITLE_ID_ACH_ROW_SPAN 0x1000u
#define TITLE_ID_ACH_ROW_PARTS 16u
#define TITLE_ACH_ROWS_MAX (TITLE_ID_ACH_ROW_SPAN / TITLE_ID_ACH_ROW_PARTS)
#define TITLE_ID_COMPAT TITLE_ID(0x000005u)
#define TITLE_ID_COMPAT_HEAD TITLE_ID(0x00002Cu)
#define TITLE_ID_COMPAT_EYEBROW TITLE_ID(0x000006u)
#define TITLE_ID_COMPAT_TIER TITLE_ID(0x000007u)
#define TITLE_ID_COMPAT_EXPECT TITLE_ID(0x000008u)
#define TITLE_ID_COMPAT_ISSUES TITLE_ID(0x00002Eu)
#define TITLE_ID_COMPAT_CATEGORY TITLE_ID(0x00002Du)
#define TITLE_ID_REMOVE_ROW TITLE_ID(0x00002Fu)
#define TITLE_ID_ENH_CARD TITLE_ID(0x000060u)
#define TITLE_ID_ENH_HEAD TITLE_ID(0x000061u)
#define TITLE_ID_ENH_EYEBROW TITLE_ID(0x000062u)
#define TITLE_ID_ENH_TAG TITLE_ID(0x000063u)
#define TITLE_ID_ENH_ROW TITLE_ID(0x000064u)
#define TITLE_ID_ENH_TEXT TITLE_ID(0x000065u)
#define TITLE_ID_ENH_PROBLEM TITLE_ID(0x000066u)
#define TITLE_ID_ENH_SESSION TITLE_ID(0x000067u)
#define TITLE_ID_ACE_ROW TITLE_ID(0x000068u)
#define TITLE_ID_ACE_TEXT TITLE_ID(0x000069u)
#define TITLE_ID_ACE_LAST TITLE_ID(0x00006Au)
#define TITLE_ID_PROMPTS TITLE_ID(0x000040u)
#define TITLE_ID_WAVES TITLE_ID(0x000050u)

/* docs/COMPATIBILITY.md category -> player tier, for a title with no player
   record of its own (#1297). The same table is in the "Status words" section
   of docs/COMPATIBILITY.md; test_player_screens.c pins both. A category
   describes the project's test route, so each maps to the most a player can
   count on: a title that only boots to a named boundary is not promised more
   than its title screen. Experimental and Untested have no tier. */
static const struct {
    const char *category;
    PlayerUiTier tier;
} s_tier_by_category[] = {
    {"Works", PLAYER_UI_TIER_PERFECT},
    {"Partially works", PLAYER_UI_TIER_IN_GAME},
    {"Boots to a named boundary", PLAYER_UI_TIER_TITLE_SCREEN},
    {"Not started", PLAYER_UI_TIER_WONT_BOOT},
    {"Not supported", PLAYER_UI_TIER_WONT_BOOT},
    {"Experimental", PLAYER_UI_TIER_NONE},
    {"Untested", PLAYER_UI_TIER_NONE}
};

PlayerUiTier player_ui_tier_for_category(const char *category) {
    if (category == NULL) return PLAYER_UI_TIER_NONE;
    for (size_t i = 0u; i < sizeof(s_tier_by_category) / sizeof(s_tier_by_category[0]);
         ++i) {
        if (strcmp(category, s_tier_by_category[i].category) == 0) {
            return s_tier_by_category[i].tier;
        }
    }
    return PLAYER_UI_TIER_NONE;
}

const char *player_ui_tier_word(PlayerUiTier tier) {
    switch (tier) {
        case PLAYER_UI_TIER_PERFECT: return NK_UI_STR(TITLE_TIER_PERFECT);
        case PLAYER_UI_TIER_PLAYABLE: return NK_UI_STR(TITLE_TIER_PLAYABLE);
        case PLAYER_UI_TIER_IN_GAME: return NK_UI_STR(TITLE_TIER_IN_GAME);
        case PLAYER_UI_TIER_MENU: return NK_UI_STR(TITLE_TIER_MENU);
        case PLAYER_UI_TIER_TITLE_SCREEN: return NK_UI_STR(TITLE_TIER_TITLE_SCREEN);
        case PLAYER_UI_TIER_WONT_BOOT: return NK_UI_STR(TITLE_TIER_WONT_BOOT);
        case PLAYER_UI_TIER_NONE:
        default: return NULL;
    }
}

/* How a tier chip is drawn; the word itself comes from the host's data. */
static uint32_t tier_rgba(const NkUiTheme *theme, PlayerUiTier tier) {
    switch (tier) {
        case PLAYER_UI_TIER_PERFECT:
        case PLAYER_UI_TIER_PLAYABLE: return theme->ready;
        case PLAYER_UI_TIER_IN_GAME:
        case PLAYER_UI_TIER_MENU: return theme->info;
        case PLAYER_UI_TIER_TITLE_SCREEN: return theme->warning;
        case PLAYER_UI_TIER_WONT_BOOT: return theme->error;
        case PLAYER_UI_TIER_NONE:
        default: return theme->muted;
    }
}

/* The package card's sentence for each package state (#1297). */
static const char *package_state_text(PlayerUiPackageState state, NkHostKind host) {
    switch (state) {
        case PLAYER_UI_PACKAGE_READY:
            return player_ui_host_is_pc(host) ? NK_UI_STR(TITLE_PACKAGE_READY)
                                              : NK_UI_STR(TITLE_PACKAGE_READY_DEVICE);
        case PLAYER_UI_PACKAGE_CHECKING: return NK_UI_STR(TITLE_PACKAGE_CHECKING);
        case PLAYER_UI_PACKAGE_BUILDING: return NK_UI_STR(TITLE_PACKAGE_BUILDING);
        case PLAYER_UI_PACKAGE_CHECK_FAILED: return NK_UI_STR(TITLE_PACKAGE_CHECK_FAILED);
        case PLAYER_UI_PACKAGE_STARTING:
            return NK_UI_STR(TITLE_PACKAGE_STARTING);
        case PLAYER_UI_PACKAGE_RUNNING:
            return NK_UI_STR(TITLE_PACKAGE_RUNNING);
        case PLAYER_UI_PACKAGE_MISSING:
        default: return NK_UI_STR(TITLE_PACKAGE_NOT_READY);
    }
}

/* ---- RetroAchievements card (#1316) ---------------------------------------------------- */

/* The glyphs an achievement's slot shows when it has no badge picture, drawn from the UI's own
   shapes in a 48 x 48 box: a filled trophy for an unlocked one, a padlock outline for a locked
   one, and an exclamation in a ring for the card's notice. An outline is a filled shape with
   the slot's own fill drawn inside it, so `ground` is the slot's fill. The arrays are static
   (nk_ui_shapes keeps a pointer until the frame is drawn) and coloured each frame from the
   theme. */
static NkUiShape g_trophy[] = {
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {12.5f, 17.0f, 5.5f}},
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {12.5f, 17.0f, 3.0f}},
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {35.5f, 17.0f, 5.5f}},
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {35.5f, 17.0f, 3.0f}},
    {NK_UI_SHAPE_RECT, 0u, 1.5f, 0u, {13.0f, 8.0f, 22.0f, 4.0f}},
    {NK_UI_SHAPE_POLY, 0u, 0.0f, 4u, {14.5f, 11.0f, 33.5f, 11.0f, 32.0f, 24.0f, 16.0f, 24.0f}},
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {24.0f, 23.5f, 8.0f}},
    {NK_UI_SHAPE_RECT, 0u, 0.0f, 0u, {22.0f, 30.0f, 4.0f, 6.0f}},
    {NK_UI_SHAPE_RECT, 0u, 1.5f, 0u, {16.5f, 35.5f, 15.0f, 4.0f}},
    {NK_UI_SHAPE_RECT, 0u, 1.5f, 0u, {14.0f, 39.0f, 20.0f, 3.5f}},
};
static const bool g_trophy_ground[] = {false, true, false, true, false,
                                       false, false, false, false, false};
static NkUiShape g_padlock[] = {
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {24.0f, 19.0f, 9.0f}},
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {24.0f, 19.0f, 6.5f}},
    {NK_UI_SHAPE_RECT, 0u, 0.0f, 0u, {14.0f, 19.0f, 20.0f, 6.0f}},
    {NK_UI_SHAPE_RECT, 0u, 0.0f, 0u, {15.0f, 19.0f, 2.5f, 6.0f}},
    {NK_UI_SHAPE_RECT, 0u, 0.0f, 0u, {30.5f, 19.0f, 2.5f, 6.0f}},
    {NK_UI_SHAPE_RECT, 0u, 3.0f, 0u, {11.5f, 24.0f, 25.0f, 18.0f}},
    {NK_UI_SHAPE_RECT, 0u, 1.5f, 0u, {14.0f, 26.5f, 20.0f, 13.0f}},
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {24.0f, 31.5f, 2.4f}},
    {NK_UI_SHAPE_RECT, 0u, 0.8f, 0u, {23.0f, 32.5f, 2.0f, 4.5f}},
};
static const bool g_padlock_ground[] = {false, true, true, false, false,
                                        false, true, false, false};
static NkUiShape g_notice_mark[] = {
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {24.0f, 24.0f, 15.0f}},
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {24.0f, 24.0f, 12.5f}},
    {NK_UI_SHAPE_RECT, 0u, 1.4f, 0u, {22.6f, 15.0f, 2.8f, 11.5f}},
    {NK_UI_SHAPE_CIRCLE, 0u, 0.0f, 0u, {24.0f, 31.5f, 1.9f}},
};
static const bool g_notice_ground[] = {false, true, false, false};

static void title_glyph_colour(NkUiShape *shapes, const bool *ground, size_t count,
                               uint32_t ink, uint32_t fill) {
    for (size_t i = 0; i < count; i++) shapes[i].rgba = ground[i] ? fill : ink;
}

/* The icon slot: `side` px square, framed in the accent for an unlocked achievement and in the
   hairline for a locked one, holding the badge picture when there is one, else `glyph`. */
static void title_achievement_slot(NkUi *ui, NkUiId id, float side, bool accent_frame,
                                   uint32_t fill, const NkUiImage *badge,
                                   const NkUiShape *glyph, size_t glyph_count) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(side), sizing_fixed(side),
                                    0.0f);
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.align_y = NK_UI_ALIGN_CENTER;
    style.fill_rgba = fill;
    style.border_rgba = accent_frame ? theme->accent_dark : theme->line_strong;
    style.border_width = accent_frame ? 2.0f : NK_UI_BORDER_HAIRLINE;
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    if (!nk_ui_begin_container(ui, id, &layout, &style)) return;
    if (badge != NULL) {
        NkUiImageArt art;
        memset(&art, 0, sizeof(art));
        art.contain = badge;
        art.contain_scale = 1.0f;
        (void)nk_ui_image_art(ui, id + 1u, sizing_fixed(side - 8.0f),
                              sizing_fixed(side - 8.0f), &art);
    } else {
        NkUiShapeArt art;
        memset(&art, 0, sizeof(art));
        art.view_w = 48.0f;
        art.view_h = 48.0f;
        art.shapes = glyph;
        art.count = glyph_count;
        (void)nk_ui_shapes(ui, id + 1u, sizing_fixed(side - 6.0f), sizing_fixed(side - 6.0f),
                           &art);
    }
    nk_ui_end_container(ui);
}

/* The icon side: 48 px at 100 % text, growing with the text so it stays as tall as a row's
   two lines. */
static float title_achievement_icon(const NkUi *ui) {
    const float scale = nk_ui_text_scale(ui);
    return (float)(int)(48.0f * (scale > 1.0f ? scale : 1.0f) + 0.5f);
}

/* One achievement: its icon slot, its title in the strong style (two lines at most), its
   description under it in the dim style (one line), and its points in a pill at the right:
   the number alone, since the card's progress line above says "N of M points" once.
   An unlocked one is at full contrast with the accent on its frame; a locked one is dimmed. */
static void title_achievement_row(NkUi *ui, size_t index, const PlayerUiAchievementRow *row,
                                  float inner) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const NkUiId id = TITLE_ID_ACH_ROW + (NkUiId)(index * TITLE_ID_ACH_ROW_PARTS);
    const float side = title_achievement_icon(ui);
    const bool unlocked = row->unlocked;
    char points[16];
    NkUiTextStyle pill_text = nk_ui_type(NK_UI_TYPE_LABEL_S, unlocked ? theme->paper
                                                                       : theme->muted);
    (void)snprintf(points, sizeof(points), NK_UI_STR(TITLE_ACHIEVEMENT_ROW_POINTS_FORMAT), (unsigned)row->points);
    const char *pill_label = nk_ui_frame_text(ui, points);
    const float pill_w = nk_ui_text_width(ui, pill_label, &pill_text) + 2.0f * 10.0f + 2.0f;
    const float text_w = inner - side - pill_w - 2.0f * NK_UI_SPACE_3 - 2.0f * NK_UI_SPACE_2;
    NkUiLayout layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                    NK_UI_SPACE_3);
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = NK_UI_SPACE_2;
    layout.padding_right = NK_UI_SPACE_2;
    layout.padding_top = NK_UI_SPACE_2;
    layout.padding_bottom = NK_UI_SPACE_2;
    style.fill_rgba = unlocked ? theme->surface_high : 0u;
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    if (!nk_ui_begin_container(ui, id, &layout, &style)) return;
    {
        const uint32_t fill = unlocked ? theme->surface_raised : theme->surface;
        if (unlocked) {
            title_glyph_colour(g_trophy, g_trophy_ground, sizeof(g_trophy) / sizeof(g_trophy[0]),
                               theme->accent_dark, theme->surface_raised);
        } else {
            title_glyph_colour(g_padlock, g_padlock_ground,
                               sizeof(g_padlock) / sizeof(g_padlock[0]), theme->dim,
                               theme->surface);
        }
        title_achievement_slot(ui, id + 1u, side, unlocked, fill, row->badge,
                               unlocked ? g_trophy : g_padlock,
                               unlocked ? sizeof(g_trophy) / sizeof(g_trophy[0])
                                        : sizeof(g_padlock) / sizeof(g_padlock[0]));
    }
    {
        NkUiLayout column = layout_make(NK_UI_LAYOUT_COLUMN,
                                        sizing_fixed(text_w > 40.0f ? text_w : 40.0f),
                                        sizing_fit(), 2.0f);
        if (nk_ui_begin_container(ui, id + 3u, &column, NULL)) {
            NkUiTextStyle title = nk_ui_type(NK_UI_TYPE_TILE_TITLE,
                                             unlocked ? theme->paper : theme->muted);
            NkUiTextStyle words = nk_ui_type(NK_UI_TYPE_CAPTION,
                                             unlocked ? theme->muted : theme->dim);
            nk_ui_text(ui, id + 4u,
                       nk_ui_text_clamp(ui, safe_text(row->title, NK_UI_STR(TITLE_ACHIEVEMENT_UNTITLED)),
                                        &title, text_w, 2), &title);
            if (row->description != NULL && row->description[0] != '\0') {
                nk_ui_text(ui, id + 5u,
                           nk_ui_text_clamp(ui, row->description, &words, text_w, 1), &words);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout pill = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(),
                                      sizing_fixed(nk_ui_line_box(ui, NK_UI_CHIP_HEIGHT,
                                                                  &pill_text)), 0.0f);
        NkUiStyle chip;
        memset(&chip, 0, sizeof(chip));
        pill.align_y = NK_UI_ALIGN_CENTER;
        pill.padding_left = 10.0f;
        pill.padding_right = 10.0f;
        chip.fill_rgba = theme->surface_high;
        chip.border_rgba = unlocked ? theme->accent_dark : theme->line_strong;
        chip.border_width = NK_UI_BORDER_HAIRLINE;
        chip.corner_radius = NK_UI_RADIUS_PILL;
        if (nk_ui_begin_container(ui, id + 6u, &pill, &chip)) {
            nk_ui_text(ui, id + 7u, pill_label, &pill_text);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* The card's notice (cheats on, the 60 FPS game logic): the toast's shape without points, an
   icon slot, an eyebrow, and the reason in the dim style. */
static void title_achievements_notice(NkUi *ui, const char *reason, float inner) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const float side = title_achievement_icon(ui);
    const float text_w = inner - side - NK_UI_SPACE_3 - 2.0f * NK_UI_SPACE_3;
    NkUiLayout layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                    NK_UI_SPACE_3);
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = NK_UI_SPACE_3;
    layout.padding_right = NK_UI_SPACE_3;
    layout.padding_top = NK_UI_SPACE_3;
    layout.padding_bottom = NK_UI_SPACE_3;
    style.fill_rgba = theme->surface_high;
    style.border_rgba = theme->warning;
    style.border_width = NK_UI_BORDER_HAIRLINE;
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    if (!nk_ui_begin_container(ui, TITLE_ID_ACHIEVEMENTS_NOTICE, &layout, &style)) return;
    title_glyph_colour(g_notice_mark, g_notice_ground,
                       sizeof(g_notice_mark) / sizeof(g_notice_mark[0]), theme->warning,
                       theme->surface_raised);
    title_achievement_slot(ui, TITLE_ID_ACHIEVEMENTS_NOTICE_SLOT, side, false,
                           theme->surface_raised, NULL, g_notice_mark,
                           sizeof(g_notice_mark) / sizeof(g_notice_mark[0]));
    {
        NkUiLayout column = layout_make(NK_UI_LAYOUT_COLUMN,
                                        sizing_fixed(text_w > 40.0f ? text_w : 40.0f),
                                        sizing_fit(), 2.0f);
        if (nk_ui_begin_container(ui, TITLE_ID_ACHIEVEMENTS_NOTICE_TEXT, &column, NULL)) {
            NkUiTextStyle words = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
            nk_ui_text_role(ui, TITLE_ID_ACHIEVEMENTS_NOTICE_EYEBROW,
                            nk_ui_caps(ui, NK_UI_STR(TITLE_ACHIEVEMENT_NONE)), NK_UI_TYPE_EYEBROW,
                            theme->warning);
            nk_ui_text(ui, TITLE_ID_ACHIEVEMENTS_NOTICE_REASON, reason, &words);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* The RetroAchievements card: "RetroAchievements · Casual" once (Casual Mode, which
   RetroAchievements formerly called Softcore), the set's progress (how
   many are unlocked, the points earned of the total, and a bar), the notice when nothing is
   earned now, the status line while no set is listed, then every achievement the player
   keeps, unlocked first. */
static void title_achievements_card(NkUi *ui, const PlayerUiTitleModel *model, float inner) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    if (!nk_ui_card_begin(ui, TITLE_ID_ACHIEVEMENTS, sizing_grow(), sizing_fit(),
                          NK_UI_SPACE_3)) {
        return;
    }
    nk_ui_eyebrow(ui, TITLE_ID_ACHIEVEMENTS_HEAD, NK_UI_STR(TITLE_ACHIEVEMENTS_HEADING));
    if (model->achievement_count > 0u) {
        char count[64];
        char points[64];
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                     NK_UI_SPACE_3);
        const size_t total = model->achievement_total > model->achievement_count
                                 ? model->achievement_total : model->achievement_count;
        row.align_y = NK_UI_ALIGN_END;
        (void)snprintf(count, sizeof(count), NK_UI_STR(TITLE_ACHIEVEMENT_UNLOCKED_FORMAT),
                       (unsigned)model->achievements_unlocked, total);
        (void)snprintf(points, sizeof(points), NK_UI_STR(TITLE_ACHIEVEMENT_POINTS_FORMAT),
                       (unsigned)model->achievements_points_unlocked,
                       (unsigned)model->achievements_points_total);
        if (nk_ui_begin_container(ui, TITLE_ID_ACHIEVEMENTS_PROGRESS, &row, NULL)) {
            /* Each one line at its own width, so the points end where the bar ends. */
            NkUiLayout spacer = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fixed(1.0f),
                                            0.0f);
            NkUiTextStyle count_style = nk_ui_type(NK_UI_TYPE_TILE_TITLE, theme->paper);
            NkUiTextStyle points_style = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
            count_style.wrap = false;
            points_style.wrap = false;
            nk_ui_text(ui, TITLE_ID_ACHIEVEMENTS_COUNT, nk_ui_frame_text(ui, count), &count_style);
            if (nk_ui_begin_container(ui, TITLE_ID_ACHIEVEMENTS_SPACER, &spacer, NULL))
                nk_ui_end_container(ui);
            nk_ui_text(ui, TITLE_ID_ACHIEVEMENTS_POINTS, nk_ui_frame_text(ui, points),
                       &points_style);
            nk_ui_end_container(ui);
        }
        nk_ui_progress_bar(ui, TITLE_ID_ACHIEVEMENTS_BAR, inner,
                           total ? (float)model->achievements_unlocked / (float)total : 0.0f,
                           0.0);
    }
    if (model->achievements_notice != NULL && model->achievements_notice[0] != '\0') {
        title_achievements_notice(ui, model->achievements_notice, inner);
    }
    /* The status line says how the set is loaded; once it is, the progress above says it. */
    if (model->achievement_count == 0u && model->achievements_status != NULL &&
        model->achievements_status[0] != '\0') {
        nk_ui_text_role(ui, TITLE_ID_ACHIEVEMENTS_STATUS, model->achievements_status,
                        NK_UI_TYPE_BODY_S, theme->muted);
    }
    if (model->achievement_count > 0u && model->achievement_rows != NULL) {
        NkUiLayout list = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                      NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, TITLE_ID_ACHIEVEMENTS_LIST, &list, NULL)) {
            for (size_t i = 0; i < model->achievement_count && i < TITLE_ACH_ROWS_MAX; i++)
                title_achievement_row(ui, i, &model->achievement_rows[i], inner);
            nk_ui_end_container(ui);
        }
    }
    if (model->achievements_more[0]) {
        nk_ui_text_role(ui, TITLE_ID_ACHIEVEMENTS_MORE, model->achievements_more,
                        NK_UI_TYPE_FOOTNOTE, theme->dim);
    }
    nk_ui_end_container(ui);
}

#ifdef NK_PLAYER_UI_REGRESSION_TEST
static bool title_find_cmd(const NkUiDrawList *list, NkUiId id, int kind, int out[4]) {
    for (uint32_t i = 0; list != NULL && i < list->ncmd; i++) {
        if (list->cmd[i].element_id == id && (kind < 0 || list->cmd[i].kind == kind)) {
            memcpy(out, list->cmd[i].bounds, sizeof(int) * 4u);
            return true;
        }
    }
    return false;
}

/* Whether part `id` is drawn and partly hidden by a scissor (the page scrolled it half out). */
static bool title_part_hidden(const NkUiDrawList *list, NkUiId id, int kind) {
    for (uint32_t i = 0; list != NULL && i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->element_id == id && c->kind == kind &&
            (c->clip[0] > c->bounds[0] || c->clip[1] > c->bounds[1] ||
             c->clip[2] < c->bounds[2] || c->clip[3] < c->bounds[3])) {
            return true;
        }
    }
    return false;
}

/* What row part `id` drew: 2 for a picture, 1 for anything else (the drawn glyph), 0 nothing. */
static int title_drawn_art(const NkUiDrawList *list, NkUiId id) {
    int art = 0;
    for (uint32_t i = 0; list != NULL && i < list->ncmd; i++) {
        if (list->cmd[i].element_id != id || list->cmd[i].kind == NK_UI_DRAW_SCISSOR_START ||
            list->cmd[i].kind == NK_UI_DRAW_SCISSOR_END) {
            continue;
        }
        if (list->cmd[i].custom_kind == NK_UI_CUSTOM_IMAGE) return 2;
        art = 1;
    }
    return art;
}

void player_ui_title_report_achievement_rows(const NkUiDrawList *list, size_t count,
                                             const PlayerUiAchievementRow *rows) {
    /* For each row the frame drew: whether it has its icon slot and what the slot shows (a
       badge picture or the drawn glyph), its state, the colour its title is drawn in, and
       whether any part runs past the card's inner edge or into the part beside or under it.
       Printed when the summary changes. */
    static char reported[8192];
    char now[sizeof(reported)];
    size_t used = 0u;
    int card[4];
    if (list == NULL || rows == NULL || !title_find_cmd(list, TITLE_ID_ACHIEVEMENTS,
                                                        NK_UI_DRAW_RECTANGLE, card)) {
        return;
    }
    now[0] = '\0';
    for (size_t i = 0; i < count && i < TITLE_ACH_ROWS_MAX; i++) {
        const NkUiId id = TITLE_ID_ACH_ROW + (NkUiId)(i * TITLE_ID_ACH_ROW_PARTS);
        int slot[4], title[4], words[4], pill[4], text[4];
        const bool has_slot = title_find_cmd(list, id + 1u, NK_UI_DRAW_RECTANGLE, slot);
        const bool has_title = title_find_cmd(list, id + 4u, NK_UI_DRAW_TEXT, title);
        const bool has_pill = title_find_cmd(list, id + 6u, NK_UI_DRAW_RECTANGLE, pill);
        const bool has_words = title_find_cmd(list, id + 5u, NK_UI_DRAW_TEXT, words);
        const bool has_text = title_find_cmd(list, id + 7u, NK_UI_DRAW_TEXT, text);
        int clipped = 0;
        /* Scrolled out of view, or part way: not reported. The page lists every row, so a
           row the page scrolls past is reported whole in another frame. */
        if (!has_slot || title_part_hidden(list, id + 1u, NK_UI_DRAW_RECTANGLE) ||
            title_part_hidden(list, id + 4u, NK_UI_DRAW_TEXT) ||
            title_part_hidden(list, id + 5u, NK_UI_DRAW_TEXT)) {
            continue;
        }
        if (!has_slot || !has_title || !has_pill || !has_text) clipped = 1;
        if (has_slot && has_title && (slot[2] > title[0] || slot[0] < card[0])) clipped = 1;
        if (has_title && has_pill && title[2] > pill[0]) clipped = 1;
        if (has_pill && (pill[2] > card[2] || text[2] > pill[2] || text[0] < pill[0])) clipped = 1;
        if (has_words && (words[2] > pill[0] || words[1] < title[3])) clipped = 1;
        const int art = title_drawn_art(list, id + 2u);
        /* The title's colour, and whether it was shortened (an ellipsis in any of its lines). */
        int ink = 0, cut = 0;
        uint32_t title_rgba = 0u;
        for (uint32_t k = 0; k < list->ncmd; k++) {
            const NkUiDrawCmd *c = &list->cmd[k];
            if (c->element_id != id + 4u || c->kind != NK_UI_DRAW_TEXT) continue;
            const NkUiTextRun *run = (const NkUiTextRun *)c->custom;
            if (!ink) title_rgba = c->rgba_pm;
            ink = 1;
            for (size_t b = 0; run != NULL && b + 3u <= run->length; b++) {
                if (memcmp(run->text + b, "\xE2\x80\xA6", 3u) == 0) cut = 1;
            }
        }
        used += (size_t)snprintf(now + used, sizeof(now) - used,
                                 "%zu:%s:slot=%d:icon=%s:ink=%08x:cut=%d:clipped=%d;", i,
                                 rows[i].unlocked ? "unlocked" : "locked", has_slot ? 1 : 0,
                                 art == 2 ? "badge" : art == 1 ? "drawn" : "none",
                                 ink ? (unsigned)title_rgba : 0u, cut, clipped);
        if (used >= sizeof(now)) break;
    }
    if (strcmp(now, reported) != 0) {
        snprintf(reported, sizeof(reported), "%s", now);
        printf("[PLAYER_UI_TEST] next_ui_title_achievement_rows %s\n", now);
    }
}
#endif

PlayerUiAction player_ui_screen_title(NkUi *ui,
                                      const PlayerUiTitleModel *model) {
    const NkUiTheme *theme;
    const PlayerUiTitle *title;
    NkUiLayout layout;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float width;
    float height;
    float gutter;
    float aside;
    if (ui == NULL || model == NULL || model->title == NULL) return result;
    theme = nk_ui_theme(ui);
    title = model->title;
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    gutter = width < NK_UI_NARROW_WIDTH ? NK_UI_GUTTER_NARROW : NK_UI_GUTTER;
    aside = (width - 2.0f * gutter - NK_UI_DETAIL_GAP) * 0.45f;
    if (aside > NK_UI_DETAIL_ASIDE_WIDTH) aside = NK_UI_DETAIL_ASIDE_WIDTH;
    aside = (float)(int)aside;

    (void)nk_ui_waves_element(ui, TITLE_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, TITLE_ID_ROOT, &layout, NULL)) {
        return result;
    }
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, TITLE_ID_TOP, PLAYER_UI_ROUTE_TITLE);
        if (tab != PLAYER_UI_ACTION_NONE) {
            result = action(tab, model->title_index, PLAYER_UI_PRIMARY_NONE);
        }
    }

    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), NK_UI_DETAIL_GAP);
    layout.padding_top = NK_UI_BODY_PAD_TOP;
    layout.padding_bottom = NK_UI_BODY_PAD_BOTTOM;
    layout.padding_left = gutter;
    layout.padding_right = gutter;
    /* The body always scrolls between the bars (#2245): what a game's page shows varies
       (the Enhancements card, the disc check, the compatibility notes, the stall notice), so
       at the design text size it can still be taller than the room, and a body that did not
       scroll pushed the prompt bar out of the window. A page that fits does not move. */
    layout.scroll = SCR_PAGE_BODY_SCROLLS;
    if (nk_ui_begin_container(ui, TITLE_ID_BODY, &layout, NULL)) {
        /* Left column: the art and what the package needs. */
        layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(aside), sizing_grow(),
                             NK_UI_SPACE_4);
        if (nk_ui_begin_container(ui, TITLE_ID_ASIDE, &layout, NULL)) {
            bool demo = title->art_kind != NK_UI_ART_NONE;
            if (title->cover != NULL) {
                char cover_size[32];
                (void)snprintf(cover_size, sizeof(cover_size), NK_UI_STR(TITLE_ART_DIMENSIONS_FORMAT),
                               (unsigned)title->cover->width, (unsigned)title->cover->height);
                nk_ui_hero(ui, TITLE_ID_HERO, NK_UI_ART_NONE, title->disc_id,
                           title->cover, NULL, aside, NK_UI_STR(TITLE_BOX_ART), cover_size);
            } else {
                nk_ui_hero(ui, TITLE_ID_HERO, (NkUiArtKind)title->art_kind, title->disc_id,
                           title->icon, title->picture, aside,
                           demo ? NK_UI_STR(TITLE_PROJECT_PICTURE) : NK_UI_STR(TITLE_YOUR_DISC),
                           NK_UI_STR(TITLE_DISC_PICTURE_DIMENSIONS));
            }
            if (nk_ui_card_begin(ui, TITLE_ID_STATUS_CARD, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                NkUiLayout head = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                head.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_STATUS_HEAD, &head, NULL)) {
                    nk_ui_eyebrow(ui, TITLE_ID_STATUS_EYEBROW, NK_UI_STR(TITLE_PACKAGE_HEADING));
                    if (title->package_state == PLAYER_UI_PACKAGE_STARTING ||
                        title->package_state == PLAYER_UI_PACKAGE_RUNNING) {
                        /* The header badge says Starting or Running once; the
                           package card describes the package. */
                        nk_ui_status_pill(ui, TITLE_ID_STATUS_PILL, "\xE2\x97\x8F", NK_UI_STR(TITLE_READY),
                                          theme->ready);
                    } else if (title->library_status != NULL &&
                               title->library_status[0] != '\0') {
                        nk_ui_status_pill(ui, TITLE_ID_STATUS_PILL, title->status_marker,
                                          title->library_status, title->status_rgba);
                    }
                    nk_ui_end_container(ui);
                }
                {
                    /* A starting or running game says so; otherwise the host's
                       reason, or the package state's own sentence. */
                    bool live = title->package_state == PLAYER_UI_PACKAGE_STARTING ||
                                title->package_state == PLAYER_UI_PACKAGE_RUNNING;
                    const char *state_text = package_state_text(title->package_state, model->host);
                    nk_ui_text_role(ui, TITLE_ID_STATUS_DETAIL,
                                    live ? state_text
                                         : safe_text(title->library_status_detail, state_text),
                                    NK_UI_TYPE_BODY_S, theme->muted);
                }
                /* The decrypted-files note belongs to the steps that need those
                   files: a built, starting or running package has them (#1297). */
                if (title->input_note != NULL && title->input_note[0] != '\0' &&
                    (title->package_state == PLAYER_UI_PACKAGE_MISSING ||
                     title->package_state == PLAYER_UI_PACKAGE_CHECKING ||
                     title->package_state == PLAYER_UI_PACKAGE_CHECK_FAILED)) {
                    NkUiLayout note = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(),
                                                  sizing_fit(), 0.0f);
                    NkUiStyle note_style;
                    memset(&note_style, 0, sizeof(note_style));
                    note_style.fill_rgba = theme->surface;
                    note_style.border_rgba = theme->line_strong;
                    note_style.border_width = NK_UI_BORDER_HAIRLINE;
                    note_style.corner_radius = NK_UI_RADIUS_CONTROL;
                    note.padding_top = note.padding_bottom = 10.0f;
                    note.padding_left = note.padding_right = 14.0f;
                    if (nk_ui_begin_container(ui, TITLE_ID_NOTE, &note, &note_style)) {
                        nk_ui_text_role(ui, TITLE_ID_NOTE_TEXT, title->input_note,
                                        NK_UI_TYPE_CAPTION, theme->info);
                        nk_ui_end_container(ui);
                    }
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }

        /* Right column: name, status, actions, compatibility. */
        layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 14.0f);
        if (nk_ui_begin_container(ui, TITLE_ID_MAIN, &layout, NULL)) {
            NkUiLayout row;
            nk_ui_eyebrow(ui, TITLE_ID_CRUMB, NK_UI_STR(TITLE_BREADCRUMB));
            nk_ui_text_role(ui, TITLE_ID_NAME, safe_text(title->title, NK_UI_STR(TITLE_UNTITLED_GAME)),
                            NK_UI_TYPE_TITLE_XL, theme->paper);
            if (title->disc_id != NULL && title->disc_id[0] != '\0') {
                nk_ui_text_role(ui, TITLE_ID_DISC, title->disc_id, NK_UI_TYPE_DATA,
                                theme->muted);
            }
            row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_2);
            if (nk_ui_begin_container(ui, TITLE_ID_CHIPS, &row, NULL)) {
                if (title->library_status != NULL && title->library_status[0] != '\0') {
                    nk_ui_status_pill(ui, TITLE_ID_CHIP_STATUS, title->status_marker,
                                      title->library_status, title->status_rgba);
                }
                nk_ui_tag(ui, TITLE_ID_CHIP_BADGE,
                          safe_text(title->badge, title->art_kind != NK_UI_ART_NONE
                                                      ? NK_UI_STR(TITLE_SHOWCASE_BADGE) : NK_UI_STR(TITLE_YOUR_DISC)));
                nk_ui_end_container(ui);
            }
            /* Disc verification: what the badge's Verified retail, Modified
               disc or Unknown dump means for this image. */
            if (title->disc_check_note != NULL && title->disc_check_note[0] != '\0') {
                nk_ui_text_role(ui, PLAYER_UI_ID_DISC_VERIFY_TITLE_NOTE, title->disc_check_note,
                                NK_UI_TYPE_BODY_S, theme->muted);
            }
            if (model->stalled_game_notice_visible &&
                nk_ui_card_begin(ui, TITLE_ID_STALL_NOTICE, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_2)) {
                nk_ui_eyebrow(ui, TITLE_ID_STALL_EYEBROW,
                              model->stalled_game_dark_picture ? NK_UI_STR(TITLE_BLACK_SCREEN) : NK_UI_STR(TITLE_NO_NEW_FRAMES));
                nk_ui_text_role(ui, TITLE_ID_STALL_TEXT,
                                model->stalled_game_dark_picture
                                    ? NK_UI_STR(TITLE_BLACK_SCREEN_DETAIL)
                                    : NK_UI_STR(TITLE_NO_NEW_FRAMES_DETAIL),
                                NK_UI_TYPE_BODY_S, theme->paper);
                row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
                row.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_STALL_ACTIONS, &row, NULL)) {
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_STALL_KEEP_WAITING,
                                     NK_UI_STR(TITLE_KEEP_WAITING), NK_UI_BUTTON_LARGE)) {
                        result = action(PLAYER_UI_ACTION_STALL_KEEP_WAITING,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_STALL_STOP_GAME,
                                     NK_UI_STR(TITLE_STOP_GAME), NK_UI_BUTTON_LARGE | NK_UI_BUTTON_DANGER)) {
                        result = action(PLAYER_UI_ACTION_STALL_STOP_GAME,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            /* The render notice (#2151): the slower path the game draws with, as a fact in
               the brand blue, never a warning colour; Hide keeps it hidden for the session.
               Never the default focus, which stays on PLAY or STOP GAME. */
            if (model->render_notice_heading != NULL && model->render_notice_heading[0] != '\0' &&
                model->render_notice_text != NULL &&
                nk_ui_card_begin(ui, TITLE_ID_RENDER_NOTICE, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_2)) {
                row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
                row.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_RENDER_HEAD, &row, NULL)) {
                    nk_ui_eyebrow(ui, TITLE_ID_RENDER_EYEBROW, NK_UI_STR(TITLE_PICTURE_HEADING));
                    nk_ui_status_pill(ui, TITLE_ID_RENDER_PILL, "\xE2\x97\x8F",
                                      model->render_notice_heading, theme->accent_dark);
                    nk_ui_end_container(ui);
                }
                nk_ui_text_role(ui, TITLE_ID_RENDER_TEXT, model->render_notice_text,
                                NK_UI_TYPE_BODY_S, theme->paper);
                if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_RENDER_NOTICE_HIDE, NK_UI_STR(TITLE_HIDE),
                                 NK_UI_BUTTON_SMALL | NK_UI_BUTTON_NOT_DEFAULT)) {
                    result = action(PLAYER_UI_ACTION_HIDE_RENDER_NOTICE, model->title_index,
                                    PLAYER_UI_PRIMARY_NONE);
                }
                nk_ui_end_container(ui);
            }
            /* The font note (owner request 2026-11-19): Authentic is the game as released, and
               stand-in or replacement fonts are not, so the page says so in one sentence, with
               the way to the PSP's own fonts and Hide. A fact in the page's own colours, never a
               warning: nothing stops the game from starting, and it is never the default focus,
               which stays on PLAY. */
            if (model->font_note_text != NULL && model->font_note_text[0] != '\0' &&
                nk_ui_card_begin(ui, TITLE_ID_FONT_NOTE, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_2)) {
                nk_ui_eyebrow(ui, TITLE_ID_FONT_EYEBROW, NK_UI_STR(TITLE_FONTS_HEADING));
                nk_ui_text_role(ui, TITLE_ID_FONT_TEXT, model->font_note_text, NK_UI_TYPE_BODY_S,
                                theme->paper);
                row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
                row.align_y = NK_UI_ALIGN_CENTER;
                {
                    /* Full-size secondary buttons (NK_UI_CONTROL_HEIGHT, 44 px), as the
                       ENHANCEMENTS card's: the small control is kept for the library's heading row
                       and the on-screen keyboard (docs/PLAYER_UI_DESIGN.md, Accessibility). */
                    NkUiFlowRow flow;
                    const uint32_t flags = NK_UI_BUTTON_NOT_DEFAULT;
                    if (nk_ui_flow_begin(ui, &flow, TITLE_ID_FONT_ACTIONS, &row,
                                         width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside -
                                             2.0f * NK_UI_SPACE_5)) {
                        nk_ui_flow_item(ui, &flow,
                                        nk_ui_button_width(ui, NK_UI_STR(TITLE_GET_PSP_FONTS), flags));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_FONT_NOTE_GET,
                                         NK_UI_STR(TITLE_GET_PSP_FONTS), flags)) {
                            result = action(PLAYER_UI_ACTION_FONT_NOTE_GET_FONTS,
                                            model->title_index, PLAYER_UI_PRIMARY_NONE);
                        }
                        nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(TITLE_HIDE), flags));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_FONT_NOTE_HIDE, NK_UI_STR(TITLE_HIDE), flags)) {
                            result = action(PLAYER_UI_ACTION_HIDE_FONT_NOTE, model->title_index,
                                            PLAYER_UI_PRIMARY_NONE);
                        }
                        nk_ui_flow_end(ui, &flow);
                    }
                }
                nk_ui_end_container(ui);
            }
            /* One action row on the column's left edge, every button the same
               52 px height; the focus ring may spill into the column gap. */
            row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
            row.align_y = NK_UI_ALIGN_CENTER;
            row.padding_top = NK_UI_SPACE_2;
            row.padding_bottom = NK_UI_SPACE_2;
            {
                /* A larger text size can wrap the row onto a second one. */
                NkUiFlowRow flow;
                if (nk_ui_flow_begin(ui, &flow, TITLE_ID_ACTIONS, &row,
                                     width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside)) {
                    /* While the no-new-frames card is up, its STOP GAME is the only
                       one on the page. */
                    bool stop_in_stall_card = model->stalled_game_notice_visible &&
                        title->primary_action == PLAYER_UI_PRIMARY_STOP_GAME;
                    const char *primary = safe_text(title->primary_action_label, NK_UI_STR(TITLE_UNAVAILABLE));
                    uint32_t primary_flags = NK_UI_BUTTON_LARGE |
                        (title->primary_action_enabled ? NK_UI_BUTTON_PRIMARY
                                                       : NK_UI_BUTTON_DISABLED);
                    uint32_t save_flags = NK_UI_BUTTON_LARGE |
                        (title->save_folder_enabled ? 0u : NK_UI_BUTTON_DISABLED);
                    if (!stop_in_stall_card) {
                        nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, primary, primary_flags));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_PRIMARY, primary, primary_flags)) {
                            result = action(PLAYER_UI_ACTION_PRIMARY, model->title_index,
                                            title->primary_action);
                        }
                    }
                    /* An older Nakagawa built the package: PLAY stays first, and
                       Rebuild sits beside it (the package card says why). */
                    if (title->rebuild_offered) {
                        nk_ui_flow_item(ui, &flow,
                                        nk_ui_button_width(ui, NK_UI_STR(TITLE_REBUILD), NK_UI_BUTTON_LARGE));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_REBUILD, NK_UI_STR(TITLE_REBUILD),
                                         NK_UI_BUTTON_LARGE | NK_UI_BUTTON_NOT_DEFAULT)) {
                            result = action(PLAYER_UI_ACTION_REBUILD, model->title_index,
                                            (PlayerUiPrimaryAction)PLAYER_UI_REBUILD_TITLE);
                        }
                    }
                    /* The build queue: Add to queue beside BUILD PACKAGE, or
                       Remove from queue while it waits (the package card says
                       its place). */
                    if (title->queue_add_offered || title->queue_remove_offered) {
                        const char *queue = title->queue_add_offered ? NK_UI_STR(TITLE_QUEUE_ADD)
                                                                     : NK_UI_STR(TITLE_QUEUE_REMOVE);
                        nk_ui_flow_item(ui, &flow,
                                        nk_ui_button_width(ui, queue, NK_UI_BUTTON_LARGE));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_QUEUE, queue,
                                         NK_UI_BUTTON_LARGE | NK_UI_BUTTON_NOT_DEFAULT)) {
                            result = action(PLAYER_UI_ACTION_QUEUE, model->title_index,
                                            (PlayerUiPrimaryAction)(title->queue_add_offered
                                                ? PLAYER_UI_QUEUE_ADD
                                                : PLAYER_UI_QUEUE_REMOVE));
                        }
                    }
                    /* Saves (#1883): back up, restore, add a save, open the folder. */
                    nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(TITLE_SAVES), save_flags));
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_SAVES, NK_UI_STR(TITLE_SAVES), save_flags)) {
                        result = action(PLAYER_UI_ACTION_OPEN_SAVES, model->title_index,
                                        PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(TITLE_BACK), NK_UI_BUTTON_LARGE));
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_BACK, NK_UI_STR(TITLE_BACK), NK_UI_BUTTON_LARGE)) {
                        result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                        PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_flow_end(ui, &flow);
                }
            }
            if (scr_input_guide_present(&model->input_guide)) {
                /* The build waits for decrypted copies from the user's disc
                   (#308): which files, where they go, and a check that runs
                   again without adding the disc again. */
                float main_width = width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside;
                scr_input_guide_box(ui, TITLE_ID_INPUT_GUIDE, &model->input_guide, main_width,
                                height >= 680.0f ? PLAYER_UI_INPUT_FILES : 2u);
                row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
                row.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_INPUT_ACTIONS, &row, NULL)) {
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_CHECK_AGAIN, NK_UI_STR(TITLE_CHECK_AGAIN), 0u)) {
                        result = action(PLAYER_UI_ACTION_CHECK_INPUT_AGAIN,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    if (model->input_guide.folder != NULL &&
                        model->input_guide.folder[0] != '\0' &&
                        nk_ui_button(ui, PLAYER_UI_ID_TITLE_OPEN_INPUT_FOLDER, NK_UI_STR(TITLE_OPEN_FOLDER),
                                     0u)) {
                        result = action(PLAYER_UI_ACTION_OPEN_BUILD_INPUT_FOLDER,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_end_container(ui);
                }
            } else if (title->build_preflight_message != NULL &&
                       title->build_preflight_message[0] != '\0') {
                nk_ui_text_role(ui, TITLE_ID_BUILD_PREFLIGHT,
                                title->build_preflight_message,
                                NK_UI_TYPE_BODY_S, theme->error);
                if (title->build_preflight_folder != NULL &&
                    title->build_preflight_folder[0] != '\0' &&
                    nk_ui_button(ui, TITLE_ID_BUILD_FOLDER, NK_UI_STR(TITLE_OPEN_FOLDER), 0u)) {
                    result = action(PLAYER_UI_ACTION_OPEN_BUILD_INPUT_FOLDER,
                                    model->title_index, PLAYER_UI_PRIMARY_NONE);
                }
            }
            /* The first start of a new package took 7 to 9 s before its window
               opened (v0.0.1 walkthrough); a chip and one line in the package
               card are easy to miss in that wait, so a card says what is
               happening, with a moving bar. */
            if (title->package_state == PLAYER_UI_PACKAGE_STARTING &&
                nk_ui_card_begin(ui, TITLE_ID_START_CARD, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                float bar = width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside -
                            2.0f * NK_UI_SPACE_5;
                char line[320];
                NkUiTextStyle name = nk_ui_type(NK_UI_TYPE_HEADING, theme->paper);
                (void)snprintf(line, sizeof(line), NK_UI_STR(TITLE_STARTING_FORMAT),
                               safe_text(title->title, NK_UI_STR(TITLE_GAME_FALLBACK)));
                nk_ui_text(ui, TITLE_ID_START_NAME,
                           nk_ui_text_clamp(ui, line, &name, bar, 1), &name);
                nk_ui_text_role(ui, TITLE_ID_START_TEXT,
                                NK_UI_STR(TITLE_STARTING_DETAIL),
                                NK_UI_TYPE_BODY_S, theme->muted);
                nk_ui_progress_bar(ui, TITLE_ID_START_BAR, bar > 40.0f ? bar : 40.0f,
                                   -1.0f, model->now_s);
                nk_ui_end_container(ui);
            }
            /* The player tier, then what to expect in plain words and any
               known issues (#1297). The docs category describes the
               project's test route, so only the developer view names it. */
            if (nk_ui_card_begin(ui, TITLE_ID_COMPAT, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                NkUiLayout head = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                head.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_COMPAT_HEAD, &head, NULL)) {
                    nk_ui_eyebrow(ui, TITLE_ID_COMPAT_EYEBROW, NK_UI_STR(TITLE_COMPATIBILITY_HEADING));
                    const char *tier = player_ui_tier_word(title->compatibility_tier);
                    if (tier != NULL) {
                        nk_ui_status_pill(ui, TITLE_ID_COMPAT_TIER, NULL, tier,
                                          tier_rgba(theme, title->compatibility_tier));
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_text_role(ui, TITLE_ID_COMPAT_EXPECT,
                                safe_text(title->compatibility_expect,
                                          NK_UI_STR(TITLE_COMPATIBILITY_UNTRIED)),
                                NK_UI_TYPE_BODY_S, theme->paper);
                if (title->compatibility_known_issues != NULL &&
                    title->compatibility_known_issues[0] != '\0') {
                    nk_ui_text_role(ui, TITLE_ID_COMPAT_ISSUES,
                                    title->compatibility_known_issues,
                                    NK_UI_TYPE_BODY_S, theme->muted);
                }
                if (title->compatibility_category != NULL &&
                    title->compatibility_category[0] != '\0') {
                    char line[160];
                    (void)snprintf(line, sizeof(line),
                                   NK_UI_STR(TITLE_COMPATIBILITY_DEVELOPER_FORMAT),
                                   title->compatibility_category);
                    nk_ui_text_role(ui, TITLE_ID_COMPAT_CATEGORY, nk_ui_frame_text(ui, line),
                                    NK_UI_TYPE_FOOTNOTE, theme->dim);
                }
                nk_ui_end_container(ui);
            }
            if (model->achievements_status != NULL || model->achievements_notice != NULL) {
                title_achievements_card(ui, model,
                                        width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside -
                                            2.0f * NK_UI_SPACE_5);
            }
            /* Enhancements, apart from what to expect of the game as released:
               the play mode for every game (Authentic, Enhanced or Ace; src/rt/ace.h),
               and Ace's 60 FPS game logic made for this game
               (docs/ENHANCEMENT_CONTRACT.md section 9). Authentic is the original game. */
            if ((title->enhancement_shown || title->ace_shown) &&
                nk_ui_card_begin(ui, TITLE_ID_ENH_CARD, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                NkUiLayout head = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                head.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_ENH_HEAD, &head, NULL)) {
                    nk_ui_eyebrow(ui, TITLE_ID_ENH_EYEBROW, NK_UI_STR(TITLE_ENHANCEMENTS_HEADING));
                    if (title->enhancement_shown) {
                        nk_ui_tag(ui, TITLE_ID_ENH_TAG,
                                  title->enhancement_released ? NK_UI_STR(TITLE_ENHANCEMENT_OPTIONAL) : NK_UI_STR(TITLE_ENHANCEMENT_EXPERIMENTAL));
                    }
                    nk_ui_end_container(ui);
                }
                /* The play mode (#2055): library default, Authentic, Enhanced or Ace for this
                   game; its Enhanced options; then Ace's 60 FPS switch on the same row. */
                {
                    float inner = width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside -
                                  2.0f * NK_UI_SPACE_5;
                    NkUiFlowRow buttons;
                    char label[48];
                    (void)snprintf(label, sizeof(label), NK_UI_STR(TITLE_ENHANCEMENT_60HZ_FORMAT),
                                   title->enhancement_on ? NK_UI_STR(TITLE_ON) : NK_UI_STR(TITLE_OFF));
                    row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
                    if (nk_ui_flow_begin(ui, &buttons, TITLE_ID_ACE_ROW, &row, inner)) {
                        if (title->ace_shown) {
                            const char *ace = safe_text(title->ace_label, NK_UI_STR(TITLE_PLAY_MODE));
                            nk_ui_flow_item(ui, &buttons, nk_ui_button_width(ui, ace, 0u));
                            if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_ACE, ace,
                                             NK_UI_BUTTON_NOT_DEFAULT |
                                                 (title->ace_in_effect ? NK_UI_BUTTON_PRIMARY : 0u) |
                                                 (title->ace_enabled ? 0u
                                                                     : NK_UI_BUTTON_DISABLED))) {
                                result = action(PLAYER_UI_ACTION_CYCLE_PLAY_MODE, model->title_index,
                                                PLAYER_UI_PRIMARY_NONE);
                            }
                        }
                        if (title->options_shown) {
                            const char *options = NK_UI_STR(TITLE_ENHANCED_OPTIONS);
                            nk_ui_flow_item(ui, &buttons, nk_ui_button_width(ui, options, 0u));
                            if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_PLAY_OPTIONS, options,
                                             NK_UI_BUTTON_NOT_DEFAULT |
                                                 (title->options_enabled ? 0u
                                                                         : NK_UI_BUTTON_DISABLED))) {
                                result = action(PLAYER_UI_ACTION_OPEN_PLAY_OPTIONS,
                                                model->title_index, PLAYER_UI_PRIMARY_NONE);
                            }
                        }
                        if (title->enhancement_shown) {
                            nk_ui_flow_item(ui, &buttons, nk_ui_button_width(ui, label, 0u));
                            if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_60FPS, label,
                                             NK_UI_BUTTON_NOT_DEFAULT |
                                             (title->enhancement_on ? NK_UI_BUTTON_PRIMARY : 0u) |
                                             (title->enhancement_enabled ? 0u
                                                                         : NK_UI_BUTTON_DISABLED))) {
                                result = action(PLAYER_UI_ACTION_TOGGLE_60FPS, model->title_index,
                                                PLAYER_UI_PRIMARY_NONE);
                            }
                        }
                        nk_ui_flow_end(ui, &buttons);
                    }
                    /* What the play mode turns on, then what it did last time, each its own
                       paragraph and each whole: the last session shows nowhere else, the parts
                       list grows with each new part, and the body scrolls (#2245), so the card
                       grows instead of cutting either sentence short. */
                    if (title->ace_shown && title->ace_line != NULL && title->ace_line[0] != '\0') {
                        nk_ui_text_role(ui, TITLE_ID_ACE_TEXT, title->ace_line, NK_UI_TYPE_BODY_S,
                                        theme->paper);
                        if (title->ace_last_session != NULL &&
                            title->ace_last_session[0] != '\0') {
                            nk_ui_text_role(ui, TITLE_ID_ACE_LAST, title->ace_last_session,
                                            NK_UI_TYPE_BODY_S, theme->muted);
                        }
                    }
                }
                if (title->enhancement_shown) {
                    nk_ui_text_role(ui, TITLE_ID_ENH_TEXT,
                                    NK_UI_STR(TITLE_ENHANCEMENT_60HZ_DETAIL),
                                    NK_UI_TYPE_BODY_S, theme->paper);
                    if (title->enhancement_problem != NULL &&
                        title->enhancement_problem[0] != '\0') {
                        char line[256];
                        (void)snprintf(line, sizeof(line),
                                       NK_UI_STR(TITLE_ENHANCEMENT_ENABLE_NOTE_FORMAT),
                                       title->enhancement_problem);
                        nk_ui_text_role(ui, TITLE_ID_ENH_PROBLEM, nk_ui_frame_text(ui, line),
                                        NK_UI_TYPE_BODY_S, theme->error);
                    }
                    if (title->enhancement_session) {
                        nk_ui_text_role(ui, TITLE_ID_ENH_SESSION,
                                        NK_UI_STR(TITLE_ENHANCEMENT_SESSION_NOTE),
                                        NK_UI_TYPE_CAPTION, theme->muted);
                    }
                }
                nk_ui_end_container(ui);
            }
            /* Add to Steam (#2152), which opens its own dialog; Report a
               problem, a link the user's browser opens; and Remove from library
               (#1304), apart from the play actions: it deletes what Nakagawa
               built, so it asks first. A larger text size wraps the row. */
            bool can_report = title->disc_id != NULL && title->disc_id[0] != '\0';
            if (can_report || title->can_remove || title->can_add_to_steam) {
                NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                             NK_UI_SPACE_3);
                NkUiFlowRow flow;
                if (nk_ui_flow_begin(ui, &flow, TITLE_ID_REMOVE_ROW, &row,
                                     width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside)) {
                    if (title->can_add_to_steam) {
                        nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, NK_UI_STR(TITLE_ADD_TO_STEAM), 0u));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_STEAM, NK_UI_STR(TITLE_ADD_TO_STEAM), 0u)) {
                            result = action(PLAYER_UI_ACTION_ADD_TO_STEAM, model->title_index,
                                            PLAYER_UI_PRIMARY_NONE);
                        }
                    }
                    if (can_report) {
                        nk_ui_flow_item(ui, &flow,
                                        nk_ui_button_width(ui, NK_UI_STR(TITLE_REPORT_PROBLEM), 0u));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_REPORT, NK_UI_STR(TITLE_REPORT_PROBLEM),
                                         0u)) {
                            result = action(PLAYER_UI_ACTION_REPORT_PROBLEM,
                                            model->title_index, PLAYER_UI_PRIMARY_NONE);
                        }
                    }
                    if (title->can_remove) {
                        nk_ui_flow_item(ui, &flow,
                                        nk_ui_button_width(ui, NK_UI_STR(TITLE_REMOVE),
                                                           NK_UI_BUTTON_DANGER));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_REMOVE, NK_UI_STR(TITLE_REMOVE),
                                         NK_UI_BUTTON_DANGER)) {
                            result = action(PLAYER_UI_ACTION_REMOVE_TITLE, model->title_index,
                                            PLAYER_UI_PRIMARY_NONE);
                        }
                    }
                    nk_ui_flow_end(ui, &flow);
                }
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    {
        static const NkUiPromptItem prompts[] = {
            {NK_UI_PROMPT_ACT_MOVE, NK_UI_STR(TITLE_MOVE)},
            {NK_UI_PROMPT_ACT_CONFIRM, NK_UI_STR(TITLE_SELECT)},
            {NK_UI_PROMPT_ACT_BACK, NK_UI_STR(TITLE_BACK)}
        };
        nk_ui_prompt_bar(ui, TITLE_ID_PROMPTS, "", prompts,
                         sizeof(prompts) / sizeof(prompts[0]));
    }
    nk_ui_end_container(ui);
    return result;
}
