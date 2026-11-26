// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-11-01.
// See NOTICE.md for upstream lineage and modification provenance.

/*
 * sdl3vk's self-tests, kept out of sdl3vk.c so the renderer reads as the renderer.
 *
 * This is not a translation unit: sdl3vk.c includes it at the point where the code used to
 * sit, because the tests reach the renderer's file-local state (swapchain, frame ring, init
 * fault seams) directly. Build it only through sdl3vk.c. It holds the capture self-test
 * (issue #57), the input-profile and pause-input self-tests, the lifecycle self-test (#1377)
 * and the window-surface self-test (#360); the entry point is sdl3vk_capture_selftest(), run by
 * gpu_capture_selftest.c.
 */

#include "fsr1_pass.h"

/* ---- capture selftest (issue #57) ---------------------------------------------------- */

/* Scratch root for the files the capture selftest writes (see sdl3vk_capture_selftest). The
 * Makefile passes -DSR_SELFTEST_BUILD_ROOT=$(BUILD_ROOT); a bare compile keeps "build". */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif
#ifdef _WIN32
#define SDL3VK_TEST_CHDIR(path) _chdir(path)
#else
#define SDL3VK_TEST_CHDIR(path) chdir(path)
#endif

static int cap_test_pattern(int x, int y, int ch) {
    /* Deterministic per-channel pattern across the whole 8-bit range; every channel
     * varies on both axes, so row-pitch shears and channel-order mistakes change bytes. */
    switch (ch) {
    case 0: return (x * 7 + y * 3) & 0xff;
    case 1: return (x * 5 + y * 11) & 0xff;
    default: return (x * 13 + y * 17 + 41) & 0xff;
    }
}

/* Sets a variable that sdl3vk_init's getenv reads. On Windows that is the C runtime's copy:
 * _putenv_s updates it and the process environment, while SetEnvironmentVariableA alone changes
 * only the process environment, which getenv never sees again (#1377). */
static int cap_test_setenv(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

static void profile_test_setenv(const char *name, const char *value) {
#ifdef _WIN32
    if (value && value[0]) {
        _putenv_s(name, value);
    } else {
        _putenv_s(name, "");
    }
#else
    if (value && value[0]) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

/* A profile name that never exists in the scratch directory. NK_INPUT_PROFILE names it whenever a
 * cell wants the default bindings. nk_input_profile_resolve_path consults its fallbacks (the legacy
 * SR_ alias, then <config>/input_profile.json) only while NK_INPUT_PROFILE is empty, so a non-empty
 * name keeps a developer's own profile from reaching a cell. */
#define PROFILE_TEST_NONE "selftest_no_input_profile.json"

static void profile_test_use_defaults(void) {
    profile_test_setenv("NK_INPUT_PROFILE", PROFILE_TEST_NONE);
}

/* Rewrites the first `from` in a saved profile fixture to `to`: a refused value the serializer
 * never writes. 0 when the file cannot be read or written whole or `from` is not in it. */
static int profile_test_rewrite(const char *path, const char *from, const char *to) {
    static char buf[256 * 1024];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1u, f);
    const int whole = feof(f) && !ferror(f);
    fclose(f);
    if (!whole) return 0;
    buf[n] = '\0';
    char *at = strstr(buf, from);
    if (!at) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    const size_t head = (size_t)(at - buf), tail = n - head - strlen(from);
    int ok = fwrite(buf, 1, head, f) == head && fputs(to, f) >= 0 &&
             fwrite(at + strlen(from), 1, tail, f) == tail;
    ok &= fclose(f) == 0;
    return ok;
}

static int cap_test_rmdir(const char *path) {
#ifdef _WIN32
    return _rmdir(path) == 0;
#else
    return rmdir(path) == 0;
#endif
}

/* Byte-exact P6 verification: exact header, exact payload, no trailing bytes. */
static int cap_test_verify_ppm(const char *path, int w, int h) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char hdr[64];
    int ok = 0;
    {
        char expect[64];
        snprintf(expect, sizeof expect, "P6\n%d %d\n255\n", w, h);
        size_t hl = strlen(expect);
        size_t got = fread(hdr, 1, hl, f);
        hdr[got] = '\0';
        if (got != hl || memcmp(hdr, expect, hl) != 0) {
            fprintf(stderr, "verify: header mismatch (got %zu bytes)\n", got);
            for (size_t i = 0; i < got; i++)
                fprintf(stderr, "  hdr byte %02u: %02x\n", (unsigned)i, (unsigned char)hdr[i]);
            fclose(f);
            return 0;
        }
            size_t body = (size_t)w * (size_t)h * 3u;
            uint8_t *buf = (uint8_t *)malloc(body);
            if (buf) {
                size_t got = fread(buf, 1, body, f);
                int extra = fgetc(f);
                if (got == body && extra == EOF) {
                    ok = 1;
                    for (int y = 0; y < h && ok; y++)
                        for (int x = 0; x < w && ok; x++) {
                            const uint8_t *px = buf + ((size_t)y * (size_t)w + x) * 3u;
                            if (px[0] != (uint8_t)cap_test_pattern(x, y, 0) ||
                                px[1] != (uint8_t)cap_test_pattern(x, y, 1) ||
                                px[2] != (uint8_t)cap_test_pattern(x, y, 2)) {
                                fprintf(stderr, "verify: first mismatch at (%d,%d) got "
                                        "%02x%02x%02x want %02x%02x%02x\n",
                                        x, y, px[0], px[1], px[2],
                                        cap_test_pattern(x, y, 0), cap_test_pattern(x, y, 1),
                                        cap_test_pattern(x, y, 2));
                                ok = 0;
                            }
                        }
                } else {
                    fprintf(stderr, "verify: body got %zu want %zu, extra=%d\n",
                            got, body, extra);
                }
                free(buf);
            }
    }
    fclose(f);
    return ok;
}

static void cap_test_fill_cpu(uint32_t *px, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[(size_t)y * (size_t)w + x] = 0xFF000000u |
                ((uint32_t)cap_test_pattern(x, y, 0) << 16) |
                ((uint32_t)cap_test_pattern(x, y, 1) << 8) |
                (uint32_t)cap_test_pattern(x, y, 2);
}

typedef struct CapTestImage {
    VkImage img;
    VkDeviceMemory mem;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    void *staging_map;
    int w, h;
} CapTestImage;

static void cap_test_image_destroy(CapTestImage *t) {
    if (t->staging_map) vkUnmapMemory(s_dev, t->staging_mem);
    if (t->staging)     vkDestroyBuffer(s_dev, t->staging, NULL);
    if (t->staging_mem) vkFreeMemory(s_dev, t->staging_mem, NULL);
    if (t->img)         vkDestroyImage(s_dev, t->img, NULL);
    if (t->mem)         vkFreeMemory(s_dev, t->mem, NULL);
    memset(t, 0, sizeof *t);
}

/* An R8G8B8A8 image in the exact shape ge_gpu.c presents: TRANSFER_DST|TRANSFER_SRC. */
static int cap_test_image_create(CapTestImage *t, int w, int h) {
    memset(t, 0, sizeof *t);
    t->w = w; t->h = h;
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imi.imageType = VK_IMAGE_TYPE_2D;
    imi.format = VK_FORMAT_R8G8B8A8_UNORM;
    imi.extent.width = (uint32_t)w; imi.extent.height = (uint32_t)h; imi.extent.depth = 1;
    imi.mipLevels = 1; imi.arrayLayers = 1; imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_dev, &imi, NULL, &t->img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(s_dev, t->img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(s_dev, &mai, NULL, &t->mem) != VK_SUCCESS ||
        vkBindImageMemory(s_dev, t->img, t->mem, 0) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)((size_t)w * (size_t)h * 4u);
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &t->staging) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    vkGetBufferMemoryRequirements(s_dev, t->staging, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(s_dev, &mai, NULL, &t->staging_mem) != VK_SUCCESS ||
        vkBindBufferMemory(s_dev, t->staging, t->staging_mem, 0) != VK_SUCCESS ||
        vkMapMemory(s_dev, t->staging_mem, 0, VK_WHOLE_SIZE, 0, &t->staging_map) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    return 1;
}

static int cap_test_image_submit(CapTestImage *t);

/* Upload the pattern and leave the image TRANSFER_SRC_OPTIMAL, exactly the contract
 * sdl3vk_present_image(_ex) requires of GE targets. */
static int cap_test_image_upload(CapTestImage *t) {
    uint8_t *dst = (uint8_t *)t->staging_map;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            uint8_t *px = dst + ((size_t)y * (size_t)t->w + x) * 4u;
            px[0] = (uint8_t)cap_test_pattern(x, y, 0);
            px[1] = (uint8_t)cap_test_pattern(x, y, 1);
            px[2] = (uint8_t)cap_test_pattern(x, y, 2);
            px[3] = 0xFF;
        }
    return cap_test_image_submit(t);
}

/* A dark frame for the picture probe: every colour byte 0 except the last pixel's first
 * colour byte, `last`, so the probe's brightest byte is `last` and only a scan of the whole
 * frame finds it. The ignored byte is 0xFF everywhere and never counts. */
static int cap_test_image_upload_dark(CapTestImage *t, uint8_t last) {
    uint8_t *dst = (uint8_t *)t->staging_map;
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; i++) {
        dst[i * 4u + 0u] = 0u;
        dst[i * 4u + 1u] = 0u;
        dst[i * 4u + 2u] = 0u;
        dst[i * 4u + 3u] = 0xFFu;
    }
    dst[(n - 1u) * 4u] = last;
    return cap_test_image_submit(t);
}

static int cap_test_image_submit(CapTestImage *t) {
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s_cmd, &bi) != VK_SUCCESS) return 0;
    barrier(s_cmd, t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = (uint32_t)t->w; bic.imageExtent.height = (uint32_t)t->h;
    bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, t->staging, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    barrier(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (vkEndCommandBuffer(s_cmd) != VK_SUCCESS) return 0;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cmd;
    uint64_t perf_submit_started = sr_perf_now_ns();
    if (vkQueueSubmit(s_queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return 0;
    if (perf_submit_started) sr_perf_vulkan_submit(perf_submit_started);
    uint64_t perf_wait_started = sr_perf_now_ns();
    if (vkQueueWaitIdle(s_queue) != VK_SUCCESS) return 0;
    if (perf_wait_started) sr_perf_vulkan_wait(perf_wait_started, 0);
    return 1;
}

/* Its fixture names are relative: sdl3vk_capture_selftest runs this from the scratch working
 * directory beneath SR_SELFTEST_BUILD_ROOT, so the files never land in the checkout. */
int sdl3vk_input_profile_selftest(void) {
    int ok = 1;

    /* ---- 5(a) No profile: sampled PSP buttons and analog bytes equal old hard-coded mapping ---- */
    {
        static const struct {
            NkHostGamepadButton host_btn;
            uint32_t expected_bit;
        } kButtonTable[] = {
            { NK_HOST_BUTTON_SOUTH,          0x4000 }, /* CROSS */
            { NK_HOST_BUTTON_EAST,           0x2000 }, /* CIRCLE */
            { NK_HOST_BUTTON_WEST,           0x8000 }, /* SQUARE */
            { NK_HOST_BUTTON_NORTH,          0x1000 }, /* TRIANGLE */
            { NK_HOST_BUTTON_START,          0x0008 }, /* START */
            { NK_HOST_BUTTON_BACK,           0x0001 }, /* SELECT */
            { NK_HOST_BUTTON_LEFT_SHOULDER,  0x0100 }, /* LTRIGGER */
            { NK_HOST_BUTTON_RIGHT_SHOULDER, 0x0200 }, /* RTRIGGER */
            { NK_HOST_BUTTON_DPAD_UP,        0x0010 }, /* UP */
            { NK_HOST_BUTTON_DPAD_DOWN,      0x0040 }, /* DOWN */
            { NK_HOST_BUTTON_DPAD_LEFT,      0x0080 }, /* LEFT */
            { NK_HOST_BUTTON_DPAD_RIGHT,     0x0020 }, /* RIGHT */
        };

        for (size_t i = 0; i < sizeof(kButtonTable) / sizeof(kButtonTable[0]); i++) {
            bool btns[NK_HOST_BUTTON_COUNT] = {false};
            int16_t axes[NK_HOST_AXIS_COUNT] = {0};
            btns[kButtonTable[i].host_btn] = true;
            uint32_t out_b = 0;
            uint8_t out_lx = 0, out_ly = 0;
            sdl3vk_map_gamepad(NULL, btns, axes, &out_b, &out_lx, &out_ly);
            if (out_b != kButtonTable[i].expected_bit) {
                fprintf(stderr, "input_profile_selftest: button %d failed: got 0x%04x expected 0x%04x\n",
                        kButtonTable[i].host_btn, out_b, kButtonTable[i].expected_bit);
                ok = 0;
            }
            if (out_lx != 128 || out_ly != 128) {
                fprintf(stderr, "input_profile_selftest: neutral analog failed on button press\n");
                ok = 0;
            }
        }

        static const NkHostGamepadButton kUnmapped[] = {
            NK_HOST_BUTTON_GUIDE, NK_HOST_BUTTON_LEFT_STICK, NK_HOST_BUTTON_RIGHT_STICK,
            NK_HOST_BUTTON_MISC1, NK_HOST_BUTTON_RIGHT_PADDLE1, NK_HOST_BUTTON_LEFT_PADDLE1,
            NK_HOST_BUTTON_RIGHT_PADDLE2, NK_HOST_BUTTON_LEFT_PADDLE2, NK_HOST_BUTTON_TOUCHPAD
        };
        for (size_t i = 0; i < sizeof(kUnmapped) / sizeof(kUnmapped[0]); i++) {
            bool btns[NK_HOST_BUTTON_COUNT] = {false};
            int16_t axes[NK_HOST_AXIS_COUNT] = {0};
            btns[kUnmapped[i]] = true;
            uint32_t out_b = 0;
            sdl3vk_map_gamepad(NULL, btns, axes, &out_b, NULL, NULL);
            if (out_b != 0) {
                fprintf(stderr, "input_profile_selftest: unmapped button %d produced 0x%04x\n",
                        kUnmapped[i], out_b);
                ok = 0;
            }
        }

        int16_t trig_samples[] = {-32768, 0, 8191, 8192, 8193, 16384, 32767};
        for (size_t i = 0; i < sizeof(trig_samples) / sizeof(trig_samples[0]); i++) {
            bool btns[NK_HOST_BUTTON_COUNT] = {false};
            int16_t axes[NK_HOST_AXIS_COUNT] = {0};
            axes[NK_HOST_AXIS_LEFT_TRIGGER] = trig_samples[i];
            axes[NK_HOST_AXIS_RIGHT_TRIGGER] = trig_samples[i];
            uint32_t out_b = 0;
            sdl3vk_map_gamepad(NULL, btns, axes, &out_b, NULL, NULL);
            uint32_t expected = 0;
            if (trig_samples[i] > 8192) expected |= (0x0100 | 0x0200);
            if (out_b != expected) {
                fprintf(stderr, "input_profile_selftest: trigger sample %d failed: got 0x%04x expected 0x%04x\n",
                        trig_samples[i], out_b, expected);
                ok = 0;
            }
        }

        static const int16_t axis_samples[] = {
            -32768, -32767, -20000, -10000, -7850, -7849, -7848,
            -100, 0, 100, 7848, 7849, 7850, 10000, 20000, 32767
        };
        for (size_t i = 0; i < sizeof(axis_samples) / sizeof(axis_samples[0]); i++) {
            for (size_t j = 0; j < sizeof(axis_samples) / sizeof(axis_samples[0]); j++) {
                int ax = axis_samples[i];
                int ay = axis_samples[j];
                /* Outside the dead zone, symmetric rounding around 128 (nk_input_profile.h),
                   computed here in floating point: 128 +- round(|v| * 127 / 32767), and 0 for
                   -32768. */
                uint8_t old_lx = 128, old_ly = 128;
                if (ax < -7849 || ax > 7849) {
                    const int k = (int)((ax < 0 ? -ax : ax) * 127.0 / 32767.0 + 0.5);
                    old_lx = (uint8_t)(ax == -32768 ? 0 : ax < 0 ? 128 - k : 128 + k);
                }
                if (ay < -7849 || ay > 7849) {
                    const int k = (int)((ay < 0 ? -ay : ay) * 127.0 / 32767.0 + 0.5);
                    old_ly = (uint8_t)(ay == -32768 ? 0 : ay < 0 ? 128 - k : 128 + k);
                }

                bool btns[NK_HOST_BUTTON_COUNT] = {false};
                int16_t axes[NK_HOST_AXIS_COUNT] = {0};
                axes[NK_HOST_AXIS_LEFTX] = (int16_t)ax;
                axes[NK_HOST_AXIS_LEFTY] = (int16_t)ay;
                uint8_t map_lx = 0, map_ly = 0;
                sdl3vk_map_gamepad(NULL, btns, axes, NULL, &map_lx, &map_ly);
                if (map_lx != old_lx || map_ly != old_ly) {
                    fprintf(stderr, "input_profile_selftest: axis ax=%d ay=%d mismatch: got (%u,%u) expected (%u,%u)\n",
                            ax, ay, map_lx, map_ly, old_lx, old_ly);
                    ok = 0;
                }
            }
        }
    }

    /* ---- 5(b) Custom profile: remaps button, deadzone, inversion ---- */
    {
        NkInputProfile custom;
        nk_input_profile_init_default(&custom);
        custom.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_NORTH;
        custom.psp_buttons[NK_PSP_BTN_TRIANGLE].primary.index = NK_HOST_BUTTON_SOUTH;
        custom.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner = 15000;
        custom.axes[NK_PSP_AXIS_ANALOG_X].inverted = true;

        bool btns[NK_HOST_BUTTON_COUNT] = {false};
        int16_t axes[NK_HOST_AXIS_COUNT] = {0};
        btns[NK_HOST_BUTTON_SOUTH] = true;
        uint32_t out_b = 0;
        uint8_t out_lx = 0, out_ly = 0;
        sdl3vk_map_gamepad(&custom, btns, axes, &out_b, &out_lx, &out_ly);
        if (out_b != 0x1000) {
            fprintf(stderr, "input_profile_selftest: custom remap failed: got 0x%04x expected 0x1000\n", out_b);
            ok = 0;
        }

        axes[NK_HOST_AXIS_LEFTX] = 10000;
        sdl3vk_map_gamepad(&custom, btns, axes, NULL, &out_lx, &out_ly);
        if (out_lx != 128) {
            fprintf(stderr, "input_profile_selftest: custom deadzone failed: got %u expected 128\n", out_lx);
            ok = 0;
        }

        axes[NK_HOST_AXIS_LEFTX] = 20000;
        sdl3vk_map_gamepad(&custom, btns, axes, NULL, &out_lx, &out_ly);
        uint8_t expected_inv = nk_input_profile_transform_axis(20000, 15000, 0, true);
        if (out_lx != expected_inv || out_lx >= 128) {
            fprintf(stderr, "input_profile_selftest: custom inverted axis failed: got %u expected %u (< 128)\n",
                    out_lx, expected_inv);
            ok = 0;
        }
    }

    /* ---- 5(c) A refused profile falls back to defaults with its diagnostic; missing file fallback ----
     * Each fixture is a saved profile whose remap of cross shows when it is in force, and which
     * loads as saved; it is then rewritten to one value the loader refuses (the serializer
     * writes neither): a future schema_version, and an axis deadzone_inner below 0. */
    {
        const char *tmp_invalid = "selftest_invalid_profile.json";
        static const struct {
            const char *name, *key;
            int deadzone;
        } kRefused[] = {
            { "future schema_version", "schema_version", NK_INPUT_DEFAULT_DEADZONE_INNER },
            { "negative axis deadzone_inner", "deadzone_inner", 12345 },
        };
        for (size_t i = 0; i < sizeof(kRefused) / sizeof(kRefused[0]); i++) {
            NkInputProfile custom;
            nk_input_profile_init_default(&custom);
            custom.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_NORTH;
            custom.psp_buttons[NK_PSP_BTN_TRIANGLE].primary.index = NK_HOST_BUTTON_SOUTH;
            custom.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner = (int16_t)kRefused[i].deadzone;
            char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
            if (nk_input_profile_save(&custom, tmp_invalid, diag, sizeof(diag)) != NK_OK) {
                fprintf(stderr, "input_profile_selftest: %s: could not write %s: %s\n",
                        kRefused[i].name, tmp_invalid, diag);
                ok = 0;
                continue;
            }
            profile_test_setenv("NK_INPUT_PROFILE", tmp_invalid);
            sdl3vk_reset_input_profile();
            sdl3vk_init_input_profile();
            const NkInputProfile *prof = sdl3vk_get_input_profile();
            if (prof->psp_buttons[NK_PSP_BTN_CROSS].primary.index != NK_HOST_BUTTON_NORTH ||
                prof->axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner != kRefused[i].deadzone) {
                fprintf(stderr, "input_profile_selftest: %s: the fixture as saved is not the profile "
                                "in force\n", kRefused[i].name);
                ok = 0;
            }
            char from[64], to[64];
            if (i == 0) {
                snprintf(from, sizeof from, "\"schema_version\": %d,", NK_INPUT_PROFILE_SCHEMA_VERSION);
                snprintf(to, sizeof to, "\"schema_version\": 999,");
            } else {
                snprintf(from, sizeof from, "\"deadzone_inner\": %d,", kRefused[i].deadzone);
                snprintf(to, sizeof to, "\"deadzone_inner\": -100,");
            }
            if (!profile_test_rewrite(tmp_invalid, from, to)) {
                fprintf(stderr, "input_profile_selftest: %s: could not rewrite %s\n",
                        kRefused[i].name, tmp_invalid);
                ok = 0;
                remove(tmp_invalid);
                continue;
            }
            NkInputProfile loaded;
            const NkResult res = nk_input_profile_load(&loaded, tmp_invalid, diag, sizeof(diag));
            if (res == NK_OK || !strstr(diag, kRefused[i].key)) {
                fprintf(stderr, "input_profile_selftest: %s: load gave %d with diagnostic '%s' (want a "
                                "refusal naming %s)\n", kRefused[i].name, (int)res, diag,
                        kRefused[i].key);
                ok = 0;
            }
            sdl3vk_reset_input_profile();
            sdl3vk_init_input_profile();
            prof = sdl3vk_get_input_profile();
            if (prof->schema_version != NK_INPUT_PROFILE_SCHEMA_VERSION ||
                prof->psp_buttons[NK_PSP_BTN_CROSS].primary.index != NK_HOST_BUTTON_SOUTH ||
                prof->axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner != NK_INPUT_DEFAULT_DEADZONE_INNER) {
                fprintf(stderr, "input_profile_selftest: %s: the refused profile did not fall back to "
                                "defaults\n", kRefused[i].name);
                ok = 0;
            }
            remove(tmp_invalid);
        }
        const NkInputProfile *prof;

        profile_test_setenv("NK_INPUT_PROFILE", "selftest_nonexistent_profile_12345.json");
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        prof = sdl3vk_get_input_profile();
        if (!prof || prof->schema_version != NK_INPUT_PROFILE_SCHEMA_VERSION) {
            fprintf(stderr, "input_profile_selftest: missing profile file did not give defaults\n");
            ok = 0;
        }

        profile_test_use_defaults();
        sdl3vk_reset_input_profile();
    }

    /* ---- 5(d) SR_PADSCRIPT still wins ---- */
    {
        const char *tmp_custom = "selftest_custom_profile.json";
        NkInputProfile custom;
        nk_input_profile_init_default(&custom);
        custom.psp_buttons[NK_PSP_BTN_CROSS].primary.index = NK_HOST_BUTTON_NORTH;
        custom.psp_buttons[NK_PSP_BTN_TRIANGLE].primary.index = NK_HOST_BUTTON_SOUTH;
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
        if (nk_input_profile_save(&custom, tmp_custom, diag, sizeof(diag)) != NK_OK) {
            fprintf(stderr, "input_profile_selftest: SR_PADSCRIPT profile save failed: %s\n", diag);
            ok = 0;
        }

        /* Without SR_PADSCRIPT the profile is in force: cross reads Triangle. */
        profile_test_setenv("NK_INPUT_PROFILE", tmp_custom);
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        const uint32_t south_profile = gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH);
        if (south_profile != 0x1000) {
            fprintf(stderr, "input_profile_selftest: the remapping profile is not in force: south gave "
                            "0x%04x expected 0x1000\n", south_profile);
            ok = 0;
        }

        profile_test_setenv("SR_PADSCRIPT", "synthetic_script_active");
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();

        uint32_t south_mapped = gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH);
        if (south_mapped != 0x4000) {
            fprintf(stderr, "input_profile_selftest: SR_PADSCRIPT active did not prioritize defaults: got 0x%04x expected 0x4000\n",
                    south_mapped);
            ok = 0;
        }

        profile_test_setenv("SR_PADSCRIPT", NULL);
        profile_test_use_defaults();
        remove(tmp_custom);
        sdl3vk_reset_input_profile();
    }

    /* ---- 5(e) Keyboard keys come from the profile (#1299) ---- */
    {
        /* The default profile reproduces the map hard-coded before schema 3, key name and
         * SDL scancode alike. */
        static const struct {
            SDL_Scancode sc;
            const char *name;
            uint32_t expected_bit;
        } kKeyTable[] = {
            { SDL_SCANCODE_RETURN, "return", 0x0008 }, /* START */
            { SDL_SCANCODE_LSHIFT, "lshift", 0x0001 }, /* SELECT */
            { SDL_SCANCODE_RSHIFT, "rshift", 0x0001 }, /* SELECT */
            { SDL_SCANCODE_X,      "x",      0x4000 }, /* CROSS */
            { SDL_SCANCODE_Z,      "z",      0x2000 }, /* CIRCLE */
            { SDL_SCANCODE_A,      "a",      0x8000 }, /* SQUARE */
            { SDL_SCANCODE_S,      "s",      0x1000 }, /* TRIANGLE */
            { SDL_SCANCODE_Q,      "q",      0x0100 }, /* LTRIGGER */
            { SDL_SCANCODE_W,      "w",      0x0200 }, /* RTRIGGER */
            { SDL_SCANCODE_UP,     "up",     0x0010 },
            { SDL_SCANCODE_DOWN,   "down",   0x0040 },
            { SDL_SCANCODE_LEFT,   "left",   0x0080 },
            { SDL_SCANCODE_RIGHT,  "right",  0x0020 },
        };
        for (size_t i = 0; i < sizeof(kKeyTable) / sizeof(kKeyTable[0]); i++) {
            bool keys[SDL_SCANCODE_COUNT] = {false};
            keys[kKeyTable[i].sc] = true;
            uint32_t held = sdl3vk_map_keyboard(NULL, keys, SDL_SCANCODE_COUNT);
            uint32_t pressed = sdl3vk_map_key(NULL, (int)kKeyTable[i].sc);
            if (held != kKeyTable[i].expected_bit || pressed != kKeyTable[i].expected_bit) {
                fprintf(stderr, "input_profile_selftest: default key %s: held 0x%04x pressed 0x%04x "
                        "expected 0x%04x\n", kKeyTable[i].name, held, pressed,
                        kKeyTable[i].expected_bit);
                ok = 0;
            }
            if (nk_host_key_from_name(kKeyTable[i].name) != (int)kKeyTable[i].sc) {
                fprintf(stderr, "input_profile_selftest: key name '%s' is not SDL scancode %d\n",
                        kKeyTable[i].name, (int)kKeyTable[i].sc);
                ok = 0;
            }
        }
        {
            bool keys[SDL_SCANCODE_COUNT] = {false};
            keys[SDL_SCANCODE_C] = true;
            if (sdl3vk_map_keyboard(NULL, keys, SDL_SCANCODE_COUNT) != 0 ||
                sdl3vk_map_key(NULL, SDL_SCANCODE_C) != 0) {
                fprintf(stderr, "input_profile_selftest: unbound default key C produced a button\n");
                ok = 0;
            }
            /* A key past the host's reported state array reads as released. */
            keys[SDL_SCANCODE_X] = true;
            if (sdl3vk_map_keyboard(NULL, keys, SDL_SCANCODE_X) != 0) {
                fprintf(stderr, "input_profile_selftest: key beyond key_count read as held\n");
                ok = 0;
            }
        }

        /* A saved profile that moves CROSS from X to C: the runtime loads it and the
         * production key paths set CROSS for C and nothing for X. */
        const char *tmp_keys = "selftest_keyboard_profile.json";
        NkInputProfile custom;
        nk_input_profile_init_default(&custom);
        custom.psp_keys[NK_PSP_BTN_CROSS].primary = nk_host_key_from_name("c");
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
        if (nk_input_profile_save(&custom, tmp_keys, diag, sizeof(diag)) != NK_OK) {
            fprintf(stderr, "input_profile_selftest: keyboard profile save failed: %s\n", diag);
            ok = 0;
        }
        profile_test_setenv("NK_INPUT_PROFILE", tmp_keys);
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        const NkInputProfile *prof = sdl3vk_get_input_profile();
        bool keys[SDL_SCANCODE_COUNT] = {false};
        keys[SDL_SCANCODE_C] = true;
        uint32_t held_c = sdl3vk_map_keyboard(prof, keys, SDL_SCANCODE_COUNT);
        keys[SDL_SCANCODE_C] = false;
        keys[SDL_SCANCODE_X] = true;
        uint32_t held_x = sdl3vk_map_keyboard(prof, keys, SDL_SCANCODE_COUNT);
        uint32_t pulse_c = keyboard_button(SDL_SCANCODE_C);
        uint32_t pulse_x = keyboard_button(SDL_SCANCODE_X);
        if (held_c != NK_PSP_BTN_CROSS_BIT || pulse_c != NK_PSP_BTN_CROSS_BIT ||
            held_x != 0 || pulse_x != 0) {
            fprintf(stderr, "input_profile_selftest: remapped key: C held 0x%04x pressed 0x%04x, "
                    "X held 0x%04x pressed 0x%04x; expected C -> 0x4000 and X -> 0\n",
                    held_c, pulse_c, held_x, pulse_x);
            ok = 0;
        }

        /* SR_PADSCRIPT keeps the default keys even with the remap on disk. */
        profile_test_setenv("SR_PADSCRIPT", "synthetic_script_active");
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        if (keyboard_button(SDL_SCANCODE_X) != NK_PSP_BTN_CROSS_BIT ||
            keyboard_button(SDL_SCANCODE_C) != 0) {
            fprintf(stderr, "input_profile_selftest: SR_PADSCRIPT active did not keep default keys\n");
            ok = 0;
        }

        profile_test_setenv("SR_PADSCRIPT", NULL);
        profile_test_use_defaults();
        remove(tmp_keys);
        sdl3vk_reset_input_profile();
    }

    return ok;
}

/* ---- pause input selftest (#1159); no Vulkan needed ------------------------------------
 * Drives the real event path (poll_input) with an SDL virtual gamepad and pushed key events:
 * Esc, F1 and Guide ask for the pause and never reach the game; while the overlay holds the
 * claim every event goes to it and the game reads a released pad; a button held when the
 * claim ends is not a fresh press until it is let go (P8); a stick pushed when the claim ends
 * reads centred until it returns; and losing the open pad asks for the pause. */

typedef struct {
    int events;
    int south_downs;
    int guide_downs;
    int escapes;
} PauseTestSink;

static void pause_test_sink(const void *event, void *user) {
    const SDL_Event *ev = (const SDL_Event *)event;
    PauseTestSink *sink = (PauseTestSink *)user;
    sink->events++;
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
        sink->south_downs++;
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
        sink->guide_downs++;
    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE)
        sink->escapes++;
}

