/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* What the screens split out of scr_title.c share. Each screen is one file
   (scr_<screen>.c); what two or more of them use lives here and in
   scr_flow.c (the one-card frame, the files-a-disc-needs box) and
   scr_form.c (the setting rows the wizard and Settings share). The ids of
   the one-card flow pages are the FLOW screen's (nk_ui.h), so their macros
   are here; every other screen keeps its own in its own file. */

#ifndef NK_PLAYER_UI_SCR_SHARED_H
#define NK_PLAYER_UI_SCR_SHARED_H

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"

#include <string.h>

static inline NkUiSizing sizing_grow(void) {
    NkUiSizing sizing = {NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static inline NkUiSizing sizing_fit(void) {
    NkUiSizing sizing = {NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static inline NkUiSizing sizing_fixed(float value) {
    NkUiSizing sizing = {NK_UI_SIZE_FIXED, value, 0.0f, 0.0f};
    return sizing;
}

static inline NkUiLayout layout_make(NkUiLayoutDirection direction, NkUiSizing width,
                              NkUiSizing height, float gap) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    layout.direction = direction;
    layout.gap = gap;
    return layout;
}

static inline PlayerUiAction action(PlayerUiActionKind kind, size_t title_index,
                             PlayerUiPrimaryAction primary) {
    PlayerUiAction result;
    result.kind = kind;
    result.primary = primary;
    result.title_index = title_index;
    return result;
}

static inline const char *safe_text(const char *text, const char *fallback) {
    return text != NULL && text[0] != '\0' ? text : fallback;
}

/* A page's body between the top bar and the prompt bar is a scroll container at
   every text size: content taller than that room (a title page with its
   starting card, what to expect and Enhancements in a 1280 x 720 window, say)
   scrolls there, with the focused control brought into view, instead of
   pushing the prompt bar off the window or being squeezed under it. Content
   that fits leaves nothing to scroll, so the page looks and behaves as before. */
#define SCR_PAGE_BODY_SCROLLS true

/* An outcome line's colour: only a warning is amber, and only a failure red. */
static inline uint32_t note_rgba(const NkUiTheme *theme, PlayerUiNoteKind kind) {
    switch (kind) {
    case PLAYER_UI_NOTE_DONE: return theme->ready;
    case PLAYER_UI_NOTE_WARNING: return theme->warning;
    case PLAYER_UI_NOTE_ERROR: return theme->error;
    case PLAYER_UI_NOTE_NEUTRAL:
    default: return theme->muted;
    }
}

#define FLOW_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_FLOW, (offset))
#define FLOW_ID_ROOT FLOW_ID(0x000001u)
#define FLOW_ID_TOP FLOW_ID(0x000010u)
#define FLOW_ID_BODY FLOW_ID(0x000020u)
#define FLOW_ID_CARD FLOW_ID(0x000021u)
#define FLOW_ID_EYEBROW FLOW_ID(0x000022u)
#define FLOW_ID_TITLE FLOW_ID(0x000023u)
#define FLOW_ID_LEAD FLOW_ID(0x000024u)
#define FLOW_ID_STAGES FLOW_ID(0x000030u)
#define FLOW_ID_STAGES_SPAN 0x10u
#define FLOW_ID_STAGE_LABELS FLOW_ID(0x000130u)
#define FLOW_ID_STAGE_LABELS_SPAN 0x10u
#define FLOW_ID_BAR FLOW_ID(0x000040u)
#define FLOW_ID_STATUS FLOW_ID(0x000041u)
#define FLOW_ID_STATUS_SPACER FLOW_ID(0x000141u)
#define FLOW_ID_MESSAGE FLOW_ID(0x000042u)
#define FLOW_ID_ELAPSED FLOW_ID(0x000043u)
#define FLOW_ID_NOTICE FLOW_ID(0x000044u)
#define FLOW_ID_SPEED FLOW_ID(0x000045u)          /* the build card's Build speed line */
#define FLOW_ID_QUEUE_LINE FLOW_ID(0x000046u)  /* the build card's build queue line */
#define FLOW_ID_LOG FLOW_ID(0x000050u)
#define FLOW_ID_LOG_SPAN 0x10u
#define FLOW_ID_ROWS FLOW_ID(0x000060u)
#define FLOW_ID_ROWS_SPAN 0x10u
#define FLOW_ID_ROWS_MORE FLOW_ID(0x000150u)       /* "and 3 more" under the rows */
#define FLOW_ID_ITEMS FLOW_ID(0x000200u)           /* Remove: + 0x10 * part + item */
#define FLOW_ID_ITEMS_SPAN 0x40u
#define FLOW_ID_TOTAL FLOW_ID(0x000070u)
#define FLOW_ID_ACTIONS FLOW_ID(0x000080u)
#define FLOW_ID_DETAILS FLOW_ID(0x000090u)
#define FLOW_ID_DETAILS_SPAN 0x8u
#define FLOW_ID_REPORT_TEXT FLOW_ID(0x000098u)
#define FLOW_ID_REPORT_ROW FLOW_ID(0x000099u)
#define FLOW_ID_PROMPTS FLOW_ID(0x0000A0u)
#define FLOW_ID_WAVES FLOW_ID(0x0000B0u)
#define FLOW_ID_NICKNAME_FIELD FLOW_ID(0x000300u)  /* the nickname editor's box (#2197) */
#define FLOW_ID_NICKNAME_TEXT FLOW_ID(0x000301u)
#define FLOW_ID_NICKNAME_NOTE FLOW_ID(0x000302u)   /* why Done was refused */
#define FLOW_ID_NICKNAME_COUNT FLOW_ID(0x000303u)  /* "12 / 32 bytes" */
#define FLOW_ID_NICKNAME_KEYS FLOW_ID(0x000310u)   /* its keyboard's rows (ui_keyboard.h) */
#define FLOW_ID_NICKNAME_KEYS_SPAN 0x10u           /* UI_KEYBOARD_PART_COUNT */

#define FLOW_MARK_DIAMOND "\xE2\x97\x86"
#define FLOW_MARK_DOT "\xE2\x97\x8F"
#define FLOW_MARK_CROSS "\xC3\x97" /* multiplication sign: in every UI face */
#define FLOW_MARK_TRIANGLE "\xE2\x96\xB2"
#define FLOW_MARK_CHECK "\xE2\x9C\x93 "
#define FLOW_MIDDOT " \xC2\xB7 "

/* What one setting row shows: its button reads "name: value". */
typedef struct {
    const char *name;
    const char *value;
    const char *sentence;
} WizardRow;

/* How a group of setting rows shares its box: the button column they line up on,
   and whether each sentence sits under its button instead of beside it. */
typedef struct {
    float cell;
    bool stacked;
} WizardColumn;

/* The online options are not built yet, so the wizard and Settings say so in one
   line where the Online mode switch and its News and Reports buttons were (#2007).
   The stored online_mode setting is unchanged; docs/ONLINE_FEATURES.md has the plan. */
#define SCR_ONLINE_NOT_BUILT_LINE \
    "Online features aren't built yet, so Nakagawa makes no connection and has " \
    "nothing to switch on."
/* The Nickname row in Settings (#2197): its button opens the nickname editor. */
#define SCR_NICKNAME_SENTENCE \
    "What games show as this PSP's nickname. It is never taken from this PC's user name."
/* A user whose stored online_mode is On still gets the switch, with what it does now, so
   they can turn it off. Nobody else sees it (#2007). */
#define SCR_ONLINE_STORED_ON_SENTENCE \
    "Online mode is on in your settings, but it does nothing yet because online " \
    "features aren't built. Switch it off to keep it off."
/* Discord status (#2259) on Settings > Online and developer. */
#define SCR_DISCORD_SENTENCE \
    "Show the game you play and for how long in Discord, through the Discord app on this " \
    "PC. Nakagawa itself connects to nothing."
/* A build that cannot show Discord status (no application ID, or no local pipes) hides the
   row, except for a stored On, which it shows with this sentence so it can be turned off. */
#define SCR_DISCORD_STORED_ON_SENTENCE \
    "Discord status is on in your settings, but this build can't show it. Switch it off " \
    "to keep it off."
/* The developer switch, in the wizard and in Settings. */
#define WIZARD_DEVELOPER_SENTENCE \
    "Show a developer log on screen to help track down problems. Stays on this PC."

/* Frame and box helpers (scr_flow.c). */
float scr_flow_begin(NkUi *ui, float width, float height, float card_width);
void scr_flow_end(NkUi *ui, const char *hint, const NkUiPromptItem *prompts, size_t count);
void scr_flow_eyebrow(NkUi *ui, const char *marker, const char *text, uint32_t rgba);
NkUiStyle scr_flow_box_style(const NkUiTheme *theme, uint32_t fill);
void scr_flow_bytes(char *out, size_t cap, unsigned long long bytes);
bool scr_input_guide_present(const PlayerUiInputGuide *guide);
void scr_input_guide_box(NkUi *ui, NkUiId base, const PlayerUiInputGuide *guide, float width,
                         size_t max_rows);

/* Setting rows (scr_form.c). */
float scr_check_column_width(NkUi *ui, const PlayerUiCheckRow *checks, size_t count, bool status,
                             NkUiTypeRole role, const char *fallback, float design);
bool scr_wizard_box_begin(NkUi *ui, uint32_t edge);
WizardColumn scr_wizard_column(NkUi *ui, const WizardRow *rows, size_t count, float width,
                              bool stack_at_design_size);
bool scr_wizard_value(NkUi *ui, NkUiId id, NkUiId text_id, const char *name, const char *value,
                      const char *sentence, bool on, float width, const WizardColumn *column);
bool scr_wizard_value_state(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                            const char *value, const char *sentence, bool on, bool disabled,
                            float width, const WizardColumn *column);
bool scr_wizard_switch(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                       const char *sentence, bool on, float width, const WizardColumn *column);
WizardColumn scr_online_developer_column(NkUi *ui, bool online_on, bool developer, float width);

#endif /* NK_PLAYER_UI_SCR_SHARED_H */
