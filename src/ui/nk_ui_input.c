/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_input.h"

#include <math.h>
#include <string.h>

#define NK_UI_REPEAT_CATCH_UP 8u

NkUiRepeatConfig nk_ui_repeat_config_default(void) {
    NkUiRepeatConfig config;
    config.initial_delay_ms = 400u;
    config.slow_ms = 110u;
    config.normal_ms = 70u;
    config.fast_ms = 45u;
    return config;
}

uint32_t nk_ui_repeat_interval_ms(const NkUiRepeatConfig *config, NkUiRepeatRate rate) {
    NkUiRepeatConfig fallback;
    uint32_t ms;
    if (config == NULL) {
        fallback = nk_ui_repeat_config_default();
        config = &fallback;
    }
    switch (rate) {
    case NK_UI_REPEAT_SLOW: ms = config->slow_ms; break;
    case NK_UI_REPEAT_FAST: ms = config->fast_ms; break;
    default: ms = config->normal_ms; break;
    }
    return ms != 0u ? ms : 1u;
}

void nk_ui_input_init(NkUiInput *input) {
    if (input == NULL) {
        return;
    }
    memset(input, 0, sizeof(*input));
    input->config = nk_ui_repeat_config_default();
    input->rate = NK_UI_REPEAT_NORMAL;
}

void nk_ui_input_set_repeat_rate(NkUiInput *input, NkUiRepeatRate rate) {
    if (input != NULL) {
        input->rate = rate;
    }
}

void nk_ui_input_set_repeat_config(NkUiInput *input, const NkUiRepeatConfig *config) {
    if (input != NULL && config != NULL) {
        input->config = *config;
    }
}

void nk_ui_input_reset(NkUiInput *input) {
    if (input == NULL) {
        return;
    }
    memset(input->down, 0, sizeof(input->down));
    memset(input->dir_button_holder_count, 0, sizeof(input->dir_button_holder_count));
    memset(input->dir_stick_held, 0, sizeof(input->dir_stick_held));
    memset(input->dir_seq, 0, sizeof(input->dir_seq));
    input->stick_dir = NK_UI_DIR_NONE;
    input->repeating = false;
    input->repeat_dir = NK_UI_DIR_NONE;
    input->pointer_down = false;
}

static NkUiEvent nk_ui_input_make_event(NkUiEventType type, int device, NkUiPadFamily family) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.device = device;
    event.pad_family = family;
    return event;
}

static size_t nk_ui_input_emit(NkUiEvent *out, size_t cap, size_t used, NkUiEvent event) {
    if (out != NULL && used < cap) {
        out[used] = event;
        return used + 1u;
    }
    return used;
}

/* Emits NK_UI_EV_DEVICE when the source of input changed. Any press noted here is not a
   touch, so it ends touch input, even on the device touch shares. */
static size_t nk_ui_input_note_device(NkUiInput *input, int device, NkUiPadFamily family,
                                      NkUiEvent *out, size_t cap, size_t used) {
    bool was_touch = input->touch;
    input->touch = false;
    if (input->have_device && input->last_device == device && input->last_family == family &&
        !was_touch) {
        return used;
    }
    input->have_device = true;
    input->last_device = device;
    input->last_family = family;
    {
        NkUiEvent event = nk_ui_input_make_event(NK_UI_EV_DEVICE, device, family);
        event.down = true;
        return nk_ui_input_emit(out, cap, used, event);
    }
}

static int nk_ui_input_dir_index(NkUiNavDir dir) {
    switch (dir) {
    case NK_UI_DIR_UP: return 0;
    case NK_UI_DIR_DOWN: return 1;
    case NK_UI_DIR_LEFT: return 2;
    case NK_UI_DIR_RIGHT: return 3;
    default: return -1;
    }
}

static NkUiNavDir nk_ui_input_dir_from_index(int index) {
    static const NkUiNavDir dirs[4] = {NK_UI_DIR_UP, NK_UI_DIR_DOWN, NK_UI_DIR_LEFT,
                                       NK_UI_DIR_RIGHT};
    return index >= 0 && index < 4 ? dirs[index] : NK_UI_DIR_NONE;
}