static void pause_test_key(SDL_Keycode key, bool repeat) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = key;
    ev.key.down = true;
    ev.key.repeat = repeat;
    SDL_PushEvent(&ev);
}

/* SDL holds back a Guide release that comes within its minimum Guide press time (250 ms)
 * and drops a new Guide press until that release is out, so a test tap waits it out. */
static void pause_test_guide_release(SDL_Joystick *joy) {
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, false);
    SDL_Delay(300);
    int quit = 0;
    poll_input(&quit);
    sdl3vk_consume_button_pulses();
}

/* One host poll followed by one game sample, the order a VBLANK sees them in. */
static uint32_t pause_test_sample(void) {
    int quit = 0;
    poll_input(&quit);
    uint32_t seen = sdl3vk_buttons();
    sdl3vk_consume_button_pulses();
    return seen;
}

#define PAUSE_CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "pause_input_selftest: " __VA_ARGS__); fputc('\n', stderr); ok = 0; } } while (0)

int sdl3vk_pause_input_selftest(void) {
    int ok = 1;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "pause_input_selftest: SDL gamepad subsystem unavailable: %s\n", SDL_GetError());
        return 0;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1u;
    desc.name = "Nakagawa pause selftest pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick *joy = id ? SDL_OpenJoystick(id) : NULL;
    if (!joy) {
        fprintf(stderr, "pause_input_selftest: virtual gamepad unavailable: %s\n", SDL_GetError());
        if (id) SDL_DetachVirtualJoystick(id);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return 0;
    }
    /* Physical button i is SDL_GamepadButton i under this mask. */
    const uint32_t cross = gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH);
    const uint32_t circle = gamepad_button(SDL_GAMEPAD_BUTTON_EAST);
    PauseTestSink sink = {0};

    (void)pause_test_sample();
    PAUSE_CHECK(s_pad != NULL, "the virtual gamepad was not opened");
    PAUSE_CHECK(cross && circle && cross != circle, "default mapping lacks cross/circle");
    (void)sdl3vk_take_pause_request();
    (void)sdl3vk_take_pad_pulses();

    /* The game sees a press, and Guide is the pause button only. */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    PAUSE_CHECK(pause_test_sample() & cross, "cross press not seen by the game");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    (void)pause_test_sample();
    (void)sdl3vk_take_pad_pulses();
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, true);
    PAUSE_CHECK(pause_test_sample() == 0, "Guide reached the game");
    PAUSE_CHECK(sdl3vk_take_pad_pulses() == 0, "Guide reached the keyboard's pad pulses");
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_USER, "Guide did not ask for the pause");
    pause_test_guide_release(joy);

    /* A profile may bind Guide (HOME is the documented choice); Guide is still the pause
     * button and the binding never fires in game. */
    {
        const char *tmp_profile = "selftest_pause_guide_profile.json";
        NkInputProfile custom;
        nk_input_profile_init_default(&custom);
        custom.psp_buttons[NK_PSP_BTN_HOME].primary.type = NK_BINDING_HOST_BUTTON;
        custom.psp_buttons[NK_PSP_BTN_HOME].primary.index = NK_HOST_BUTTON_GUIDE;
        char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
        PAUSE_CHECK(nk_input_profile_save(&custom, tmp_profile, diag, sizeof(diag)) == NK_OK,
                    "profile save failed: %s", diag);
        profile_test_setenv("NK_INPUT_PROFILE", tmp_profile);
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
        const NkInputProfile *guide_prof = sdl3vk_get_input_profile();
        PAUSE_CHECK(guide_prof->psp_buttons[NK_PSP_BTN_HOME].primary.type == NK_BINDING_HOST_BUTTON &&
                    guide_prof->psp_buttons[NK_PSP_BTN_HOME].primary.index == NK_HOST_BUTTON_GUIDE,
                    "the Guide profile is not the one in force");
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, true);
        (void)pause_test_sample();
        PAUSE_CHECK((pause_test_sample() & NK_PSP_BTN_HOME_BIT) == 0, "a Guide binding reached the game");
        PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_USER,
                    "Guide bound in the profile did not ask for the pause");
        pause_test_guide_release(joy);
        profile_test_use_defaults();
        remove(tmp_profile);
        sdl3vk_reset_input_profile();
        sdl3vk_init_input_profile();
    }

    /* Esc and F1 ask; a held key's repeats do not ask again. */
    pause_test_key(SDLK_ESCAPE, false);
    (void)pause_test_sample();
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_USER, "Esc did not ask for the pause");
    PAUSE_CHECK(s_key_count == 0, "Esc reached the key queue");
    pause_test_key(SDLK_F1, false);
    (void)pause_test_sample();
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_USER, "F1 did not ask for the pause");
    pause_test_key(SDLK_ESCAPE, true);
    (void)pause_test_sample();
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_NONE, "a key repeat asked again");

    /* The overlay holds the claim: everything is its own. */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    (void)pause_test_sample();        /* cross held before the pause opens */
    sdl3vk_set_input_claim(pause_test_sink, &sink);
    PAUSE_CHECK(sdl3vk_input_claimed(), "the claim did not take");
    PAUSE_CHECK(sdl3vk_buttons() == 0, "a held button shows through the claim");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    (void)pause_test_sample();
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    PAUSE_CHECK(pause_test_sample() == 0, "a press reached the game through the claim");
    PAUSE_CHECK(sdl3vk_take_pad_pulses() == 0, "a press reached the pad pulses through the claim");
    PAUSE_CHECK(sink.south_downs == 1, "the overlay saw %d cross presses, expected 1", sink.south_downs);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, true);
    pause_test_key(SDLK_ESCAPE, false);
    (void)pause_test_sample();
    pause_test_guide_release(joy);
    PAUSE_CHECK(sink.guide_downs == 1 && sink.escapes == 1, "the overlay saw Guide %d times and Esc %d times, expected 1 and 1", sink.guide_downs, sink.escapes);
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_NONE, "a request latched under the claim");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
    (void)pause_test_sample();
    uint8_t lx = 0, ly = 0;
    sdl3vk_analog(&lx, &ly);
    PAUSE_CHECK(lx == 128 && ly == 128, "the stick reached the game through the claim");

    /* P8: cross is still held as the overlay closes. */
    sdl3vk_set_input_claim(NULL, NULL);
    PAUSE_CHECK(!sdl3vk_input_claimed(), "the claim did not end");
    PAUSE_CHECK((sdl3vk_buttons() & cross) == 0, "the held cross shows at release");
    for (int i = 0; i < 3; i++)
        PAUSE_CHECK((pause_test_sample() & cross) == 0, "the held cross became a press (sample %d)", i);
    sdl3vk_analog(&lx, &ly);
    PAUSE_CHECK(lx == 128, "the pushed stick shows before it returns");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, true);
    PAUSE_CHECK(pause_test_sample() & circle, "a button pressed after the release is not seen");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    PAUSE_CHECK((pause_test_sample() & (cross | circle)) == 0, "released buttons still show");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    PAUSE_CHECK(pause_test_sample() & cross, "cross pressed again after its release is not seen");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    (void)pause_test_sample();
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    (void)pause_test_sample();
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
    (void)pause_test_sample();
    sdl3vk_analog(&lx, &ly);
    PAUSE_CHECK(lx > 128, "the stick pushed again after centring is not seen");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    (void)pause_test_sample();
    (void)sdl3vk_take_pad_pulses();

    /* Losing the open pad asks for the pause, and a pad still connected takes over: it was
     * passed by when it arrived, since one was open then. */
    SDL_JoystickID id2 = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick *joy2 = id2 ? SDL_OpenJoystick(id2) : NULL;
    PAUSE_CHECK(joy2 != NULL, "a second virtual gamepad is unavailable: %s", SDL_GetError());
    (void)pause_test_sample();
    PAUSE_CHECK(s_pad && SDL_GetGamepadID(s_pad) == id, "a second pad arriving replaced the open one");
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    (void)pause_test_sample();
    PAUSE_CHECK(s_pad && SDL_GetGamepadID(s_pad) == id2, "the pad still connected was not opened");
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_PAD_LOST,
                "losing the pad in use, with another connected, did not ask for the pause");
    if (joy2) {
        SDL_SetJoystickVirtualButton(joy2, SDL_GAMEPAD_BUTTON_SOUTH, true);
        PAUSE_CHECK(pause_test_sample() & cross, "the pad that took over is not read");
        SDL_SetJoystickVirtualButton(joy2, SDL_GAMEPAD_BUTTON_SOUTH, false);
        (void)pause_test_sample();
        (void)sdl3vk_take_pad_pulses();
        SDL_CloseJoystick(joy2);
        SDL_DetachVirtualJoystick(id2);
        (void)pause_test_sample();
    }
    PAUSE_CHECK(s_pad == NULL, "the removed pad stayed open");
    PAUSE_CHECK(sdl3vk_take_pause_request() == SDL3VK_PAUSE_REQUEST_PAD_LOST, "losing the pad did not ask for the pause");

    if (s_pad) { SDL_CloseGamepad(s_pad); s_pad = NULL; }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    nk_input_gate_init(&s_gate);
    s_pause_request = SDL3VK_PAUSE_REQUEST_NONE;
    s_button_pulse = s_pad_pulse = 0;
    s_buttons = 0;
    s_lx = s_ly = 128;
    s_pad_present = 0;
    if (ok) printf("pause_input_selftest: OK\n");
    return ok;
}
#undef PAUSE_CHECK

/* ---- mouse aiming selftest (#1791); no Vulkan needed -----------------------------------
 * Drives the real event path (poll_input) with pushed mouse events: with the profile's aim
 * off the nub never moves; with it on, movement since the last poll deflects the nub and a
 * still mouse returns it to centre; a focus loss or an overlay claim throws the movement
 * away, so nothing a mouse did before is applied after. */
#define AIM_CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "mouse_aim_selftest: " __VA_ARGS__); fputc('\n', stderr); ok = 0; } } while (0)

static void aim_test_motion(float dx, float dy) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.xrel = dx;
    ev.motion.yrel = dy;
    SDL_PushEvent(&ev);
}

/* The clock and the pointer grab are the aim step's seams: the test sets the time between
 * polls and records what the window was asked to do. */
static uint64_t aim_test_now, aim_test_step;
static uint64_t aim_test_clock(void) { aim_test_now += aim_test_step; return aim_test_now; }
static int aim_test_grab_calls, aim_test_grab_last = -1;
static bool aim_test_grab_result = true;
static bool aim_test_grab(bool want) {
    aim_test_grab_calls++;
    aim_test_grab_last = want ? 1 : 0;
    return aim_test_grab_result;
}

static void aim_test_stick(uint8_t *lx, uint8_t *ly) {
    int quit = 0;
    poll_input(&quit);
    sdl3vk_analog(lx, ly);
}

static int sdl3vk_mouse_aim_selftest(void) {
    int ok = 1;
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        fprintf(stderr, "mouse_aim_selftest: SDL events unavailable: %s\n", SDL_GetError());
        return 0;
    }
    const char *tmp_profile = "selftest_mouse_aim_profile.json";
    char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
    uint8_t lx, ly;
    int quit = 0;
    NkInputProfile custom;
    s_aim_clock_ns = aim_test_clock;
    s_aim_relative_mode = aim_test_grab;
    aim_test_now = 1000000000ull;
    aim_test_step = AIM_REFERENCE_POLL_NS;
    aim_test_grab_calls = 0;
    aim_test_grab_last = -1;
    aim_test_grab_result = true;

    /* Off (the default): a moving mouse leaves the nub at centre. */
    sdl3vk_reset_input_profile();
    nk_input_profile_init_default(&custom);
    AIM_CHECK(nk_input_profile_save(&custom, tmp_profile, diag, sizeof(diag)) == NK_OK,
              "profile save failed: %s", diag);
    profile_test_setenv("NK_INPUT_PROFILE", tmp_profile);
    sdl3vk_reset_input_profile();
    sdl3vk_init_input_profile();
    poll_input(&quit);
    aim_test_motion(40.0f, 40.0f);
    aim_test_stick(&lx, &ly);
    AIM_CHECK(lx == 128 && ly == 128, "aim off: the mouse moved the nub to %u,%u", lx, ly);
    AIM_CHECK(aim_test_grab_calls == 0, "aim off: the pointer was grabbed (%d calls)", aim_test_grab_calls);

    /* On: 8 counts right and 16 up at 100% are half and full deflection. */
    custom.aim.mouse = true;
    AIM_CHECK(nk_input_profile_save(&custom, tmp_profile, diag, sizeof(diag)) == NK_OK,
              "profile save failed: %s", diag);
    sdl3vk_reset_input_profile();
    sdl3vk_init_input_profile();
    poll_input(&quit);
    aim_test_motion(8.0f, -16.0f);
    aim_test_stick(&lx, &ly);
    AIM_CHECK(lx == 128 + 63 && ly == 128 - 127, "aim on: nub %u,%u, expected %d,%d", lx, ly,
              128 + 63, 128 - 127);
    /* A fraction of a count is kept for the next poll instead of being lost. */
    aim_test_motion(0.5f, 0.0f);
    aim_test_stick(&lx, &ly);
    AIM_CHECK(lx == 128, "aim on: half a count moved the nub to %u", lx);
    aim_test_motion(0.5f, 0.0f);
    aim_test_stick(&lx, &ly);
    AIM_CHECK(lx == 128 + 7, "aim on: two half counts gave %u, expected %d", lx, 128 + 7);

    /* A focus loss in the same poll throws the movement away. */
    {
        SDL_Event lost;
        SDL_zero(lost);
        lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        aim_test_motion(16.0f, 16.0f);
        SDL_PushEvent(&lost);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 && ly == 128, "a focus loss left the nub at %u,%u", lx, ly);
    }

    /* While the overlay has the input the mouse is the overlay's, and nothing it did is
     * applied after the claim ends. */
    {
        PauseTestSink sink = {0};
        sdl3vk_set_input_claim(pause_test_sink, &sink);
        aim_test_motion(16.0f, 16.0f);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 && ly == 128, "a claimed mouse moved the nub to %u,%u", lx, ly);
        AIM_CHECK(aim_test_grab_last == 0 && !s_mouse_captured,
                  "the overlay holds the input but the pointer is still grabbed");
        sdl3vk_set_input_claim(NULL, NULL);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 && ly == 128, "the claim's mouse movement reached the game (%u,%u)",
                  lx, ly);
        AIM_CHECK(aim_test_grab_last == 1 && s_mouse_captured,
                  "the pointer was not grabbed again once the game had the input");
    }

    /* The grab is asked for once, not every poll, and a request that could not be applied
     * (no window) is not remembered as done: it is asked again until it works. */
    {
        int calls = aim_test_grab_calls;
        aim_test_stick(&lx, &ly);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(aim_test_grab_calls == calls, "a held grab was asked for again (%d calls)",
                  aim_test_grab_calls - calls);
        PauseTestSink sink = {0};
        sdl3vk_set_input_claim(pause_test_sink, &sink);
        aim_test_stick(&lx, &ly);
        sdl3vk_set_input_claim(NULL, NULL);
        aim_test_grab_result = false;
        aim_test_grab_calls = 0;
        aim_test_stick(&lx, &ly);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(!s_mouse_captured && aim_test_grab_calls == 2,
                  "a failed grab was latched as done (captured=%d, %d calls)", s_mouse_captured,
                  aim_test_grab_calls);
        aim_test_grab_result = true;
        aim_test_stick(&lx, &ly);
        AIM_CHECK(s_mouse_captured, "the grab was not retried after it could succeed");
        /* The real call with no window (headless) does nothing and latches nothing. */
        s_aim_relative_mode = aim_relative_mode_default;
        s_mouse_captured = false;
        aim_test_stick(&lx, &ly);
        AIM_CHECK(!s_mouse_captured, "a run with no window latched a grab");
        s_aim_relative_mode = aim_test_grab;
        aim_test_stick(&lx, &ly);
    }

    /* The nub follows the hand's speed, not the frame rate: the same movement over twice the
     * time is half the deflection, a frame hitch is not a saturated nub, polls faster than
     * 1/240 s count as 1/240 s, and a stall throws its movement away. */
    {
        aim_reset();
        aim_test_stick(&lx, &ly);                 /* first poll after a reset: one reference poll */
        aim_test_step = 2 * AIM_REFERENCE_POLL_NS;
        aim_test_motion(16.0f, 0.0f);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 + 63, "30 fps: 16 counts gave %u, expected %d", lx, 128 + 63);
        aim_test_step = 6 * AIM_REFERENCE_POLL_NS;
        aim_test_motion(16.0f, 0.0f);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 + 15, "a 100 ms hitch: 16 counts gave %u, expected %d", lx, 128 + 15);
        aim_test_step = 1000000ull;
        aim_test_motion(2.0f, 0.0f);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 + 63, "1 ms poll: 2 counts gave %u, expected %d", lx, 128 + 63);
        aim_test_step = AIM_STALL_NS + 1;
        aim_test_motion(40.0f, 40.0f);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 && ly == 128, "a stall's movement reached the game (%u,%u)", lx, ly);
        aim_test_step = AIM_REFERENCE_POLL_NS;
    }

    /* SR_PADSCRIPT drives the pad by script: aiming is off and the pointer is not grabbed,
     * and it comes back when the script ends. */
    {
        aim_reset();
        profile_test_setenv("SR_PADSCRIPT", "synthetic_script_active");
        aim_test_motion(16.0f, 16.0f);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 && ly == 128, "SR_PADSCRIPT: the mouse moved the nub to %u,%u", lx, ly);
        AIM_CHECK(aim_test_grab_last == 0 && !s_mouse_captured,
                  "SR_PADSCRIPT: the pointer is still grabbed");
        profile_test_setenv("SR_PADSCRIPT", NULL);
        aim_test_stick(&lx, &ly);
        aim_test_motion(16.0f, 0.0f);
        aim_test_stick(&lx, &ly);
        AIM_CHECK(lx == 128 + 127, "aim did not come back after SR_PADSCRIPT ended (%u)", lx);
    }

    /* One count is about 7 nub units at 100%, and no deadzone hides it. */
    aim_reset();
    aim_test_motion(1.0f, 0.0f);
    aim_test_stick(&lx, &ly);
    AIM_CHECK(lx == 128 + 7, "one count gave %u, expected %d", lx, 128 + 7);

    profile_test_use_defaults();
    remove(tmp_profile);
    sdl3vk_reset_input_profile();
    aim_reset();
    s_aim_clock_ns = aim_clock_ns_default;
    s_aim_relative_mode = aim_relative_mode_default;
    s_mouse_captured = false;
    nk_input_gate_init(&s_gate);
    s_button_pulse = s_pad_pulse = 0;
    s_buttons = 0;
    s_lx = s_ly = 128;
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    if (ok) printf("mouse_aim_selftest: OK\n");
    return ok;
}
#undef AIM_CHECK

/* ---- mouse buttons selftest (#1791); no Vulkan needed ----------------------------------
 * Drives the real event path (poll_input) with pushed mouse button events. Unbound (every
 * default mapping), a click changes nothing the game reads. Bound, a click presses its PSP
 * control while held and still reaches the game when it is shorter than one poll; a side
 * button pushes the stick; a focus loss lets go of every button; a click the overlay had is
 * never held for the game; and SR_PADSCRIPT keeps the default mapping. */
#define MBTN_CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "mouse_buttons_selftest: " __VA_ARGS__); fputc('\n', stderr); ok = 0; } } while (0)

static void mbtn_test_button(Uint8 button, bool down) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.button = button;
    ev.button.down = down;
    ev.button.clicks = 1;
    SDL_PushEvent(&ev);
}

/* One host poll and one game sample: the buttons and stick the game reads. */
static uint32_t mbtn_test_sample(uint8_t *lx, uint8_t *ly) {
    int quit = 0;
    poll_input(&quit);
    uint32_t seen = sdl3vk_buttons();
    sdl3vk_consume_button_pulses();
    sdl3vk_analog(lx, ly);
    return seen;
}

