/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The pause menu and its controls page. The menu is one card: GAME PAUSED, the
   game's name, then RESUME (the default) over SETTINGS, CONTROLS and QUIT TO
   LIBRARY, with VRAM VIEWER and PROFILE before QUIT in developer mode. Back resumes.
   In a window too short for that card at its full spacing (the default 960 x 544
   game window in developer mode, say) the menu is compact: tighter gaps, the
   game's name on one line, RESUME at the standard control height and the other
   entries at the small one. Only a window too short even for that (480 x 272)
   scrolls the card, with RESUME shown first and focused.
   The controls page lists what drives each PSP control, read from the bindings the game
   is using. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <string.h>

#define MENU_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_OVERLAY, (offset))
#define MENU_ID_EYEBROW MENU_ID(0x001001u)
#define MENU_ID_TITLE MENU_ID(0x001002u)
#define MENU_ID_DISC MENU_ID(0x001003u)
#define MENU_ID_ACTIONS MENU_ID(0x001004u)
#define MENU_ID_HEAD MENU_ID(0x001005u)
#define MENU_ID_ACE MENU_ID(0x001006u)      /* Ace Mode's caption */
#define MENU_ID_ACE_LINE MENU_ID(0x001040u) /* + line, below MENU_ACE_LINES: a caption on more than one */
#define MENU_ID_ACE_LINE_SPAN 0x20u
/* The most lines the caption takes, in a window with room for them: one id each. */
#define MENU_ACE_LINES ((int)MENU_ID_ACE_LINE_SPAN)

#define CONTROLS_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_OVERLAY, (offset))
#define CONTROLS_ID_EYEBROW CONTROLS_ID(0x003001u)
#define CONTROLS_ID_TITLE CONTROLS_ID(0x003002u)
#define CONTROLS_ID_TABLES CONTROLS_ID(0x003003u)
#define CONTROLS_ID_TABLE CONTROLS_ID(0x003010u)    /* + 0x100 per table */
#define CONTROLS_ID_TABLE_SPAN 0x200u
#define CONTROLS_ID_NOTE CONTROLS_ID(0x003004u)
#define CONTROLS_ID_FOOT CONTROLS_ID(0x003005u)

#define OVERLAY_DASH "\xE2\x80\x94"

static const char *nonempty(const char *text) {
    return text != NULL && text[0] != '\0' ? text : NULL;
}

/* The dialog card's padding above and below its content and the gap between its
   children (nk_ui_dialog_begin_scroll). */
#define MENU_CARD_PAD_Y (26.0f + 22.0f)
#define MENU_CARD_GAP 14.0f

/* How many lines Ace Mode's caption takes at `inner`: one when it fits there as
   it is, else its words as a list wrapped between its items (nk_ui_list_line). */
static int menu_ace_lines(NkUi *ui, const NkOverlayModel *model, float inner) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle caption = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
    const char *words = nonempty(model->ace_caption);
    int lines;
    if (words == NULL) return 0;
    if (nk_ui_text_width(ui, words, &caption) <= inner) return 1;
    lines = nk_ui_list_line_count(ui, words, &caption, inner);
    return lines > MENU_ACE_LINES ? MENU_ACE_LINES : lines;
}

/* How tall the menu card lays out at its full spacing: the heading (with the
   game's name on as many lines as it takes, two at most, and the caption on
   `ace_lines`) over the entries. */