static bool nk_ui_input_dir_held_any(const NkUiInput *input, int index) {
    return input->dir_button_holder_count[index] > 0u || input->dir_stick_held[index];
}

/* Where `device` sits among the holders of direction button `index`, or the count. */
static size_t nk_ui_input_dir_holder_slot(const NkUiInput *input, int index, int device) {
    size_t i;
    for (i = 0; i < input->dir_button_holder_count[index]; ++i) {
        if (input->dir_button_holders[index][i] == device) {
            return i;
        }
    }
    return input->dir_button_holder_count[index];
}

static void nk_ui_input_start_repeat(NkUiInput *input, NkUiNavDir dir, int device,
                                     NkUiPadFamily family, double now_s) {
    input->repeating = true;
    input->repeat_dir = dir;
    input->repeat_device = device;
    input->repeat_family = family;
    input->next_repeat_s = now_s + (double)input->config.initial_delay_ms / 1000.0;
}

/* source 0 is a key or D-pad button of `device`, 1 the stick. */
static size_t nk_ui_input_dir_press(NkUiInput *input, int source, NkUiNavDir dir, int device,
                                    NkUiPadFamily family, double now_s,
                                    NkUiEvent *out, size_t cap, size_t used) {
    int index = nk_ui_input_dir_index(dir);
    NkUiEvent event;
    bool was_held;
    if (index < 0) {
        return used;
    }
    was_held = nk_ui_input_dir_held_any(input, index);
    if (source == 0) {
        size_t slot = nk_ui_input_dir_holder_slot(input, index, device);
        if (slot < input->dir_button_holder_count[index]) {
            return used; /* this device already holds it: an OS key repeat */
        }
        /* The table fits every tracked device; were it ever full, the direction is
           held already, so the press would add nothing but the device note. */
        if (slot < NK_UI_INPUT_DIR_HOLDERS) {
            input->dir_button_holders[index][slot] = device;
            input->dir_button_holder_count[index] = slot + 1u;
        }
    } else {
        if (input->dir_stick_held[index]) {
            return used;
        }
        input->dir_stick_held[index] = true;
    }
    used = nk_ui_input_note_device(input, device, family, out, cap, used);
    if (was_held) {
        /* Another device or the stick already holds this direction: it stays one
           hold, so no second step and no restarted repeat. */
        return used;
    }
    input->dir_seq[index] = ++input->seq;
    event = nk_ui_input_make_event(NK_UI_EV_NAV, device, family);
    event.dir = dir;
    event.down = true;
    used = nk_ui_input_emit(out, cap, used, event);
    nk_ui_input_start_repeat(input, dir, device, family, now_s);
    return used;
}

static void nk_ui_input_dir_release(NkUiInput *input, int source, NkUiNavDir dir, int device,
                                    double now_s) {
    int index = nk_ui_input_dir_index(dir);
    int i;
    int best = -1;
    if (index < 0) {
        return;
    }
    if (source == 0) {
        size_t slot = nk_ui_input_dir_holder_slot(input, index, device);
        size_t count = input->dir_button_holder_count[index];
        if (slot >= count) {
            return; /* a release from a device that does not hold this direction */
        }
        input->dir_button_holders[index][slot] = input->dir_button_holders[index][count - 1u];
        input->dir_button_holder_count[index] = count - 1u;
    } else {
        if (!input->dir_stick_held[index]) {
            return;
        }
        input->dir_stick_held[index] = false;
    }
    if (!input->repeating || input->repeat_dir != dir || nk_ui_input_dir_held_any(input, index)) {
        return;
    }
    /* The repeating direction let go: fall back to the most recent one still held,
       with a fresh initial delay, or stop. */
    for (i = 0; i < 4; ++i) {
        if (nk_ui_input_dir_held_any(input, i) &&
            (best < 0 || input->dir_seq[i] > input->dir_seq[best])) {
            best = i;
        }
    }
    if (best < 0) {
        input->repeating = false;
        input->repeat_dir = NK_UI_DIR_NONE;
    } else {
        nk_ui_input_start_repeat(input, nk_ui_input_dir_from_index(best),
                                 input->repeat_device, input->repeat_family, now_s);
    }
}