static int sdl3vk_mouse_buttons_selftest(void) {
    int ok = 1;
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        fprintf(stderr, "mouse_buttons_selftest: SDL events unavailable: %s\n", SDL_GetError());
        return 0;
    }
    const char *tmp_profile = "selftest_mouse_buttons_profile.json";
    char diag[NK_INPUT_DIAGNOSTIC_MAX_LEN] = {0};
    uint8_t lx, ly;
    uint32_t seen;
    NkInputProfile custom;
    s_aim_relative_mode = aim_test_grab;   /* aiming stays off; nothing may grab the pointer */
    aim_test_grab_calls = 0;
    s_mouse_held = 0;

    /* Unbound (the default mapping): with every button held, and with a click inside one poll,
     * the game reads exactly what it reads with no mouse at all. */
    nk_input_profile_init_default(&custom);
    MBTN_CHECK(nk_input_profile_save(&custom, tmp_profile, diag, sizeof(diag)) == NK_OK,
               "profile save failed: %s", diag);
    profile_test_setenv("NK_INPUT_PROFILE", tmp_profile);
    sdl3vk_reset_input_profile();
    sdl3vk_init_input_profile();
    {
        uint8_t base_lx, base_ly;
        const uint32_t base = mbtn_test_sample(&base_lx, &base_ly);
        for (Uint8 b = SDL_BUTTON_LEFT; b <= SDL_BUTTON_X2; b++) mbtn_test_button(b, true);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == base && lx == base_lx && ly == base_ly,
                   "unbound: held buttons changed the pad (0x%x %u,%u; without 0x%x %u,%u)",
                   seen, lx, ly, base, base_lx, base_ly);
        for (Uint8 b = SDL_BUTTON_LEFT; b <= SDL_BUTTON_X2; b++) {
            mbtn_test_button(b, false);
            mbtn_test_button(b, true);
            mbtn_test_button(b, false);
        }
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == base && lx == base_lx && ly == base_ly,
                   "unbound: clicks changed the pad (0x%x %u,%u)", seen, lx, ly);
        MBTN_CHECK(aim_test_grab_calls == 0, "a click grabbed the pointer");
    }

    /* Bound: the left button is Cross, the right one R beside its W key, the back side button
     * pushes the stick up. */
    custom.psp_keys[NK_PSP_BTN_CROSS].primary = NK_HOST_KEY_MOUSE_LEFT;
    custom.psp_keys[NK_PSP_BTN_RTRIGGER].secondary = NK_HOST_KEY_MOUSE_RIGHT;
    custom.stick_keys[NK_PSP_STICK_UP].secondary = NK_HOST_KEY_MOUSE_X1;
    MBTN_CHECK(nk_input_profile_save(&custom, tmp_profile, diag, sizeof(diag)) == NK_OK,
               "profile save failed: %s", diag);
    sdl3vk_reset_input_profile();
    sdl3vk_init_input_profile();
    (void)mbtn_test_sample(&lx, &ly);

    mbtn_test_button(SDL_BUTTON_LEFT, true);
    seen = mbtn_test_sample(&lx, &ly);
    MBTN_CHECK(seen == NK_PSP_BTN_CROSS_BIT, "left held: the game read 0x%x", seen);
    seen = mbtn_test_sample(&lx, &ly);
    MBTN_CHECK(seen == NK_PSP_BTN_CROSS_BIT, "left still held: the game read 0x%x", seen);
    mbtn_test_button(SDL_BUTTON_LEFT, false);
    seen = mbtn_test_sample(&lx, &ly);
    MBTN_CHECK(seen == 0, "left released: the game read 0x%x", seen);

    /* A click inside one poll is seen once, then gone. */
    mbtn_test_button(SDL_BUTTON_RIGHT, true);
    mbtn_test_button(SDL_BUTTON_RIGHT, false);
    seen = mbtn_test_sample(&lx, &ly);
    MBTN_CHECK(seen == NK_PSP_BTN_RTRIGGER_BIT, "a quick right click: the game read 0x%x", seen);
    seen = mbtn_test_sample(&lx, &ly);
    MBTN_CHECK(seen == 0, "the quick click stayed pressed (0x%x)", seen);

    /* A side button is a stick key: the stick goes to its edge and comes back. */
    mbtn_test_button(SDL_BUTTON_X1, true);
    seen = mbtn_test_sample(&lx, &ly);
    MBTN_CHECK(seen == 0 && lx == 128 && ly == 0, "back button held: 0x%x %u,%u", seen, lx, ly);
    mbtn_test_button(SDL_BUTTON_X1, false);
    (void)mbtn_test_sample(&lx, &ly);
    MBTN_CHECK(lx == 128 && ly == 128, "back button released: the stick stayed at %u,%u", lx, ly);

    /* A focus loss lets go of a held button: its release goes to another window. */
    {
        SDL_Event lost;
        SDL_zero(lost);
        lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        mbtn_test_button(SDL_BUTTON_LEFT, true);
        (void)mbtn_test_sample(&lx, &ly);
        SDL_PushEvent(&lost);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == 0 && s_mouse_held == 0, "a focus loss left the click held (0x%x)", seen);
    }

    /* Not mouse buttons: a button past the second side button, a tap or a pen that SDL
     * reports as the mouse, and a click while the on-screen keyboard has the input. */
    {
        SDL_Event tap;
        SDL_zero(tap);
        tap.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        tap.button.button = SDL_BUTTON_LEFT;
        tap.button.down = true;
        tap.button.which = SDL_TOUCH_MOUSEID;
        SDL_PushEvent(&tap);
        tap.button.which = SDL_PEN_MOUSEID;
        SDL_PushEvent(&tap);
        mbtn_test_button(SDL_BUTTON_X2 + 1, true);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == 0 && s_mouse_held == 0, "a tap, a pen or button 6 pressed 0x%x", seen);
        mbtn_test_button(SDL_BUTTON_X2 + 1, false);
        /* A tap or a pen lifting does not let go of a mouse button held down. */
        mbtn_test_button(SDL_BUTTON_LEFT, true);
        (void)mbtn_test_sample(&lx, &ly);
        tap.type = SDL_EVENT_MOUSE_BUTTON_UP;
        tap.button.down = false;
        tap.button.which = SDL_TOUCH_MOUSEID;
        SDL_PushEvent(&tap);
        tap.button.which = SDL_PEN_MOUSEID;
        SDL_PushEvent(&tap);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == NK_PSP_BTN_CROSS_BIT, "a tap or a pen lifting let go of left (0x%x)", seen);
        mbtn_test_button(SDL_BUTTON_LEFT, false);
        (void)mbtn_test_sample(&lx, &ly);
        sdl3vk_set_text_claim(1);
        mbtn_test_button(SDL_BUTTON_LEFT, true);
        seen = mbtn_test_sample(&lx, &ly);
        sdl3vk_set_text_claim(0);
        MBTN_CHECK(seen == 0 && s_mouse_held == 0, "the on-screen keyboard's click pressed 0x%x", seen);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == 0, "the on-screen keyboard's click was held for the game (0x%x)", seen);
        mbtn_test_button(SDL_BUTTON_LEFT, false);
        (void)mbtn_test_sample(&lx, &ly);
    }

    /* The keyboard reads what it read before mouse buttons joined its array: every key alone,
     * with a click bound and not held, maps as SDL's own 512-entry array maps. */
    {
        const NkInputProfile *prof = sdl3vk_get_input_profile();
        static bool sdl_keys[SDL_SCANCODE_COUNT];
        bool merged[NK_HOST_KEY_LIMIT];
        int differ = 0;
        for (int s = 0; s < SDL_SCANCODE_COUNT; s++) {
            memset(sdl_keys, 0, sizeof(sdl_keys));
            sdl_keys[s] = true;
            host_key_state(sdl_keys, SDL_SCANCODE_COUNT, 0u, merged);
            uint8_t a_lx = 128, a_ly = 128, b_lx = 128, b_ly = 128;
            (void)nk_input_profile_eval_key_analog(prof, sdl_keys, SDL_SCANCODE_COUNT, &a_lx, &a_ly);
            (void)nk_input_profile_eval_key_analog(prof, merged, NK_HOST_KEY_LIMIT, &b_lx, &b_ly);
            if (sdl3vk_map_keyboard(prof, sdl_keys, SDL_SCANCODE_COUNT) !=
                    sdl3vk_map_keyboard(prof, merged, NK_HOST_KEY_LIMIT) ||
                a_lx != b_lx || a_ly != b_ly) {
                differ++;
            }
        }
        MBTN_CHECK(prof != NULL && prof->psp_keys[NK_PSP_BTN_CROSS].primary == NK_HOST_KEY_MOUSE_LEFT,
                   "the bound profile is not the one in force");
        MBTN_CHECK(differ == 0, "%d keys map differently through the merged array", differ);
        host_key_state(NULL, 0, 1u << (NK_HOST_KEY_MOUSE_X1 - NK_HOST_KEY_MOUSE_FIRST), merged);
        MBTN_CHECK(merged[NK_HOST_KEY_MOUSE_X1] && !merged[NK_HOST_KEY_MOUSE_LEFT] && !merged[0],
                   "the held mask did not land on its own key");
    }

    /* A click the overlay had is the overlay's: pressed under the claim and still held after
     * it ends, it is not a press for the game. One held before the claim and let go under it
     * is released for the game too. */
    {
        PauseTestSink sink = {0};
        sdl3vk_set_input_claim(pause_test_sink, &sink);
        mbtn_test_button(SDL_BUTTON_LEFT, true);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == 0 && s_mouse_held == 0, "a claimed click reached the game (0x%x)", seen);
        sdl3vk_set_input_claim(NULL, NULL);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == 0, "the overlay's click was held for the game (0x%x)", seen);
        mbtn_test_button(SDL_BUTTON_LEFT, false);
        (void)mbtn_test_sample(&lx, &ly);

        mbtn_test_button(SDL_BUTTON_LEFT, true);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == NK_PSP_BTN_CROSS_BIT, "left before the claim: 0x%x", seen);
        sdl3vk_set_input_claim(pause_test_sink, &sink);
        mbtn_test_button(SDL_BUTTON_LEFT, false);
        (void)mbtn_test_sample(&lx, &ly);
        sdl3vk_set_input_claim(NULL, NULL);
        seen = mbtn_test_sample(&lx, &ly);
        MBTN_CHECK(seen == 0 && s_mouse_held == 0,
                   "a click let go under the claim stayed held (0x%x)", seen);
    }

    /* SR_PADSCRIPT drives the pad with the default mapping: a bound click does nothing. */
    profile_test_setenv("SR_PADSCRIPT", "synthetic_script_active");
    mbtn_test_button(SDL_BUTTON_LEFT, true);
    seen = mbtn_test_sample(&lx, &ly);
    MBTN_CHECK((seen & NK_PSP_BTN_CROSS_BIT) == 0, "SR_PADSCRIPT: a click pressed Cross (0x%x)", seen);
    mbtn_test_button(SDL_BUTTON_LEFT, false);
    (void)mbtn_test_sample(&lx, &ly);
    profile_test_setenv("SR_PADSCRIPT", NULL);

    profile_test_use_defaults();
    remove(tmp_profile);
    sdl3vk_reset_input_profile();
    s_aim_relative_mode = aim_relative_mode_default;
    s_mouse_captured = false;
    s_mouse_held = 0;
    nk_input_gate_init(&s_gate);
    s_button_pulse = s_pad_pulse = 0;
    s_buttons = 0;
    s_lx = s_ly = 128;
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    if (ok) printf("mouse_buttons_selftest: OK\n");
    return ok;
}
#undef MBTN_CHECK

/* ---- the on-screen keyboard's keys (sdl3vk.h, text entry); no Vulkan needed ------------
 * Drives the real poll (poll_input) with pushed key events and a stand-in for SDL's key array.
 * While the keyboard is claimed no held key reaches the game, as a button or as the stick;
 * a key still held as it closes (the Enter that confirmed the field) stays hidden until it is
 * let go; pressed again, it is the game's. */
#define TKEY_CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "text_keys_selftest: " __VA_ARGS__); fputc('\n', stderr); ok = 0; } } while (0)

static bool s_tkey_state[SDL_SCANCODE_COUNT];
static const bool *tkey_state(int *count) {
    if (count) *count = SDL_SCANCODE_COUNT;
    return s_tkey_state;
}

static void tkey_set(SDL_Scancode sc, bool down) {
    s_tkey_state[sc] = down;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.scancode = sc;
    ev.key.down = down;
    SDL_PushEvent(&ev);
}

static int sdl3vk_text_keys_selftest(void) {
    int ok = 1;
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        fprintf(stderr, "text_keys_selftest: SDL events unavailable: %s\n", SDL_GetError());
        return 0;
    }
    memset(s_tkey_state, 0, sizeof s_tkey_state);
    s_key_state_fn = tkey_state;
    sdl3vk_reset_input_profile();
    sdl3vk_init_input_profile();
    const uint32_t start = 0x0008u, cross = NK_PSP_BTN_CROSS_BIT;
    uint8_t lx, ly;
    uint32_t seen;

    /* The game's keys: x is Cross and i pushes the stick (the default profile). */
    tkey_set(SDL_SCANCODE_X, true);
    (void)mbtn_test_sample(&lx, &ly);
    seen = mbtn_test_sample(&lx, &ly);
    TKEY_CHECK(seen & cross, "x held is not Cross for the game (0x%x)", seen);
    tkey_set(SDL_SCANCODE_X, false);
    (void)mbtn_test_sample(&lx, &ly);
    tkey_set(SDL_SCANCODE_I, true);
    (void)mbtn_test_sample(&lx, &ly);
    TKEY_CHECK(lx != 128 || ly != 128, "i held does not push the stick");
    tkey_set(SDL_SCANCODE_I, false);
    (void)mbtn_test_sample(&lx, &ly);

    /* The keyboard has the keys: typed and held, none reaches the game. */
    sdl3vk_set_text_claim(1);
    tkey_set(SDL_SCANCODE_X, true);
    tkey_set(SDL_SCANCODE_I, true);
    for (int i = 0; i < 3; i++) {
        seen = mbtn_test_sample(&lx, &ly);
        TKEY_CHECK(seen == 0 && lx == 128 && ly == 128,
                   "keys held for the on-screen keyboard reached the game (sample %d: 0x%x %u,%u)",
                   i, seen, lx, ly);
    }
    tkey_set(SDL_SCANCODE_X, false);
    tkey_set(SDL_SCANCODE_I, false);
    (void)mbtn_test_sample(&lx, &ly);

    /* Enter confirms the field and is still held as the keyboard closes. */
    tkey_set(SDL_SCANCODE_RETURN, true);
    seen = mbtn_test_sample(&lx, &ly);
    TKEY_CHECK(seen == 0, "Enter for the on-screen keyboard reached the game (0x%x)", seen);
    sdl3vk_set_text_claim(0);
    for (int i = 0; i < 3; i++) {
        seen = mbtn_test_sample(&lx, &ly);
        TKEY_CHECK((seen & start) == 0, "the Enter that closed the keyboard became START (sample %d)", i);
    }
    tkey_set(SDL_SCANCODE_RETURN, false);
    seen = mbtn_test_sample(&lx, &ly);
    TKEY_CHECK(seen == 0, "released keys still show (0x%x)", seen);
    tkey_set(SDL_SCANCODE_RETURN, true);
    seen = mbtn_test_sample(&lx, &ly);
    TKEY_CHECK(seen & start, "Enter pressed again after the keyboard closed is not START (0x%x)", seen);
    tkey_set(SDL_SCANCODE_RETURN, false);
    (void)mbtn_test_sample(&lx, &ly);

    /* Typed text: each well-formed UTF-8 character is one SDL3VK_KEY_TEXT. A lead byte cut
     * short, a stray continuation byte, an overlong form, a surrogate and a value past
     * U+10FFFF queue nothing, and the byte that cut a sequence short is read on its own. */
    {
        static const struct { const char *in; int n; uint32_t cp[3]; } texts[] = {
            { "a\xC3\xA9\xE2\x82\xAC", 3, { 0x61u, 0xE9u, 0x20ACu } },
            { "\xF0\x9F\x98\x80", 1, { 0x1F600u } },
            { "\xC3" "A", 1, { 0x41u } },
            { "\xE2\x82" "B", 1, { 0x42u } },
            { "\x80" "E", 1, { 0x45u } },
            { "\xC0\x80" "C", 1, { 0x43u } },
            { "\xE0\x80\xAF", 0, { 0u } },
            { "\xED\xA0\x80" "D", 1, { 0x44u } },
            { "\xF4\x90\x80\x80", 0, { 0u } },
        };
        Sdl3VkKeyEvent kev[8];
        while (sdl3vk_take_key_events(kev, 8) > 0) {}
        for (size_t t = 0; t < sizeof texts / sizeof texts[0]; t++) {
            queue_text_utf8(texts[t].in);
            const int n = sdl3vk_take_key_events(kev, 8);
            int same = n == texts[t].n;
            for (int i = 0; same && i < n; i++)
                same = kev[i].key == SDL3VK_KEY_TEXT && kev[i].codepoint == texts[t].cp[i];
            TKEY_CHECK(same, "text case %u queued %d character(s), first U+%04X (want %d, U+%04X)",
                       (unsigned)t, n, n > 0 ? (unsigned)kev[0].codepoint : 0u, texts[t].n,
                       (unsigned)texts[t].cp[0]);
        }
    }

    s_key_state_fn = NULL;
    memset(s_text_keys, 0, sizeof s_text_keys);
    s_key_head = 0;
    s_key_count = 0;
    sdl3vk_reset_input_profile();
    nk_input_gate_init(&s_gate);
    s_button_pulse = s_pad_pulse = 0;
    s_buttons = 0;
    s_lx = s_ly = 128;
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    if (ok) printf("text_keys_selftest: OK\n");
    return ok;
}
#undef TKEY_CHECK

/* Set while the lifecycle selftest holds its own SDL video reference: SDL_WasInit cannot tell
 * that reference from one sdl3vk leaked, so the per-step count then checks the gamepad
 * subsystem only, and the selftest checks video once after releasing its reference. */
static int s_lifecycle_test_holds_video = 0;

/* Objects sdl3vk_init can create, and its SDL subsystems, that are still held. */
static int lifecycle_live_objects(void) {
    static const PresentFrame empty;
    int n = (s_win != NULL) + (s_pad != NULL) + (s_inst != VK_NULL_HANDLE) +
            (s_validation_msgr != VK_NULL_HANDLE) + (s_surf != VK_NULL_HANDLE) +
            (s_dev != VK_NULL_HANDLE) + (s_pool != VK_NULL_HANDLE) + (s_cmd != VK_NULL_HANDLE) +
            (s_fence != VK_NULL_HANDLE) + (s_swap != VK_NULL_HANDLE) + s_sdl_subsystems;
    for (int i = 0; i < PRESENT_FRAMES; i++)
        n += memcmp(&s_frame[i], &empty, sizeof empty) != 0;
    for (size_t i = 0; i < sizeof(s_swap_sem_done) / sizeof(s_swap_sem_done[0]); i++)
        n += s_swap_sem_done[i] != VK_NULL_HANDLE;
    return n + (SDL_WasInit(s_lifecycle_test_holds_video ? SDL_INIT_GAMEPAD
                                                         : SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) != 0);
}

/* A failed sdl3vk_init's selftest status: SKIP (77) while the host has no Vulkan presentation
 * (SDL video, the window, the instance, the surface or a device that presents to it), and a
 * failure (1) once init found a presenting device, because every later step is the renderer's
 * own. */
static int cap_test_init_status(void) {
    return s_init_found_device ? 1 : 77;
}

static int cap_test_init_failed(void) {
    const int status = cap_test_init_status();
    if (status == 77)
        fprintf(stderr, "gpu capture selftest: SKIP (Vulkan initialization unavailable)\n");
    else
        fprintf(stderr, "gpu capture selftest: sdl3vk_init failed after it found a presenting "
                        "device\n");
    return status;
}

/* Init unwind, repeated shutdown and re-init (#1377). A failure injected at each fallible step
 * of sdl3vk_init in turn must leave no Vulkan object, window or SDL subsystem alive and raise no
 * validation error (with validation on, vkDestroyDevice and vkDestroyInstance report any child
 * the unwind missed). Returns 0, 77 when Vulkan is unavailable, or 1. */
static int sdl3vk_lifecycle_selftest(void) {
    if (!sdl3vk_init("Nakagawa GPU lifecycle selftest")) return cap_test_init_failed();
    const int steps = s_init_steps;
    const int errors = sdl3vk_validation_error_count();
    int ok = 1;
    if (!s_validation_on) {
        fprintf(stderr, "lifecycle: SR_VULKAN_VALIDATION=1 did not reach sdl3vk_init; "
                        "the validation layer is off\n");
        ok = 0;
    }
    /* The test's own instance keeps the driver loaded between the cycles below: the NVIDIA
     * Windows driver was seen to fault in a worker thread of its own when it is unloaded and
     * loaded again many times in quick succession. It is not one of sdl3vk's objects. */
    VkInstanceCreateInfo keep_ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance keep = VK_NULL_HANDLE;
    (void)vkCreateInstance(&keep_ci, NULL, &keep);
    /* The test holds its own SDL video reference across the cycles too. sdl3vk_shutdown
     * releases only its own (SDL_QuitSubSystem), so the video backend's display connection
     * stays open instead of being reopened some 70 times: under xvfb on a loaded CI runner
     * one reconnect was seen to fail ("x11 not available"), which the loop then reported as
     * an unwind at the wrong step. sdl3vk's own objects are still counted below; a video
     * reference it leaked in any cycle is caught once the test releases its own. */
    const int keep_video = SDL_InitSubSystem(SDL_INIT_VIDEO) ? 1 : 0;
    s_lifecycle_test_holds_video = keep_video;
    sdl3vk_shutdown();
    sdl3vk_shutdown();
    if (lifecycle_live_objects() != 0) {
        fprintf(stderr, "lifecycle: %d object(s) alive after shutdown\n", lifecycle_live_objects());
        ok = 0;
    }
    for (int step = 1; ok && step <= steps; step++) {
        s_init_fault_step = step;
        int inited = sdl3vk_init("Nakagawa GPU lifecycle selftest");
        s_init_fault_step = 0;
        int live = lifecycle_live_objects();
        if (inited || s_init_steps != step || live != 0) {
            fprintf(stderr, "lifecycle: init step %d of %d failed (%s) and left %d object(s) alive\n",
                    step, steps, inited || s_init_steps != step ? "not where injected" : "as injected",
                    live);
            ok = 0;
        }
        /* SDL_Init failing is a host without Vulkan presentation (SKIP); the last step failing,
         * on a host whose device was found, is a renderer failure, never a SKIP. */
        const int status = cap_test_init_status();
        if ((step == 1 && status != 77) || (step == steps && status != 1)) {
            fprintf(stderr, "lifecycle: init step %d of %d failed with selftest status %d (want %d)\n",
                    step, steps, status, step == 1 ? 77 : 1);
            ok = 0;
        }
    }
    /* A failure at each checked step of a present (#1377) is reported; the renderer is
     * terminal exactly when the step leaves a frame, or an acquired image, that cannot be
     * handed back; a terminal renderer refuses the next present, a recovered one presents it;
     * and shutdown and re-init leave no object alive and raise no validation error. A
     * suboptimal acquire is presented, not rebuilt over with its image still acquired. */
    static const struct {
        const char *name;
        int step;
        VkResult result;
        int want, terminal;     /* the faulted present's result; the renderer state after it */
    } pcases[] = {
        { "frame fence wait",       PSTEP_FRAME_WAIT,  VK_ERROR_DEVICE_LOST,          -1, 1 },
        { "acquire, out of memory", PSTEP_ACQUIRE,     VK_ERROR_OUT_OF_HOST_MEMORY,   -1, 0 },
        { "acquire, device lost",   PSTEP_ACQUIRE,     VK_ERROR_DEVICE_LOST,          -1, 1 },
        { "acquire, suboptimal",    PSTEP_ACQUIRE,     VK_SUBOPTIMAL_KHR,              1, 0 },
        { "image fence wait",       PSTEP_IMAGE_WAIT,  VK_ERROR_DEVICE_LOST,          -1, 1 },
        { "command buffer reset",   PSTEP_RESET_CMD,   VK_ERROR_OUT_OF_DEVICE_MEMORY, -1, 1 },
        { "command buffer begin",   PSTEP_BEGIN_CMD,   VK_ERROR_OUT_OF_HOST_MEMORY,   -1, 1 },
        { "command buffer end",     PSTEP_END_CMD,     VK_ERROR_OUT_OF_DEVICE_MEMORY, -1, 1 },
        { "fence reset",            PSTEP_RESET_FENCE, VK_ERROR_OUT_OF_DEVICE_MEMORY, -1, 1 },
        { "submit",                 PSTEP_SUBMIT,      VK_ERROR_DEVICE_LOST,          -1, 1 },
    };
    static uint32_t px[PSP_W * PSP_H];
    const int npcases = (int)(sizeof pcases / sizeof pcases[0]);
    for (int i = 0; ok && i < npcases; i++) {
        if (!sdl3vk_init("Nakagawa GPU lifecycle selftest")) {
            fprintf(stderr, "lifecycle: re-init before present case '%s' failed\n", pcases[i].name);
            ok = 0;
            break;
        }
        /* Every frame submitted once, so the next present waits on its frame's fence. */
        int warm = 1;
        for (int k = 0; k < PRESENT_FRAMES; k++) warm &= sdl3vk_present_rgba(px) == 1;
        /* The image fence wait runs only when the acquired image was last drawn by the other
         * frame, which a swapchain whose image count is a multiple of PRESENT_FRAMES never
         * does: mark every image as drawn by the other frame (its fence was submitted above). */
        if (pcases[i].step == PSTEP_IMAGE_WAIT)
            for (uint32_t k = 0; k < s_swap_n; k++)
                s_swap_img_fence[k] = s_frame[(s_frame_cursor + 1) % PRESENT_FRAMES].fence;
        s_pstep_fault = pcases[i].step;
        s_pstep_fault_result = pcases[i].result;
        int got = 0, presents = 0;
        while (s_pstep_fault != PSTEP_NONE && presents < 8) {
            got = sdl3vk_present_rgba(px);
            presents++;
        }
        int reached = s_pstep_fault == PSTEP_NONE;
        s_pstep_fault = PSTEP_NONE;
        int terminal = sdl3vk_renderer_terminal();
        int reported = s_terminal_reported;
        int after = sdl3vk_present_rgba(px);
        /* One line names the transition, whichever step printed it; the refused present after
         * it prints none. */
        unsigned lines = s_terminal_lines;
        sdl3vk_shutdown();
        int live = lifecycle_live_objects();
        if (!warm || !reached || got != pcases[i].want || terminal != pcases[i].terminal ||
            reported != terminal || lines != (unsigned)terminal ||
            after != (pcases[i].terminal ? -1 : 1) || live != 0) {
            fprintf(stderr, "lifecycle: present fault '%s': warm=%d reached=%d present=%d (want %d) "
                            "terminal=%d (want %d, named %d in %u line(s)) next present=%d, %d "
                            "object(s) alive after shutdown\n", pcases[i].name, warm, reached, got,
                    pcases[i].want, terminal, pcases[i].terminal, reported, lines, after, live);
            ok = 0;
        }
    }
    /* Two faults the renderer goes on from. An armed capture whose readback buffer cannot be
     * made has acquired nothing yet: the capture fails and the next presents are shown. A
     * suboptimal present whose rebuild finds the window minimized (a 0x0 surface) was shown:
     * the old swapchain stays, and the next presents are shown. Neither leaves an image or a
     * semaphore behind for the validation layer to report. */
    for (int c = 0; ok && c < 2; c++) {
        const char *name = c == 0 ? "capture buffer allocation" : "suboptimal present, minimized";
        if (!sdl3vk_init("Nakagawa GPU lifecycle selftest")) {
            fprintf(stderr, "lifecycle: re-init before present case '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int errors_before = sdl3vk_validation_error_count();
        int warm = 1;
        for (int k = 0; k < PRESENT_FRAMES; k++) warm &= sdl3vk_present_rgba(px) == 1;
        int armed = 1;
        if (c == 0) {
            armed = sr_capture_arm("selftest_lifecycle.ppm");
            s_pstep_fault = PSTEP_CAP_ALLOC;
            s_pstep_fault_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        } else {
            s_swap_extent_zero_fault = 1;
            s_pstep_fault = PSTEP_PRESENT;
            s_pstep_fault_result = VK_SUBOPTIMAL_KHR;
        }
        const int got = sdl3vk_present_rgba(px);
        const int reached = s_pstep_fault == PSTEP_NONE && s_swap_extent_zero_fault == 0;
        s_pstep_fault = PSTEP_NONE;
        s_swap_extent_zero_fault = 0;
        const int capture = c == 0 ? sr_capture_result() : -1;
        const int terminal = sdl3vk_renderer_terminal();
        int after = 1;
        for (int k = 0; k <= PRESENT_FRAMES; k++) after &= sdl3vk_present_rgba(px) == 1;
        const int verrors = sdl3vk_validation_error_count() - errors_before;
        sdl3vk_shutdown();
        remove("selftest_lifecycle.ppm");
        const int live = lifecycle_live_objects();
        const int want = c == 0 ? -1 : 1;
        if (!warm || !armed || !reached || got != want || capture != -1 || terminal || !after ||
            verrors != 0 || live != 0) {
            fprintf(stderr, "lifecycle: present fault '%s': warm=%d armed=%d reached=%d present=%d "
                            "(want %d) capture=%d (want -1) terminal=%d (want 0) next presents "
                            "shown=%d, %d validation error(s), %d object(s) alive after "
                            "shutdown\n", name, warm, armed, reached, got, want, capture,
                    terminal, after, verrors, live);
            ok = 0;
        }
    }
    /* A file capture's fence wait is the present's own (wait_present_frame). With
     * SR_GPU_TIMESTAMPS the captured frame's GPU time is read like any other present's. A failed
     * wait leaves the renderer terminal, as the copy into the readback buffer may still be
     * running; the frame was presented, so the present reports it shown and the capture fails.
     * Every slot is waited first, so the capture's wait is the only fence wait the present
     * makes. */
    for (int c = 0; ok && c < 2; c++) {
        const char *name = c == 0 ? "captured frame's GPU time" : "capture fence wait, device lost";
        char saved_ts[64] = {0};
        const char *ts_env = getenv("SR_GPU_TIMESTAMPS");
        if (ts_env) snprintf(saved_ts, sizeof saved_ts, "%s", ts_env);
        if (c == 0 && !cap_test_setenv("SR_GPU_TIMESTAMPS", "1")) {
            fprintf(stderr, "lifecycle: cannot set SR_GPU_TIMESTAMPS\n");
            ok = 0;
            break;
        }
        const int inited = sdl3vk_init("Nakagawa GPU lifecycle selftest");
        if (c == 0 && !cap_test_setenv("SR_GPU_TIMESTAMPS", saved_ts[0] ? saved_ts : "0")) {
            fprintf(stderr, "lifecycle: cannot restore SR_GPU_TIMESTAMPS\n");
            ok = 0;
        }
        if (!inited) {
            fprintf(stderr, "lifecycle: re-init before present case '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int pool = s_pts_pool != VK_NULL_HANDLE;
        int warm = 1;
        for (int k = 0; k < PRESENT_FRAMES; k++) warm &= sdl3vk_present_rgba(px) == 1;
        warm &= wait_all_present_frames();
        const uint64_t timed_before = s_pts_timed;
        const int armed = sr_capture_arm("selftest_lifecycle.ppm");
        if (c == 1) {
            s_pstep_fault = PSTEP_FRAME_WAIT;
            s_pstep_fault_result = VK_ERROR_DEVICE_LOST;
        }
        const PresentFrame *f = &s_frame[s_frame_cursor];
        const int got = sdl3vk_present_rgba(px);
        const int reached = s_pstep_fault == PSTEP_NONE;
        s_pstep_fault = PSTEP_NONE;
        const int capture = sr_capture_result();
        const int terminal = sdl3vk_renderer_terminal();
        const int reported = s_terminal_reported;
        const uint64_t timed = s_pts_timed - timed_before;
        const int ts_left = f->ts_written;
        const int after = sdl3vk_present_rgba(px);
        /* The failed capture wait prints the one line naming the transition (#1377). */
        const unsigned lines = s_terminal_lines;
        sdl3vk_shutdown();
        remove("selftest_lifecycle.ppm");
        const int live = lifecycle_live_objects();
        const int want_capture = c == 0 ? 1 : -1;
        const uint64_t want_timed = c == 0 && pool ? 1u : 0u;
        if (!warm || !armed || !reached || got != 1 || capture != want_capture ||
            terminal != c || reported != c || lines != (unsigned)c || after != (c ? -1 : 1) ||
            (c == 0 && (timed != want_timed || ts_left)) || live != 0) {
            fprintf(stderr, "lifecycle: present case '%s': warm=%d armed=%d reached=%d present=%d "
                            "(want 1) capture=%d (want %d) terminal=%d named %d in %u line(s) "
                            "(want %d) next present=%d, GPU times read %llu (want %llu, pool %d, "
                            "left %d), %d object(s) alive after shutdown\n", name, warm, armed,
                    reached, got, capture, want_capture, terminal, reported, lines, c, after,
                    (unsigned long long)timed, (unsigned long long)want_timed, pool, ts_left, live);
            ok = 0;
        }
    }
    /* VK_INCOMPLETE from the physical-device or the instance-layer list is a success: the
     * entries written are valid. Init still finds the device, and the validation layer the run
     * asked for, and shutdown leaves nothing alive. */
    for (int c = 0; ok && c < 2; c++) {
        const char *name = c == 0 ? "physical devices" : "instance layers";
        s_enum_incomplete_fault = c == 0 ? ENUM_FAULT_DEVICES : ENUM_FAULT_LAYERS;
        const int inited = sdl3vk_init("Nakagawa GPU lifecycle selftest");
        const int reached = s_enum_incomplete_fault == 0;
        s_enum_incomplete_fault = 0;
        const int validated = s_validation_on && s_validation_msgr != VK_NULL_HANDLE;
        if (inited) sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        if (!inited || !reached || !validated || live != 0) {
            fprintf(stderr, "lifecycle: VK_INCOMPLETE from the %s list: init %d (want 1), fault "
                            "reached %d, validation layer on %d, %d object(s) alive after "
                            "shutdown\n", name, inited, reached, validated, live);
            ok = 0;
        }
    }
    if (keep) vkDestroyInstance(keep, NULL);
    if (keep_video) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        s_lifecycle_test_holds_video = 0;
        if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "lifecycle: SDL video still initialised after the cycles "
                            "(a reference sdl3vk_init took was not released)\n");
            ok = 0;
        }
    }
    if (sdl3vk_validation_error_count() != errors) {
        fprintf(stderr, "lifecycle: %d validation error(s) during the unwinds\n",
                sdl3vk_validation_error_count() - errors);
        ok = 0;
    }
    if (!ok) return 1;
    printf("gpu lifecycle selftest: init unwind at each of %d steps OK; %d present faults OK\n",
           steps, npcases + 4);
    return 0;
}

/* ---- window-surface loss (#360) -----------------------------------------------------------
 * Android destroys the window's surface when the app goes to the background and makes a new one
 * when it comes back. A lost surface is replaced: the old swapchain and VkSurfaceKHR go, a new
 * surface is made for the same window and checked for presentation, and a swapchain is built on
 * it, while the device and what was made on it (the frame ring, its images, the command pool)
 * stay. The present that met the loss is not shown and the next one is. While the platform has
 * no window surface (SDL's background events, or no surface to make one from), presents are not
 * shown (SDL3VK_PRESENT_DISPLAY_BUSY) and the renderer is not terminal. A new surface that cannot
 * be made in the foreground or cannot be presented to, and a lost device, stay terminal with one
 * line. The recovering cases run twice in one session, as the device check does ("Home, wait ten
 * seconds, resume, twice"). Returns 0, 77 when Vulkan is unavailable, or 1. */

static int surface_test_push(Uint32 type) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = type;
    ev.common.timestamp = SDL_GetTicksNS();
    return SDL_PushEvent(&ev) ? 1 : 0;
}

