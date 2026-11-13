/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The player's on-screen keyboard (#2197); ui_keyboard.h describes it. The character
   keys are drawn here from the nk_ui pieces a button is made of (nk_ui_button capitalises
   its label, and a keyboard has to show which case its letters type); the other keys are
   nk_ui buttons. */

#include "ui_keyboard.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <string.h>

static const char k_rows[UI_KEYBOARD_CHAR_ROWS][UI_KEYBOARD_KEYS_PER_ROW + 1u] = {
    "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-", "!?@#&*()_+"
};

void ui_keyboard_init(UiKeyboard *keyboard, char *text, size_t cap, UiKeyboardFilter accept,
                      void *user) {
    if (keyboard == NULL) return;
    memset(keyboard, 0, sizeof(*keyboard));
    keyboard->text = text;
    keyboard->cap = cap;
    keyboard->accept = accept;
    keyboard->user = user;
}

size_t ui_keyboard_utf8(uint32_t codepoint, char *out) {
    unsigned char *s = (unsigned char *)out;
    size_t n;
    if (out == NULL) return 0u;
    out[0] = '\0';
    if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) return 0u;
    if (codepoint < 0x80u) {
        s[0] = (unsigned char)codepoint;
        n = 1u;
    } else if (codepoint < 0x800u) {
        s[0] = (unsigned char)(0xC0u | (codepoint >> 6));
        s[1] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        n = 2u;
    } else if (codepoint < 0x10000u) {
        s[0] = (unsigned char)(0xE0u | (codepoint >> 12));
        s[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        s[2] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        n = 3u;
    } else {
        s[0] = (unsigned char)(0xF0u | (codepoint >> 18));
        s[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        s[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        s[3] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        n = 4u;
    }
    out[n] = '\0';
    return n;
}

/* Reads one well-formed UTF-8 character from `s`: its length, with *codepoint set, or
   0 for a byte that does not start one (an overlong form, a lone continuation byte, a
   cut-off sequence, a surrogate or a value past U+10FFFF). */
static size_t utf8_read(const unsigned char *s, uint32_t *codepoint) {
    size_t extra;
    uint32_t cp;
    uint32_t min;
    if (s[0] < 0x80u) {
        *codepoint = s[0];
        return 1u;
    }
    if (s[0] >= 0xC2u && s[0] <= 0xDFu) {
        extra = 1u;
        cp = s[0] & 0x1Fu;
        min = 0x80u;
    } else if (s[0] >= 0xE0u && s[0] <= 0xEFu) {
        extra = 2u;
        cp = s[0] & 0x0Fu;
        min = 0x800u;
    } else if (s[0] >= 0xF0u && s[0] <= 0xF4u) {
        extra = 3u;
        cp = s[0] & 0x07u;
        min = 0x10000u;
    } else {
        return 0u;
    }
    for (size_t k = 1u; k <= extra; ++k) {
        if ((s[k] & 0xC0u) != 0x80u) return 0u; /* also stops at the NUL */
        cp = (cp << 6) | (s[k] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0u;
    *codepoint = cp;
    return 1u + extra;
}

bool ui_keyboard_can_type(const UiKeyboard *keyboard, uint32_t codepoint) {
    char bytes[5];
    size_t n;
    if (keyboard == NULL || keyboard->text == NULL || codepoint == 0u) return false;
    n = ui_keyboard_utf8(codepoint, bytes);
    if (n == 0u || strlen(keyboard->text) + n > keyboard->cap) return false;
    return keyboard->accept == NULL || keyboard->accept(codepoint, keyboard->user);
}

bool ui_keyboard_type(UiKeyboard *keyboard, const char *utf8) {
    const unsigned char *s = (const unsigned char *)utf8;
    bool typed = false;
    size_t length;
    if (keyboard == NULL || keyboard->text == NULL || utf8 == NULL) return false;
    length = strlen(keyboard->text);
    for (size_t i = 0u; s[i] != 0u;) {
        uint32_t cp = 0u;
        size_t n = utf8_read(s + i, &cp);
        if (n == 0u) {
            ++i; /* not a character: left out */
            continue;
        }
        if (keyboard->accept != NULL && !keyboard->accept(cp, keyboard->user)) {
            i += n; /* refused by the filter: left out */
            continue;
        }
        if (length + n > keyboard->cap) break;
        memcpy(keyboard->text + length, s + i, n);
        length += n;
        keyboard->text[length] = '\0';
        typed = true;
        i += n;
    }
    return typed;
}

bool ui_keyboard_delete(UiKeyboard *keyboard) {
    size_t length;
    if (keyboard == NULL || keyboard->text == NULL) return false;
    length = strlen(keyboard->text);
    if (length == 0u) return false;
    while (length > 0u && ((unsigned char)keyboard->text[length - 1u] & 0xC0u) == 0x80u) {
        --length;
    }
    if (length > 0u) --length;
    keyboard->text[length] = '\0';
    return true;
}

int ui_keyboard_count_text(const UiKeyboard *keyboard, char *out, size_t cap) {
    size_t used = keyboard != NULL && keyboard->text != NULL ? strlen(keyboard->text) : 0u;
    size_t most = keyboard != NULL ? keyboard->cap : 0u;
    if (out == NULL || cap == 0u) return 0;
    return snprintf(out, cap, "%zu / %zu bytes", used, most);
}

uint32_t ui_keyboard_key_char(unsigned place, bool shifted) {
    char c;
    if (place >= UI_KEYBOARD_CHAR_KEYS) return 0u;
    c = k_rows[place / UI_KEYBOARD_KEYS_PER_ROW][place % UI_KEYBOARD_KEYS_PER_ROW];
    if (shifted && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return (uint32_t)(unsigned char)c;
}

/* Types one character; true when it was taken. */
static bool type_codepoint(UiKeyboard *keyboard, uint32_t codepoint) {
    char bytes[5];
    if (!ui_keyboard_can_type(keyboard, codepoint)) return false;
    (void)ui_keyboard_utf8(codepoint, bytes);
    return ui_keyboard_type(keyboard, bytes);
}

/* A character key: a small button, as nk_ui_button draws one, whose label keeps its
   case. `usable` false draws the label dimmed (the key cannot type now); it stays
   focusable. True on the frame it is activated. */
static bool character_key(NkUi *ui, NkUiId id, const char *label, bool usable) {
    const uint32_t flags = NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW | NK_UI_BUTTON_ROW;
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY, 0u);
    NkUiLayout layout;
    NkUiStyle style;
    bool focused;
    bool activated;
    nk_ui_register_button(ui, id, flags);
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED,
                                 nk_ui_line_box(ui, NK_UI_CONTROL_HEIGHT_SMALL, &text), 0.0f,
                                 0.0f};
    layout.direction = NK_UI_LAYOUT_ROW;
    layout.padding_left = NK_UI_SPACE_2;
    layout.padding_right = NK_UI_SPACE_2;
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.align_y = NK_UI_ALIGN_CENTER;
    memset(&style, 0, sizeof(style));
    /* A key that cannot type now (the cap, or the filter) loses its outline as well as
       its label colour, so colour is not the only cue: its shape changes too. The
       outline is drawn inside the key, so the key keeps its size. */
    style.border_width = usable ? NK_UI_BORDER_HAIRLINE : 0.0f;
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    style.border_rgba = theme->line_strong;
    focused = nk_ui_button_focused(ui, id);
    if (nk_ui_pressed(ui, id)) {
        style.fill_rgba = theme->pressed;
    } else if (focused || nk_ui_hovered(ui, id)) {
        style.fill_rgba = theme->surface_high;
    }
    style.transition = NK_UI_TRANSITION_COLOR;
    style.transition_s = (float)NK_UI_MOTION_FAST;
    if (!nk_ui_begin_container(ui, id, &layout, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    activated = nk_ui_take_activation(ui, id);
    text.rgba = usable ? nk_ui_button_text_rgba(ui, NK_UI_BUTTON_SMALL) : theme->muted;
    text.wrap = false;
    nk_ui_text(ui, id, label, &text);
    nk_ui_end_container(ui);
    return activated;
}

static NkUiLayout keyboard_row(float gap) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_ROW;
    layout.gap = gap;
    layout.align_y = NK_UI_ALIGN_CENTER;
    return layout;
}

void ui_keyboard_take_pad(NkUi *ui, UiKeyboardPad *pad) {
    if (pad == NULL) return;
    pad->remove = nk_ui_take_event(ui, NK_UI_EV_OPTIONS, NULL);
    pad->space = nk_ui_take_event(ui, NK_UI_EV_DETAILS, NULL);
    pad->shift = nk_ui_take_event(ui, NK_UI_EV_TAB, NULL);
    pad->done = nk_ui_take_event(ui, NK_UI_EV_MENU, NULL);
    pad->back = nk_ui_take_event(ui, NK_UI_EV_BACK, NULL);
}

UiKeyboardResult ui_keyboard_draw(NkUi *ui, UiKeyboard *keyboard, const UiKeyboardView *view) {
    UiKeyboardResult result = UI_KEYBOARD_NONE;
    NkUiLayout block;
    if (ui == NULL || keyboard == NULL || keyboard->text == NULL || view == NULL) {
        return UI_KEYBOARD_NONE;
    }
    block = keyboard_row(0.0f);
    block.direction = NK_UI_LAYOUT_COLUMN;
    block.gap = NK_UI_SPACE_1;
    if (!nk_ui_begin_container(ui, view->part_base, &block, NULL)) return UI_KEYBOARD_NONE;
    for (unsigned r = 0u; r < UI_KEYBOARD_CHAR_ROWS; ++r) {
        NkUiLayout row = keyboard_row(NK_UI_SPACE_1);
        if (!nk_ui_begin_container(ui, view->part_base + 1u + r, &row, NULL)) continue;
        for (unsigned k = 0u; k < UI_KEYBOARD_KEYS_PER_ROW; ++k) {
            unsigned place = r * UI_KEYBOARD_KEYS_PER_ROW + k;
            uint32_t cp = ui_keyboard_key_char(place, keyboard->shifted);
            char label[5];
            (void)ui_keyboard_utf8(cp, label);
            if (character_key(ui, view->key_base + place, label,
                              ui_keyboard_can_type(keyboard, cp)) &&
                type_codepoint(keyboard, cp)) {
                result = UI_KEYBOARD_EDITED;
            }
        }
        nk_ui_end_container(ui);
    }
    {
        const uint32_t flags = NK_UI_BUTTON_SMALL | NK_UI_BUTTON_ROW;
        NkUiLayout row = keyboard_row(NK_UI_SPACE_1);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, view->part_base + 1u + UI_KEYBOARD_CHAR_ROWS, &row, NULL)) {
            if (nk_ui_button(ui, view->key_base + UI_KEYBOARD_KEY_SHIFT, "Shift", flags)) {
                keyboard->shifted = !keyboard->shifted;
                result = UI_KEYBOARD_EDITED;
            }
            if (nk_ui_button(ui, view->key_base + UI_KEYBOARD_KEY_SPACE, "Space",
                             flags | NK_UI_BUTTON_GROW) &&
                type_codepoint(keyboard, ' ')) {
                result = UI_KEYBOARD_EDITED;
            }
            if (nk_ui_button(ui, view->key_base + UI_KEYBOARD_KEY_DELETE, "Delete", flags) &&
                ui_keyboard_delete(keyboard)) {
                result = UI_KEYBOARD_EDITED;
            }
            if (nk_ui_button(ui, view->cancel_id,
                             view->cancel_label != NULL ? view->cancel_label : "Cancel",
                             flags | NK_UI_BUTTON_NOT_DEFAULT)) {
                result = UI_KEYBOARD_CANCEL;
            }
            if (nk_ui_button(ui, view->done_id,
                             view->done_label != NULL ? view->done_label : "Done",
                             flags | NK_UI_BUTTON_PRIMARY)) {
                result = UI_KEYBOARD_DONE;
            }
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
    /* The pad's shortcuts, taken every frame the keyboard is up so none reaches the
       screen under it (or before it, by the screen: view->pad). A key's own press
       above wins over them. */
    {
        UiKeyboardPad taken;
        const UiKeyboardPad *pad = view->pad;
        if (pad == NULL) {
            ui_keyboard_take_pad(ui, &taken);
            pad = &taken;
        }
        if (result != UI_KEYBOARD_NONE) return result;
        if (pad->back) return UI_KEYBOARD_CANCEL;
        if (pad->done) return UI_KEYBOARD_DONE;
        if (pad->remove && ui_keyboard_delete(keyboard)) return UI_KEYBOARD_EDITED;
        if (pad->space && type_codepoint(keyboard, ' ')) return UI_KEYBOARD_EDITED;
        if (pad->shift) {
            keyboard->shifted = !keyboard->shifted;
            return UI_KEYBOARD_EDITED;
        }
    }
    return result;
}