static float menu_full_height(NkUi *ui, const NkOverlayModel *model, float inner, int ace_lines) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle eyebrow = nk_ui_type(NK_UI_TYPE_EYEBROW, theme->muted);
    NkUiTextStyle title = nk_ui_type(NK_UI_TYPE_TITLE, theme->paper);
    NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
    NkUiTextStyle caption = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
    const char *name = nonempty(model->title_name);
    float title_lines = name != NULL && nk_ui_text_width(ui, name, &title) > inner ? 2.0f : 1.0f;
    float head = nk_ui_line_height(ui, &eyebrow) + NK_UI_SPACE_1 +
                 title_lines * nk_ui_line_height(ui, &title);
    float entries = model->developer ? 5.0f : 3.0f; /* those under RESUME */
    if (nonempty(model->disc_id) != NULL) head += NK_UI_SPACE_1 + nk_ui_line_height(ui, &data);
    if (ace_lines > 0) {
        head += NK_UI_SPACE_1 + (float)ace_lines * nk_ui_line_height(ui, &caption);
    }
    return MENU_CARD_PAD_Y + head + MENU_CARD_GAP + NK_UI_SPACE_2 +
           NK_UI_CONTROL_HEIGHT_LARGE + entries * (NK_UI_SPACE_3 + NK_UI_CONTROL_HEIGHT);
}

/* How many of the caption's lines a compact card has room for with the game's
   name and RESUME in view under them: the rest of the card may scroll, those two
   and the caption do not. At least one. */
static int menu_compact_ace_lines(NkUi *ui, const NkOverlayModel *model, float room) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle eyebrow = nk_ui_type(NK_UI_TYPE_EYEBROW, theme->muted);
    NkUiTextStyle title = nk_ui_type(NK_UI_TYPE_TITLE, theme->paper);
    NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
    NkUiTextStyle caption = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
    float line = nk_ui_line_height(ui, &caption);
    float rest = MENU_CARD_PAD_Y + nk_ui_line_height(ui, &eyebrow) + nk_ui_line_height(ui, &title) +
                 MENU_CARD_GAP + NK_UI_CONTROL_HEIGHT;
    int lines;
    if (nonempty(model->disc_id) != NULL) rest += nk_ui_line_height(ui, &data);
    lines = line > 0.0f && room > rest ? (int)((room - rest) / line) : 0;
    return lines > 1 ? lines : 1;
}