static int surface_test_warm(const uint32_t *px) {
    int warm = 1;
    for (int k = 0; k < PRESENT_FRAMES; k++) warm &= sdl3vk_present_rgba(px) == 1;
    return warm;
}

static int sdl3vk_surface_selftest(void) {
    static uint32_t px[PSP_W * PSP_H];
    if (!sdl3vk_init("Nakagawa GPU surface selftest")) return cap_test_init_failed();
    sdl3vk_shutdown();
    const int errors = sdl3vk_validation_error_count();
    int ok = 1, cases = 0;
    /* As in the lifecycle selftest: the test's own instance and SDL video reference keep the
     * driver and the display connection up across the init cycles below. */
    VkInstanceCreateInfo keep_ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance keep = VK_NULL_HANDLE;
    (void)vkCreateInstance(&keep_ci, NULL, &keep);
    const int keep_video = SDL_InitSubSystem(SDL_INIT_VIDEO) ? 1 : 0;
    s_lifecycle_test_holds_video = keep_video;

    /* (a) The surface is lost at the acquire, or at the present (which was enqueued, so its
     * semaphore wait still runs). */
    for (int c = 0; ok && c < 2; c++) {
        const char *name = c == 0 ? "surface lost at the acquire" : "surface lost at the present";
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        const VkDevice dev = s_dev;
        const VkCommandPool pool = s_pool;
        const VkImage fbimg = s_frame[0].fbimg;
        for (int rep = 0; ok && rep < 2; rep++) {
            const uint64_t surf0 = s_surface_gen, sc0 = sdl3vk_swapchain_generation();
            const int armed = sr_capture_arm("selftest_surface.ppm");
            if (c == 0) {
                s_pstep_fault = PSTEP_ACQUIRE;
                s_pstep_fault_result = VK_ERROR_SURFACE_LOST_KHR;
            } else {
                sdl3vk_present_fault_inject(VK_ERROR_SURFACE_LOST_KHR);
            }
            const int got = sdl3vk_present_rgba(px);
            const int reached = s_pstep_fault == PSTEP_NONE && !s_present_fault_armed;
            s_pstep_fault = PSTEP_NONE;
            sdl3vk_present_fault_clear();
            const int capture = sr_capture_result();
            const int terminal = sdl3vk_renderer_terminal();
            const unsigned lines = s_terminal_lines;
            const uint64_t surfaces = s_surface_gen - surf0;
            const uint64_t chains = sdl3vk_swapchain_generation() - sc0;
            const int next = sdl3vk_present_rgba(px);
            const int after = surface_test_warm(px);
            const int kept = s_dev == dev && s_pool == pool && s_frame[0].fbimg == fbimg;
            remove("selftest_surface.ppm");
            if (!warm || !armed || !reached || got != -1 || capture != -1 || terminal ||
                lines != 0 || surfaces != 1 || chains == 0 || next != 1 || !after || !kept) {
                fprintf(stderr, "surface: '%s' (loss %d): warm=%d armed=%d reached=%d present=%d "
                                "(want -1) capture=%d (want -1) terminal=%d in %u line(s) (want 0) "
                                "surfaces replaced %llu (want 1) swapchains made %llu (want >0) "
                                "next present=%d (want 1) later shown=%d device objects kept=%d\n",
                        name, rep + 1, warm, armed, reached, got, capture, terminal, lines,
                        (unsigned long long)surfaces, (unsigned long long)chains, next, after,
                        kept);
                ok = 0;
            }
        }
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        if (verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': %d validation error(s), %d object(s) alive after "
                            "shutdown\n", name, verrors, live);
            ok = 0;
        }
        cases++;
    }

    /* (b) No window surface: the app is in the background (SDL's own events, as SDL sends them on
     * Android), or the surface was lost before those events arrived and the platform has none to
     * make a new one from. Presents are not shown and nothing is terminal; on the return the
     * surface is replaced and the next present is shown. */
    for (int c = 0; ok && c < 2; c++) {
        const char *name = c == 0 ? "background and return" : "surface lost with no window surface";
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        for (int rep = 0; ok && rep < 2; rep++) {
            const uint64_t surf0 = s_surface_gen, sc0 = sdl3vk_swapchain_generation();
            const uint64_t busy0 = sdl3vk_present_busy_count();
            int pushed = 1, reached = 1, got;
            if (c == 0) {
                pushed = surface_test_push(SDL_EVENT_WILL_ENTER_BACKGROUND) &&
                         surface_test_push(SDL_EVENT_DID_ENTER_BACKGROUND);
                got = sdl3vk_present_rgba(px);
            } else {
                s_no_window_surface_fault = 1;
                s_pstep_fault = PSTEP_ACQUIRE;
                s_pstep_fault_result = VK_ERROR_SURFACE_LOST_KHR;
                got = sdl3vk_present_rgba(px);
                reached = s_pstep_fault == PSTEP_NONE;
                s_pstep_fault = PSTEP_NONE;
            }
            int busy = 1;
            for (int k = 0; k < 3; k++) busy &= sdl3vk_present_rgba(px) == SDL3VK_PRESENT_DISPLAY_BUSY;
            const uint64_t busy_n = sdl3vk_present_busy_count() - busy0;
            const int held_terminal = sdl3vk_renderer_terminal();
            const uint64_t held_surfaces = s_surface_gen - surf0;
            const int held_marked = s_surface_withheld;   /* the line marking the start was printed */
            if (c == 0) {
                pushed &= surface_test_push(SDL_EVENT_WILL_ENTER_FOREGROUND) &&
                          surface_test_push(SDL_EVENT_DID_ENTER_FOREGROUND);
            } else {
                s_no_window_surface_fault = 0;
            }
            const int back = sdl3vk_present_rgba(px);
            const uint64_t surfaces = s_surface_gen - surf0;
            const uint64_t chains = sdl3vk_swapchain_generation() - sc0;
            const int after = surface_test_warm(px);
            const int want_got = c == 0 ? SDL3VK_PRESENT_DISPLAY_BUSY : -1;
            const uint64_t want_busy = c == 0 ? 4u : 3u;
            if (!warm || !pushed || !reached || got != want_got || !busy || busy_n != want_busy ||
                held_terminal || held_surfaces != 0 || back != 1 || surfaces != 1 || chains == 0 ||
                !after || sdl3vk_renderer_terminal() || s_terminal_lines != 0 || !held_marked ||
                s_surface_withheld) {
                fprintf(stderr, "surface: '%s' (round %d): warm=%d pushed=%d reached=%d first "
                                "present=%d (want %d) later presents not shown=%d, %llu counted "
                                "(want %llu) terminal=%d surfaces made with none %llu (want 0) "
                                "present on return=%d (want 1) surfaces replaced %llu (want 1) "
                                "swapchains made %llu (want >0) later shown=%d terminal lines %u "
                                "held back marked %d then %d (want 1 then 0)\n",
                        name, rep + 1, warm, pushed, reached, got, want_got, busy,
                        (unsigned long long)busy_n, (unsigned long long)want_busy, held_terminal,
                        (unsigned long long)held_surfaces, back, (unsigned long long)surfaces,
                        (unsigned long long)chains, after, s_terminal_lines, held_marked,
                        s_surface_withheld);
                ok = 0;
            }
        }
        s_no_window_surface_fault = 0;
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        if (verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': %d validation error(s), %d object(s) alive after "
                            "shutdown\n", name, verrors, live);
            ok = 0;
        }
        cases++;
    }

    /* (c) What stays terminal: one line names it and the next present is refused. */
    static const struct {
        const char *name;
        int create_fault, support_fault, at_present;
        VkResult result;
    } neg[] = {
        { "new surface cannot be made",         1, 0, 0, VK_ERROR_SURFACE_LOST_KHR },
        { "new surface cannot be presented to", 0, 1, 0, VK_ERROR_SURFACE_LOST_KHR },
        { "device lost at the acquire",         0, 0, 0, VK_ERROR_DEVICE_LOST },
        { "device lost at the present",         0, 0, 1, VK_ERROR_DEVICE_LOST },
        { "new surface cannot be made after a lost present", 1, 0, 1, VK_ERROR_SURFACE_LOST_KHR },
    };
    for (size_t i = 0; ok && i < sizeof neg / sizeof neg[0]; i++) {
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", neg[i].name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        const uint64_t surf0 = s_surface_gen;
        s_surface_create_fault = neg[i].create_fault;
        s_surface_support_fault = neg[i].support_fault;
        if (neg[i].at_present) {
            sdl3vk_present_fault_inject((int)neg[i].result);
        } else {
            s_pstep_fault = PSTEP_ACQUIRE;
            s_pstep_fault_result = neg[i].result;
        }
        const int got = sdl3vk_present_rgba(px);
        const int reached = s_pstep_fault == PSTEP_NONE && !s_present_fault_armed &&
                            s_surface_create_fault == 0 && s_surface_support_fault == 0;
        s_pstep_fault = PSTEP_NONE;
        sdl3vk_present_fault_clear();
        s_surface_create_fault = s_surface_support_fault = 0;
        const int terminal = sdl3vk_renderer_terminal();
        const int reported = s_terminal_reported;
        const int next = sdl3vk_present_rgba(px);
        const unsigned lines = s_terminal_lines;
        const uint64_t surfaces = s_surface_gen - surf0;
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        if (!warm || !reached || got != -1 || !terminal || !reported || lines != 1 || next != -1 ||
            surfaces != 0 || verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': warm=%d reached=%d present=%d (want -1) terminal=%d "
                            "named %d in %u line(s) (want 1, 1, 1) next present=%d (want -1) "
                            "surfaces replaced %llu (want 0), %d validation error(s), %d "
                            "object(s) alive after shutdown\n", neg[i].name, warm, reached, got,
                    terminal, reported, lines, next, (unsigned long long)surfaces, verrors, live);
            ok = 0;
        }
        cases++;
    }

    /* (d) The pause overlay meets the same loss at its acquire or its present: it is told to
     * compose its image again for the new swapchain, and shows it; when the new surface cannot be
     * made it is terminal with one line. */
    for (int c = 0; c < 4; c++) {
        const int at_present = c & 1, create_fault = c >> 1;
        const char *name = c == 0 ? "pause overlay, lost at the acquire"
                         : c == 1 ? "pause overlay, lost at the present"
                         : c == 2 ? "pause overlay, lost at the acquire, new surface cannot be made"
                                  : "pause overlay, lost at the present, new surface cannot be made";
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        int ow = 0, oh = 0, orgba = 0, ow2 = 0, oh2 = 0;
        int shown = 0, lost = 0, reached = 0, again = 0, terminal = 1;
        unsigned lines = 0;
        uint64_t surfaces = 0;
        uint32_t *opx = NULL;
        if (sdl3vk_overlay_extent(&ow, &oh, &orgba) > 0 &&
            (opx = (uint32_t *)calloc((size_t)ow * (size_t)oh, 4u)) != NULL) {
            shown = sdl3vk_present_overlay(opx, ow * 4);
            const uint64_t surf0 = s_surface_gen;
            s_surface_create_fault = create_fault;
            s_pstep_fault = at_present ? PSTEP_PRESENT : PSTEP_ACQUIRE;
            s_pstep_fault_result = VK_ERROR_SURFACE_LOST_KHR;
            lost = sdl3vk_present_overlay(NULL, 0);
            reached = s_pstep_fault == PSTEP_NONE && s_surface_create_fault == 0;
            s_pstep_fault = PSTEP_NONE;
            s_surface_create_fault = 0;
            terminal = sdl3vk_renderer_terminal();
            lines = s_terminal_lines;
            surfaces = s_surface_gen - surf0;
            if (sdl3vk_overlay_extent(&ow2, &oh2, &orgba) > 0 && ow2 == ow && oh2 == oh)
                again = sdl3vk_present_overlay(opx, ow * 4);
            else if (create_fault)
                again = sdl3vk_present_overlay(opx, ow * 4);   /* refused: -1 */
        }
        free(opx);
        sdl3vk_overlay_release();
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        const int want_lost = create_fault ? -1 : 2, want_again = create_fault ? -1 : 1;
        if (!warm || shown != 1 || !reached || lost != want_lost || terminal != create_fault ||
            lines != (unsigned)create_fault || surfaces != (uint64_t)!create_fault ||
            again != want_again || verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': warm=%d shown=%d reached=%d present on the loss=%d "
                            "(want %d) terminal=%d in %u line(s) (want %d) surfaces replaced %llu "
                            "(want %d) %dx%d then %dx%d, next present=%d (want %d), %d validation "
                            "error(s), %d object(s) alive after shutdown\n", name, warm, shown,
                    reached, lost, want_lost, terminal, lines, create_fault,
                    (unsigned long long)surfaces, !create_fault, ow, oh, ow2, oh2, again,
                    want_again, verrors, live);
            ok = 0;
        }
        cases++;
    }

    /* (e) The surface is lost while the swapchain is rebuilt after a suboptimal present (a
     * rotation or fold as Home destroys the window): at the capabilities query or at
     * vkCreateSwapchainKHR. The suboptimal frame was shown; the renderer is not terminal, and the
     * next present replaces the surface and is shown. */
    for (int c = 0; c < 2; c++) {
        const char *name = c == 0 ? "surface lost at the rebuild's capabilities query"
                                  : "surface lost at the rebuild's vkCreateSwapchainKHR";
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        const uint64_t surf0 = s_surface_gen;
        s_pstep_fault = PSTEP_PRESENT;
        s_pstep_fault_result = VK_SUBOPTIMAL_KHR;
        s_swap_surface_lost_fault = c == 0 ? SWAP_LOST_CAPS : SWAP_LOST_CREATE;
        const int got = sdl3vk_present_rgba(px);
        const int reached = s_pstep_fault == PSTEP_NONE && s_swap_surface_lost_fault == SWAP_LOST_NONE;
        s_pstep_fault = PSTEP_NONE;
        s_swap_surface_lost_fault = SWAP_LOST_NONE;
        const int terminal = sdl3vk_renderer_terminal();
        const unsigned lines = s_terminal_lines;
        const int next = sdl3vk_present_rgba(px);
        const uint64_t surfaces = s_surface_gen - surf0;
        const int after = surface_test_warm(px);
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        if (!warm || !reached || got != 1 || terminal || lines != 0 || next != 1 || surfaces != 1 ||
            !after || verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': warm=%d reached=%d suboptimal present=%d (want 1) "
                            "terminal=%d in %u line(s) (want 0) next present=%d (want 1) surfaces "
                            "replaced %llu (want 1) later shown=%d, %d validation error(s), %d "
                            "object(s) alive after shutdown\n", name, warm, reached, got, terminal,
                    lines, next, (unsigned long long)surfaces, after, verrors, live);
            ok = 0;
        }
        cases++;
    }

    /* (f) A new surface lost at its first swapchain, and an unenqueued present whose rebuild finds
     * the surface lost. A surface lost at the present is replaced in that present; when the new
     * one is lost at once with the platform's window still there, every new surface would go the
     * same way: terminal with one line, and no further surface is made. When the window went with
     * it (Android clears SDL's window pointer before it abandons the surface), presents are not
     * shown until the window is back, and then the surface is replaced. A present that was not
     * enqueued rebuilds the swapchain to hand its image back; a surface lost there is replaced by
     * the next present, for a game frame and for the pause overlay. */
    for (int c = 0; c < 4; c++) {
        const char *name = c == 0 ? "new surface lost at once, window still there"
                         : c == 1 ? "new surface lost at once as the window goes"
                         : c == 2 ? "unenqueued present, rebuild finds the surface lost"
                                  : "pause overlay, unenqueued present, rebuild finds the surface lost";
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        const uint64_t surf0 = s_surface_gen;
        int got = 0, reached = 0, busy = 1, held_surfaces = 0, back = 0, after = 0, shown = 1;
        int held_terminal = 0, held_marked = 0, want_got = -1, want_back = 1, loss_terminal = 0;
        int held_extent = 0;
        uint64_t want_surfaces = 1;
        if (c < 2) {
            sdl3vk_present_fault_inject(VK_ERROR_SURFACE_LOST_KHR);
            s_swap_surface_lost_fault = c == 0 ? SWAP_LOST_CAPS : SWAP_LOST_CAPS_WINDOW_GONE;
            got = sdl3vk_present_rgba(px);
            /* With the window still there, the present that met the loss ends it: no second
             * replacement is tried. */
            loss_terminal = sdl3vk_renderer_terminal();
            reached = !s_present_fault_armed && s_swap_surface_lost_fault == SWAP_LOST_NONE;
            sdl3vk_present_fault_clear();
            s_swap_surface_lost_fault = SWAP_LOST_NONE;
            for (int k = 0; k < 3; k++) {
                const int r = sdl3vk_present_rgba(px);
                busy &= r == (c == 0 ? -1 : SDL3VK_PRESENT_DISPLAY_BUSY);
            }
            /* With the window gone there is no swapchain: the pause overlay's extent query
             * says there is nothing to show now (0), not a terminal renderer (-1), so a pause
             * open in this state goes on waiting for the window. */
            if (c == 1) {
                int ew = 0, eh = 0, ergba = 0;
                held_extent = sdl3vk_overlay_extent(&ew, &eh, &ergba);
            }
            held_terminal = sdl3vk_renderer_terminal();
            held_surfaces = (int)(s_surface_gen - surf0);
            held_marked = s_surface_withheld;
            s_no_window_surface_fault = 0;
            back = sdl3vk_present_rgba(px);
            if (c == 0) {
                want_back = -1;
            } else {
                want_surfaces = 2;
                after = surface_test_warm(px);
            }
        } else if (c == 2) {
            sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_HOST_MEMORY);
            s_swap_surface_lost_fault = SWAP_LOST_CREATE;
            got = sdl3vk_present_rgba(px);
            reached = !s_present_fault_armed && s_swap_surface_lost_fault == SWAP_LOST_NONE;
            sdl3vk_present_fault_clear();
            s_swap_surface_lost_fault = SWAP_LOST_NONE;
            held_terminal = sdl3vk_renderer_terminal();
            back = sdl3vk_present_rgba(px);
            after = surface_test_warm(px);
        } else {
            int ow = 0, oh = 0, orgba = 0;
            uint32_t *opx = NULL;
            if (sdl3vk_overlay_extent(&ow, &oh, &orgba) > 0 &&
                (opx = (uint32_t *)calloc((size_t)ow * (size_t)oh, 4u)) != NULL) {
                shown = sdl3vk_present_overlay(opx, ow * 4) == 1;
                s_pstep_fault = PSTEP_PRESENT;
                s_pstep_fault_result = VK_ERROR_OUT_OF_HOST_MEMORY;
                s_swap_surface_lost_fault = SWAP_LOST_CREATE;
                got = sdl3vk_present_overlay(NULL, 0);
                reached = s_pstep_fault == PSTEP_NONE && s_swap_surface_lost_fault == SWAP_LOST_NONE;
                s_pstep_fault = PSTEP_NONE;
                s_swap_surface_lost_fault = SWAP_LOST_NONE;
                held_terminal = sdl3vk_renderer_terminal();
                int ow2 = 0, oh2 = 0;
                if (sdl3vk_overlay_extent(&ow2, &oh2, &orgba) > 0 && ow2 == ow && oh2 == oh)
                    back = sdl3vk_present_overlay(opx, ow * 4);
            } else {
                shown = 0;
            }
            free(opx);
            sdl3vk_overlay_release();
            after = surface_test_warm(px);
        }
        const int terminal = sdl3vk_renderer_terminal();
        const unsigned lines = s_terminal_lines;
        const uint64_t surfaces = s_surface_gen - surf0;
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        const int want_terminal = c == 0;
        int bad = !warm || !shown || !reached || got != want_got || back != want_back ||
                  terminal != want_terminal || lines != (unsigned)want_terminal ||
                  surfaces != want_surfaces || verrors != 0 || live != 0;
        if (c < 2)
            bad |= !busy || held_surfaces != 1 || held_terminal != want_terminal ||
                   loss_terminal != want_terminal;
        if (c == 1) bad |= !held_marked || held_extent != 0;
        if (c >= 2) bad |= held_terminal;
        if (c != 0) bad |= !after;
        if (bad) {
            fprintf(stderr, "surface: '%s': warm=%d shown=%d reached=%d present on the loss=%d "
                            "(want -1) terminal at the loss=%d later presents as wanted=%d "
                            "(want 1) overlay extent query then=%d (want 0 as the window goes) "
                            "terminal then=%d "
                            "surfaces made by then %d (want 1) held back marked %d next "
                            "present=%d (want %d) terminal=%d in %u line(s) (want %d) surfaces "
                            "replaced %llu (want %llu) later shown=%d, %d validation error(s), %d "
                            "object(s) alive after shutdown\n", name, warm, shown, reached, got,
                    loss_terminal, busy, held_extent, held_terminal, held_surfaces, held_marked,
                    back,
                    want_back, terminal,
                    lines, want_terminal, (unsigned long long)surfaces,
                    (unsigned long long)want_surfaces, after, verrors, live);
            ok = 0;
        }
        cases++;
    }

    /* (g) A surface lost again and again after a working swapchain. A replacement lost before any
     * frame was shown on it, with the window still there, is terminal with one line: at every
     * acquire, at every present, and in the pause overlay, after exactly one replacement. The
     * window going at the second loss, or a frame shown between losses, keeps it recoverable. */
    for (int c = 0; c < 5; c++) {
        const char *name = c == 0 ? "surface lost at every acquire, window still there"
                         : c == 1 ? "surface lost at every present, window still there"
                         : c == 2 ? "pause overlay, surface lost at every present, window still there"
                         : c == 3 ? "surface lost again as the window goes"
                                  : "surface lost, shown, lost, shown";
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        const uint64_t surf0 = s_surface_gen;
        int r[4] = {0, 0, 0, 0}, shown = 1, back = 1, held_busy = 1;
        uint32_t *opx = NULL;
        int ow = 0, oh = 0, orgba = 0;
        if (c == 2) {
            if (sdl3vk_overlay_extent(&ow, &oh, &orgba) > 0 &&
                (opx = (uint32_t *)calloc((size_t)ow * (size_t)oh, 4u)) != NULL)
                shown = sdl3vk_present_overlay(opx, ow * 4) == 1;
            else
                shown = 0;
        }
        for (int k = 0; shown && k < 4; k++) {
            if (c == 3 && k == 1) s_no_window_surface_fault = 1;
            if (c == 1) {
                sdl3vk_present_fault_inject(VK_ERROR_SURFACE_LOST_KHR);
            } else if (c != 4 || (k & 1) == 0) {
                s_pstep_fault = c == 2 ? PSTEP_PRESENT : PSTEP_ACQUIRE;
                s_pstep_fault_result = VK_ERROR_SURFACE_LOST_KHR;
            }
            if (c == 2) {
                int w2 = 0, h2 = 0;
                /* The overlay asks for the extent before it composes; a terminal renderer has
                 * none, and the overlay is refused. */
                r[k] = sdl3vk_overlay_extent(&w2, &h2, &orgba) > 0 && w2 == ow && h2 == oh
                     ? sdl3vk_present_overlay(opx, ow * 4) : -1;
            } else {
                r[k] = sdl3vk_present_rgba(px);
            }
            s_pstep_fault = PSTEP_NONE;
            sdl3vk_present_fault_clear();
            if (c == 3 && k >= 1)
                held_busy &= k == 1 ? r[k] == -1 : r[k] == SDL3VK_PRESENT_DISPLAY_BUSY;
        }
        if (c == 3) {
            s_no_window_surface_fault = 0;
            back = sdl3vk_present_rgba(px);
        }
        free(opx);
        if (c == 2) sdl3vk_overlay_release();
        const int terminal = sdl3vk_renderer_terminal();
        const unsigned lines = s_terminal_lines;
        const uint64_t surfaces = s_surface_gen - surf0;
        /* Afterwards: shown again where it stays recoverable, refused where it is terminal. */
        const int after = c >= 3 ? surface_test_warm(px) : sdl3vk_present_rgba(px) == -1;
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        /* Wanted per present: a loss on the surface init made is replaced (-1, or 2 for the
         * overlay, which composes again), a loss on that replacement before anything was shown
         * on it is terminal (-1), and every later present is refused (-1). */
        int seq_ok;
        uint64_t want_surfaces = 1;
        int want_terminal = 1;
        if (c < 3) {
            seq_ok = r[0] == (c == 2 ? 2 : -1) && r[1] == -1 && r[2] == -1 && r[3] == -1;
        } else if (c == 3) {
            seq_ok = r[0] == -1 && held_busy && back == 1;
            want_surfaces = 2;
            want_terminal = 0;
        } else {
            seq_ok = r[0] == -1 && r[1] == 1 && r[2] == -1 && r[3] == 1;
            want_surfaces = 2;
            want_terminal = 0;
        }
        if (!warm || !shown || !seq_ok || terminal != want_terminal ||
            lines != (unsigned)want_terminal || surfaces != want_surfaces || !after ||
            verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': warm=%d shown=%d presents %d %d %d %d, held back as "
                            "wanted=%d, on return=%d (want %s) terminal=%d in %u line(s) (want %d) "
                            "surfaces replaced %llu (want %llu) afterwards as wanted=%d, %d "
                            "validation error(s), %d object(s) alive after shutdown\n", name, warm,
                    shown, r[0], r[1], r[2], r[3], held_busy, back,
                    c < 3 ? "replaced, then -1 -1 -1" : c == 3 ? "-1 -1 busy busy, then 1"
                                                          : "-1 1 -1 1",
                    terminal, lines, want_terminal, (unsigned long long)surfaces,
                    (unsigned long long)want_surfaces, after, verrors, live);
            ok = 0;
        }
        cases++;
    }

    /* (h) A new surface lost at once while the window is minimized: the minimized exemption is
     * for a surface that was not lost, so this is terminal with one line, not held back. The
     * window is reported minimized through a selftest-build seam (s_window_minimized_fault), so
     * the row runs on every host, Xvfb included. */