size_t nk_ui_input_button(NkUiInput *input, NkUiInputButton button, bool down,
                          int device, NkUiPadFamily family, double now_s,
                          NkUiEvent *out, size_t cap) {
    size_t used = 0u;
    NkUiEvent event;
    NkUiEventType type;
    NkUiNavDir dir = NK_UI_DIR_NONE;
    if (input == NULL || (unsigned)button >= (unsigned)NK_UI_BTN_COUNT || !isfinite(now_s)) {
        return 0u;
    }
    switch (button) {
    case NK_UI_BTN_UP: case NK_UI_BTN_DOWN: case NK_UI_BTN_LEFT: case NK_UI_BTN_RIGHT: {
        static const NkUiNavDir map[4] = {NK_UI_DIR_UP, NK_UI_DIR_DOWN, NK_UI_DIR_LEFT,
                                          NK_UI_DIR_RIGHT};
        dir = map[button - NK_UI_BTN_UP];
        if (down) {
            used = nk_ui_input_dir_press(input, 0, dir, device, family, now_s, out, cap, used);
        } else {
            nk_ui_input_dir_release(input, 0, dir, device, now_s);
        }
        return used;
    }
    case NK_UI_BTN_CONFIRM: type = NK_UI_EV_ACTIVATE; break;
    case NK_UI_BTN_BACK: type = NK_UI_EV_BACK; break;
    case NK_UI_BTN_OPTIONS: type = NK_UI_EV_OPTIONS; break;
    case NK_UI_BTN_DETAILS: type = NK_UI_EV_DETAILS; break;
    case NK_UI_BTN_TAB_PREV: type = NK_UI_EV_TAB; dir = NK_UI_DIR_LEFT; break;
    case NK_UI_BTN_TAB_NEXT: type = NK_UI_EV_TAB; dir = NK_UI_DIR_RIGHT; break;
    case NK_UI_BTN_MENU: type = NK_UI_EV_MENU; break;
    case NK_UI_BTN_CYCLE_NEXT: type = NK_UI_EV_CYCLE; dir = NK_UI_DIR_RIGHT; break;
    case NK_UI_BTN_PAGE_PREV: type = NK_UI_EV_PAGE; dir = NK_UI_DIR_LEFT; break;
    case NK_UI_BTN_PAGE_NEXT: type = NK_UI_EV_PAGE; dir = NK_UI_DIR_RIGHT; break;
    case NK_UI_BTN_FIRST: type = NK_UI_EV_EDGE; dir = NK_UI_DIR_UP; break;
    case NK_UI_BTN_LAST: type = NK_UI_EV_EDGE; dir = NK_UI_DIR_DOWN; break;
    default: type = NK_UI_EV_CYCLE; dir = NK_UI_DIR_LEFT; break;
    }
    if (input->down[button] == down) {
        return 0u; /* an OS key repeat, or a release that never had a press */
    }
    input->down[button] = down;
    event = nk_ui_input_make_event(type, device, family);
    event.dir = dir;
    if (down) {
        used = nk_ui_input_note_device(input, device, family, out, cap, used);
        event.down = true;
        return nk_ui_input_emit(out, cap, used, event);
    }
    if (type == NK_UI_EV_ACTIVATE) {
        event.down = false;
        return nk_ui_input_emit(out, cap, used, event);
    }
    return 0u;
}