NkOverlayAction nk_overlay_page_menu(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Resume"}
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    float inner = nk_overlay_page_begin(ui, model, 480.0f);
    bool compact;
    uint32_t entry_size;
    int ace_lines;
    if (inner <= 0.0f) return result;
    ace_lines = menu_ace_lines(ui, model, inner);
    compact = menu_full_height(ui, model, inner, ace_lines) > nk_overlay_card_room(model);
    if (compact) {
        int room = menu_compact_ace_lines(ui, model, nk_overlay_card_room(model));
        if (ace_lines > room) ace_lines = room;
    }
    entry_size = compact ? NK_UI_BUTTON_SMALL : 0u;
    {
        NkUiLayout head = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                            nk_overlay_fit(), compact ? 0.0f : NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, MENU_ID_HEAD, &head, NULL)) {
            nk_ui_eyebrow(ui, MENU_ID_EYEBROW, "Game paused");
            if (nonempty(model->title_name) != NULL) {
                NkUiTextStyle title = nk_ui_type(NK_UI_TYPE_TITLE, theme->paper);
                title.wrap = true;
                nk_ui_text(ui, MENU_ID_TITLE,
                           nk_ui_text_clamp(ui, model->title_name, &title, inner,
                                            compact ? 1 : 2),
                           &title);
            } else {
                nk_ui_text_role(ui, MENU_ID_TITLE, "Paused", NK_UI_TYPE_TITLE, theme->paper);
            }
            if (nonempty(model->disc_id) != NULL) {
                nk_ui_text_role(ui, MENU_ID_DISC, model->disc_id, NK_UI_TYPE_DATA, theme->muted);
            }
            /* Enhanced or Ace is on for this run: its name and what it does. Nothing when Authentic.
               Words too long for one line are a list on as many lines as it takes; a card
               short of room for them counts the rest on its last line ("and N more"). */
            if (nonempty(model->ace_caption) != NULL) {
                NkUiTextStyle caption = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                NkUiLayout lines = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                                     nk_overlay_fit(), 0.0f);
                if (nk_ui_text_width(ui, model->ace_caption, &caption) <= inner) {
                    nk_ui_text(ui, MENU_ID_ACE,
                               nk_ui_text_clamp(ui, model->ace_caption, &caption, inner, 1), &caption);
                } else if (nk_ui_begin_container(ui, MENU_ID_ACE, &lines, NULL)) {
                    const char *rest = model->ace_caption;
                    caption.wrap = false;
                    for (int line = 0; line < ace_lines && rest[0] != '\0'; ++line) {
                        nk_ui_text(ui, MENU_ID_ACE_LINE + (NkUiId)line,
                                   nk_ui_list_line(ui, &rest, &caption, inner, line + 1 == ace_lines),
                                   &caption);
                    }
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout column = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                              nk_overlay_fit(),
                                              compact ? NK_UI_SPACE_2 : NK_UI_SPACE_3);
        column.padding_top = compact ? 0.0f : NK_UI_SPACE_2;
        if (nk_ui_begin_container(ui, MENU_ID_ACTIONS, &column, NULL)) {
            if (nk_ui_button(ui, NK_OVERLAY_ID_RESUME, "Resume",
                             NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_GROW |
                                 (compact ? 0u : NK_UI_BUTTON_LARGE))) {
                result = nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0);
            }
            if (nk_ui_button(ui, NK_OVERLAY_ID_SETTINGS, "Settings",
                             NK_UI_BUTTON_GROW | entry_size)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_SETTINGS, 0u);
            }
            if (nk_ui_button(ui, NK_OVERLAY_ID_CONTROLS, "Controls",
                             NK_UI_BUTTON_GROW | entry_size)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_CONTROLS, 0u);
            }
            if (model->developer && nk_ui_button(ui, NK_OVERLAY_ID_VRAM, "VRAM viewer",
                                                 NK_UI_BUTTON_GROW | entry_size)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_VRAM, 0u);
            }
            if (model->developer && nk_ui_button(ui, NK_OVERLAY_ID_PROFILE, "Profile",
                                                 NK_UI_BUTTON_GROW | entry_size)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_PROFILE, 0u);
            }
            if (nk_ui_button(ui, NK_OVERLAY_ID_QUIT, "Quit to library",
                             NK_UI_BUTTON_GROW | entry_size)) {
                nk_overlay_go(state, NK_OVERLAY_SCREEN_QUIT, 0u);
            }
            nk_ui_end_container(ui);
        }
    }
    if (result.kind == NK_OVERLAY_ACT_NONE && nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        result = nk_overlay_action(NK_OVERLAY_ACT_RESUME, 0);
    }
    nk_overlay_page_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    return result;
}

