/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_smooth.h - Ace Mode's in-between frames (src/rt/ace.h, docs/ENHANCEMENT_CONTRACT.md section
 * 10): block-matched motion over two private copies of the presented frames, recorded into the
 * present's command buffer. Needs a queue with compute and nothing of the GE: it works on what
 * the presenter already holds, GPU target or CPU frame alike.
 *
 * Every image here is private. The in-between is shown by blitting it to the swapchain, so it
 * never reaches guest VRAM, a capture, or anything the game can read.
 *
 * It loads every Vulkan function through the instance's vkGetInstanceProcAddr and calls nothing
 * of the runtime: ace-smooth-selftest links it with the C library alone (invariant S1).
 */
#ifndef SR_ACE_SMOOTH_H
#define SR_ACE_SMOOTH_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "../ace.h"   /* AceSmoothAuxStats */

typedef struct AceBetween AceBetween;

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
} AceSmoothInit;

/* NULL, with the reason, when the queue has no compute or a Vulkan object cannot be made. */
AceBetween *ace_smooth_create(const AceSmoothInit *init, char *error, size_t error_size);
/* Waits for the device to go idle, then frees everything. NULL is fine. */
void ace_smooth_destroy(AceBetween *s);

/* The HUD mask is one bit per guest pixel of the 480x272 display, row-major, bit (i & 31) of word
 * i / 32: set where the frame's through-mode (2D) draws covered it. */
#define ACE_SMOOTH_HUD_W 480
#define ACE_SMOOTH_HUD_H 272
#define ACE_SMOOTH_HUD_WORDS ((ACE_SMOOTH_HUD_W * ACE_SMOOTH_HUD_H) / 32)

/* What the GE knows about a presented frame besides its pixels. Either part may be absent. */
typedef struct {
    /* ACE_SMOOTH_HUD_WORDS words, or NULL: no mask (the frame is matched as before). */
    const uint32_t *hud_bits;
    /* The frame's depth image (D16, or D32 under depth=32: ace_hud_depth_float), in depth_layout,
     * with the presented frame at its top-left at
     * the same scale; VK_NULL_HANDLE when the frame wrote no depth. It is only read, and is left
     * in depth_layout. It needs VK_IMAGE_USAGE_TRANSFER_SRC_BIT. */
    VkImage depth;
    VkImageLayout depth_layout;
    int depth_w, depth_h;
} AceSmoothAux;

/* Copies `src` (w x h, in TRANSFER_SRC_OPTIMAL) into the private frames. With `between`, and a
 * previous frame of the same size, also records the in-between of the two and returns it in
 * TRANSFER_SRC_OPTIMAL; otherwise returns VK_NULL_HANDLE. `src` is only read. A size change
 * waits for the device and starts over. */
VkImage ace_smooth_record(AceBetween *s, VkCommandBuffer cmd, VkImage src, int w, int h, int between);
/* The same, with the frame's HUD mask and depth (`aux` may be NULL, which is ace_smooth_record).
 * Masked pixels never steer the motion search and are never dragged by it; depth makes a vector
 * that lands on one surface in both frames fit better than one that crosses an edge. The mask
 * and depth are used only when both frames of the pair have them. */
VkImage ace_smooth_record_aux(AceBetween *s, VkCommandBuffer cmd, VkImage src, int w, int h, int between,
                              const AceSmoothAux *aux);
/* Records the transition of the newest private frame to TRANSFER_SRC_OPTIMAL and returns it,
 * for the present that shows the held frame. VK_NULL_HANDLE when there is none. */
VkImage ace_smooth_held(AceBetween *s, VkCommandBuffer cmd);
/* The same for the private frame before the newest one: the real frame recorded just before
 * the latest ace_smooth_record call, for a present that shows an owed frame (ace.h, ACE_SHOW_OWED).
 * VK_NULL_HANDLE when the pass does not have it (nothing recorded before, or the size changed). */
VkImage ace_smooth_previous(AceBetween *s, VkCommandBuffer cmd);
/* The present that showed the owed frame failed, so the frame the show pass recorded with it
 * never reached the screen: the frame recorded before it is the newest again, as if that record
 * had not been made, and there is no previous frame until the next record. Only for a record
 * made without an in-between (ACE_SHOW_OWED records none). */
void ace_smooth_unrecord(AceBetween *s);
/* The pair counts (pairs, pairs_mask, pairs_depth, depth_refused) of AceSmoothAuxStats; the
 * other fields are left as they are. */
void ace_smooth_aux_stats(const AceBetween *s, AceSmoothAuxStats *out);
/* The size of what ace_smooth_held returns. */
void ace_smooth_size(const AceBetween *s, int *w, int *h);

/* How far each in-between moved what changed, measured on the GPU right after it is made
 * (shaders/ace_measure.comp). The pass looks at a 480x272 grid of texels, whatever the frame's size,
 * at the same texel of the old frame, the new frame and the in-between. A difference is the largest
 * of the r, g and b differences, in 1/255. The last ACE_SMOOTH_MOTION_RING in-betweens keep theirs. */
#define ACE_SMOOTH_MOTION_RING 8
typedef struct {
    uint32_t moving;   /* grid pixels whose two real frames differ by more than 40/255 in a channel */
    uint32_t eq_old;   /* of those, within 6/255 of the old frame */
    uint32_t eq_new;   /* of the rest, within 6/255 of the new frame */
    uint32_t sum_old;  /* over moving pixels: the largest channel difference to the old frame, in 1/255 */
    uint32_t sum_new;  /* the same to the new frame */
    uint32_t novel;    /* pixels the real frames agree on (<= 40/255) that the in-between shows > 64/255 from both */
    uint32_t sampled;  /* grid pixels looked at: 480 * 272 */
} AceSmoothMotion;
/* The serial of the newest in-between made, counting from 1; 0 before the first. Every pair
 * ace_smooth_record_aux makes an in-between for takes the next serial, when it is recorded. */
uint64_t ace_smooth_between_serial(const AceBetween *s);
/* The measurement of in-between `serial`. The caller decides when: only after the command buffer
 * that made it has finished on the GPU. Returns 0 for serial 0, a serial not made yet, or one
 * older than the ring keeps (serial + ACE_SMOOTH_MOTION_RING <= newest). It also returns 0 when the
 * command buffer that made it never ran: the command buffer writes the serial into the record after
 * the measure, and the record must carry it. Returns 1 and fills `out` otherwise. */
int ace_smooth_motion(const AceBetween *s, uint64_t serial, AceSmoothMotion *out);

#endif /* SR_ACE_SMOOTH_H */