size_t nk_ui_input_stick(NkUiInput *input, float x, float y, int device,
                         NkUiPadFamily family, double now_s,
                         NkUiEvent *out, size_t cap) {
    NkUiNavDir previous;
    NkUiNavDir next;
    size_t used = 0u;
    float ax;
    float ay;
    if (input == NULL || !isfinite(x) || !isfinite(y) || !isfinite(now_s)) {
        return 0u;
    }
    ax = fabsf(x);
    ay = fabsf(y);
    previous = input->stick_dir;
    next = previous;
    if (previous == NK_UI_DIR_NONE) {
        if (ax >= NK_UI_STICK_ENGAGE || ay >= NK_UI_STICK_ENGAGE) {
            if (ax > ay) {
                next = x > 0.0f ? NK_UI_DIR_RIGHT : NK_UI_DIR_LEFT;
            } else {
                next = y > 0.0f ? NK_UI_DIR_DOWN : NK_UI_DIR_UP;
            }
        }
    } else {
        bool horizontal = previous == NK_UI_DIR_LEFT || previous == NK_UI_DIR_RIGHT;
        float along = horizontal ? (previous == NK_UI_DIR_RIGHT ? x : -x)
                                 : (previous == NK_UI_DIR_DOWN ? y : -y);
        float across = horizontal ? ay : ax;
        if (along < NK_UI_STICK_RELEASE) {
            next = NK_UI_DIR_NONE;
            /* A stick swung round to another axis engages that direction at once. */
            if (across >= NK_UI_STICK_ENGAGE) {
                if (horizontal) {
                    next = y > 0.0f ? NK_UI_DIR_DOWN : NK_UI_DIR_UP;
                } else {
                    next = x > 0.0f ? NK_UI_DIR_RIGHT : NK_UI_DIR_LEFT;
                }
            }
        } else if (across >= NK_UI_STICK_ENGAGE && across > along) {
            /* The other axis took over while the old one is still past its release. */
            if (horizontal) {
                next = y > 0.0f ? NK_UI_DIR_DOWN : NK_UI_DIR_UP;
            } else {
                next = x > 0.0f ? NK_UI_DIR_RIGHT : NK_UI_DIR_LEFT;
            }
        }
    }
    if (next == previous) {
        return 0u;
    }
    input->stick_dir = next;
    if (previous != NK_UI_DIR_NONE) {
        nk_ui_input_dir_release(input, 1, previous, device, now_s);
    }
    if (next != NK_UI_DIR_NONE) {
        used = nk_ui_input_dir_press(input, 1, next, device, family, now_s, out, cap, used);
    }
    return used;
}

static size_t nk_ui_input_pointer_from(NkUiInput *input, float x, float y, bool down,
                                       bool touch, NkUiEvent *out, size_t cap) {
    size_t used = 0u;
    NkUiEvent event;
    if (input == NULL || !isfinite(x) || !isfinite(y)) {
        return 0u;
    }
    if (input->have_pointer && input->pointer_x == x && input->pointer_y == y &&
        input->pointer_down == down) {
        return 0u;
    }
    /* A click or a tap makes the mouse's device the active one; motion alone does not. */
    if (down != input->pointer_down) {
        used = nk_ui_input_note_device(input, NK_UI_DEVICE_KEYBOARD_MOUSE, NK_UI_PAD_KEYBOARD,
                                       out, cap, used);
        input->touch = touch;
    }
    input->have_pointer = true;
    input->pointer_x = x;
    input->pointer_y = y;
    input->pointer_down = down;
    event = nk_ui_input_make_event(NK_UI_EV_POINTER, NK_UI_DEVICE_KEYBOARD_MOUSE,
                                   NK_UI_PAD_KEYBOARD);
    event.x = x;
    event.y = y;
    event.down = down;
    return nk_ui_input_emit(out, cap, used, event);
}

size_t nk_ui_input_pointer(NkUiInput *input, float x, float y, bool down,
                           NkUiEvent *out, size_t cap) {
    return nk_ui_input_pointer_from(input, x, y, down, false, out, cap);
}

size_t nk_ui_input_touch(NkUiInput *input, float x, float y, bool down,
                         NkUiEvent *out, size_t cap) {
    return nk_ui_input_pointer_from(input, x, y, down, true, out, cap);
}

size_t nk_ui_input_system_back(NkUiInput *input, bool down, double now_s,
                               NkUiEvent *out, size_t cap) {
    NkUiEvent event;
    if (input == NULL || !isfinite(now_s) || input->down[NK_UI_BTN_BACK] == down) {
        return 0u;
    }
    input->down[NK_UI_BTN_BACK] = down;
    if (!down) {
        return 0u;
    }
    event = nk_ui_input_make_event(NK_UI_EV_BACK, nk_ui_input_last_device(input),
                                   input->have_device ? input->last_family
                                                      : NK_UI_PAD_KEYBOARD);
    event.down = true;
    return nk_ui_input_emit(out, cap, 0u, event);
}