#ifdef SR_VIDEOREC_SELFTEST
    if (sdl3vk_init("Nakagawa GPU surface selftest")) {
        const char *name = "new surface lost at once while minimized";
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        const uint64_t surf0 = s_surface_gen;
        s_window_minimized_fault = 1;
        const int minimized = sdl3vk_window_minimized();
        /* Minimized before the loss: the pause overlay's extent query says there is nothing to
         * show now (0), not a terminal renderer (-1). */
        int ew = 0, eh = 0, ergba = 0;
        const int min_extent = sdl3vk_overlay_extent(&ew, &eh, &ergba);
        const int min_terminal = sdl3vk_renderer_terminal();
        s_surface_lost = 1;   /* as an acquire or a present loss leaves it */
        s_swap_surface_lost_fault = SWAP_LOST_CAPS;
        const int got = sdl3vk_present_rgba(px);
        const int reached = s_swap_surface_lost_fault == SWAP_LOST_NONE;
        s_swap_surface_lost_fault = SWAP_LOST_NONE;
        s_window_minimized_fault = 0;
        const int terminal = sdl3vk_renderer_terminal();
        const unsigned lines = s_terminal_lines;
        const uint64_t surfaces = s_surface_gen - surf0;
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        if (!warm || !minimized || min_extent != 0 || min_terminal || !reached || got != -1 ||
            !terminal || lines != 1 || surfaces != 1 || verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': warm=%d minimized=%d overlay extent query=%d "
                            "terminal=%d (want 0, 0) reached=%d present=%d (want -1) "
                            "terminal=%d in %u line(s) (want 1, 1) surfaces replaced %llu "
                            "(want 1), %d validation error(s), %d object(s) alive after "
                            "shutdown\n", name, warm, minimized, min_extent, min_terminal,
                    reached, got, terminal, lines, (unsigned long long)surfaces, verrors, live);
            ok = 0;
        }
        cases++;
    } else {
        fprintf(stderr, "surface: init before 'new surface lost at once while minimized' "
                        "failed\n");
        ok = 0;
    }
#endif

    /* (i) What lets a replacement nothing was shown on be replaced in turn. A loss on the surface
     * init made leaves such a replacement; SDL's background and foreground events then arrive in
     * one poll, as Android delivers a whole background and return once the app runs again (SDL
     * blocks in its event pump while paused). The background event lets the next surface be
     * replaced, so the return is shown. And the pause overlay alone: a loss, an overlay frame
     * shown on the replacement, a loss again: that shown overlay frame lets the second loss be
     * replaced too. Neither is terminal. */
    for (int c = 0; c < 2; c++) {
        const char *name = c == 0 ? "loss, then background and return in one poll"
                                  : "pause overlay alone: loss, frame shown, loss";
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        const uint64_t surf0 = s_surface_gen;
        int r[6] = {0, 0, 0, 0, 0, 0}, seq_ok = 0, unproven = 0, pushed = 1;
        if (c == 0) {
            s_pstep_fault = PSTEP_ACQUIRE;
            s_pstep_fault_result = VK_ERROR_SURFACE_LOST_KHR;
            r[0] = sdl3vk_present_rgba(px);
            s_pstep_fault = PSTEP_NONE;
            unproven = !s_surface_proven;   /* the replacement has shown nothing yet */
            pushed = surface_test_push(SDL_EVENT_WILL_ENTER_BACKGROUND) &&
                     surface_test_push(SDL_EVENT_DID_ENTER_BACKGROUND) &&
                     surface_test_push(SDL_EVENT_WILL_ENTER_FOREGROUND) &&
                     surface_test_push(SDL_EVENT_DID_ENTER_FOREGROUND);
            r[1] = sdl3vk_present_rgba(px);
            r[2] = surface_test_warm(px);
            seq_ok = r[0] == -1 && unproven && r[1] == 1 && r[2] == 1;
        } else {
            int ow = 0, oh = 0, orgba = 0;
            uint32_t *opx = NULL;
            if (sdl3vk_overlay_extent(&ow, &oh, &orgba) > 0)
                opx = (uint32_t *)calloc((size_t)ow * (size_t)oh, 4u);
            if (opx) {
                r[0] = sdl3vk_present_overlay(opx, ow * 4);
                for (int k = 1; k < 6; k++) {
                    if (k == 1 || k == 3) {
                        s_pstep_fault = PSTEP_ACQUIRE;
                        s_pstep_fault_result = VK_ERROR_SURFACE_LOST_KHR;
                    }
                    int w2 = 0, h2 = 0;
                    r[k] = sdl3vk_overlay_extent(&w2, &h2, &orgba) > 0 && w2 == ow && h2 == oh
                         ? sdl3vk_present_overlay(opx, ow * 4) : -9;
                    s_pstep_fault = PSTEP_NONE;
                    if (k == 1) unproven = !s_surface_proven;
                }
            }
            free(opx);
            sdl3vk_overlay_release();
            seq_ok = r[0] == 1 && r[1] == 2 && unproven && r[2] == 1 && r[3] == 2 && r[4] == 1 &&
                     r[5] == 1;
        }
        const int terminal = sdl3vk_renderer_terminal();
        const unsigned lines = s_terminal_lines;
        const uint64_t surfaces = s_surface_gen - surf0;
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        if (!warm || !pushed || !seq_ok || terminal || lines != 0 || surfaces != 2 ||
            verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': warm=%d pushed=%d presents %d %d %d %d %d %d (want "
                            "%s) replacement unproven=%d (want 1) terminal=%d in %u line(s) "
                            "(want 0) surfaces replaced %llu (want 2), %d validation error(s), "
                            "%d object(s) alive after shutdown\n", name, warm, pushed, r[0],
                    r[1], r[2], r[3], r[4], r[5],
                    c == 0 ? "-1 1 1, rest 0" : "1 2 1 2 1 1", unproven, terminal, lines,
                    (unsigned long long)surfaces, verrors, live);
            ok = 0;
        }
        cases++;
    }

    /* (j) The pause overlay is open when the app goes to the background and returns, and the
     * surface it then replaces, in the overlay's extent query as the pause loop asks it first,
     * cannot be made or presented to. The query reports the terminal renderer (-1), not "nothing
     * to show now" (0), so the pause loop ends (gui_pause_frame) and the game meets the terminal
     * renderer, with the one line the same failure names outside a pause (c). Each later query
     * and overlay present is refused. */
    for (int c = 0; ok && c < 2; c++) {
        const char *name = c == 0 ? "pause overlay, background and return, new surface cannot be "
                                    "made"
                                  : "pause overlay, background and return, new surface cannot be "
                                    "presented to";
        if (!sdl3vk_init("Nakagawa GPU surface selftest")) {
            fprintf(stderr, "surface: init before '%s' failed\n", name);
            ok = 0;
            break;
        }
        const int verr0 = sdl3vk_validation_error_count();
        const int warm = surface_test_warm(px);
        const uint64_t surf0 = s_surface_gen;
        int ow = 0, oh = 0, orgba = 0, shown = 0, pushed = 0, polled = 0, reached = 0;
        int extent = 0, again = 0, refused = 0, terminal = 0, reported = 0;
        unsigned lines = 0;
        uint32_t *opx = NULL;
        if (sdl3vk_overlay_extent(&ow, &oh, &orgba) > 0 &&
            (opx = (uint32_t *)calloc((size_t)ow * (size_t)oh, 4u)) != NULL) {
            shown = sdl3vk_present_overlay(opx, ow * 4);
            pushed = surface_test_push(SDL_EVENT_WILL_ENTER_BACKGROUND) &&
                     surface_test_push(SDL_EVENT_DID_ENTER_BACKGROUND) &&
                     surface_test_push(SDL_EVENT_WILL_ENTER_FOREGROUND) &&
                     surface_test_push(SDL_EVENT_DID_ENTER_FOREGROUND);
            polled = sdl3vk_poll();   /* the pause loop's poll reads the four events */
            s_surface_create_fault = c == 0;
            s_surface_support_fault = c == 1;
            int w2 = 0, h2 = 0;
            extent = sdl3vk_overlay_extent(&w2, &h2, &orgba);
            reached = s_surface_create_fault == 0 && s_surface_support_fault == 0;
            s_surface_create_fault = s_surface_support_fault = 0;
            terminal = sdl3vk_renderer_terminal();
            reported = s_terminal_reported;
            again = sdl3vk_overlay_extent(&w2, &h2, &orgba);
            refused = sdl3vk_present_overlay(opx, ow * 4);
            lines = s_terminal_lines;
        }
        free(opx);
        sdl3vk_overlay_release();
        const uint64_t surfaces = s_surface_gen - surf0;
        const int verrors = sdl3vk_validation_error_count() - verr0;
        sdl3vk_shutdown();
        const int live = lifecycle_live_objects();
        if (!warm || shown != 1 || !pushed || polled != 1 || !reached || extent != -1 ||
            !terminal || !reported || lines != 1 || again != -1 || refused != -1 ||
            surfaces != 0 || verrors != 0 || live != 0) {
            fprintf(stderr, "surface: '%s': warm=%d shown=%d (want 1) pushed=%d polled=%d "
                            "reached=%d extent query=%d (want -1) terminal=%d named %d in %u "
                            "line(s) (want 1, 1, 1) next query=%d present=%d (want -1, -1) "
                            "surfaces replaced %llu (want 0), %d validation error(s), %d "
                            "object(s) alive after shutdown\n", name, warm, shown, pushed, polled,
                    reached, extent, terminal, reported, lines, again, refused,
                    (unsigned long long)surfaces, verrors, live);
            ok = 0;
        }
        cases++;
    }

    if (keep) vkDestroyInstance(keep, NULL);
    if (keep_video) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        s_lifecycle_test_holds_video = 0;
    }
    if (sdl3vk_validation_error_count() != errors) {
        fprintf(stderr, "surface: %d validation error(s)\n", sdl3vk_validation_error_count() - errors);
        ok = 0;
    }
    if (!ok) return 1;
    printf("gpu surface selftest: %d window-surface cases OK\n", cases);
    return 0;
}

/* The presentation upscaler as gui.c registers it (SR_PRESENT_UPSCALE=fsr1), remembering what
 * it was asked for and what it returned. */
static Fsr1Pass *s_test_fsr;
static VkImage s_test_fsr_out;
static int s_test_fsr_calls, s_test_fsr_args[4];

static void *test_fsr_pass(void *cmd, void *src, int w, int h, int out_w, int out_h, int kind,
                           int *anime4k) {
    (void)kind; (void)anime4k;
    s_test_fsr_calls++;
    s_test_fsr_args[0] = w;
    s_test_fsr_args[1] = h;
    s_test_fsr_args[2] = out_w;
    s_test_fsr_args[3] = out_h;
    s_test_fsr_out = fsr1_record(s_test_fsr, (VkCommandBuffer)cmd, (VkImage)src, w, h, out_w,
                                 out_h, FSR1_SHARPNESS_DEFAULT);
    return (void *)s_test_fsr_out;
}

/* A UI pass as gui.c registers one for the render notice and Ace Mode's UI (#2151): it paints
 * the whole swapchain image after the frame blit, which no capture or guest byte may see. */
static int s_test_ui_calls;

static int test_ui_pass(const Sdl3VkUiPass *p) {
    VkCommandBuffer cmd = (VkCommandBuffer)p->cmd;
    VkImage image = (VkImage)p->images[p->image_index];
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &b);
    VkClearColorValue magenta = {{1.0f, 0.0f, 1.0f, 1.0f}};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1,
                         &b.subresourceRange);
    s_test_ui_calls++;
    return 1;
}

/* The slowed-logic notice's route: a pass that records nothing, answers `s_test_logic_drew`, and
 * keeps what the presenter asked of it. */
static int s_test_logic_drew, s_test_logic_asked, s_test_logic_calls;
static unsigned s_test_logic_rate_seen;

static int test_logic_pass(const Sdl3VkUiPass *p) {
    s_test_logic_calls++;
    s_test_logic_asked = p->logic_slowed;
    s_test_logic_rate_seen = p->logic_rate;
    return s_test_logic_drew;
}

static int test_logic_slowed(unsigned *rate) {
    if (rate) *rate = 52u;
    return 1;
}

/* A whole file, to compare two captures byte for byte; NULL when it cannot be read. */
static uint8_t *test_read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    long n;
    *size = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (data = (uint8_t *)malloc((size_t)n)) != NULL) {
        *size = fread(data, 1, (size_t)n, f);
        if (*size != (size_t)n) { free(data); data = NULL; *size = 0; }
    }
    fclose(f);
    return data;
}

#ifdef SR_VIDEOREC_SELFTEST
/* The recording's upscaler as gui.c registers it (sdl3vk_set_record_upscale_pass), on its own
 * pass, remembering what it was asked for. */
static Fsr1Pass *s_test_rec_fsr;
static int s_test_rec_calls, s_test_rec_args[4];

static void *test_rec_fsr_pass(void *cmd, void *src, int w, int h, int out_w, int out_h,
                               int kind, int *anime4k) {
    (void)kind; (void)anime4k;
    s_test_rec_calls++;
    s_test_rec_args[0] = w;
    s_test_rec_args[1] = h;
    s_test_rec_args[2] = out_w;
    s_test_rec_args[3] = out_h;
    return (void *)fsr1_record(s_test_rec_fsr, (VkCommandBuffer)cmd, (VkImage)src, w, h, out_w,
                               out_h, FSR1_SHARPNESS_DEFAULT);
}

/* The picture the last present recorded (BGRX, tightly packed) and the filter rec_record named
 * for it, read from its readback once that present has finished. */
static int test_rec_read(uint8_t *out, size_t bytes, int *filter) {
    PresentFrame *f = s_rec_pending;
    if (!f || !wait_present_frame(f)) return 0;
    CapBuffer *c = &s_rec_buf[f - s_frame];
    if (!cap_invalidate_readback(c) || c->alloc < bytes) return 0;
    memcpy(out, c->map, bytes);
    *filter = s_rec_filter[f - s_frame];
    return 1;
}

/* The recording's upscaler when gui.c could not make its pass: asked, it returns nothing. */
static void *test_rec_null_pass(void *cmd, void *src, int w, int h, int out_w, int out_h,
                                int kind, int *anime4k) {
    (void)cmd; (void)src; (void)w; (void)h; (void)out_w; (void)out_h; (void)kind; (void)anime4k;
    s_test_rec_calls++;
    return NULL;
}

/* One recorded present of src at a w x h recording under scaling `mode` and the recording
 * upscaler `pass` (or none): its picture and the filter rec_record named. The slot is then let
 * go unwritten, so a picture of another size never reaches the recorder. */
static int test_rec_case(VkImage src, int mode, Sdl3VkUpscalePass pass, int w, int h,
                         uint8_t *pic, int *filter) {
    s_scaling = mode;
    s_rec_w = (uint32_t)w;
    s_rec_h = (uint32_t)h;
    s_test_rec_calls = 0;
    *filter = -1;
    sdl3vk_set_record_upscale_pass(pass);
    int ok = sdl3vk_present_image_ex((void *)src, PSP_W, PSP_H) == 1 &&
             test_rec_read(pic, (size_t)w * (size_t)h * 4u, filter);
    sdl3vk_set_record_upscale_pass(NULL);
    s_rec_pending = NULL;
    return ok;
}

/* A separate FSR 1 pass's output for src at w x h, read back into ref (RGBA, tightly packed). */
static int test_rec_reference(Fsr1Pass *pass, CapBuffer *ref, VkImage src, int w, int h) {
    if (!cap_ensure(ref, (uint32_t)w, (uint32_t)h)) return 0;
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s_cmd, &bi) != VK_SUCCESS) return 0;
    VkImage out = fsr1_record(pass, s_cmd, src, PSP_W, PSP_H, w, h, FSR1_SHARPNESS_DEFAULT);
    int ok = out && cap_record(s_cmd, ref, out, w, h);
    ok = vkEndCommandBuffer(s_cmd) == VK_SUCCESS && ok;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cmd;
    return ok && vkQueueSubmit(s_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS &&
           vkQueueWaitIdle(s_queue) == VK_SUCCESS && cap_invalidate_readback(ref);
}

/* Pixels of the BGRX picture (pw wide) at x0,y0 that differ from the RGBA reference rw x rh;
 * the first is put in *first_x, *first_y. */
static size_t test_rec_diff(const uint8_t *pic, int pw, int x0, int y0, const uint8_t *ref,
                            int rw, int rh, int *first_x, int *first_y) {
    size_t bad = 0;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            const uint8_t *a = pic + ((size_t)(y0 + y) * (size_t)pw + (size_t)(x0 + x)) * 4u;
            const uint8_t *b = ref + ((size_t)y * (size_t)rw + (size_t)x) * 4u;
            if (a[0] != b[2] || a[1] != b[1] || a[2] != b[0]) {
                if (!bad++) { *first_x = x0 + x; *first_y = y0 + y; }
            }
        }
    return bad;
}
#endif

/* ---- present pacing: the guest clock never waits for the display ------------------------
 * A fake display (the acquire seam) holds every swapchain image: an acquire that may wait blocks
 * for PACE_TEST_BLOCK_MS, as a full FIFO queue does, and one that may not returns VK_NOT_READY.
 * Presents paced to the guest's 59.94 Hz VBLANK must keep that cadence: none may wait, each is
 * reported DISPLAY_BUSY and counted, and presents resume once the display frees. With no-wait
 * off (deterministic-clock and turbo runs) the present still waits and shows the frame. */
#define PACE_TEST_BLOCK_MS 50u
#define PACE_TEST_FRAMES   12
/* Presents that may take longer than a VBLANK period: a loaded host may stall the test's thread for
 * a scheduler quantum inside one, which says nothing about the present path. A present that waits
 * for the busy display (PACE_TEST_BLOCK_MS) is late every time. */
#define PACE_TEST_LATE_MAX 2
static int s_pace_test_busy;     /* the fake display holds every image */
static int s_pace_test_waits;    /* acquires that were allowed to wait while it did */
static int s_pace_test_acquires; /* every acquire the seam saw */
static int s_pace_test_slot_pending;   /* the fake frame slot's submission has not finished */

static VkResult pace_test_fence_status(VkDevice dev, VkFence fence) {
    return s_pace_test_slot_pending ? VK_NOT_READY : vkGetFenceStatus(dev, fence);
}

static VkResult pace_test_acquire(VkDevice dev, VkSwapchainKHR swap, uint64_t timeout,
                                  VkSemaphore sem, VkFence fence, uint32_t *idx) {
    s_pace_test_acquires++;
    if (s_pace_test_busy) {
        if (timeout == 0) return VK_NOT_READY;
        s_pace_test_waits++;
        SDL_Delay(PACE_TEST_BLOCK_MS);
    }
    return vkAcquireNextImageKHR(dev, swap, timeout, sem, fence, idx);
}

/* A show pass that only counts its calls. */
static int s_pace_test_pass_calls;
static void *pace_test_show_pass(void *cmd, void *src, int *w, int *h) {
    (void)cmd; (void)src; (void)w; (void)h;
    s_pace_test_pass_calls++;
    return NULL;
}

static int sdl3vk_present_pacing_selftest(void) {
    static uint32_t px[PSP_W * PSP_H];
    const uint64_t period = 16683333u;   /* one guest VBLANK, 1001/60000 s */
    int ok = 1;
    sdl3vk_set_present_nowait(1);
    s_acquire_fn = pace_test_acquire;

    /* The real display may be busy too, so the warm-up only requires no failure. */
    for (int k = 0; k < PRESENT_FRAMES; k++) {
        int r = sdl3vk_present_rgba(px);
        if (r != 1 && r != SDL3VK_PRESENT_DISPLAY_BUSY) {
            fprintf(stderr, "pacing: warm-up present %d returned %d\n", k, r); ok = 0;
        }
    }

    s_pace_test_busy = 1;
    s_pace_test_waits = 0;
    const uint64_t busy_before = sdl3vk_present_busy_count();
    int busy = 0, late = 0;
    uint64_t worst = 0;
    const uint64_t t0 = SDL_GetTicksNS();
    for (int k = 0; k < PACE_TEST_FRAMES; k++) {
        const uint64_t due = t0 + (uint64_t)k * period;
        uint64_t now = SDL_GetTicksNS();
        if (now < due) SDL_DelayPrecise(due - now);
        /* Only the present's own time is measured: a late wake from the delay, or a stall between
         * presents, is the host's, not the present path's. */
        const uint64_t start = SDL_GetTicksNS();
        if (sdl3vk_present_rgba(px) == SDL3VK_PRESENT_DISPLAY_BUSY) busy++;
        const uint64_t spent = SDL_GetTicksNS() - start;
        /* A present that takes a whole period holds the guest past its next VBLANK. */
        if (spent > period) late++;
        if (spent > worst) worst = spent;
    }
    const uint64_t counted = sdl3vk_present_busy_count() - busy_before;
    if (s_pace_test_waits != 0 || late > PACE_TEST_LATE_MAX || busy != PACE_TEST_FRAMES ||
        counted != (uint64_t)PACE_TEST_FRAMES) {
        fprintf(stderr, "pacing: with the display holding every image, %d of %d present(s) waited "
                        "for it, %d present(s) took longer than a guest VBLANK period (worst %.1f ms), "
                        "%d present(s) reported DISPLAY_BUSY and %llu were counted (want 0, at most "
                        "%d, %d, %d)\n", s_pace_test_waits, PACE_TEST_FRAMES, late, (double)worst / 1e6,
                busy, (unsigned long long)counted, PACE_TEST_LATE_MAX, PACE_TEST_FRAMES, PACE_TEST_FRAMES);
        ok = 0;
    }

    /* Smooth motion's held present is a game-frame present too: with the display holding every
     * image it returns DISPLAY_BUSY before its show pass runs. Nothing of the held frame was
     * shown, so its caller still owes that frame (gui_ace_vblank). */
    {
        Sdl3VkShowPass saved = s_show_pass;
        s_pace_test_pass_calls = 0;
        sdl3vk_set_show_pass(pace_test_show_pass);
        const int held = sdl3vk_present_held();
        sdl3vk_set_show_pass(saved);
        if (held != SDL3VK_PRESENT_DISPLAY_BUSY || s_pace_test_pass_calls != 0) {
            fprintf(stderr, "pacing: with the display holding every image, the held present returned %d "
                            "and ran its show pass %d time(s) (want DISPLAY_BUSY and 0)\n", held,
                    s_pace_test_pass_calls);
            ok = 0;
        }
    }

    /* The display frees: presents are shown again (the real display may still need a VBLANK). */
    s_pace_test_busy = 0;
    int shown = 0;
    for (int k = 0; k < 30 && !shown; k++) {
        shown = sdl3vk_present_rgba(px) == 1;
        if (!shown) SDL_DelayPrecise(period);
    }
    if (!shown) { fprintf(stderr, "pacing: presents did not resume once the display freed\n"); ok = 0; }

    /* The frame slot about to be reused is still pending (a FIFO display that has not released
     * its acquire semaphore, or a GPU frames behind): the present returns DISPLAY_BUSY before
     * any acquire, keeps the slot, and the next present after it signals is shown. Every slot is
     * submitted first, so the poll reaches the fence. */
    int submitted = 0;
    for (int k = 0; k < 60 && submitted < PRESENT_FRAMES; k++) {
        if (sdl3vk_present_rgba(px) == 1) submitted++;
        else SDL_DelayPrecise(period);
    }
    s_pace_test_slot_pending = 1;
    s_fence_status_fn = pace_test_fence_status;
    const int acquires_before = s_pace_test_acquires;
    const int cursor_before = s_frame_cursor;
    const uint64_t slot_busy_before = s_present_busy_slot;
    const int pending_result = sdl3vk_present_rgba(px);
    const int slot_acquires = s_pace_test_acquires - acquires_before;
    const int slot_cursor = s_frame_cursor;
    const uint64_t slot_counted = s_present_busy_slot - slot_busy_before;
    s_pace_test_slot_pending = 0;
    int after_slot = 0;
    for (int k = 0; k < 30 && after_slot != 1; k++) {
        after_slot = sdl3vk_present_rgba(px);
        if (after_slot != 1) SDL_DelayPrecise(period);
    }
    s_fence_status_fn = NULL;
    if (submitted < PRESENT_FRAMES || pending_result != SDL3VK_PRESENT_DISPLAY_BUSY ||
        slot_acquires != 0 || slot_cursor != cursor_before || slot_counted != 1u ||
        after_slot != 1) {
        fprintf(stderr, "pacing: with the frame slot pending (%d of %d slots submitted first), the "
                        "present returned %d after %d acquire(s), moved the slot cursor %d -> %d, "
                        "counted %llu slot drop(s), and the next present returned %d (want "
                        "DISPLAY_BUSY, 0 acquires, the cursor kept, 1 counted, then 1)\n",
                submitted, PRESENT_FRAMES, pending_result, slot_acquires, cursor_before,
                slot_cursor, (unsigned long long)slot_counted, after_slot);
        ok = 0;
    }

    /* No-wait off: the present waits for the display and shows the frame. */
    sdl3vk_set_present_nowait(0);
    s_pace_test_busy = 1;
    s_pace_test_waits = 0;
    int r = sdl3vk_present_rgba(px);
    if (r != 1 || s_pace_test_waits != 1) {
        fprintf(stderr, "pacing: with no-wait off the present returned %d after %d wait(s) "
                        "(want 1 after 1)\n", r, s_pace_test_waits);
        ok = 0;
    }
    s_pace_test_busy = 0;
    s_acquire_fn = NULL;
    if (ok) printf("gpu capture selftest: present pacing OK (%d presents, none waited for a busy "
                   "display; %d took longer than a VBLANK period, the longest %.1f ms)\n", PACE_TEST_FRAMES,
                   late, (double)worst / 1e6);
    return ok;
}

