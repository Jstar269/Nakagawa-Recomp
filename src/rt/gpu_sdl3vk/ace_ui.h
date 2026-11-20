/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_ui.h - Ace Mode's in-game UI (src/rt/ace.h): the launch notice, the speed readout (with the
 * slowed-logic warning under it) and the crisp on-screen keyboard, drawn at the window's resolution over the frame the presenter
 * blitted onto the swapchain image, through nk_ui's Vulkan backend (src/ui/backend/nk_ui_vk.c).
 *
 * Its pixels exist only on the swapchain image: no guest-visible byte, guest frame or capture
 * changes. An Ace run makes one; an authentic run makes one only when the render notice is due
 * (host_notice below; docs/ENHANCEMENT_CONTRACT.md, section 10.24) or the run applies cheats
 * (cheats_marker below), so an authentic run with nothing to say registers no UI pass and draws
 * exactly what it drew before.
 *
 * It loads every Vulkan function through the instance's vkGetInstanceProcAddr and calls nothing
 * of the runtime: ace-ui-selftest's first link has it with nk_ui, its Vulkan backend and the C
 * library alone (invariant S1).
 */
#ifndef SR_ACE_UI_H
#define SR_ACE_UI_H

#include "nk_ui.h"
#include "../osk_overlay.h"

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct AceUi AceUi;

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t frames_in_flight;           /* the presenter's frame slots */
    const NkUiTextBackend *text;         /* the UI fonts; must outlive the AceUi */
} AceUiInit;

/* What one present shows. NULL or empty strings show nothing. */
typedef struct {
    int pixel_w, pixel_h;                /* the swapchain image */
    float display_scale;                 /* the window's display scale (1.0 at 100%) */
    double now_s;
    int reduce_motion;                   /* the player's choice: the notice cuts */
    int high_contrast;                   /* paper on surface_high with a strong edge */
    const char *notice_head;             /* the mode: "Enhanced" or "Ace" (#2055); NULL is Enhanced */
    /* The active features in words, a list joined by " \xC2\xB7 " (ace.h ace_feature_words), or
     * NULL. A list too long for one line wraps between its items onto at most
     * ACE_UI_NOTICE_LINES lines, or stays on one while the keyboard is open; items past that are
     * counted on the last line ("and N more"), never cut, and the pause menu lists them all. */
    const char *notice;
    float notice_alpha;                  /* 0 to 1 */
    const char *readout;                 /* "100%  30 FPS", or NULL */
    int readout_warning;                 /* PC LIMITED or GPU LIMITED: in warning */
    /* The slowed 60 FPS logic warning ("60 FPS logic: 52/s - SLOWED", sdl3vk_logic_notice_text),
     * or NULL: a chip in the error colour at the top left, under the readout when it shows. The
     * presenter hands it to the pass with the HUD off, instead of copying its own box to the
     * same corner. */
    const char *logic_notice;
    /* The cheats marker ("Cheats on: 2", src/rt/cheats.h, #2639), in any mode, or NULL: a chip at
     * the top left, under the readout and the slowed-logic warning. It does not fade and cannot
     * be hidden: it shows for as long as the run applies cheats. */
    const char *cheats_marker;
    const OskOverlay *keyboard;          /* an open keyboard, or NULL */
    /* The render notice (gui.c, #2151), in any mode: a heading, what happened in words and how
     * to hide it, in one chip at the bottom left that does not fade. It steps aside while the
     * keyboard is open. NULL or empty words show nothing. */
    const char *host_notice_head;
    const char *host_notice;
    const char *host_notice_hint;
    /* A short-lived message from the local player process, in a top-right toast of a fixed
     * width with an accent edge: an icon slot at the left, then host_toast_head (the eyebrow, in
     * the accent), host_toast (an unlock's title in the strong style, else the message; two lines
     * at most, ending in an ellipsis), and host_toast_detail under it in the dim style ("25 points
     * Â· Softcore"; NULL for none). The slot shows host_toast_badge (the achievement's
     * badge picture, which must outlive the frame) or, without one, a trophy drawn from the UI's
     * shapes for an unlock (host_toast_unlock) and an exclamation mark for any other message. */
    const char *host_toast_head;
    const char *host_toast;
    const char *host_toast_detail;
    const struct NkUiImage *host_toast_badge;
    int host_toast_unlock;
    float host_toast_alpha;
} AceUiFrame;

/* The swapchain image a present draws into, in TRANSFER_DST_OPTIMAL before and after. */
typedef struct {
    VkCommandBuffer cmd;
    uint32_t frame_slot;                 /* [0, frames_in_flight), its fence already waited */
    VkFormat format;
    VkExtent2D extent;
    const VkImage *images;
    uint32_t image_count;
    uint32_t image_index;
    uint64_t swapchain_gen;              /* changes when the images do */
} AceUiTarget;

/* NULL, with the reason, when a Vulkan object cannot be made or the backend is not built in. */
AceUi *ace_ui_create(const AceUiInit *init, char *error, size_t error_size);
/* Waits for the queue to go idle, then frees everything. NULL is fine. */
void ace_ui_destroy(AceUi *ui);
/* Whether the frame has anything to draw. */
int ace_ui_frame_wants(const AceUiFrame *frame);
/* Records the frame into target->cmd: the backend's uploads, then one render pass that loads
 * the image and draws over it. Returns 1 when it drew, 0 when there was nothing to draw or it
 * could not (then nothing that draws was recorded). */
int ace_ui_record(AceUi *ui, const AceUiTarget *target, const AceUiFrame *frame);