size_t nk_ui_input_pointer_leave(NkUiInput *input, NkUiEvent *out, size_t cap) {
    if (input == NULL || !input->have_pointer) {
        return 0u;
    }
    input->have_pointer = false;
    return nk_ui_input_emit(out, cap, 0u,
                            nk_ui_input_make_event(NK_UI_EV_POINTER_LEAVE,
                                                   NK_UI_DEVICE_KEYBOARD_MOUSE,
                                                   NK_UI_PAD_KEYBOARD));
}

size_t nk_ui_input_wheel(NkUiInput *input, float x, float y, NkUiNavDir dir,
                         NkUiEvent *out, size_t cap) {
    size_t used = 0u;
    NkUiEvent event;
    if (input == NULL || !isfinite(x) || !isfinite(y) || nk_ui_input_dir_index(dir) < 0) {
        return 0u;
    }
    used = nk_ui_input_note_device(input, NK_UI_DEVICE_KEYBOARD_MOUSE, NK_UI_PAD_KEYBOARD,
                                   out, cap, used);
    event = nk_ui_input_make_event(NK_UI_EV_WHEEL, NK_UI_DEVICE_KEYBOARD_MOUSE,
                                   NK_UI_PAD_KEYBOARD);
    event.x = x;
    event.y = y;
    event.dir = dir;
    event.down = true;
    return nk_ui_input_emit(out, cap, used, event);
}

size_t nk_ui_input_text(NkUiInput *input, uint32_t codepoint, NkUiEvent *out, size_t cap) {
    NkUiEvent event;
    if (input == NULL || codepoint == 0u) {
        return 0u;
    }
    event = nk_ui_input_make_event(NK_UI_EV_TEXT, NK_UI_DEVICE_KEYBOARD_MOUSE,
                                   NK_UI_PAD_KEYBOARD);
    event.codepoint = codepoint;
    event.down = true;
    return nk_ui_input_emit(out, cap, 0u, event);
}

size_t nk_ui_input_device_change(NkUiInput *input, int device, NkUiPadFamily family,
                                 bool connected, NkUiEvent *out, size_t cap) {
    NkUiEvent event;
    if (input == NULL) {
        return 0u;
    }
    {
        size_t i;
        size_t found = input->pad_count;
        for (i = 0; i < input->pad_count; ++i) {
            if (input->pad_ids[i] == device) {
                found = i;
                break;
            }
        }
        if (found < input->pad_count) {
            /* Drop the old entry; a reconnect moves the pad to the end. */
            memmove(&input->pad_ids[found], &input->pad_ids[found + 1u],
                    (input->pad_count - found - 1u) * sizeof(input->pad_ids[0]));
            memmove(&input->pad_families[found], &input->pad_families[found + 1u],
                    (input->pad_count - found - 1u) * sizeof(input->pad_families[0]));
            --input->pad_count;
        }
        if (connected && device != NK_UI_DEVICE_KEYBOARD_MOUSE) {
            if (input->pad_count == NK_UI_INPUT_MAX_PADS) {
                memmove(&input->pad_ids[0], &input->pad_ids[1],
                        (input->pad_count - 1u) * sizeof(input->pad_ids[0]));
                memmove(&input->pad_families[0], &input->pad_families[1],
                        (input->pad_count - 1u) * sizeof(input->pad_families[0]));
                --input->pad_count;
            }
            input->pad_ids[input->pad_count] = device;
            input->pad_families[input->pad_count] = family;
            ++input->pad_count;
        }
    }
    if (connected) {
        input->have_device = true;
        input->last_device = device;
        input->last_family = family;
        input->touch = false;
    } else if (input->have_device && input->last_device == device) {
        input->have_device = false;
    }
    event = nk_ui_input_make_event(NK_UI_EV_DEVICE, device, family);
    event.down = connected;
    return nk_ui_input_emit(out, cap, 0u, event);
}