/* ---- what a show pass learns of its present, and the finished-submission count ---------------
 * Each show pass is told once how its present ended and which submission carries its commands.
 * The count of finished submissions moves only when the presenter has seen a fence signalled: it
 * never passes a pending submission. A present that fails after its submission (an out-of-date
 * swapchain) keeps its slot and is told it failed; the swapchain rebuild waits for every frame,
 * so the count then reaches that submission. */
static int s_done_test_calls, s_done_test_result;
static uint64_t s_done_test_submission;
static void done_test_show_done(int result, uint64_t submission) {
    s_done_test_calls++;
    s_done_test_result = result;
    s_done_test_submission = submission;
}

static int sdl3vk_present_completed_selftest(void) {
    static uint32_t px[PSP_W * PSP_H];
    int ok = 1;
    const Sdl3VkShowPass saved_pass = s_show_pass;
    const Sdl3VkShowDone saved_done = s_show_done;
    sdl3vk_set_present_nowait(0);
    sdl3vk_set_show_pass(pace_test_show_pass);
    sdl3vk_set_show_done(done_test_show_done);
    s_done_test_calls = 0;

    const uint64_t first = s_present_submitted + 1u;
    const int shown = sdl3vk_present_rgba(px);
    const int shown_calls = s_done_test_calls, shown_result = s_done_test_result;
    const uint64_t shown_submission = s_done_test_submission;
    const uint64_t completed_after_shown = sdl3vk_present_completed();   /* its fence not yet seen */
    const int cursor = s_frame_cursor;
    sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
    const int failed = sdl3vk_present_rgba(px);
    const int failed_calls = s_done_test_calls, failed_result = s_done_test_result;
    const uint64_t failed_submission = s_done_test_submission;
    const int failed_cursor = s_frame_cursor;
    const uint64_t completed_after_fail = sdl3vk_present_completed();
    const int again = sdl3vk_present_rgba(px);
    const uint64_t completed_again = sdl3vk_present_completed();
    if (shown != 1 || shown_calls != 1 || shown_result != 1 || shown_submission != first ||
        completed_after_shown >= first || failed != -1 || failed_calls != 2 || failed_result != -1 || failed_submission != first + 1u ||
        failed_cursor != cursor || completed_after_fail != first + 1u || again != 1 ||
        s_done_test_calls != 3 || s_done_test_submission != first + 2u || completed_again < first + 1u ||
        completed_again > s_present_submitted) {
        fprintf(stderr, "show done: shown %d (%d call(s), %d, submission %llu of %llu, %llu finished); out "
                        "of date %d (%d, submission %llu, cursor %d -> %d, %llu finished); next %d (%d "
                        "call(s), submission %llu, %llu finished) (want 1, 1 call, 1, the first, an older "
                        "one finished; -1, -1, the next, "
                        "the cursor kept, finished by the rebuild; 1, 3 calls, the one after, the "
                        "failed one finished)\n",
                shown, shown_calls, shown_result, (unsigned long long)shown_submission,
                (unsigned long long)first, (unsigned long long)completed_after_shown, failed, failed_result, (unsigned long long)failed_submission,
                cursor, failed_cursor, (unsigned long long)completed_after_fail, again,
                s_done_test_calls, (unsigned long long)s_done_test_submission,
                (unsigned long long)completed_again);
        ok = 0;
    }

    /* A slot still pending under no-wait: the present returns DISPLAY_BUSY before its show pass, so
     * no pass is told anything, and the count does not move. Every slot is submitted first, so the
     * poll reaches the fence. */
    for (int k = 0; k < PRESENT_FRAMES; k++) (void)sdl3vk_present_rgba(px);
    sdl3vk_set_present_nowait(1);
    s_pace_test_slot_pending = 1;
    s_fence_status_fn = pace_test_fence_status;
    const int calls_before = s_done_test_calls;
    const uint64_t completed_before = sdl3vk_present_completed();
    const int pending = sdl3vk_present_rgba(px);
    const uint64_t completed_pending = sdl3vk_present_completed();
    s_pace_test_slot_pending = 0;
    s_fence_status_fn = NULL;
    sdl3vk_set_present_nowait(0);
    /* Every fence waited: the count is the newest submission. */
    const int all = wait_all_present_frames();
    if (pending != SDL3VK_PRESENT_DISPLAY_BUSY || s_done_test_calls != calls_before ||
        completed_pending != completed_before || !all ||
        sdl3vk_present_completed() != s_present_submitted) {
        fprintf(stderr, "show done: with the slot pending the present returned %d, told %d pass(es) and "
                        "moved the count %llu -> %llu; after every wait the count is %llu of %llu (want "
                        "DISPLAY_BUSY, none, unmoved, every submission)\n",
                pending, s_done_test_calls - calls_before, (unsigned long long)completed_before,
                (unsigned long long)completed_pending, (unsigned long long)sdl3vk_present_completed(),
                (unsigned long long)s_present_submitted);
        ok = 0;
    }
    sdl3vk_set_show_pass(saved_pass);
    sdl3vk_set_show_done(saved_done);
    if (ok) printf("gpu capture selftest: show pass outcomes and the finished-submission count OK\n");
    return ok;
}

static int sdl3vk_capture_selftest_body(void) {
    int ok = 1;
    int surface_failed = 0;

    /* ---- pure framebuffer-capture policy (fbcap_policy.h); no Vulkan needed --------- */
    if (sr_fbcap_owner(0, 0) != SR_FBCAP_NONE) {
        fprintf(stderr, "policy: nothing due must not own the slot\n"); ok = 0;
    }
    if (sr_fbcap_owner(1, 1) != SR_FBCAP_FBDUMP ||
        sr_fbcap_owner(1, 0) != SR_FBCAP_FBDUMP) {
        fprintf(stderr, "policy: SR_FBDUMP must outrank SR_FBSNAP\n"); ok = 0;
    }
    if (sr_fbcap_owner(0, 1) != SR_FBCAP_FBSNAP) {
        fprintf(stderr, "policy: SR_FBSNAP due must own the slot\n"); ok = 0;
    }
    {
        char p[128];
        if (!sr_fbcap_path(SR_FBCAP_FBSNAP, 12, p, sizeof p) ||
            strcmp(p, "build/snapshots/frame_0012.ppm") != 0) {
            fprintf(stderr, "policy: FBSNAP path wrong: '%s'\n", p); ok = 0;
        }
        if (!sr_fbcap_path(SR_FBCAP_FBDUMP, 0, p, sizeof p) ||
            strcmp(p, "present_source.ppm") != 0) {
            fprintf(stderr, "policy: FBDUMP path wrong: '%s'\n", p); ok = 0;
        }
        if (sr_fbcap_path(SR_FBCAP_NONE, 0, p, sizeof p) || p[0] != '\0') {
            fprintf(stderr, "policy: NONE owner must not produce a path\n"); ok = 0;
        }
    }
    if (sr_fbcap_exit_status(SR_FBCAP_NONE, 1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_NONE, -1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, 1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, 0) != 1 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, -1) != 1) {
        fprintf(stderr, "policy: exit-status table wrong\n"); ok = 0;
    }
    if (!ok) return 1;

    /* ---- readback memory type (#1296); no Vulkan needed ----------------------------- */
    /* An RTX 3080's types: 3 is visible+coherent but uncached, 4 adds HOST_CACHED. A capture
     * or probe buffer the CPU reads must take a cached type when one is allowed: the first
     * visible+coherent type made a full read of a 4x frame cost about a second. */
    {
        const uint32_t vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        const uint32_t coh = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const uint32_t cac = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        const uint32_t dev = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const uint32_t rtx3080[6] = { 0u, dev, dev, vis | coh, vis | coh | cac, dev | vis | coh };
        const uint32_t cached_noncoherent[3] = { vis | coh, vis | cac, 0u };
        const uint32_t noncoherent_only[2] = { dev, vis };
        int nc = -1;
        if (cap_pick_mem_type(rtx3080, 6u, 0x3fu, &nc) != 4u || nc != 0) {
            fprintf(stderr, "readback memory: the cached+coherent type must win\n"); ok = 0;
        }
        if (cap_pick_mem_type(rtx3080, 6u, 0x2fu, &nc) != 3u || nc != 0) {
            fprintf(stderr, "readback memory: without a cached type, the first visible+coherent\n");
            ok = 0;
        }
        if (cap_pick_mem_type(cached_noncoherent, 3u, 0x7u, &nc) != 1u || nc != 1) {
            fprintf(stderr, "readback memory: a cached non-coherent type beats an uncached "
                            "coherent one, and must be invalidated\n");
            ok = 0;
        }
        if (cap_pick_mem_type(noncoherent_only, 2u, 0x3u, &nc) != 1u || nc != 1) {
            fprintf(stderr, "readback memory: the last resort is any host-visible type\n"); ok = 0;
        }
        if (cap_pick_mem_type(rtx3080, 6u, 0x07u, &nc) != UINT32_MAX) {
            fprintf(stderr, "readback memory: no host-visible type allowed must be refused\n");
            ok = 0;
        }
    }
    if (!ok) return 1;

    /* ---- cap_test_verify_ppm closes a file it refuses, so the removal that follows works
     * (on Windows an open file cannot be removed) ------------------------------------- */
    {
        const char *bad = "selftest_bad_header.ppm";
        FILE *bf = fopen(bad, "wb");
        int wrote = bf && fputs("P6\n1 1\n255\n", bf) >= 0 && fwrite("abc", 1, 3, bf) == 3;
        if (bf) wrote &= fclose(bf) == 0;
        const int verified = wrote && cap_test_verify_ppm(bad, 2, 1);
        const int removed = remove(bad) == 0;
        if (!wrote || verified || !removed) {
            fprintf(stderr, "verify: a header-mismatch file was %s, verified %d (want 0), removed "
                            "%d (want 1)\n", wrote ? "written" : "not written", verified, removed);
            ok = 0;
        }
    }
    if (!ok) return 1;

    /* Every profile cell starts from the default bindings: no developer's profile reaches it. */
    profile_test_use_defaults();
    sdl3vk_reset_input_profile();

    /* ---- input profile mapping selftest (#357); no Vulkan needed ------------------- */
    if (!sdl3vk_input_profile_selftest()) {
        fprintf(stderr, "gpu capture selftest: input profile selftest failed\n");
        return 1;
    }

    /* ---- pause input claim and release mask (#1159); no Vulkan needed -------------- */
    if (!sdl3vk_pause_input_selftest()) {
        fprintf(stderr, "gpu capture selftest: pause input selftest failed\n");
        return 1;
    }

    /* ---- mouse aiming (#1791); no Vulkan needed ------------------------------------ */
    if (!sdl3vk_mouse_aim_selftest()) {
        fprintf(stderr, "gpu capture selftest: mouse aim selftest failed\n");
        return 1;
    }

    /* ---- mouse buttons (#1791); no Vulkan needed ----------------------------------- */
    if (!sdl3vk_mouse_buttons_selftest()) {
        fprintf(stderr, "gpu capture selftest: mouse buttons selftest failed\n");
        return 1;
    }

    /* ---- the on-screen keyboard's keys; no Vulkan needed ----------------------------- */
    if (!sdl3vk_text_keys_selftest()) {
        fprintf(stderr, "gpu capture selftest: text keys selftest failed\n");
        return 1;
    }

    /* ---- validation-layer requirement ---------------------------------------------- */
    {
        /* Every installed layer is read, however many there are; a layer installed between the
         * count and the list makes it VK_INCOMPLETE, and the entries written are still searched. */
        uint32_t n = 0;
        VkLayerProperties *lp = NULL;
        int have = 0;
        if (vkEnumerateInstanceLayerProperties(&n, NULL) == VK_SUCCESS && n > 0 &&
            (lp = (VkLayerProperties *)calloc(n, sizeof *lp)) != NULL) {
            const VkResult lr = vkEnumerateInstanceLayerProperties(&n, lp);
            if (lr == VK_SUCCESS || lr == VK_INCOMPLETE)
                for (uint32_t i = 0; i < n; i++)
                    if (strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) have = 1;
            free(lp);
        }
        if (!have) {
            fprintf(stderr, "gpu capture selftest: SKIP (VK_LAYER_KHRONOS_validation "
                            "not installed)\n");
            return 77;
        }
    }
    if (!cap_test_setenv("SR_VULKAN_VALIDATION", "1")) {
        fprintf(stderr, "gpu capture selftest: cannot set SR_VULKAN_VALIDATION\n");
        return 1;
    }
    const char *terminal_fault = getenv("SR_GPU_CAPTURE_TERMINAL_CASE");
    if (!terminal_fault || !terminal_fault[0]) {
        int lifecycle = sdl3vk_lifecycle_selftest();
        if (lifecycle != 0) return lifecycle;
        /* A failure here fails the run, once the capture cases below have run too. */
        int surface = sdl3vk_surface_selftest();
        if (surface == 77) return surface;
        surface_failed = surface != 0;
    }
    if (!sdl3vk_init("Nakagawa GPU capture selftest")) return cap_test_init_failed();
    /* Every "no validation error" check below means something only if the layer is on. */
    if (!s_validation_on) {
        fprintf(stderr, "gpu capture selftest: SR_VULKAN_VALIDATION=1 did not reach sdl3vk_init; "
                        "the validation layer is off\n");
        sdl3vk_shutdown();
        return 1;
    }

    /* ---- filter selection policy (issue #68) --------------------------------------- */
    /* Exact integer scaling must select VK_FILTER_NEAREST */
    if (sdl3vk_present_filter(480, 272, 960, 544) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 480x272 -> 960x544 must select NEAREST\n"); ok = 0;
    }
    if (sdl3vk_present_filter(960, 544, 960, 544) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 960x544 -> 960x544 must select NEAREST\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 480, 272) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 480x272 -> 480x272 must select NEAREST\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 1440, 816) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 480x272 -> 1440x816 must select NEAREST\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 1920, 1088) != VK_FILTER_NEAREST) {
        fprintf(stderr, "filter: 480x272 -> 1920x1088 must select NEAREST\n"); ok = 0;
    }
    /* Fractional and non-integer scaling must select VK_FILTER_LINEAR */
    if (sdl3vk_present_filter(480, 272, 720, 408) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 480x272 -> 720x408 (1.5x) must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 1200, 680) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 480x272 -> 1200x680 (2.5x) must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 1000, 566) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 480x272 -> 1000x566 fractional must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(960, 544, 1440, 816) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 960x544 -> 1440x816 fractional must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(480, 272, 960, 540) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: 480x272 -> 960x540 non-uniform must select LINEAR\n"); ok = 0;
    }
    if (sdl3vk_present_filter(0, 0, 960, 544) != VK_FILTER_LINEAR) {
        fprintf(stderr, "filter: invalid dimensions must select LINEAR\n"); ok = 0;
    }

    /* ---- CPU framebuffer path (B8G8R8A8 fbimg) -------------------------------------- */
    static uint32_t px[PSP_W * PSP_H];
    cap_test_fill_cpu(px, PSP_W, PSP_H);
#ifdef SR_VIDEOREC_SELFTEST
    /* Count the clear command from present_common itself. The initial 2x window is an exact
     * fit for 480x272; a 512x272 upload is wider than the same window and leaves top/bottom bars. */
    {
        const int saved_scaling = s_scaling;
        const int dw = (int)s_swap_ext.width, dh = (int)s_swap_ext.height;
        s_scaling = SDL3VK_SCALING_SHARP;
        Sdl3VkPresentPlan exact = sdl3vk_present_plan(s_scaling, PSP_W, PSP_H, dw, dh,
                                                       (int)s_max_image_dim);
        if (exact.x != 0 || exact.y != 0 || exact.w != dw || exact.h != dh || dw <= 0 || dh <= 0) {
            fprintf(stderr, "present clear: test window is %dx%d, not an exact fit for 480x272\n", dw, dh);
            ok = 0;
        } else {
            s_selftest_present_clear_commands = 0u;
            if (sdl3vk_present_rgba(px) != 1 || s_selftest_present_clear_commands != 0u) {
                fprintf(stderr, "present clear: exact-fit production present emitted %u clear command(s), want 0\n",
                        s_selftest_present_clear_commands);
                ok = 0;
            }
        }
        const int wide_w = PSP_W + 32;
        Sdl3VkPresentPlan bars = sdl3vk_present_plan(s_scaling, wide_w, PSP_H, dw, dh,
                                                      (int)s_max_image_dim);
        if (!sdl3vk_present_needs_clear(&bars, dw, dh)) {
            fprintf(stderr, "present clear: %dx%d source plan for %dx%d has no uncovered bar area\n",
                    wide_w, PSP_H, dw, dh);
            ok = 0;
        } else {
            uint32_t *wide = (uint32_t *)malloc((size_t)wide_w * PSP_H * sizeof(*wide));
            if (!wide) {
                fprintf(stderr, "present clear: could not allocate the synthetic wide source\n");
                ok = 0;
            } else {
                for (size_t i = 0; i < (size_t)wide_w * PSP_H; i++)
                    wide[i] = 0xff204080u ^ (uint32_t)(i * 0x00010101u);
                s_selftest_present_clear_commands = 0u;
                if (sdl3vk_present_rgba_ex(wide, wide_w, PSP_H) != 1 ||
                    s_selftest_present_clear_commands != 1u) {
                    fprintf(stderr, "present clear: letterboxed production present emitted %u clear command(s), want 1\n",
                            s_selftest_present_clear_commands);
                    ok = 0;
                }
                free(wide);
            }
        }
        s_scaling = saved_scaling;
        if (ok) puts("gpu capture selftest: production present emitted one bar clear and no exact-fit clear");
    }