/* The frame's viewport: its window at its display scale, with a smallest layout of
 * ACE_UI_MIN_LOGICAL_W x ACE_UI_MIN_LOGICAL_H, so in a small window (a 480 x 272 game window)
 * the keyboard is drawn smaller rather than cut off. */
#define ACE_UI_MIN_LOGICAL_W 640.0f
#define ACE_UI_MIN_LOGICAL_H 420.0f
void ace_ui_viewport(const AceUiFrame *frame, NkUiViewport *out);
/* CPU only, for tests and for ace_ui_record: the frame's layout in an open nk_ui frame whose
 * viewport is ace_ui_viewport's. */
void ace_ui_layout(NkUi *ui, const AceUiFrame *frame);

/* Element ids, in the overlay's family (nk_overlay.h) above its pages' range. */
#define ACE_UI_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_OVERLAY, 0x00A000u + (offset))
#define ACE_UI_ID_ROOT ACE_UI_ID(0x001u)
#define ACE_UI_ID_TOP ACE_UI_ID(0x002u)
#define ACE_UI_ID_READOUT ACE_UI_ID(0x003u)
#define ACE_UI_ID_READOUT_TEXT ACE_UI_ID(0x004u)
#define ACE_UI_ID_SPACER ACE_UI_ID(0x005u)
#define ACE_UI_ID_NOTICE ACE_UI_ID(0x006u)
#define ACE_UI_ID_NOTICE_HEAD ACE_UI_ID(0x007u)
#define ACE_UI_ID_TOP_LEFT ACE_UI_ID(0x008u)     /* the readout, the logic warning, the cheats marker */
#define ACE_UI_ID_LOGIC ACE_UI_ID(0x0E0u)
#define ACE_UI_ID_LOGIC_TEXT ACE_UI_ID(0x0E1u)
#define ACE_UI_ID_CHEATS ACE_UI_ID(0x0E4u)
#define ACE_UI_ID_CHEATS_TEXT ACE_UI_ID(0x0E5u)
#define ACE_UI_ID_NOTICE_TEXT ACE_UI_ID(0x040u)  /* + line, below ACE_UI_NOTICE_LINES */
#define ACE_UI_ID_FILL ACE_UI_ID(0x009u)
#define ACE_UI_ID_KEYBOARD ACE_UI_ID(0x010u)
#define ACE_UI_ID_KEYBOARD_DESC ACE_UI_ID(0x011u)
#define ACE_UI_ID_KEYBOARD_FIELD ACE_UI_ID(0x012u)
#define ACE_UI_ID_KEYBOARD_TEXT ACE_UI_ID(0x013u)
#define ACE_UI_ID_KEYBOARD_HINT ACE_UI_ID(0x014u)
#define ACE_UI_ID_KEYBOARD_GRID ACE_UI_ID(0x015u)
#define ACE_UI_ID_KEYBOARD_ROWS ACE_UI_ID(0x020u)  /* + row */
#define ACE_UI_ID_HOST_FILL ACE_UI_ID(0x0F0u)
#define ACE_UI_ID_HOST_ROW ACE_UI_ID(0x0F1u)
#define ACE_UI_ID_HOST_NOTICE ACE_UI_ID(0x0F2u)
#define ACE_UI_ID_HOST_NOTICE_HEAD ACE_UI_ID(0x0F3u)
#define ACE_UI_ID_HOST_NOTICE_TEXT ACE_UI_ID(0x0F4u)  /* + line, below ACE_UI_HOST_NOTICE_LINES */
#define ACE_UI_ID_HOST_NOTICE_HINT ACE_UI_ID(0x0F8u)
#define ACE_UI_ID_HOST_TOAST_ROW ACE_UI_ID(0x0F9u)
#define ACE_UI_ID_HOST_TOAST ACE_UI_ID(0x0FAu)
#define ACE_UI_ID_HOST_TOAST_HEAD ACE_UI_ID(0x0FBu)
#define ACE_UI_ID_HOST_TOAST_TEXT ACE_UI_ID(0x0FCu)  /* + line, below ACE_UI_HOST_TOAST_LINES */
#define ACE_UI_ID_HOST_TOAST_EDGE ACE_UI_ID(0x0D0u)  /* the accent edge */
#define ACE_UI_ID_HOST_TOAST_SLOT ACE_UI_ID(0x0D1u)  /* the icon slot */
#define ACE_UI_ID_HOST_TOAST_ICON ACE_UI_ID(0x0D2u)  /* its badge picture or drawn glyph */
#define ACE_UI_ID_HOST_TOAST_WORDS ACE_UI_ID(0x0D3u) /* the column of eyebrow, title and detail */
#define ACE_UI_ID_HOST_TOAST_DETAIL ACE_UI_ID(0x0D4u)
/* The toast's width when the window has room, and its icon slot's side, in layout px. */
#define ACE_UI_HOST_TOAST_WIDTH 400.0f
#define ACE_UI_HOST_TOAST_ICON 48.0f
/* The render notice's words wrap to at most this many lines; more ends in an ellipsis. */
#define ACE_UI_HOST_NOTICE_LINES 3
#define ACE_UI_HOST_TOAST_LINES 2
/* The launch notice's list wraps to at most this many lines: room for every part's words at the
 * smallest layout (ace-ui-selftest), and for the render notice under them. */
#define ACE_UI_NOTICE_LINES 11
#define ACE_UI_ID_KEYBOARD_KEYS ACE_UI_ID(0x100u)  /* + 2 * cell, + 1 its label */

#endif /* SR_ACE_UI_H */
