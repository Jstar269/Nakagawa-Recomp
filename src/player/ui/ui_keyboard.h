/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_KEYBOARD_H
#define PLAYER_UI_KEYBOARD_H

/* The player's on-screen keyboard (#2197): a grid of keys for a controller or the mouse,
   drawn from the nk_ui buttons, that edits one UTF-8 text in place. It knows nothing of
   what the text is for: the caller gives the buffer, the most bytes it may hold, and a
   character filter. Today the nickname editor is its caller.

   The keys, row by row (the layout of src/rt/osk_overlay.h's guest keyboard):
       1 2 3 4 5 6 7 8 9 0
       q w e r t y u i o p
       a s d f g h j k l '
       z x c v b n m , . -
       ! ? @ # & * ( ) _ +
       SHIFT  SPACE  DELETE  <cancel>  <done>
   Shift turns the letters over between lower and upper case. Every key is one focus
   stop; left and right stay in a key's row and stop at its ends, as the find box's
   keys do, and up and down move between rows. A press is one key however long it is
   held: the nk_ui buttons take only the first press, never its repeats.

   Every way of typing goes through ui_keyboard_type: a key, a physical keyboard's
   text, a pasted run. A character is taken only when the filter accepts it and it
   fits in the cap, so the text is never longer than cap bytes and never holds a
   character the filter refuses. A key that cannot be typed now (its character
   refused, or no room left) is drawn dimmed and does nothing; it keeps its focus
   stop, so the focus never jumps when the text fills up. */

#include "nk_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_KEYBOARD_CHAR_ROWS 5u
#define UI_KEYBOARD_KEYS_PER_ROW 10u
#define UI_KEYBOARD_CHAR_KEYS (UI_KEYBOARD_CHAR_ROWS * UI_KEYBOARD_KEYS_PER_ROW)

/* The keys' places: key_base + place is a key's focus id. 0 to 49 are the
   character keys, row by row; Done and Cancel use the caller's own ids. */
enum {
    UI_KEYBOARD_KEY_SHIFT = UI_KEYBOARD_CHAR_KEYS,
    UI_KEYBOARD_KEY_SPACE,
    UI_KEYBOARD_KEY_DELETE,
    UI_KEYBOARD_KEY_COUNT
};
/* The key a keyboard opens on: q, the first letter. */
#define UI_KEYBOARD_KEY_DEFAULT UI_KEYBOARD_KEYS_PER_ROW
/* The layout's own elements: part_base + 0 is the key block, + 1 to 6 its rows. */
#define UI_KEYBOARD_PART_COUNT (UI_KEYBOARD_CHAR_ROWS + 2u)

/* Whether `codepoint` may be typed into the text; `user` is the caller's. */
typedef bool (*UiKeyboardFilter)(uint32_t codepoint, void *user);

/* One keyboard's text and state. The caller keeps it between frames. */
typedef struct {
    char *text;              /* NUL-terminated UTF-8, edited in place */
    size_t cap;              /* the most bytes before the NUL: the buffer is cap + 1 */
    UiKeyboardFilter accept; /* NULL takes every well-formed character */
    void *user;
    bool shifted;            /* the letters show, and type, in upper case */
} UiKeyboard;

/* One frame's presses of the pad's shortcuts (X or Square, Y or Triangle, L or R, Start,
   B). A screen that draws something before the keyboard that would take them first (the
   library's top bar takes L / R and Start) takes them with ui_keyboard_take_pad before
   drawing it and hands them to ui_keyboard_draw. */
typedef struct {
    bool remove; /* X or Square: delete */
    bool space;  /* Y or Triangle: a space */
    bool shift;  /* L or R */
    bool done;   /* Start */
    bool back;   /* B, Esc: Cancel */
} UiKeyboardPad;

/* How one keyboard is drawn: its ids and its two closing keys' words. */
typedef struct {
    NkUiId key_base;          /* + the key's place: UI_KEYBOARD_KEY_COUNT focusable ids */
    NkUiId part_base;         /* + 0 to UI_KEYBOARD_PART_COUNT - 1: its rows */
    NkUiId done_id;
    NkUiId cancel_id;
    const char *done_label;   /* "Done" when NULL */
    const char *cancel_label; /* "Cancel" when NULL */
    const UiKeyboardPad *pad; /* taken earlier this frame; NULL: taken while it is drawn */
} UiKeyboardView;

typedef enum {
    UI_KEYBOARD_NONE = 0, /* nothing pressed, or a key that could not type */
    UI_KEYBOARD_EDITED,   /* the text or the letter case changed */
    UI_KEYBOARD_DONE,     /* Done, or the pad's Start */
    UI_KEYBOARD_CANCEL    /* Cancel, or Back (the pad's B, Esc) */
} UiKeyboardResult;

/* Points `keyboard` at `text` (cap + 1 bytes, NUL-terminated) with `accept`. */
void ui_keyboard_init(UiKeyboard *keyboard, char *text, size_t cap, UiKeyboardFilter accept,
                      void *user);
/* Whether typing `codepoint` now would be taken: a character (not a surrogate, not
   past U+10FFFF), the filter accepts it, and its UTF-8 bytes fit in the cap. */
bool ui_keyboard_can_type(const UiKeyboard *keyboard, uint32_t codepoint);
/* Types `utf8`, a character at a time. A character the filter refuses, or a byte that
   is not well-formed UTF-8, is left out; typing stops at the first character that does
   not fit. True when anything was typed. */
bool ui_keyboard_type(UiKeyboard *keyboard, const char *utf8);
/* Removes the last character, all of its bytes. True when there was one. */
bool ui_keyboard_delete(UiKeyboard *keyboard);
/* "12 / 32 bytes": how much of the cap the text uses. Returns what snprintf does. */
int ui_keyboard_count_text(const UiKeyboard *keyboard, char *out, size_t cap);
/* The character key `place` types (0 to UI_KEYBOARD_CHAR_KEYS - 1) in this case, or 0. */
uint32_t ui_keyboard_key_char(unsigned place, bool shifted);
/* `codepoint` in UTF-8 in out (at least 5 bytes, NUL-terminated); its length, or 0 for
   a surrogate or a value past U+10FFFF. */
size_t ui_keyboard_utf8(uint32_t codepoint, char *out);

/* Takes this frame's pad shortcuts into `pad`, so nothing drawn after this sees them. */
void ui_keyboard_take_pad(NkUi *ui, UiKeyboardPad *pad);

/* Draws the keyboard in the open container and answers this frame's presses: a key
   types or deletes through the rules above, Shift turns the letter case over, and the
   pad's shortcuts work while it is drawn (X or Square deletes, Y or Triangle types a
   space, L or R is Shift, Start is Done, B is Cancel). */
UiKeyboardResult ui_keyboard_draw(NkUi *ui, UiKeyboard *keyboard, const UiKeyboardView *view);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_KEYBOARD_H */