#endif
    if (terminal_fault && terminal_fault[0]) {
        /* The surface-lost leg is the one present fault the renderer goes on from (#360): the
         * window's surface is replaced, so the faulted present is not shown and the renderer is
         * not terminal, and the next present is shown. The other legs stay terminal. */
        const int recovers = strcmp(terminal_fault, "surface-lost") == 0;
        VkResult fault_result;
        if (strcmp(terminal_fault, "surface-lost") == 0) {
            fault_result = VK_ERROR_SURFACE_LOST_KHR;
        } else if (strcmp(terminal_fault, "unclassified") == 0) {
            fault_result = (VkResult)-9999;
        } else if (strcmp(terminal_fault, "device-lost") == 0) {
            fault_result = VK_ERROR_DEVICE_LOST;
        } else {
            fprintf(stderr, "gpu capture selftest: unknown terminal fault case '%s'\n",
                    terminal_fault);
            ok = 0;
            fault_result = VK_SUCCESS;
        }
        if (ok) {
            uint64_t sem0 = sdl3vk_frame_semaphore_generation();
            if (!sr_capture_arm("selftest_terminal.ppm")) {
                fprintf(stderr, "terminal fault %s: capture arm failed\n", terminal_fault);
                ok = 0;
            } else {
                sdl3vk_present_fault_inject((int)fault_result);
                if (sdl3vk_present_rgba(px) != -1 || sr_capture_result() != -1) {
                    fprintf(stderr, "terminal fault %s: expected failed present and capture\n",
                            terminal_fault);
                    ok = 0;
                }
                if (sdl3vk_renderer_terminal() != !recovers) {
                    fprintf(stderr, "terminal fault %s: renderer terminal state %d (want %d)\n",
                            terminal_fault, sdl3vk_renderer_terminal(), !recovers);
                    ok = 0;
                }
                if (s_terminal_reported != !recovers || s_terminal_lines != (recovers ? 0u : 1u)) {
                    fprintf(stderr, "terminal fault %s: %u line(s) named a terminal transition "
                                    "(want %u)\n", terminal_fault, s_terminal_lines,
                            recovers ? 0u : 1u);
                    ok = 0;
                }
                if (sdl3vk_frame_semaphore_generation() != sem0) {
                    fprintf(stderr, "terminal fault %s: semaphore generation changed\n",
                            terminal_fault);
                    ok = 0;
                }
                if (recovers && (s_surface_gen != 1u || sdl3vk_present_rgba(px) != 1)) {
                    fprintf(stderr, "terminal fault %s: %llu surface(s) replaced (want 1), or the "
                                    "next present was not shown\n", terminal_fault,
                            (unsigned long long)s_surface_gen);
                    ok = 0;
                }
            }
        }
        if (sdl3vk_validation_error_count() != 0) {
            fprintf(stderr, "terminal fault %s: validation reported errors\n", terminal_fault);
            ok = 0;
        }
        sdl3vk_shutdown();
        if (sdl3vk_validation_error_count() != 0) {
            fprintf(stderr, "terminal fault %s: validation reported teardown errors\n",
                    terminal_fault);
            ok = 0;
        }
        if (!ok) return 1;
        printf("gpu capture selftest: %s %s OK\n",
               recovers ? "recovered fault" : "terminal fault", terminal_fault);
        return 0;
    }
    if (!sr_capture_arm("selftest_cpu.ppm")) {
        fprintf(stderr, "cpu: capture arm refused\n"); ok = 0;
    }
    if (sdl3vk_present_rgba(px) != 1) {
        fprintf(stderr, "cpu: present failed\n"); ok = 0;
    }
    if (sr_capture_result() != 1) {
        fprintf(stderr, "cpu: capture result %d (expected 1)\n", sr_capture_result());
        ok = 0;
    }
    if (strcmp(sr_capture_source_label(), "cpu-framebuffer") != 0) {
        fprintf(stderr, "cpu: source label '%s'\n", sr_capture_source_label());
        ok = 0;
    }
    if (!cap_test_verify_ppm("selftest_cpu.ppm", PSP_W, PSP_H)) {
        fprintf(stderr, "cpu: PPM bytes wrong (header, channel order, pitch, or truncation)\n");
        ok = 0;
    }
    remove("selftest_cpu.ppm");

    /* ---- the F3 HUD composes over the picture (#314, LIVE_FRAME_OVERLAY) ---------------
     * The performance HUD, and in developer mode the profile HUD, are copied into a rectangle
     * of the window's image after the frame blit: a rectangle inside the window that leaves
     * the rest of the picture showing, at the 16-pixel inset, and the HUD surface the software
     * renderer drew for it holds the HUD. The frame the game drew, the source the window is
     * blitted from, is untouched: a capture of the same present is the same bytes as
     * with the HUD off. Off again, nothing is copied. */
    {
        static const int modes[] = { SDL3VK_HUD_PERF, SDL3VK_HUD_PROFILE };
        static const int want_w[] = { HUD_W, PROFILE_HUD_W };
        static const int want_h[] = { HUD_H, PROFILE_HUD_H };
        const int saved_developer = s_developer;
        const int saved_mode = s_hud_enabled;
        s_developer = 1;
        for (int m = 0; m < 2; m++) {
            const unsigned before = s_hud_last.copies;
            const char *what = m ? "profile HUD" : "performance HUD";
            hud_set_mode(modes[m]);
            if (!sr_capture_arm("selftest_hud.ppm") || sdl3vk_present_rgba(px) != 1 ||
                sr_capture_result() != 1) {
                fprintf(stderr, "hud: %s present or capture failed\n", what); ok = 0;
                continue;
            }
            if (!cap_test_verify_ppm("selftest_hud.ppm", PSP_W, PSP_H)) {
                fprintf(stderr, "hud: with the %s on, the captured frame is not the frame the "
                                "game drew\n", what);
                ok = 0;
            }
            remove("selftest_hud.ppm");
            if (s_hud_last.copies != before + 1u || s_hud_last.w != want_w[m] ||
                s_hud_last.h != want_h[m]) {
                fprintf(stderr, "hud: %s copies %u (want %u), %dx%d (want %dx%d)\n", what,
                        s_hud_last.copies - before, 1u, s_hud_last.w, s_hud_last.h, want_w[m],
                        want_h[m]);
                ok = 0;
                continue;
            }
            if (s_hud_last.x != 16 || s_hud_last.y != 16 ||
                s_hud_last.x + s_hud_last.w > s_hud_last.swap_w ||
                s_hud_last.y + s_hud_last.h > s_hud_last.swap_h ||
                (s_hud_last.w >= s_hud_last.swap_w && s_hud_last.h >= s_hud_last.swap_h)) {
                fprintf(stderr, "hud: %s rectangle (%d,%d %dx%d) in a %dx%d image does not sit "
                                "over part of the picture\n", what, s_hud_last.x, s_hud_last.y,
                        s_hud_last.w, s_hud_last.h, s_hud_last.swap_w, s_hud_last.swap_h);
                ok = 0;
            }
            /* What was copied is the HUD: its background and border, as drawn. */
            Uint8 r = 0, g = 0, b = 0, a = 0, br = 0, bg = 0, bb = 0, ba = 0;
            if (!s_hud_surf || !SDL_ReadSurfacePixel(s_hud_surf, 2, s_hud_last.h - 2, &r, &g, &b, &a) ||
                !SDL_ReadSurfacePixel(s_hud_surf, 0, 0, &br, &bg, &bb, &ba) ||
                r != 20 || g != 24 || b != 32 || br != 70 || bg != 90 || bb != 130) {
                fprintf(stderr, "hud: %s surface shows %u,%u,%u inside and %u,%u,%u on the "
                                "border\n", what, r, g, b, br, bg, bb);
                ok = 0;
            }
        }
        hud_set_mode(SDL3VK_HUD_OFF);
        {
            const unsigned before = s_hud_last.copies;
            if (sdl3vk_present_rgba(px) != 1 || s_hud_last.copies != before) {
                fprintf(stderr, "hud: with the HUD off a present still copied a HUD\n");
                ok = 0;
            }
        }
        /* A swapchain that is not 8-bit RGBA or BGRA (a 10-bit or a half-float surface, which
         * create_swapchain takes when the surface offers no B8G8R8A8_UNORM) gets no HUD: the
         * HUD's 4-byte texels would show the wrong colours there, or be too few for the copy.
         * The format stands in for the one create_swapchain stored. */
        {
            static const VkFormat other[] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                              VK_FORMAT_R16G16B16A16_SFLOAT };
            const VkFormat saved_fmt = s_swap_fmt;
            hud_set_mode(SDL3VK_HUD_PERF);
            for (size_t i = 0; i < sizeof other / sizeof other[0]; i++) {
                const unsigned before = s_hud_last.copies;
                s_swap_fmt = other[i];
                const int shown = sdl3vk_present_rgba(px);
                s_swap_fmt = saved_fmt;
                if (shown != 1 || s_hud_last.copies != before) {
                    fprintf(stderr, "hud: a present to swapchain format %d returned %d and copied "
                                    "%u HUD(s) (want 1 and none)\n", (int)other[i], shown,
                            s_hud_last.copies - before);
                    ok = 0;
                }
            }
            hud_set_mode(SDL3VK_HUD_OFF);
        }
        s_developer = saved_developer;
        hud_set_mode(saved_mode);
    }

    /* ---- the slowed-logic notice never shares the readout's corner -----------------------
     * With the HUD off, slowed 60 FPS logic goes to the UI pass, which shows it under its
     * readout; the presenter's own box, copied to the same top-left corner, would sit under the
     * readout's chip. So the box is copied only with no pass, or when the pass drew nothing; with
     * the HUD on the HUD carries the line and the pass is not asked. */
    {
        int (*saved_cb)(unsigned *) = s_logic_slowed_cb;
        const int saved_mode = s_hud_enabled;
        char words[64];
        s_logic_slowed_cb = test_logic_slowed;
        hud_set_mode(SDL3VK_HUD_OFF);
        static const struct { int pass, drew, hud; unsigned copies; int asked; const char *what; } cases[] = {
            { 0, 0, 0, 1u, 0, "no UI pass: the presenter's box" },
            { 1, 1, 0, 0u, 1, "a pass that drew it: no box in its corner" },
            { 1, 0, 0, 1u, 1, "a pass that drew nothing: the presenter's box" },
            { 1, 1, 1, 1u, 0, "the HUD on: in the HUD, the pass not asked" },
        };
        for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            const unsigned before = s_hud_last.copies;
            const int calls = s_test_logic_calls;
            s_test_logic_drew = cases[c].drew;
            s_test_logic_asked = -1;
            s_test_logic_rate_seen = 0u;
            hud_set_mode(cases[c].hud ? SDL3VK_HUD_PERF : SDL3VK_HUD_OFF);
            sdl3vk_set_ui_pass(cases[c].pass ? test_logic_pass : NULL);
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "logic notice: %s: the present failed\n", cases[c].what); ok = 0;
            }
            sdl3vk_set_ui_pass(NULL);
            const int want_h = cases[c].hud ? HUD_H + LOGIC_NOTICE_H : LOGIC_NOTICE_H;
            if (s_hud_last.copies - before != cases[c].copies ||
                (cases[c].copies && (s_hud_last.h != want_h || s_hud_last.w != HUD_W))) {
                fprintf(stderr, "logic notice: %s: %u box copies (want %u), last %dx%d (want %dx%d)\n",
                        cases[c].what, s_hud_last.copies - before, cases[c].copies, s_hud_last.w,
                        s_hud_last.h, HUD_W, want_h);
                ok = 0;
            }
            if (cases[c].pass && (s_test_logic_calls != calls + 1 || s_test_logic_asked != cases[c].asked ||
                                  (cases[c].asked && s_test_logic_rate_seen != 52u))) {
                fprintf(stderr, "logic notice: %s: the pass ran %d time(s), asked %d (want %d) at %u/s\n",
                        cases[c].what, s_test_logic_calls - calls, s_test_logic_asked, cases[c].asked,
                        s_test_logic_rate_seen);
                ok = 0;
            }
        }
        /* The HUD on in a window too short for it (300x100 against the 240x130 HUD and notice):
         * the HUD draws nothing, so the notice goes to the pass, else to the presenter's own
         * box, which fits. The extent stands in for the one create_swapchain stored. */
        {
            static const struct { int pass, drew; unsigned copies; const char *what; } small[] = {
                { 0, 0, 1u, "HUD on, window too small, no UI pass: the presenter's box" },
                { 1, 1, 0u, "HUD on, window too small, a pass that drew it" },
            };
            const VkExtent2D saved_ext = s_swap_ext;
            hud_set_mode(SDL3VK_HUD_PERF);
            for (size_t c = 0; c < sizeof small / sizeof small[0]; c++) {
                const unsigned before = s_hud_last.copies;
                const int calls = s_test_logic_calls;
                s_test_logic_drew = small[c].drew;
                s_test_logic_asked = -1;
                sdl3vk_set_ui_pass(small[c].pass ? test_logic_pass : NULL);
                s_swap_ext.width = 300u;
                s_swap_ext.height = 100u;
                const int shown = sdl3vk_present_rgba(px);
                s_swap_ext = saved_ext;
                sdl3vk_set_ui_pass(NULL);
                if (shown != 1 || s_hud_last.copies - before != small[c].copies ||
                    (small[c].copies && (s_hud_last.w != HUD_W || s_hud_last.h != LOGIC_NOTICE_H)) ||
                    (small[c].pass && (s_test_logic_calls != calls + 1 || s_test_logic_asked != 1))) {
                    fprintf(stderr, "logic notice: %s: present %d, %u box copies (want %u), last "
                                    "%dx%d, the pass asked %d (want 1)\n", small[c].what, shown,
                            s_hud_last.copies - before, small[c].copies, s_hud_last.w,
                            s_hud_last.h, s_test_logic_asked);
                    ok = 0;
                }
            }
        }
        if (strcmp(sdl3vk_logic_notice_text(words, sizeof(words), 52u), "60 FPS logic: 52/s - SLOWED") != 0) {
            fprintf(stderr, "logic notice: the words read \"%s\"\n", words);
            ok = 0;
        }
        s_logic_slowed_cb = saved_cb;
        hud_set_mode(saved_mode);
    }

    /* ---- a sharp-scaling image that could not be made is tried again --------------------
     * In a 900x500 place a 480x272 frame is a fractional fit: sharp scaling enlarges it with
     * nearest filtering first, into an image of its own. When that image cannot be made the
     * present is one linear blit from the frame, and later presents of that size with the same
     * swapchain do not try again; a new swapchain (a resize, a re-init) does. The extent stands
     * in for the one create_swapchain stored. */
    {
        const int saved_scaling = s_scaling;
        const VkExtent2D saved_ext = s_swap_ext;
        s_scaling = SDL3VK_SCALING_SHARP;
        const Sdl3VkPresentPlan plan = sdl3vk_present_plan(s_scaling, PSP_W, PSP_H, 900, 500,
                                                           (int)s_max_image_dim);
        int sw[3] = { 0, 0, 0 }, shown = 1;
        int reached = 0;
        s_prescale_fault = 1;
        for (int k = 0; k < 3; k++) {
            if (k == 2) shown &= create_swapchain();   /* stores the window's real extent */
            s_swap_ext.width = 900u;
            s_swap_ext.height = 500u;
            shown &= sdl3vk_present_rgba(px) == 1;
            if (k == 0) reached = s_prescale_fault == 0;
            sw[k] = s_last_blit.sw;
        }
        s_prescale_fault = 0;
        s_swap_ext = saved_ext;
        s_scaling = saved_scaling;
        if (plan.prescale < 2 || !shown || !reached || sw[0] != PSP_W || sw[1] != PSP_W ||
            sw[2] != plan.prescale * PSP_W) {
            fprintf(stderr, "sharp scaling: prescale %d, presents shown %d, fault reached %d; the "
                            "frame blits read %d, %d, then %d wide after a new swapchain (want "
                            "%d, %d, %d)\n", plan.prescale, shown, reached, sw[0], sw[1], sw[2],
                    PSP_W, PSP_W, plan.prescale * PSP_W);
            ok = 0;
        }
    }

    /* ---- presentation upscaler (#1273, SR_PRESENT_UPSCALE=fsr1) ------------------------
     * The real FSR 1 pass on the present path, under the validation layer. A 480x272 frame in the
     * larger window runs the pass at its place's size, and the frame blit reads the pass's image
     * 1:1 at that size with nearest filtering: what the window shows is the FSR output, not the
     * bilinear blit of the frame. A capture of the same present still holds the guest's frame. */
    {
        Sdl3VkInfo vi;
        memset(&vi, 0, sizeof(vi));
        char err[160] = "";
        s_test_fsr = NULL;
        if (sdl3vk_get_vk(&vi) && vi.get_instance_proc_addr) {
            Fsr1Init init;
            memset(&init, 0, sizeof(init));
            init.get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)vi.get_instance_proc_addr;
            init.instance = (VkInstance)vi.instance;
            init.physical_device = (VkPhysicalDevice)vi.physical;
            init.device = (VkDevice)vi.device;
            init.queue_family = vi.queue_family;
            s_test_fsr = fsr1_create(&init, err, sizeof(err), NULL);
        }
        Sdl3VkPresentPlan plan = sdl3vk_present_plan(s_scaling, PSP_W, PSP_H, (int)s_swap_ext.width,
                                                     (int)s_swap_ext.height, (int)s_max_image_dim);
        if (!s_test_fsr) {
            fprintf(stderr, "upscale: fsr1_create failed (%s)\n", err[0] ? err : "no device");
            ok = 0;
        } else if (plan.w <= PSP_W || plan.h <= PSP_H) {
            fprintf(stderr, "upscale: the %ux%u window gives a %dx%d place, not an enlargement\n",
                    s_swap_ext.width, s_swap_ext.height, plan.w, plan.h);
            ok = 0;
        } else {
            s_test_fsr_out = VK_NULL_HANDLE;
            s_test_fsr_calls = 0;
            memset(&s_last_blit, 0, sizeof(s_last_blit));
            sdl3vk_set_upscale_pass(test_fsr_pass);
            if (!sr_capture_arm("selftest_fsr.ppm")) {
                fprintf(stderr, "upscale: capture arm refused\n"); ok = 0;
            }
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "upscale: present failed\n"); ok = 0;
            }
            sdl3vk_set_upscale_pass(NULL);
            if (s_test_fsr_calls != 1 || s_test_fsr_args[0] != PSP_W || s_test_fsr_args[1] != PSP_H ||
                s_test_fsr_args[2] != plan.w || s_test_fsr_args[3] != plan.h) {
                fprintf(stderr, "upscale: the pass ran %d times for %dx%d to %dx%d (want once, "
                                "%dx%d to %dx%d)\n", s_test_fsr_calls, s_test_fsr_args[0],
                        s_test_fsr_args[1], s_test_fsr_args[2], s_test_fsr_args[3], PSP_W, PSP_H,
                        plan.w, plan.h);
                ok = 0;
            }
            if (s_test_fsr_out == VK_NULL_HANDLE || s_last_blit.src != s_test_fsr_out) {
                fprintf(stderr, "upscale: the window's blit did not read the FSR 1 image\n"); ok = 0;
            }
            if (s_last_blit.sw != plan.w || s_last_blit.sh != plan.h || s_last_blit.dx != plan.x ||
                s_last_blit.dy != plan.y || s_last_blit.dw != plan.w || s_last_blit.dh != plan.h ||
                s_last_blit.filter != VK_FILTER_NEAREST) {
                fprintf(stderr, "upscale: the blit is %dx%d to %dx%d at %d,%d, filter %d; want the "
                                "FSR image 1:1, %dx%d at %d,%d, nearest\n", s_last_blit.sw,
                        s_last_blit.sh, s_last_blit.dw, s_last_blit.dh, s_last_blit.dx,
                        s_last_blit.dy, (int)s_last_blit.filter, plan.w, plan.h, plan.x, plan.y);
                ok = 0;
            }
            if (sr_capture_result() != 1 || !cap_test_verify_ppm("selftest_fsr.ppm", PSP_W, PSP_H)) {
                fprintf(stderr, "upscale: the capture is not the guest's 480x272 frame\n"); ok = 0;
            }
            remove("selftest_fsr.ppm");
            /* Without the pass the same present blits the frame itself again. */
            memset(&s_last_blit, 0, sizeof(s_last_blit));
            if (sdl3vk_present_rgba(px) != 1 || s_last_blit.src == VK_NULL_HANDLE ||
                s_last_blit.src == s_test_fsr_out) {
                fprintf(stderr, "upscale: with the pass removed, the blit still read its image\n");
                ok = 0;
            }
        }
        fsr1_destroy(s_test_fsr);
        s_test_fsr = NULL;
        if (ok) printf("gpu capture selftest: FSR 1 present path OK (%dx%d to %dx%d)\n", PSP_W,
                       PSP_H, plan.w, plan.h);
    }

    /* ---- a recording with the upscaler on (#2138, SR_VIDEOREC_DIR + SR_PRESENT_UPSCALE) -----
     * A 960x544 recording of a 480x272 GE frame, through the production record path. With the
     * recording's FSR 1 pass set, the recorded picture is byte for byte what a separate FSR 1
     * pass makes of the same frame at that size, and rec_record names it fsr1. Without the pass
     * the same present records sharp scaling's picture, which at exactly 2x is one nearest blit,
     * so it is named nearest. The recorder counts each written frame under its picture's filter.
     * Then each size and mode below states the filter its blits are, worked out by hand from
     * the fit (480x272 into the recording, aspect kept, centred), never from the product's rules. */
#ifdef SR_VIDEOREC_SELFTEST
    {
        enum { RW = 2 * PSP_W, RH = 2 * PSP_H, CW = 1000, CH = 600 };
        const size_t rbytes = (size_t)RW * (size_t)RH * 4u;
        const size_t cbytes = (size_t)CW * (size_t)CH * 4u;
        const char *dir = "selftest_videorec_fsr";
        const int scaling0 = s_scaling;
        Sdl3VkInfo vi;
        memset(&vi, 0, sizeof(vi));
        char err[160] = "";
        Fsr1Pass *ref_fsr = NULL;
        s_test_rec_fsr = NULL;
        if (sdl3vk_get_vk(&vi) && vi.get_instance_proc_addr) {
            Fsr1Init init;
            memset(&init, 0, sizeof(init));
            init.get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)vi.get_instance_proc_addr;
            init.instance = (VkInstance)vi.instance;
            init.physical_device = (VkPhysicalDevice)vi.physical;
            init.device = (VkDevice)vi.device;
            init.queue_family = vi.queue_family;
            s_test_rec_fsr = fsr1_create(&init, err, sizeof(err), NULL);
            if (s_test_rec_fsr) ref_fsr = fsr1_create(&init, err, sizeof(err), NULL);
        }
        uint8_t *fsr_pic = (uint8_t *)malloc(rbytes);
        uint8_t *plain_pic = (uint8_t *)malloc(rbytes);
        uint8_t *case_pic = (uint8_t *)malloc(cbytes);
        uint8_t *sharp_pic = (uint8_t *)malloc(cbytes);
        CapBuffer ref;
        memset(&ref, 0, sizeof(ref));
        CapTestImage src;
        memset(&src, 0, sizeof(src));
        int fsr_filter = -1, plain_filter = -1;
        if (!s_test_rec_fsr || !ref_fsr || !fsr_pic || !plain_pic || !case_pic || !sharp_pic ||
            !cap_test_image_create(&src, PSP_W, PSP_H) || !cap_test_image_upload(&src)) {
            fprintf(stderr, "record upscale: setup failed (%s)\n", err[0] ? err : "memory or image");
            ok = 0;
        } else if (!sr_videorec_test_start(dir, "raw")) {
            fprintf(stderr, "record upscale: the raw recording did not start\n"); ok = 0;
        } else {
            s_rec_state = -1;           /* rec_on() asks the recorder again */
            s_rec_w = RW;               /* as SR_VIDEOREC_SIZE=960x544 would */
            s_rec_h = RH;
            s_scaling = SDL3VK_SCALING_SHARP;
            s_test_rec_calls = 0;
            sdl3vk_set_record_upscale_pass(test_rec_fsr_pass);
            if (sdl3vk_present_image_ex((void *)src.img, PSP_W, PSP_H) != 1 ||
                !test_rec_read(fsr_pic, rbytes, &fsr_filter)) {
                fprintf(stderr, "record upscale: the upscaled present was not recorded\n"); ok = 0;
            }
            sr_videorec_vblank(0u, 0u);
            sdl3vk_set_record_upscale_pass(NULL);
            if (sdl3vk_present_image_ex((void *)src.img, PSP_W, PSP_H) != 1 ||
                !test_rec_read(plain_pic, rbytes, &plain_filter)) {
                fprintf(stderr, "record upscale: the plain present was not recorded\n"); ok = 0;
            }
            sr_videorec_vblank(1u, 16683u);
            if (s_test_rec_calls != 1 || s_test_rec_args[0] != PSP_W || s_test_rec_args[1] != PSP_H ||
                s_test_rec_args[2] != RW || s_test_rec_args[3] != RH) {
                fprintf(stderr, "record upscale: the pass ran %d times for %dx%d to %dx%d (want "
                                "once, %dx%d to %dx%d)\n", s_test_rec_calls, s_test_rec_args[0],
                        s_test_rec_args[1], s_test_rec_args[2], s_test_rec_args[3], PSP_W, PSP_H,
                        RW, RH);
                ok = 0;
            }
            if (fsr_filter != SR_VIDEOREC_FILTER_FSR1 || plain_filter != SR_VIDEOREC_FILTER_NEAREST) {
                fprintf(stderr, "record upscale: the pictures were named %s and %s (want fsr1, "
                                "then nearest)\n", sr_videorec_filter_name(fsr_filter),
                        sr_videorec_filter_name(plain_filter));
                ok = 0;
            }
            /* The reference: a second FSR 1 pass on the same frame, at the recording's size. */
            int fx = 0, fy = 0;
            if (!test_rec_reference(ref_fsr, &ref, src.img, RW, RH)) {
                fprintf(stderr, "record upscale: the reference FSR 1 pass failed\n"); ok = 0;
            } else if (ok) {
                size_t bad = test_rec_diff(fsr_pic, RW, 0, 0, (const uint8_t *)ref.map, RW, RH,
                                           &fx, &fy);
                if (bad) {
                    fprintf(stderr, "record upscale: %zu recorded pixels differ from the FSR 1 "
                                    "pass's own output, first at %d,%d\n", bad, fx, fy);
                    ok = 0;
                }
                if (memcmp(fsr_pic, plain_pic, rbytes) == 0) {
                    fprintf(stderr, "record upscale: the FSR 1 recording equals the nearest one\n");
                    ok = 0;
                }
            }
            SrVideoRecStats st;
            sr_videorec_test_stats(&st);
            if (st.frames != 2u || st.filter_frames[SR_VIDEOREC_FILTER_FSR1] != 1u ||
                st.filter_frames[SR_VIDEOREC_FILTER_NEAREST] != 1u || st.width != (uint32_t)RW ||
                st.height != (uint32_t)RH) {
                fprintf(stderr, "record upscale: the recorder wrote %llu frames at %ux%u, %llu "
                                "fsr1 and %llu nearest\n", (unsigned long long)st.frames, st.width,
                        st.height, (unsigned long long)st.filter_frames[SR_VIDEOREC_FILTER_FSR1],
                        (unsigned long long)st.filter_frames[SR_VIDEOREC_FILTER_NEAREST]);
                ok = 0;
            }

            /* Each case's filter, from the fit worked by hand:
             * - 960x544 is exactly 2x: any mode but FSR 1 is one nearest blit.
             * - 1000x600 fits 1000x566 at 0,17 (566 = 1000*272/480, rounded down). Sharp
             *   prescales nearest to 3x (1440x816) and shrinks it linearly; bilinear is one
             *   linear blit; integer is 2x, 960x544 at 20,28, nearest; FSR 1 fills 1000x566.
             * - 320x180 fits 317x180 at 1,0: a shrink, so every mode is one linear blit and
             *   FSR 1 is not asked (EASU does not shrink).
             * - With the recording's pass unavailable (gui.c could not make it), the pass is
             *   asked once, gives nothing, and the picture is sharp's own. */
            enum { PASS_NONE, PASS_FSR, PASS_NULL };
            static const struct {
                const char *what;
                int mode, pass, w, h, want, calls;
            } cases[] = {
                { "bilinear at 960x544", SDL3VK_SCALING_BILINEAR, PASS_NONE, RW, RH,
                  SR_VIDEOREC_FILTER_NEAREST, 0 },
                { "integer at 960x544", SDL3VK_SCALING_INTEGER, PASS_NONE, RW, RH,
                  SR_VIDEOREC_FILTER_NEAREST, 0 },
                { "sharp at 1000x600", SDL3VK_SCALING_SHARP, PASS_NONE, CW, CH,
                  SR_VIDEOREC_FILTER_SHARP, 0 },
                { "bilinear at 1000x600", SDL3VK_SCALING_BILINEAR, PASS_NONE, CW, CH,
                  SR_VIDEOREC_FILTER_LINEAR, 0 },
                { "integer at 1000x600", SDL3VK_SCALING_INTEGER, PASS_NONE, CW, CH,
                  SR_VIDEOREC_FILTER_NEAREST, 0 },
                { "sharp at 320x180", SDL3VK_SCALING_SHARP, PASS_NONE, 320, 180,
                  SR_VIDEOREC_FILTER_LINEAR, 0 },
                { "integer at 320x180", SDL3VK_SCALING_INTEGER, PASS_NONE, 320, 180,
                  SR_VIDEOREC_FILTER_LINEAR, 0 },
                { "FSR 1 at 320x180", SDL3VK_SCALING_SHARP, PASS_FSR, 320, 180,
                  SR_VIDEOREC_FILTER_LINEAR, 0 },
                { "FSR 1 at 1000x600", SDL3VK_SCALING_SHARP, PASS_FSR, CW, CH,
                  SR_VIDEOREC_FILTER_FSR1, 1 },
                { "no FSR 1 pass at 1000x600", SDL3VK_SCALING_SHARP, PASS_NULL, CW, CH,
                  SR_VIDEOREC_FILTER_SHARP, 1 },
            };
            for (size_t i = 0; ok && i < sizeof cases / sizeof cases[0]; i++) {
                Sdl3VkUpscalePass pass = cases[i].pass == PASS_FSR ? test_rec_fsr_pass
                                       : cases[i].pass == PASS_NULL ? test_rec_null_pass : NULL;
                int got = -1;
                if (!test_rec_case(src.img, cases[i].mode, pass, cases[i].w, cases[i].h, case_pic,
                                   &got)) {
                    fprintf(stderr, "record upscale: %s was not recorded\n", cases[i].what);
                    ok = 0;
                    break;
                }
                if (got != cases[i].want || s_test_rec_calls != cases[i].calls) {
                    fprintf(stderr, "record upscale: %s was named %s with the pass asked %d "
                                    "times (want %s, %d)\n", cases[i].what,
                            sr_videorec_filter_name(got), s_test_rec_calls,
                            sr_videorec_filter_name(cases[i].want), cases[i].calls);
                    ok = 0;
                }
                if (cases[i].pass == PASS_NONE && cases[i].want == SR_VIDEOREC_FILTER_SHARP)
                    memcpy(sharp_pic, case_pic, cbytes);
                if (cases[i].pass == PASS_NULL && memcmp(case_pic, sharp_pic, cbytes) != 0) {
                    fprintf(stderr, "record upscale: %s differs from sharp's picture\n",
                            cases[i].what);
                    ok = 0;
                }
                if (cases[i].pass == PASS_FSR && cases[i].want == SR_VIDEOREC_FILTER_FSR1) {
                    enum { FH = CW * PSP_H / PSP_W, FY = (CH - FH) / 2 };
                    if (s_test_rec_args[2] != CW || s_test_rec_args[3] != FH) {
                        fprintf(stderr, "record upscale: %s asked for %dx%d (want %dx%d)\n",
                                cases[i].what, s_test_rec_args[2], s_test_rec_args[3], CW, FH);
                        ok = 0;
                    } else if (!test_rec_reference(ref_fsr, &ref, src.img, CW, FH)) {
                        fprintf(stderr, "record upscale: the %dx%d reference failed\n", CW, FH);
                        ok = 0;
                    } else {
                        size_t bad = test_rec_diff(case_pic, CW, 0, FY, (const uint8_t *)ref.map,
                                                   CW, FH, &fx, &fy);
                        size_t lit = 0;
                        for (int y = 0; y < CH; y++) {
                            if (y >= FY && y < FY + FH) continue;
                            for (int x = 0; x < CW; x++) {
                                const uint8_t *a = case_pic + ((size_t)y * CW + (size_t)x) * 4u;
                                lit += a[0] || a[1] || a[2];
                            }
                        }
                        if (bad || lit) {
                            fprintf(stderr, "record upscale: %s: %zu pixels differ from the pass "
                                            "at 0,%d (first %d,%d) and %zu bar pixels are lit\n",
                                    cases[i].what, bad, FY, fx, fy, lit);
                            ok = 0;
                        }
                    }
                }
            }
        }
        s_scaling = scaling0;
        sdl3vk_set_record_upscale_pass(NULL);
        sr_videorec_test_reset();
        (void)wait_all_present_frames();
        s_rec_state = -1;
        s_rec_w = s_rec_h = 0;
        rec_image_free(&s_rec_target);
        rec_image_free(&s_rec_pre);
        cap_free_buffer(&ref);
        cap_test_image_destroy(&src);
        fsr1_destroy(ref_fsr);
        fsr1_destroy(s_test_rec_fsr);
        s_test_rec_fsr = NULL;
        free(fsr_pic);
        free(plain_pic);
        free(case_pic);
        free(sharp_pic);
        {
            static const char *const files[] = { "record.json", "record.json.tmp", "video.bgr0",
                                                 "audio.wav", "frames.csv", "logic.srt" };
            char p[96];
            for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
                snprintf(p, sizeof p, "%s/%s", dir, files[i]);
                remove(p);
            }
            (void)cap_test_rmdir(dir);
        }
        if (ok) printf("gpu capture selftest: FSR 1 recording OK (%dx%d to %dx%d, equals the "
                       "pass's output, differs from nearest; %u sizes and modes named by their "
                       "blits)\n", PSP_W, PSP_H, RW, RH, 10u);
    }
#else
    fprintf(stderr, "record upscale: built without SR_VIDEOREC_SELFTEST, so the recording cell "
                    "cannot run\n");
    ok = 0;