/* One mapping table: a header row and one row per control, hairlines between. */
static void controls_table(NkUi *ui, NkUiId id, const NkOverlayControlRow *rows, size_t count,
                           float row_height, NkUiSizing width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout table = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, width, nk_overlay_fit(), 0.0f);
    static const char *const heads[3] = {"PSP", "Keyboard", "Controller"};
    size_t i;
    if (!nk_ui_begin_container(ui, id, &table, NULL)) return;
    for (i = 0; i <= count; ++i) {
        NkUiId row_id = (NkUiId)(id + 1u + (NkUiId)i * 8u);
        NkUiLayout row = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                           nk_overlay_fixed(i == 0u ? 30.0f : row_height),
                                           NK_UI_SPACE_3);
        const char *cells[3];
        int c;
        row.align_y = NK_UI_ALIGN_CENTER;
        if (i == 0u) {
            cells[0] = heads[0];
            cells[1] = heads[1];
            cells[2] = heads[2];
        } else {
            const NkOverlayControlRow *r = &rows[i - 1u];
            cells[0] = nonempty(r->psp) != NULL ? r->psp : OVERLAY_DASH;
            cells[1] = nonempty(r->keyboard) != NULL ? r->keyboard : OVERLAY_DASH;
            cells[2] = nonempty(r->controller) != NULL ? r->controller : OVERLAY_DASH;
            nk_ui_divider(ui, (NkUiId)(row_id + 7u), true);
        }
        if (!nk_ui_begin_container(ui, row_id, &row, NULL)) continue;
        for (c = 0; c < 3; ++c) {
            NkUiLayout cell = nk_overlay_layout(NK_UI_LAYOUT_ROW,
                                                c == 0 ? nk_overlay_fixed(96.0f)
                                                       : nk_overlay_grow(),
                                                nk_overlay_fit(), 0.0f);
            if (!nk_ui_begin_container(ui, (NkUiId)(row_id + 1u + (NkUiId)c), &cell, NULL)) {
                continue;
            }
            if (i == 0u) {
                nk_ui_eyebrow(ui, (NkUiId)(row_id + 4u + (NkUiId)c), cells[c]);
            } else if (c == 0) {
                nk_ui_text_role(ui, (NkUiId)(row_id + 4u), cells[0], NK_UI_TYPE_BODY_S,
                                theme->paper);
            } else {
                NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA,
                                                c == 1 ? theme->muted : theme->paper);
                nk_ui_text(ui, (NkUiId)(row_id + 4u + (NkUiId)c),
                           nk_ui_text_clamp(ui, cells[c], &data, 400.0f, 1), &data);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

NkOverlayAction nk_overlay_page_controls(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    float height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    size_t count = model->controls != NULL ? model->control_count : 0u;
    /* Short windows put the list in two tables side by side so the card fits. */
    bool split = height < 720.0f && count > 6u;
    float row_height = split ? 34.0f : 38.0f;
    float inner;
    if (count > NK_OVERLAY_MAX_CONTROL_ROWS) count = NK_OVERLAY_MAX_CONTROL_ROWS;
    inner = nk_overlay_page_begin(ui, model, split ? 880.0f : 640.0f);
    if (inner <= 0.0f) return result;
    nk_ui_eyebrow(ui, CONTROLS_ID_EYEBROW, "Game paused \xC2\xB7 Controls");
    nk_ui_text_role(ui, CONTROLS_ID_TITLE, "Controls", NK_UI_TYPE_TITLE, theme->paper);
    if (count == 0u) {
        nk_ui_text_role(ui, CONTROLS_ID_NOTE, "No controls are mapped for this game.",
                        NK_UI_TYPE_BODY, theme->muted);
    } else if (split) {
        NkUiLayout tables = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                              nk_overlay_fit(), NK_UI_SPACE_6);
        size_t first = (count + 1u) / 2u;
        if (nk_ui_begin_container(ui, CONTROLS_ID_TABLES, &tables, NULL)) {
            controls_table(ui, CONTROLS_ID_TABLE, model->controls, first, row_height,
                           nk_overlay_grow());
            controls_table(ui, CONTROLS_ID_TABLE + 0x100u, model->controls + first,
                           count - first, row_height, nk_overlay_grow());
            nk_ui_end_container(ui);
        }
    } else {
        controls_table(ui, CONTROLS_ID_TABLE, model->controls, count, row_height,
                       nk_overlay_grow());
    }
    {
        NkUiTextStyle foot = nk_ui_type(NK_UI_TYPE_FOOTNOTE, theme->dim);
        foot.wrap = true;
        nk_ui_text(ui, CONTROLS_ID_FOOT,
                   "Esc, F1 or the controller's Guide button opens this menu. In Enhanced and "
                   "Ace, hold Tab or press F9 to fast forward. Change the controller buttons in "
                   "the library's Settings.", &foot);
    }
    if (nk_ui_button(ui, NK_OVERLAY_ID_CONTROLS_BACK, "Back", 0u) ||
        nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, NK_OVERLAY_ID_CONTROLS);
    }
    nk_overlay_page_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    return result;
}
