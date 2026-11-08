/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * anime4k_pass.h - the movie upscaler (#1273): Anime4K's CNN x2 upscaler (bloc97/Anime4K, MIT,
 * in third_party/anime4k), run as Vulkan compute passes on CPU-presented movie frames. Each stage
 * doubles the picture: a small convolutional network predicts the detail a bilinear doubling
 * loses, which suits the flat colours and dark lines of animated and pre-rendered movies. The
 * presenter then takes the result to the window's size with FSR 1 (fsr1_pass.h).
 *
 * Presentation only. The pass reads the shown image and writes private images; the decoded movie
 * frame, the guest's frame, guest VRAM and captures stay as they were. Off unless
 * SR_MOVIE_UPSCALE=anime4k asks for it.
 *
 * It loads every Vulkan function through the instance's vkGetInstanceProcAddr and calls nothing
 * of the runtime: anime4k-selftest links it with the C library alone.
 */
#ifndef SR_ANIME4K_PASS_H
#define SR_ANIME4K_PASS_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct Anime4kPass Anime4kPass;

/* The most stages a picture gets: 480x272 becomes 1920x1088. */
#define ANIME4K_MAX_STAGES 2

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t queue_family;
    /* Selftest seam: SPIR-V to use for one pass instead of the built-in shader (a mutant), or
     * NULL. The player never sets it. */
    int mutant_pass;
    const uint32_t *mutant_spirv;
    size_t mutant_spirv_bytes;
} Anime4kInit;

/* SR_MOVIE_UPSCALE: NULL, empty or "off" is 0, "fsr1" is 1, "anime4k" is 2, anything else -1. */
enum { MOVIE_UPSCALE_OFF = 0, MOVIE_UPSCALE_FSR1 = 1, MOVIE_UPSCALE_ANIME4K = 2 };
int anime4k_parse_movie_mode(const char *value);

/* How many 2x stages take a w-wide picture toward out_w: each runs while the picture is still
 * more than 1.2 times narrower than out_w (Anime4K's own rule), up to ANIME4K_MAX_STAGES. */
int anime4k_stages_for(int w, int h, int out_w, int out_h);

/* NULL, with the reason, when the queue has no compute or a Vulkan object cannot be made. */
Anime4kPass *anime4k_create(const Anime4kInit *init, char *error, size_t error_size);
/* Waits for the device to go idle, then frees everything. NULL is fine. */
void anime4k_destroy(Anime4kPass *p);

/* Records `stages` (1..ANIME4K_MAX_STAGES) doublings of `src` (w x h, in TRANSFER_SRC_OPTIMAL,
 * any 8-bit-per-channel colour format a blit converts) and returns the private result, (w << stages)
 * x (h << stages), RGBA16F, in TRANSFER_SRC_OPTIMAL. `src` is only read. A size change waits for
 * the device and rebuilds the images. VK_NULL_HANDLE when they cannot be made: a size whose
 * last stage is wider or taller than the device's maxImageDimension2D is refused at once, with
 * the images of the last good size left as they are (they may still be in flight), and a size
 * whose Vulkan images or memory fail is freed after the wait. Either failure is said once on
 * stderr and that size is not tried again, so a failed size costs no device wait on later
 * presents; the caller then shows `src` as it would without the pass. */
VkImage anime4k_record(Anime4kPass *p, VkCommandBuffer cmd, VkImage src, int w, int h, int stages);

/* How many times the pass has built its images for a new size (a device wait each time). */
unsigned anime4k_size_builds(const Anime4kPass *p);

#endif /* SR_ANIME4K_PASS_H */