#endif

    /* ---- the UI pass (#2151): the render notice changes no captured byte ----------------
     * With a pass that paints the whole swapchain image after the blit, the present's capture is
     * still the guest's frame byte for byte and the frame blit is the one a present without the
     * pass makes; the two captures are the same file. Removed, the pass does not run again. */
    {
        uint8_t *on = NULL, *off = NULL;
        size_t on_size = 0, off_size = 0;
        const int calls = s_test_ui_calls;
        memset(&s_last_blit, 0, sizeof(s_last_blit));
        sdl3vk_set_ui_pass(test_ui_pass);
        if (!sr_capture_arm("selftest_ui_on.ppm") || sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "ui pass: the present with the pass failed\n"); ok = 0;
        }
        sdl3vk_set_ui_pass(NULL);
        const int on_result = sr_capture_result();
        const int on_sw = s_last_blit.sw, on_sh = s_last_blit.sh, on_dx = s_last_blit.dx;
        const int on_dy = s_last_blit.dy, on_dw = s_last_blit.dw, on_dh = s_last_blit.dh;
        const VkFilter on_filter = s_last_blit.filter;
        memset(&s_last_blit, 0, sizeof(s_last_blit));
        if (!sr_capture_arm("selftest_ui_off.ppm") || sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "ui pass: the present without the pass failed\n"); ok = 0;
        }
        if (s_test_ui_calls != calls + 1) {
            fprintf(stderr, "ui pass: it ran %d times (want once with it set, never once removed)\n",
                    s_test_ui_calls - calls);
            ok = 0;
        }
        if (on_result != 1 || !cap_test_verify_ppm("selftest_ui_on.ppm", PSP_W, PSP_H)) {
            fprintf(stderr, "ui pass: the capture with the pass is not the guest's frame\n"); ok = 0;
        }
        if (sr_capture_result() != 1 || !cap_test_verify_ppm("selftest_ui_off.ppm", PSP_W, PSP_H)) {
            fprintf(stderr, "ui pass: the capture without the pass is not the guest's frame\n"); ok = 0;
        }
        if (s_last_blit.src == VK_NULL_HANDLE || on_sw != s_last_blit.sw || on_sh != s_last_blit.sh ||
            on_dx != s_last_blit.dx || on_dy != s_last_blit.dy || on_dw != s_last_blit.dw ||
            on_dh != s_last_blit.dh || on_filter != s_last_blit.filter) {
            fprintf(stderr, "ui pass: the frame blit changed with the pass (%dx%d to %dx%d at %d,%d "
                            "against %dx%d to %dx%d at %d,%d)\n", on_sw, on_sh, on_dw, on_dh, on_dx,
                    on_dy, s_last_blit.sw, s_last_blit.sh, s_last_blit.dw, s_last_blit.dh,
                    s_last_blit.dx, s_last_blit.dy);
            ok = 0;
        }
        on = test_read_file("selftest_ui_on.ppm", &on_size);
        off = test_read_file("selftest_ui_off.ppm", &off_size);
        if (!on || !off || on_size != off_size || memcmp(on, off, on_size) != 0) {
            fprintf(stderr, "ui pass: the captures with and without the pass differ (%zu and %zu "
                            "bytes)\n", on_size, off_size);
            ok = 0;
        }
        free(on);
        free(off);
        remove("selftest_ui_on.ppm");
        remove("selftest_ui_off.ppm");
        if (ok) printf("gpu capture selftest: UI pass present OK (capture and blit unchanged)\n");
    }

    /* ---- pause overlay (#1159): shown, repeated without an upload, never captured ------ */
    {
        int ow = 0, oh = 0, orgba = 0;
        if (sdl3vk_overlay_extent(&ow, &oh, &orgba) <= 0) {
            fprintf(stderr, "overlay: no extent for a live swapchain\n"); ok = 0;
        } else {
            const size_t opitch = (size_t)ow * 4u;
            uint8_t *opx = (uint8_t *)malloc(opitch * (size_t)oh);
            if (!opx) {
                fprintf(stderr, "overlay: out of memory\n"); ok = 0;
            } else {
                for (size_t i = 0; i < opitch * (size_t)oh; i++) opx[i] = (uint8_t)(i * 7u);
                if (!sr_capture_arm("selftest_overlay.ppm")) {
                    fprintf(stderr, "overlay: capture arm refused\n"); ok = 0;
                }
                int first = sdl3vk_present_overlay(NULL, 0);
                if (first != 2) {
                    fprintf(stderr, "overlay: a present before any image returned %d (expected 2)\n", first);
                    ok = 0;
                }
                int shown = sdl3vk_present_overlay(opx, (int)opitch);
                int again = sdl3vk_present_overlay(NULL, 0);
                if (shown != 1 || again != 1) {
                    fprintf(stderr, "overlay: presents returned %d, %d (expected 1, 1)\n", shown, again);
                    ok = 0;
                }
                if (sr_capture_result() != 0) {
                    fprintf(stderr, "overlay: an overlay present serviced the capture\n"); ok = 0;
                }
                sr_capture_cancel();
                FILE *f = fopen("selftest_overlay.ppm", "rb");
                if (f) { fclose(f); fprintf(stderr, "overlay: capture file must not exist\n"); ok = 0; }
                free(opx);
            }
            sdl3vk_overlay_release();
        }
    }

    /* ---- cancel: an armed capture never serviced reports "nothing attempted" --------- */
    if (!sr_capture_arm("selftest_cancel.ppm")) {
        fprintf(stderr, "cancel: arm refused\n"); ok = 0;
    }
    sr_capture_cancel();
    if (sr_capture_result() != 0) {
        fprintf(stderr, "cancel: result %d (expected 0)\n", sr_capture_result());
        ok = 0;
    }
    {
        FILE *f = fopen("selftest_cancel.ppm", "rb");
        if (f) { fclose(f); fprintf(stderr, "cancel: file must not exist\n"); ok = 0; }
    }

    /* ---- GPU render-target path (R8G8B8A8, same 480x272 region) --------------------- */
    CapTestImage gpu;
    if (!cap_test_image_create(&gpu, PSP_W, PSP_H) || !cap_test_image_upload(&gpu)) {
        fprintf(stderr, "gpu: image setup failed\n"); ok = 0;
    } else {
        if (!sr_capture_arm("selftest_gpu.ppm")) {
            fprintf(stderr, "gpu: capture arm refused\n"); ok = 0;
        }
        if (sdl3vk_present_image_ex((void *)gpu.img, PSP_W, PSP_H) != 1) {
            fprintf(stderr, "gpu: present failed\n"); ok = 0;
        }
        if (sr_capture_result() != 1) {
            fprintf(stderr, "gpu: capture result %d (expected 1)\n", sr_capture_result());
            ok = 0;
        }
        if (strcmp(sr_capture_source_label(), "gpu-render-target") != 0) {
            fprintf(stderr, "gpu: source label '%s'\n", sr_capture_source_label());
            ok = 0;
        }
        if (!cap_test_verify_ppm("selftest_gpu.ppm", PSP_W, PSP_H)) {
            fprintf(stderr, "gpu: PPM bytes wrong (BGRA/RGBA handling, pitch, or truncation)\n");
            ok = 0;
        }
        remove("selftest_gpu.ppm");
        cap_test_image_destroy(&gpu);
    }

    /* ---- scaled GPU path: a larger source exercises row pitch and buffer growth ------ */
    CapTestImage big;
    if (!cap_test_image_create(&big, 960, 544) || !cap_test_image_upload(&big)) {
        fprintf(stderr, "scaled: image setup failed\n"); ok = 0;
    } else {
        if (!sr_capture_arm("selftest_scaled.ppm")) {
            fprintf(stderr, "scaled: capture arm refused\n"); ok = 0;
        }
        if (sdl3vk_present_image_ex((void *)big.img, 960, 544) != 1) {
            fprintf(stderr, "scaled: present failed\n"); ok = 0;
        }
        if (sr_capture_result() != 1) {
            fprintf(stderr, "scaled: capture result %d (expected 1)\n", sr_capture_result());
            ok = 0;
        }
        if (!cap_test_verify_ppm("selftest_scaled.ppm", 960, 544)) {
            fprintf(stderr, "scaled: PPM bytes wrong (pitch or truncation)\n"); ok = 0;
        }
        remove("selftest_scaled.ppm");
        /* A region no image can have is refused before anything is recorded: an armed capture
         * fails, nothing reaches the validation layer, and the next present is shown. */
        {
            static const int bad[][2] = { { 0, 544 }, { 960, 0 }, { -4, 544 }, { 960, -1 } };
            const int errors_before = sdl3vk_validation_error_count();
            for (size_t i = 0; i < 2u * (sizeof bad / sizeof bad[0]); i++) {
                const int w = bad[i / 2u][0], h = bad[i / 2u][1], arm = (int)(i & 1u);
                const int armed = !arm || sr_capture_arm("selftest_bad_region.ppm");
                const int got = sdl3vk_present_image_ex((void *)big.img, w, h);
                const int capture = arm ? sr_capture_result() : -1;
                if (!armed || got != -1 || capture != -1) {
                    fprintf(stderr, "scaled: a %dx%d region%s gave present %d capture %d (want -1, "
                                    "-1)\n", w, h, arm ? " with a capture armed" : "", got, capture);
                    ok = 0;
                }
                remove("selftest_bad_region.ppm");
            }
            const int shown = sdl3vk_present_image_ex((void *)big.img, 960, 544);
            /* That present carries no capture, so nothing waited for it: the image it read
             * is destroyed below only once every frame has finished. */
            const int finished = wait_all_present_frames();
            if (shown != 1 || !finished || sdl3vk_renderer_terminal() ||
                sdl3vk_validation_error_count() != errors_before) {
                fprintf(stderr, "scaled: after the refused regions the present gave %d, finished %d, "
                                "terminal %d, %d validation error(s)\n", shown, finished,
                        sdl3vk_renderer_terminal(),
                        sdl3vk_validation_error_count() - errors_before);
                ok = 0;
            }
        }
        cap_test_image_destroy(&big);
    }

    /* ---- picture probe at a 4x render scale (#1296) ---------------------------------
     * The probe's readback is not waited for or scanned inside the present: the present
     * returns with the probe in flight and no result, a file capture can be armed and
     * published for the next present, and the probe's own pixels (not the newer frame's)
     * give the result once its fence has signalled. Its buffer comes from the same picker
     * as the readback memory cells above. */
    CapTestImage dark;
    if (!cap_test_image_create(&dark, 2048, 1088) || !cap_test_image_upload_dark(&dark, 8u)) {
        fprintf(stderr, "probe: image setup failed\n"); ok = 0;
    } else {
        uint32_t b = 0u;
        if (!sr_capture_arm_probe()) {
            fprintf(stderr, "probe: arm refused\n"); ok = 0;
        }
        if (sdl3vk_present_image_ex((void *)dark.img, 2048, 1088) != 1) {
            fprintf(stderr, "probe: present failed\n"); ok = 0;
        }
        if (!sr_capture_probe_in_flight() || sr_capture_probe_take(&b)) {
            fprintf(stderr, "probe: the present must return with the probe in flight and no "
                            "result (no fence wait or scan inside the present)\n");
            ok = 0;
        }
        if (sr_capture_arm_probe()) {
            fprintf(stderr, "probe: a second probe must be refused while one is in flight\n");
            ok = 0;
            sr_capture_cancel();
        }
        if (!s_probe.buf || s_probe.buf == s_cap.buf) {
            fprintf(stderr, "probe: the probe must read back into its own buffer\n"); ok = 0;
        } else {
            /* The memory type wiring: the probe buffer took the picker's choice. */
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(s_dev, s_probe.buf, &mr);
            uint32_t flags[VK_MAX_MEMORY_TYPES];
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                flags[i] = (uint32_t)mp.memoryTypes[i].propertyFlags;
            uint32_t want = cap_pick_mem_type(flags, mp.memoryTypeCount, mr.memoryTypeBits, NULL);
            if (want == UINT32_MAX || s_probe.mem_flags != flags[want]) {
                fprintf(stderr, "probe: the probe buffer must use the readback memory picker\n");
                ok = 0;
            }
        }
        CapTestImage pat;
        if (!cap_test_image_create(&pat, PSP_W, PSP_H) || !cap_test_image_upload(&pat)) {
            fprintf(stderr, "probe: pattern image setup failed\n"); ok = 0;
        } else {
            if (!sr_capture_arm("selftest_after_probe.ppm")) {
                fprintf(stderr, "probe: a file capture must be accepted while a probe is in "
                                "flight\n");
                ok = 0;
            }
            if (sdl3vk_present_image_ex((void *)pat.img, PSP_W, PSP_H) != 1 ||
                sr_capture_result() != 1 ||
                !cap_test_verify_ppm("selftest_after_probe.ppm", PSP_W, PSP_H)) {
                fprintf(stderr, "probe: the file capture after a probe must be exact\n"); ok = 0;
            }
            remove("selftest_after_probe.ppm");
            cap_test_image_destroy(&pat);
        }
        (void)sr_capture_probe_poll(1);
        if (sr_capture_probe_in_flight() || !sr_capture_probe_take(&b) || b != 8u) {
            fprintf(stderr, "probe: result %u (expected 8, the probe's own frame)\n", b); ok = 0;
        }

        /* The cost left once the GPU is done: invalidate and scan of a 2048x1088 dark
         * frame, the full-scan case. Reported, not asserted (host-dependent). */
        if (!sr_capture_arm_probe() ||
            sdl3vk_present_image_ex((void *)dark.img, 2048, 1088) != 1 ||
            !sdl3vk_wait_image((void *)dark.img)) {
            fprintf(stderr, "probe: timing probe setup failed\n"); ok = 0;
        }
        uint64_t t0 = SDL_GetTicksNS();
        (void)sr_capture_probe_poll(0);
        uint64_t t1 = SDL_GetTicksNS();
        if (sr_capture_probe_in_flight() || !sr_capture_probe_take(&b) || b != 8u) {
            fprintf(stderr, "probe: a completed readback must publish without waiting\n"); ok = 0;
        }
        fprintf(stderr, "probe: 2048x1088 dark frame scanned in %.3f ms (memory flags 0x%x)\n",
                (double)(t1 - t0) / 1e6, (unsigned)s_probe.mem_flags);

        /* Published before its frame slot is reused, with no blocking poll: after
         * PRESENT_FRAMES more presents the probe is no longer in flight. */
        if (!sr_capture_arm_probe() ||
            sdl3vk_present_image_ex((void *)dark.img, 2048, 1088) != 1) {
            fprintf(stderr, "probe: second probe setup failed\n"); ok = 0;
        }
        for (int i = 0; i < PRESENT_FRAMES; i++)
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "probe: present %d after the probe failed\n", i); ok = 0;
            }
        if (sr_capture_probe_in_flight() || !sr_capture_probe_take(&b) || b != 8u) {
            fprintf(stderr, "probe: not published before its frame slot was reused\n"); ok = 0;
        }

        /* A present that fails after the probe was recorded leaves no probe in flight and no
         * result, as before: the next present simply carries a new probe. */
        sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
        if (!sr_capture_arm_probe() ||
            sdl3vk_present_image_ex((void *)dark.img, 2048, 1088) != -1 ||
            sr_capture_probe_in_flight() || sr_capture_probe_take(&b)) {
            fprintf(stderr, "probe: a failed present must leave no probe and no result\n"); ok = 0;
        }
        (void)sdl3vk_wait_image((void *)dark.img);
        cap_test_image_destroy(&dark);
    }

    /* ---- production publication path -------------------------------------------------
     * FBSNAP's build/snapshots directory is not created by the build. The capture
     * publisher owns that boundary and must create it before opening its temp sibling. */
    {
        const char *p = "build/snapshots/selftest_frame.ppm";
        cap_test_rmdir("build/snapshots");
        cap_test_rmdir("build");
        if (!sr_capture_arm(p)) {
            fprintf(stderr, "pubdir: arm refused\n"); ok = 0;
        } else {
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "pubdir: present failed\n"); ok = 0;
            }
            if (sr_capture_result() != 1) {
                fprintf(stderr, "pubdir: result=%d (expected 1)\n",
                        sr_capture_result()); ok = 0;
            }
            if (!cap_test_verify_ppm(p, PSP_W, PSP_H)) {
                fprintf(stderr, "pubdir: PPM bytes wrong\n"); ok = 0;
            }
        }
        remove(p);
        cap_test_rmdir("build/snapshots");
        cap_test_rmdir("build");
    }

    /* ---- publication failure vs present truth, then recovery -------------------------
     * A regular file blocks the requested parent directory. The frame still reached the
     * presentation engine, so only the capture fails; removing the blocker must let the
     * immediately following request succeed. */
    {
        const char *p = "blocked/selftest_x.ppm";
        FILE *f = fopen("blocked", "wb");
        if (!f) {
            fprintf(stderr, "blocked: could not create file blocker\n"); ok = 0;
        } else {
            fputc(0, f); fclose(f);
        }
        if (!sr_capture_arm(p)) {
            fprintf(stderr, "blocked: arm refused\n"); ok = 0;
        } else {
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "blocked: present must succeed despite publication failure\n"); ok = 0;
            }
            if (sr_capture_result() != -1) {
                fprintf(stderr, "blocked: result=%d (expected -1)\n",
                        sr_capture_result()); ok = 0;
            }
        }
        {
            FILE *chk = fopen(p, "rb");
            if (chk) { fclose(chk); fprintf(stderr, "blocked: file must not exist\n"); ok = 0; }
        }
        remove(p);
        remove("blocked");

        if (!sr_capture_arm(p)) {
            fprintf(stderr, "blocked-recover: arm refused\n"); ok = 0;
        } else if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "blocked-recover: present failed\n"); ok = 0;
        } else if (sr_capture_result() != 1 || !cap_test_verify_ppm(p, PSP_W, PSP_H)) {
            fprintf(stderr, "blocked-recover: capture did not recover\n"); ok = 0;
        }
        remove(p);
        cap_test_rmdir("blocked");
    }

    /* ---- stale-arm and rejected-arm protection --------------------------------------
     * Cancellation represents a present slot dropped by the output cap. The next present
     * must not service that old path, and a refused replacement arm must not expose the
     * previous capture's result. */
    {
        const char *p = "selftest_stale.ppm";
        if (!sr_capture_arm(p)) {
            fprintf(stderr, "stale: arm refused\n"); ok = 0;
        }
        sr_capture_cancel();
        if (sr_capture_result() != 0) {
            fprintf(stderr, "stale: cancelled result=%d\n",
                    sr_capture_result()); ok = 0;
        }
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "stale: later present failed\n"); ok = 0;
        }
        {
            FILE *chk = fopen(p, "rb");
            if (chk) { fclose(chk); fprintf(stderr, "stale: cancelled arm published\n"); ok = 0; }
        }
        remove(p);

    }

    /* ---- repeated requests ---------------------------------------------------------- */
    {
        const char *p1 = "selftest_repeat_a.ppm";
        const char *p2 = "selftest_repeat_b.ppm";
        if (!sr_capture_arm(p1)) {
            fprintf(stderr, "repeat: first arm refused\n"); ok = 0;
        }
        if (sr_capture_arm(p2)) {
            fprintf(stderr, "repeat: second arm while pending accepted\n"); ok = 0;
        }
        if (sr_capture_result() != 0) {
            fprintf(stderr, "repeat: refused arm left stale result=%d\n", sr_capture_result()); ok = 0;
        }
        if (sdl3vk_present_rgba(px) != 1 || sr_capture_result() != 1) {
            fprintf(stderr, "repeat: first capture did not complete\n"); ok = 0;
        }
        if (!cap_test_verify_ppm(p1, PSP_W, PSP_H)) {
            fprintf(stderr, "repeat: first path PPM bytes wrong\n"); ok = 0;
        }
        {
            FILE *chk = fopen(p2, "rb");
            if (chk) { fclose(chk); fprintf(stderr, "repeat: refused path published\n"); ok = 0; }
        }
        remove(p1);
        remove(p2);
    }

    /* ---- present semaphores follow the swapchain image (#1799) ----------------------
     * A present's wait on its semaphore is known to be over only once its image is acquired
     * again; no fence covers it. Frames drawn from one slot put the frame ring out of step
     * with the swapchain images, as a driver that hands images out of order does, and none of
     * them may signal a semaphore that an earlier present of another image may still be
     * waiting on: the validation layer reports that as an ERROR. */
    {
        int errors0 = sdl3vk_validation_error_count();
        for (int i = 0; i < 8 * PRESENT_FRAMES; i++) {
            s_frame_cursor = 0;
            if (sdl3vk_present_rgba(px) != 1) {
                fprintf(stderr, "present semaphores: present %d from one frame slot failed\n", i);
                ok = 0;
            }
        }
        if (sdl3vk_validation_error_count() != errors0) {
            fprintf(stderr, "present semaphores: %d validation error(s) presenting from one frame "
                            "slot (a semaphore signalled before its image was acquired again)\n",
                    sdl3vk_validation_error_count() - errors0);
            ok = 0;
        }
        /* Each image has one of its own, made with the swapchain; no slot past its images. */
        for (size_t i = 0; i < sizeof(s_swap_sem_done) / sizeof(s_swap_sem_done[0]); i++) {
            int dup = 0;
            for (size_t k = 0; k < i; k++) dup |= s_swap_sem_done[k] == s_swap_sem_done[i];
            if ((s_swap_sem_done[i] != VK_NULL_HANDLE) != (i < s_swap_n) ||
                (s_swap_sem_done[i] != VK_NULL_HANDLE && dup)) {
                fprintf(stderr, "present semaphores: slot %u of %u swapchain image(s) is wrong\n",
                        (unsigned)i, (unsigned)s_swap_n);
                ok = 0;
            }
        }
    }

    /* ---- present error recovery and disposition tests ------------------------------- */
    {
        /* Test 1: VK_SUBOPTIMAL_KHR (enqueued rebuild) */
        uint64_t sc0 = sdl3vk_swapchain_generation();
        sdl3vk_present_fault_inject(VK_SUBOPTIMAL_KHR);
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present fault SUBOPTIMAL: expected present success (1)\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0) {
            fprintf(stderr, "present fault SUBOPTIMAL: expected swapchain rebuild\n"); ok = 0;
        }

        /* The enqueued present/rebuild path must still publish the capture recorded in
         * the same submission; rebuilding the swapchain cannot silently drop it. */
        sc0 = sdl3vk_swapchain_generation();
        if (!sr_capture_arm("selftest_subopt.ppm")) {
            fprintf(stderr, "present fault SUBOPTIMAL: capture arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_SUBOPTIMAL_KHR);
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present fault SUBOPTIMAL capture: present failed\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0 || sr_capture_result() != 1) {
            fprintf(stderr, "present fault SUBOPTIMAL capture: rebuild/result invalid\n"); ok = 0;
        }
        if (!cap_test_verify_ppm("selftest_subopt.ppm", PSP_W, PSP_H)) {
            fprintf(stderr, "present fault SUBOPTIMAL capture: PPM bytes wrong\n"); ok = 0;
        }
        remove("selftest_subopt.ppm");

        /* Test 2: VK_ERROR_OUT_OF_DATE_KHR (enqueued present / stale swapchain) */
        sc0 = sdl3vk_swapchain_generation();
        uint64_t sem0 = sdl3vk_frame_semaphore_generation();
        if (!sr_capture_arm("selftest_ood.ppm")) {
            fprintf(stderr, "present fault OOD: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault OOD: expected present failure (-1)\n"); ok = 0;
        }
        if (sr_capture_result() != -1) {
            fprintf(stderr, "present fault OOD: capture result must fail (-1)\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0) {
            fprintf(stderr, "present fault OOD: expected swapchain rebuild\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() != sem0) {
            fprintf(stderr, "present fault OOD: enqueued present must NOT recreate semaphore\n"); ok = 0;
        }

        /* Test 3: VK_ERROR_OUT_OF_HOST_MEMORY (unenqueued hard error / semaphore recovery).
         * The image the failed present held stays acquired until the swapchain is rebuilt
         * (#1377), so the failure rebuilds it. */
        sem0 = sdl3vk_frame_semaphore_generation();
        sc0 = sdl3vk_swapchain_generation();
        if (!sr_capture_arm("selftest_oom.ppm")) {
            fprintf(stderr, "present fault OOM: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_HOST_MEMORY);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault OOM: expected present failure (-1)\n"); ok = 0;
        }
        if (sr_capture_result() != -1) {
            fprintf(stderr, "present fault OOM: capture result must fail (-1)\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() <= sem0) {
            fprintf(stderr, "present fault OOM: expected frame semaphore recovery/recreation\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0 || sdl3vk_renderer_terminal()) {
            fprintf(stderr, "present fault OOM: the still-acquired image must be handed back by a "
                            "swapchain rebuild\n"); ok = 0;
        }

        /* Verify frame slot recovery: next normal present must succeed cleanly without validation errors */
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present recovery: post-OOM normal present failed\n"); ok = 0;
        }
        /* More unenqueued presents in a row than the swapchain has images: without the rebuild
         * every one would keep an image and the next acquire would wait forever. */
        for (uint32_t k = 0; ok && k <= s_swap_n; k++) {
            sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_HOST_MEMORY);
            if (sdl3vk_present_rgba(px) != -1 || sdl3vk_renderer_terminal()) {
                fprintf(stderr, "present fault OOM: repeated failure %u was not recovered\n", (unsigned)k);
                ok = 0;
            }
        }
        if (ok && sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present recovery: present after repeated OOM failures failed\n"); ok = 0;
        }

    }

    /* A swapchain replacement with a frame still in flight waits until the old swapchain's
     * images are unused before destroying it; with validation on, destroying it earlier is
     * VUID-vkDestroySwapchainKHR-swapchain-01282. A failed wait keeps the swapchain (#1377). */
    {
        int in_flight = 0, after = 0;
        for (int i = 0; i < PRESENT_FRAMES; i++) in_flight += s_frame[i].submitted;
        uint64_t sc0 = sdl3vk_swapchain_generation();
        if (!create_swapchain() || sdl3vk_swapchain_generation() <= sc0) {
            fprintf(stderr, "swapchain: replacement failed\n"); ok = 0;
        }
        for (int i = 0; i < PRESENT_FRAMES; i++) after += s_frame[i].submitted;
        if (in_flight == 0 || after != 0) {
            fprintf(stderr, "swapchain: replacement with %d frame(s) in flight left %d unwaited\n",
                    in_flight, after);
            ok = 0;
        }

        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "swapchain: present before the failed wait failed\n"); ok = 0;
        }
        VkSwapchainKHR kept = s_swap;
        /* A minimized window (a 0x0 surface) keeps the swapchain without waiting for the frames
         * or the queue: an armed queue-idle fault is never reached, the frame in flight stays
         * in flight and the renderer goes on. */
        {
            int before = 0, still = 0;
            for (int i = 0; i < PRESENT_FRAMES; i++) before += s_frame[i].submitted;
            sc0 = sdl3vk_swapchain_generation();
            s_swap_extent_zero_fault = 1;
            s_idle_fault_result = VK_ERROR_DEVICE_LOST;
            const int made = create_swapchain();
            const int idle_waited = s_idle_fault_result == VK_SUCCESS;
            s_idle_fault_result = VK_SUCCESS;
            s_swap_extent_zero_fault = 0;
            for (int i = 0; i < PRESENT_FRAMES; i++) still += s_frame[i].submitted;
            if (made || s_swap != kept || sdl3vk_swapchain_generation() != sc0 ||
                sdl3vk_renderer_terminal() || idle_waited || before == 0 || still != before) {
                fprintf(stderr, "swapchain: a minimized window's rebuild made %d, waited the "
                                "queue %d, left %d of %d frame(s) in flight, terminal %d (want "
                                "0, 0, all, 0)\n", made, idle_waited, still, before,
                        sdl3vk_renderer_terminal());
                ok = 0;
            }
            s_renderer_terminal = 0;
        }
        sc0 = sdl3vk_swapchain_generation();
        s_idle_fault_result = VK_ERROR_DEVICE_LOST;
        if (create_swapchain() || s_swap != kept || sdl3vk_swapchain_generation() != sc0 ||
            !sdl3vk_renderer_terminal() || s_idle_fault_result != VK_SUCCESS) {
            fprintf(stderr, "swapchain: a failed queue-idle wait must keep the swapchain and "
                            "make the renderer terminal\n");
            ok = 0;
        }
    }

    /* Test 4: a quit consumed by a present is not lost. The present path drains the
     * event queue every frame, so a window close almost always lands there; the
     * process-exit decision is made later by sdl3vk_poll(). */
    {
        s_renderer_terminal = 0;
        create_swapchain();
        SDL_Event quit_ev;
        SDL_zero(quit_ev);
        quit_ev.type = SDL_EVENT_QUIT;
        if (!SDL_PushEvent(&quit_ev)) {
            fprintf(stderr, "quit: SDL_PushEvent failed: %s\n", SDL_GetError()); ok = 0;
        }
        (void)sdl3vk_present_rgba(px);
        if (sdl3vk_poll() != 0) {
            fprintf(stderr, "quit: a quit consumed by a present was lost; sdl3vk_poll() still reports alive\n");
            ok = 0;
        }
        s_quit_requested = 0;
    }

    if (!sdl3vk_present_pacing_selftest()) ok = 0;
    if (!sdl3vk_present_completed_selftest()) ok = 0;

    int errors = sdl3vk_validation_error_count();
    if (errors != 0) {
        fprintf(stderr, "validation: %d ERROR-severity messages; expected 0\n", errors);
        ok = 0;
    }
    sdl3vk_shutdown();
    sdl3vk_shutdown();   /* after a full run (swapchain, overlay, capture, prescale): a no-op */
    errors = sdl3vk_validation_error_count();
    if (errors != 0) {
        fprintf(stderr, "validation: %d ERROR-severity teardown messages; expected 0\n", errors);
        ok = 0;
    }
    if (lifecycle_live_objects() != 0) {
        fprintf(stderr, "shutdown: %d object(s) alive after a full run\n", lifecycle_live_objects());
        ok = 0;
    }
    if (surface_failed) {
        fprintf(stderr, "gpu capture selftest: the window-surface cases failed (above)\n");
        ok = 0;
    }
    if (!ok) return 1;
    puts("gpu capture selftest: OK");
    return 0;
}

/* Runs the body from a scratch working directory beneath SR_SELFTEST_BUILD_ROOT. Its relative
 * paths (the FBSNAP default build/snapshots, the selftest_*.ppm frames, the input-profile
 * fixtures) resolve against the working directory, so they land in scratch, never in the
 * checkout's build/ or its root. The original working directory is restored on every path. */
int sdl3vk_capture_selftest(void) {
    static const char scratch_rel[] = SR_SELFTEST_BUILD_ROOT "/gpu_capture_selftest";
    char original[1024];
    char scratch[1024];
    if (!nk_platform_absolute_path(".", original, sizeof original) ||
        !nk_platform_mkdir_p(scratch_rel) ||
        !nk_platform_absolute_path(scratch_rel, scratch, sizeof scratch)) {
        fprintf(stderr, "gpu capture selftest: cannot prepare scratch directory '%s'\n",
                scratch_rel);
        return 1;
    }
    if (SDL3VK_TEST_CHDIR(scratch) != 0) {
        fprintf(stderr, "gpu capture selftest: cannot enter scratch directory '%s'\n", scratch);
        return 1;
    }
    int rc = sdl3vk_capture_selftest_body();
    if (SDL3VK_TEST_CHDIR(original) != 0) {
        fprintf(stderr, "gpu capture selftest: cannot restore working directory '%s'\n", original);
        return 1;
    }
    return rc;
}