size_t nk_ui_input_poll(NkUiInput *input, double now_s, NkUiEvent *out, size_t cap) {
    size_t used = 0u;
    double step;
    unsigned fired = 0u;
    if (input == NULL || !isfinite(now_s) || !input->repeating) {
        return 0u;
    }
    step = (double)nk_ui_repeat_interval_ms(&input->config, input->rate) / 1000.0;
    while (input->repeating && now_s >= input->next_repeat_s &&
           fired < NK_UI_REPEAT_CATCH_UP && used < cap) {
        NkUiEvent event = nk_ui_input_make_event(NK_UI_EV_NAV, input->repeat_device,
                                                 input->repeat_family);
        event.dir = input->repeat_dir;
        event.down = true;
        event.repeat = true;
        used = nk_ui_input_emit(out, cap, used, event);
        input->next_repeat_s += step;
        ++fired;
    }
    if (input->repeating && now_s >= input->next_repeat_s && fired == NK_UI_REPEAT_CATCH_UP) {
        /* Far behind (a stall): drop the missed repeats instead of replaying them. */
        input->next_repeat_s = now_s + step;
    }
    return used;
}

bool nk_ui_input_next_wake(const NkUiInput *input, double *at_s) {
    if (input == NULL || !input->repeating) {
        return false;
    }
    if (at_s != NULL) {
        *at_s = input->next_repeat_s;
    }
    return true;
}

NkUiPromptStyle nk_ui_input_prompt_style(const NkUiInput *input) {
    if (input == NULL) {
        return NK_UI_PROMPT_KEYCAPS;
    }
    if (input->have_device) {
        return nk_ui_prompt_style_for_family(input->last_family);
    }
    if (input->pad_count > 0u) {
        return nk_ui_prompt_style_for_family(input->pad_families[input->pad_count - 1u]);
    }
    return NK_UI_PROMPT_KEYCAPS;
}

bool nk_ui_input_touch_active(const NkUiInput *input) {
    return input != NULL && input->touch;
}

bool nk_ui_input_pad_connected(const NkUiInput *input) {
    return input != NULL && input->pad_count > 0u;
}

NkUiPromptStyle nk_ui_input_pad_prompt_style(const NkUiInput *input) {
    if (input == NULL) {
        return NK_UI_PROMPT_NEUTRAL;
    }
    if (input->have_device && input->last_family != NK_UI_PAD_KEYBOARD) {
        return nk_ui_prompt_style_for_family(input->last_family);
    }
    if (input->pad_count > 0u) {
        NkUiPromptStyle style =
            nk_ui_prompt_style_for_family(input->pad_families[input->pad_count - 1u]);
        return style == NK_UI_PROMPT_KEYCAPS ? NK_UI_PROMPT_NEUTRAL : style;
    }
    return NK_UI_PROMPT_NEUTRAL;
}

NkUiPadFamily nk_ui_input_last_family(const NkUiInput *input) {
    return input != NULL && input->have_device ? input->last_family : NK_UI_PAD_NEUTRAL;
}

int nk_ui_input_last_device(const NkUiInput *input) {
    return input != NULL && input->have_device ? input->last_device
                                               : NK_UI_DEVICE_KEYBOARD_MOUSE;
}

uint32_t nk_ui_utf8_next(const char **cursor, const char *end) {
    const unsigned char *p;
    uint32_t lead;
    uint32_t value;
    unsigned extra;
    unsigned i;
    if (cursor == NULL || *cursor == NULL || *cursor >= end) {
        return 0u;
    }
    p = (const unsigned char *)*cursor;
    lead = p[0];
    if (lead < 0x80u) {
        *cursor += 1;
        return lead;
    }
    if (lead >= 0xC2u && lead <= 0xDFu) {
        extra = 1u; value = lead & 0x1Fu;
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
        extra = 2u; value = lead & 0x0Fu;
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
        extra = 3u; value = lead & 0x07u;
    } else {
        *cursor += 1;
        return 0xFFFDu;
    }
    if ((size_t)(end - *cursor) <= extra) {
        *cursor += 1;
        return 0xFFFDu;
    }
    for (i = 1; i <= extra; ++i) {
        if ((p[i] & 0xC0u) != 0x80u) {
            *cursor += 1;
            return 0xFFFDu;
        }
        value = (value << 6) | (p[i] & 0x3Fu);
    }
    if ((extra == 2u && (value < 0x800u || (value >= 0xD800u && value <= 0xDFFFu))) ||
        (extra == 3u && (value < 0x10000u || value > 0x10FFFFu))) {
        *cursor += 1;
        return 0xFFFDu;
    }
    *cursor += extra + 1u;
    return value;
}
