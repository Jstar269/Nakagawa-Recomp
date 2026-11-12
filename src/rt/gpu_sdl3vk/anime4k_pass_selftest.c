/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * anime4k_pass_selftest.c - the movie upscaler (anime4k_pass.c, Anime4K CNN x2 from
 * third_party/anime4k) on a headless Vulkan device, beside FSR 1 (fsr1_pass.c). Links the two
 * passes, the C library and libm alone. Pictures are source-owned.
 *
 *   stages          the stage count rule and the SR_MOVIE_UPSCALE values (no device needed)
 *   line-art        one stage at 2x lands closer to a 4x4-supersampled line-art truth than FSR 1
 *                   (EASU + RCAS) at 2x: error at most 85% of FSR 1's
 *   flat            a flat colour from a BGRA source stays within 2 through two stages, 120x68 to
 *                   480x272
 *   source-unread   the pass never writes the image it is given, and returns its own
 *   limits          a size past the device's image limit is refused without building images or
 *                   touching the last good size's, and the pass still works at the good size
 *   alloc-failure   a size whose vkCreateImage fails (wrapped through the init's proc-addr hook) is
 *                   built once and not tried again, and the pass still works at a good size
 *   cost            a 480x272 frame through two stages and FSR 1 to 3811x2160, by timestamps
 *
 * --mutant PASS FILE runs line-art with another shader for one pass; it must then fail.
 * Exit 77 (SKIP) without a Vulkan loader or device.
 */
#include "anime4k_pass.h"
#include "fsr1_pass.h"

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int s_fail;
#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail = 1; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                       printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

typedef struct {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkBuffer xfer;
    VkDeviceMemory xfer_memory;
    uint32_t *map;
} Gpu;
static Gpu g;

typedef struct { VkImage image; VkDeviceMemory memory; } Source;

#define GPU_FN(name) static PFN_vk##name name
GPU_FN(CreateInstance); GPU_FN(DestroyInstance); GPU_FN(EnumeratePhysicalDevices);
GPU_FN(GetPhysicalDeviceQueueFamilyProperties); GPU_FN(GetPhysicalDeviceMemoryProperties);
GPU_FN(CreateDevice); GPU_FN(DestroyDevice); GPU_FN(GetDeviceProcAddr); GPU_FN(GetDeviceQueue);
GPU_FN(CreateCommandPool); GPU_FN(DestroyCommandPool); GPU_FN(AllocateCommandBuffers);
GPU_FN(CreateFence); GPU_FN(DestroyFence); GPU_FN(WaitForFences); GPU_FN(ResetFences);
GPU_FN(CreateImage); GPU_FN(DestroyImage); GPU_FN(GetImageMemoryRequirements);
GPU_FN(CreateBuffer); GPU_FN(DestroyBuffer); GPU_FN(GetBufferMemoryRequirements);
GPU_FN(AllocateMemory); GPU_FN(FreeMemory); GPU_FN(BindImageMemory); GPU_FN(BindBufferMemory);
GPU_FN(MapMemory); GPU_FN(BeginCommandBuffer); GPU_FN(EndCommandBuffer); GPU_FN(ResetCommandBuffer);
GPU_FN(CmdPipelineBarrier); GPU_FN(CmdCopyImageToBuffer); GPU_FN(CmdCopyBufferToImage);
GPU_FN(CmdBlitImage); GPU_FN(GetPhysicalDeviceProperties);
GPU_FN(CreateQueryPool); GPU_FN(DestroyQueryPool); GPU_FN(CmdResetQueryPool);
GPU_FN(CmdWriteTimestamp); GPU_FN(GetQueryPoolResults);
GPU_FN(QueueSubmit); GPU_FN(DeviceWaitIdle);
#undef GPU_FN

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    GetPhysicalDeviceMemoryProperties(g.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

#define XFER_BYTES (2048u * 1088u * 4u)

static int gpu_open(char *why, size_t cap) {
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (lib) g.gipa = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib) *(void **)&g.gipa = dlsym(lib, "vkGetInstanceProcAddr");
#endif
    if (!g.gipa) { snprintf(why, cap, "no Vulkan loader"); return 0; }
    CreateInstance = (PFN_vkCreateInstance)g.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "anime4k_selftest", 1, NULL, 0,
                              VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (!CreateInstance || CreateInstance(&ici, NULL, &g.instance) != VK_SUCCESS) {
        snprintf(why, cap, "no usable Vulkan driver");
        return 0;
    }
#define INST(name) name = (PFN_vk##name)g.gipa(g.instance, "vk" #name)
    INST(DestroyInstance); INST(EnumeratePhysicalDevices); INST(GetPhysicalDeviceQueueFamilyProperties);
    INST(GetPhysicalDeviceMemoryProperties); INST(CreateDevice); INST(GetDeviceProcAddr);
    INST(GetPhysicalDeviceProperties);
#undef INST
    VkPhysicalDevice devs[8];
    uint32_t nd = 8;
    if (EnumeratePhysicalDevices(g.instance, &nd, devs) < VK_SUCCESS || nd == 0) {
        snprintf(why, cap, "no Vulkan device");
        return 0;
    }
    for (uint32_t d = 0; d < nd && !g.physical; d++) {
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16;
        GetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
        for (uint32_t q = 0; q < nq; q++)
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                g.physical = devs[d];
                g.family = q;
                break;
            }
    }
    if (!g.physical) { snprintf(why, cap, "no graphics and compute queue"); return 0; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (CreateDevice(g.physical, &dci, NULL, &g.device) != VK_SUCCESS) {
        snprintf(why, cap, "vkCreateDevice failed");
        return 0;
    }
#define DEV(name) name = (PFN_vk##name)GetDeviceProcAddr(g.device, "vk" #name)
    DEV(DestroyDevice); DEV(GetDeviceQueue); DEV(CreateCommandPool); DEV(DestroyCommandPool);
    DEV(AllocateCommandBuffers); DEV(CreateFence); DEV(DestroyFence); DEV(WaitForFences);
    DEV(ResetFences); DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements);
    DEV(CreateBuffer); DEV(DestroyBuffer); DEV(GetBufferMemoryRequirements); DEV(AllocateMemory);
    DEV(FreeMemory); DEV(BindImageMemory); DEV(BindBufferMemory); DEV(MapMemory);
    DEV(BeginCommandBuffer); DEV(EndCommandBuffer); DEV(ResetCommandBuffer); DEV(CmdPipelineBarrier);
    DEV(CmdCopyImageToBuffer); DEV(CmdCopyBufferToImage); DEV(QueueSubmit); DEV(DeviceWaitIdle);
    DEV(CmdBlitImage); DEV(CreateQueryPool); DEV(DestroyQueryPool); DEV(CmdResetQueryPool);
    DEV(CmdWriteTimestamp); DEV(GetQueryPoolResults);
#undef DEV
    GetDeviceQueue(g.device, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g.family };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
                                        VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if (CreateCommandPool(g.device, &pci, NULL, &g.pool) != VK_SUCCESS) return -1;
    cai.commandPool = g.pool;
    if (AllocateCommandBuffers(g.device, &cai, &g.cb) != VK_SUCCESS ||
        CreateFence(g.device, &fci, NULL, &g.fence) != VK_SUCCESS) return -1;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = XFER_BYTES;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (CreateBuffer(g.device, &bi, NULL, &g.xfer) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    GetBufferMemoryRequirements(g.device, g.xfer, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &g.xfer_memory) != VK_SUCCESS ||
        BindBufferMemory(g.device, g.xfer, g.xfer_memory, 0) != VK_SUCCESS ||
        MapMemory(g.device, g.xfer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&g.map) != VK_SUCCESS) return -1;
    return 1;
}

static void gpu_close(void) {
    if (g.device) {
        DeviceWaitIdle(g.device);
        if (g.xfer) DestroyBuffer(g.device, g.xfer, NULL);
        if (g.xfer_memory) FreeMemory(g.device, g.xfer_memory, NULL);
        if (g.fence) DestroyFence(g.device, g.fence, NULL);
        if (g.pool) DestroyCommandPool(g.device, g.pool, NULL);
        DestroyDevice(g.device, NULL);
    }
    if (g.instance) DestroyInstance(g.instance, NULL);
}

static void cmd_begin(void) {
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ResetCommandBuffer(g.cb, 0);
    BeginCommandBuffer(g.cb, &bi);
}

static void cmd_end(void) {
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cb;
    EndCommandBuffer(g.cb);
    ResetFences(g.device, 1, &g.fence);
    QueueSubmit(g.queue, 1, &si, g.fence);
    WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    CmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, NULL, 0, NULL, 1, &b);
}

static void source_fill(const Source *src, int w, int h, int fresh) {
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    cmd_begin();
    barrier(src->image, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CmdCopyBufferToImage(g.cb, g.xfer, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd_end();
}

/* Reads an image in TRANSFER_SRC_OPTIMAL into g.map. */
static void image_read(VkImage image, int w, int h) {
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    cmd_begin();
    CmdCopyImageToBuffer(g.cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.xfer, 1, &c);
    cmd_end();
}

/* ---- images and the reference ------------------------------------------------------------------ */

static int image_make(Source *im, int w, int h, VkFormat format) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (CreateImage(g.device, &ii, NULL, &im->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    GetImageMemoryRequirements(g.device, im->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &im->memory) != VK_SUCCESS ||
        BindImageMemory(g.device, im->image, im->memory, 0) != VK_SUCCESS) return 0;
    return 1;
}

static void image_free(Source *im) {
    DeviceWaitIdle(g.device);
    if (im->image) DestroyImage(g.device, im->image, NULL);
    if (im->memory) FreeMemory(g.device, im->memory, NULL);
    memset(im, 0, sizeof(*im));
}

/* Bilinear w x h -> ow x oh with a linear blit, the presenter's own non-integer filter, left in
 * TRANSFER_SRC_OPTIMAL and read into g.map. */
static void bilinear(const Source *src, int w, int h, int ow, int oh) {
    Source dst;
    memset(&dst, 0, sizeof(dst));
    if (!image_make(&dst, ow, oh, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "reference image"); return; }
    VkImageBlit b;
    memset(&b, 0, sizeof(b));
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = 1;
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[1].x = w; b.srcOffsets[1].y = h; b.srcOffsets[1].z = 1;
    b.dstOffsets[1].x = ow; b.dstOffsets[1].y = oh; b.dstOffsets[1].z = 1;
    cmd_begin();
    barrier(dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CmdBlitImage(g.cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    barrier(dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd_end();
    image_read(dst.image, ow, oh);
    image_free(&dst);
}

static VkImage upscale(Fsr1Pass *p, const Source *src, int w, int h, int ow, int oh, float sharp) {
    cmd_begin();
    VkImage out = fsr1_record(p, g.cb, src->image, w, h, ow, oh, sharp);
    cmd_end();
    if (out) image_read(out, ow, oh);
    return out;
}

static int s_mutant;

/* ---- Anime4K ------------------------------------------------------------------------------------ */

/* An RGBA16F image (w x h, TRANSFER_SRC_OPTIMAL) read into g.map as RGBA8 through a blit. */
static void read_float_image(VkImage image, int w, int h) {
    Source tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (!image_make(&tmp, w, h, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "readback image"); return; }
    VkImageBlit b;
    memset(&b, 0, sizeof(b));
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = 1;
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[1].x = w; b.srcOffsets[1].y = h; b.srcOffsets[1].z = 1;
    b.dstOffsets[1] = b.srcOffsets[1];
    cmd_begin();
    barrier(tmp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CmdBlitImage(g.cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    barrier(tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd_end();
    image_read(tmp.image, w, h);
    image_free(&tmp);
}

static VkImage a4k_run(Anime4kPass *a, const Source *src, int w, int h, int stages) {
    cmd_begin();
    VkImage out = anime4k_record(a, g.cb, src->image, w, h, stages);
    cmd_end();
    if (out) read_float_image(out, w << stages, h << stages);
    return out;
}

/* Line art: dark strokes 3 px wide (at the truth's scale) at several angles, a ring, and two
 * outlined colour fills on a light ground, the look of an animated or pre-rendered movie frame.
 * The truth is drawn at 2x with 4x4 supersampling; the input is its exact 2x2 box average. */
static double seg_dist(double px, double py, double ax, double ay, double bx, double by) {
    double vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    double t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return dx * dx + dy * dy;
}

static void art_sample(double x, double y, double rgb[3]) {
    static const double seg[][4] = {
        { 20, 20, 220, 40 }, { 30, 230, 200, 60 }, { 240, 20, 250, 250 }, { 60, 140, 460, 150 },
        { 300, 30, 460, 250 }, { 280, 250, 470, 200 }, { 100, 100, 101, 260 },
    };
    rgb[0] = 236; rgb[1] = 232; rgb[2] = 220;
    if (x > 330 && x < 440 && y > 40 && y < 120) { rgb[0] = 200; rgb[1] = 64; rgb[2] = 52; }
    if (x > 130 && x < 230 && y > 170 && y < 240) { rgb[0] = 52; rgb[1] = 96; rgb[2] = 196; }
    int dark = 0;
    for (size_t i = 0; i < sizeof(seg) / sizeof(seg[0]); i++)
        if (seg_dist(x, y, seg[i][0], seg[i][1], seg[i][2], seg[i][3]) < 1.5 * 1.5) dark = 1;
    double r = sqrt((x - 380) * (x - 380) + (y - 190) * (y - 190));
    if (fabs(r - 50) < 1.5) dark = 1;
    if ((fabs(x - 330) < 1.5 || fabs(x - 440) < 1.5) && y > 40 && y < 120) dark = 1;
    if ((fabs(y - 40) < 1.5 || fabs(y - 120) < 1.5) && x > 330 && x < 440) dark = 1;
    if (dark) { rgb[0] = 24; rgb[1] = 20; rgb[2] = 28; }
}

#define ART_W 240
#define ART_H 136
static uint32_t s_truth[ART_W * 2 * ART_H * 2];

static uint32_t pack(const double c[3]) {
    uint32_t v = 0xFF000000u;
    for (int k = 0; k < 3; k++) {
        int q = (int)(c[k] + 0.5);
        v |= (uint32_t)(q < 0 ? 0 : q > 255 ? 255 : q) << (8 * k);
    }
    return v;
}

static void paint_art(void) {
    const int tw = ART_W * 2, th = ART_H * 2;
    static double acc[ART_W * 2 * ART_H * 2][3];
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            double sum[3] = { 0, 0, 0 }, rgb[3];
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    art_sample(x + (sx + 0.5) / 4.0, y + (sy + 0.5) / 4.0, rgb);
                    for (int k = 0; k < 3; k++) sum[k] += rgb[k] / 16.0;
                }
            for (int k = 0; k < 3; k++) acc[y * tw + x][k] = sum[k];
            s_truth[y * tw + x] = pack(sum);
        }
    for (int y = 0; y < ART_H; y++)
        for (int x = 0; x < ART_W; x++) {
            double c[3];
            for (int k = 0; k < 3; k++)
                c[k] = (acc[(2 * y) * tw + 2 * x][k] + acc[(2 * y) * tw + 2 * x + 1][k] +
                        acc[(2 * y + 1) * tw + 2 * x][k] + acc[(2 * y + 1) * tw + 2 * x + 1][k]) / 4.0;
            g.map[y * ART_W + x] = pack(c);
        }
}

/* Mean absolute difference from the truth, all three channels, inside a 4 px border. */
static double art_error(void) {
    const int tw = ART_W * 2, th = ART_H * 2;
    double sum = 0;
    long n = 0;
    for (int y = 4; y < th - 4; y++)
        for (int x = 4; x < tw - 4; x++) {
            uint32_t a = g.map[y * tw + x], b = s_truth[y * tw + x];
            for (int k = 0; k < 3; k++) {
                int d = (int)(a >> (8 * k) & 0xFF) - (int)(b >> (8 * k) & 0xFF);
                sum += d < 0 ? -d : d;
            }
            n += 3;
        }
    return sum / (double)n;
}

/* line-art: one Anime4K stage at 2x lands closer to the truth than FSR 1 (EASU + RCAS) at 2x, by a
 * measured margin: its error is at most 85% of FSR 1's. Bilinear is printed for scale. */
static void cell_line_art(Anime4kPass *a, Fsr1Pass *f) {
    const int w = ART_W, h = ART_H, ow = w * 2, oh = h * 2;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "source image"); return; }
    paint_art();
    source_fill(&src, w, h, 1);
    bilinear(&src, w, h, ow, oh);
    double eb = art_error();
    upscale(f, &src, w, h, ow, oh, FSR1_SHARPNESS_DEFAULT);
    double ef = art_error();
    VkImage out = a4k_run(a, &src, w, h, 1);
    CHECK(out != VK_NULL_HANDLE, "Anime4K recorded");
    double ea = art_error();
    printf("anime4k-selftest: line art at 2x, mean error from the truth: bilinear %.2f, FSR 1 %.2f, "
           "Anime4K %.2f\n", eb, ef, ea);
    CHECK(ea <= ef * 0.85, "Anime4K is not sharper than FSR 1 on line art (error %.2f vs %.2f)%s", ea, ef,
          s_mutant ? " [the mutant is caught]" : "");
    image_free(&src);
}

/* flat: a flat colour stays within 2 of itself through two stages (120x68 to 480x272), and the
 * output has the doubled sizes. */
static void cell_flat_a4k(Anime4kPass *a) {
    const int w = 120, h = 68;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_B8G8R8A8_UNORM)) { CHECK(0, "source image"); return; }
    for (int i = 0; i < w * h; i++) g.map[i] = 0xFFC08030u;  /* B 30, G 80, R C0 */
    source_fill(&src, w, h, 1);
    VkImage out = a4k_run(a, &src, w, h, 2);
    CHECK(out != VK_NULL_HANDLE, "two stages recorded");
    long off = 0;
    int worst = 0;
    for (int i = 0; i < w * 4 * h * 4; i++) {
        uint32_t v = g.map[i];
        int d[3] = { abs((int)(v & 0xFF) - 0xC0), abs((int)(v >> 8 & 0xFF) - 0x80), abs((int)(v >> 16 & 0xFF) - 0x30) };
        for (int k = 0; k < 3; k++) {
            if (d[k] > worst) worst = d[k];
            if (d[k] > 2) { off++; break; }
        }
    }
    CHECK(off == 0, "%ld of %d pixels left the flat colour (worst %d)", off, w * 4 * h * 4, worst);
    image_free(&src);
}

/* source-unread: Anime4K never writes the image it is given. */
static void cell_source_a4k(Anime4kPass *a) {
    const int w = 64, h = 48;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "source image"); return; }
    static uint32_t before[64 * 48];
    for (int i = 0; i < w * h; i++) {
        uint32_t x = (uint32_t)i * 2654435761u;
        before[i] = (x ^ x >> 15) | 0xFF000000u;
        g.map[i] = before[i];
    }
    source_fill(&src, w, h, 1);
    VkImage out = a4k_run(a, &src, w, h, 1);
    CHECK(out != VK_NULL_HANDLE && out != src.image, "Anime4K returns its own image");
    image_read(src.image, w, h);
    CHECK(memcmp(before, g.map, sizeof(before)) == 0, "Anime4K changed the source image");
    image_free(&src);
}

/* A size past the device's image limit is refused before anything is touched: no image build (so no
 * device wait), the last good size's images stay (no build when the caller goes back to it), and
 * the pass still works. 32768 << 1 is past every real device's limit. The allocation-failure path
 * is cell_alloc_failure's. */
static void cell_limits(Anime4kPass *a) {
    const int w = 64, h = 48;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "source image"); return; }
    source_fill(&src, w, h, 1);
    VkImage good = a4k_run(a, &src, w, h, 1);
    CHECK(good != VK_NULL_HANDLE, "the pass works at a good size");
    unsigned builds = anime4k_size_builds(a);
    cmd_begin();
    VkImage first = anime4k_record(a, g.cb, src.image, 32768, 1, 1);
    VkImage again = anime4k_record(a, g.cb, src.image, 32768, 1, 1);
    cmd_end();
    CHECK(first == VK_NULL_HANDLE && again == VK_NULL_HANDLE,
          "a size past the device's image limit is refused");
    CHECK(anime4k_size_builds(a) == builds, "a refused size builds no images, so it waits for no device");
    VkImage back = a4k_run(a, &src, w, h, 1);
    CHECK(back != VK_NULL_HANDLE, "the pass still works at the good size after a refusal");
    CHECK(anime4k_size_builds(a) == builds, "going back to the good size rebuilds nothing");
    image_free(&src);
}

/* The allocation-failure path, which the limit check never reaches: vkCreateImage is wrapped (through
 * Anime4kInit's own proc-addr hook, so the pass has no test seam) to fail for one width. A size whose
 * images cannot be made is built once, freed after that build's device wait, and not tried again. */
static PFN_vkGetDeviceProcAddr s_real_dev_addr;
static PFN_vkCreateImage s_real_create_image;
static uint32_t s_fail_width;  /* vkCreateImage fails for images this wide, when not 0 */

static VKAPI_ATTR VkResult VKAPI_CALL failing_create_image(VkDevice device, const VkImageCreateInfo *info,
                                                           const VkAllocationCallbacks *alloc,
                                                           VkImage *image) {
    if (s_fail_width && info->extent.width == s_fail_width) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return s_real_create_image(device, info, alloc, image);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wrapped_device_addr(VkDevice device, const char *name) {
    PFN_vkVoidFunction fn = s_real_dev_addr(device, name);
    if (strcmp(name, "vkCreateImage") == 0) {
        s_real_create_image = (PFN_vkCreateImage)fn;
        return (PFN_vkVoidFunction)failing_create_image;
    }
    return fn;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wrapped_instance_addr(VkInstance instance, const char *name) {
    PFN_vkVoidFunction fn = g.gipa(instance, name);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        s_real_dev_addr = (PFN_vkGetDeviceProcAddr)fn;
        return (PFN_vkVoidFunction)wrapped_device_addr;
    }
    return fn;
}

static void cell_alloc_failure(void) {
    Anime4kInit ai;
    memset(&ai, 0, sizeof(ai));
    ai.get_instance_proc_addr = wrapped_instance_addr;
    ai.instance = g.instance;
    ai.physical_device = g.physical;
    ai.device = g.device;
    ai.queue_family = g.family;
    char why[160] = "";
    Anime4kPass *a = anime4k_create(&ai, why, sizeof(why));
    CHECK(a != NULL, "anime4k_create with a wrapped vkCreateImage: %s", why);
    if (!a) return;
    const int w = 64, h = 48;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_R8G8B8A8_UNORM)) {
        CHECK(0, "source image");
        anime4k_destroy(a);
        return;
    }
    source_fill(&src, w, h, 1);
    unsigned builds = anime4k_size_builds(a);
    s_fail_width = 100;
    cmd_begin();
    VkImage first = anime4k_record(a, g.cb, src.image, 100, 50, 1);
    VkImage again = anime4k_record(a, g.cb, src.image, 100, 50, 1);
    cmd_end();
    s_fail_width = 0;
    CHECK(first == VK_NULL_HANDLE && again == VK_NULL_HANDLE, "a size whose images cannot be made is refused");
    CHECK(anime4k_size_builds(a) == builds + 1, "that size is built once, then not tried again");
    VkImage ok = a4k_run(a, &src, w, h, 1);
    CHECK(ok != VK_NULL_HANDLE, "the pass works at a good size after an allocation failure");
    image_free(&src);
    anime4k_destroy(a);
}

static void cell_stages(void) {
    CHECK(anime4k_stages_for(480, 272, 3811, 2160) == 2, "480x272 to 4K takes two stages");
    CHECK(anime4k_stages_for(480, 272, 960, 544) == 1, "480x272 to 960x544 takes one stage");
    CHECK(anime4k_stages_for(480, 272, 560, 317) == 0, "under 1.2x takes none");
    CHECK(anime4k_stages_for(480, 272, 1920, 1088) == 2, "4x takes two stages");
    CHECK(anime4k_parse_movie_mode(NULL) == MOVIE_UPSCALE_OFF && anime4k_parse_movie_mode("off") == 0 &&
          anime4k_parse_movie_mode("fsr1") == MOVIE_UPSCALE_FSR1 &&
          anime4k_parse_movie_mode("anime4k") == MOVIE_UPSCALE_ANIME4K &&
          anime4k_parse_movie_mode("Anime4K") == -1, "SR_MOVIE_UPSCALE values");
}

static int cmp_d(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return a < b ? -1 : a > b;
}

/* cost: a 480x272 movie frame through two stages (1920x1088), then FSR 1 to 3811x2160. */
static void cell_cost_a4k(Anime4kPass *a, Fsr1Pass *f) {
    VkPhysicalDeviceProperties props;
    GetPhysicalDeviceProperties(g.physical, &props);
    if (props.limits.timestampPeriod <= 0.0f) return;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, 480, 272, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "source image"); return; }
    for (int i = 0; i < 480 * 272; i++) g.map[i] = (uint32_t)i * 2654435761u | 0xFF000000u;
    source_fill(&src, 480, 272, 1);
    VkQueryPoolCreateInfo qi;
    memset(&qi, 0, sizeof(qi));
    qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 3;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (CreateQueryPool(g.device, &qi, NULL, &pool) != VK_SUCCESS) { image_free(&src); return; }
    double a4k[15], all[15];
    int n = 0;
    for (int r = 0; r < 16; r++) {
        cmd_begin();
        CmdResetQueryPool(g.cb, pool, 0, 3);
        CmdWriteTimestamp(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
        VkImage mid = anime4k_record(a, g.cb, src.image, 480, 272, 2);
        CmdWriteTimestamp(g.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
        VkImage out = mid ? fsr1_record(f, g.cb, mid, 1920, 1088, 3811, 2160, FSR1_SHARPNESS_DEFAULT)
                          : VK_NULL_HANDLE;
        CmdWriteTimestamp(g.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 2);
        cmd_end();
        CHECK(out != VK_NULL_HANDLE, "the 4K movie chain recorded");
        uint64_t t[3] = { 0, 0, 0 };
        if (GetQueryPoolResults(g.device, pool, 0, 3, sizeof t, t, sizeof t[0],
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS || r == 0)
            continue;
        a4k[n] = (double)(t[1] - t[0]) * props.limits.timestampPeriod / 1e6;
        all[n] = (double)(t[2] - t[0]) * props.limits.timestampPeriod / 1e6;
        n++;
    }
    if (n) {
        qsort(a4k, (size_t)n, sizeof a4k[0], cmp_d);
        qsort(all, (size_t)n, sizeof all[0], cmp_d);
        printf("anime4k-selftest: cost of a 480x272 movie frame on %s: Anime4K two stages %.3f ms, "
               "with FSR 1 to 3811x2160 %.3f ms (median of %d)\n", props.deviceName, a4k[n / 2], all[n / 2], n);
    }
    DestroyQueryPool(g.device, pool, NULL);
    image_free(&src);
}

static uint32_t *read_spirv(const char *path, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = n > 0 && n % 4 == 0 ? (uint32_t *)malloc((size_t)n) : NULL;
    if (code && fread(code, 1, (size_t)n, f) != (size_t)n) { free(code); code = NULL; }
    fclose(f);
    *bytes = code ? (size_t)n : 0;
    return code;
}

int main(int argc, char **argv) {
    char why[160] = "";
    uint32_t *mutant = NULL;
    size_t mutant_bytes = 0;
    int mutant_pass = -1;
    if (argc >= 4 && strcmp(argv[1], "--mutant") == 0) {
        mutant_pass = atoi(argv[2]);
        mutant = read_spirv(argv[3], &mutant_bytes);
        if (!mutant) { printf("anime4k-selftest: cannot read %s\n", argv[3]); return 2; }
        s_mutant = 1;
    }
    cell_stages();
    int up = gpu_open(why, sizeof(why));
    if (up <= 0) {
        printf("SKIP anime4k-selftest: %s\n", why[0] ? why : "the device could not be set up");
        gpu_close();
        free(mutant);
        return up == 0 ? 77 : 1;
    }
    Anime4kInit ai;
    memset(&ai, 0, sizeof(ai));
    ai.get_instance_proc_addr = g.gipa;
    ai.instance = g.instance;
    ai.physical_device = g.physical;
    ai.device = g.device;
    ai.queue_family = g.family;
    ai.mutant_pass = mutant_pass;
    ai.mutant_spirv = mutant;
    ai.mutant_spirv_bytes = mutant_bytes;
    Anime4kPass *a = anime4k_create(&ai, why, sizeof(why));
    CHECK(a != NULL, "anime4k_create: %s", why);
    Fsr1Init fi;
    memset(&fi, 0, sizeof(fi));
    fi.get_instance_proc_addr = g.gipa;
    fi.instance = g.instance;
    fi.physical_device = g.physical;
    fi.device = g.device;
    fi.queue_family = g.family;
    Fsr1Pass *f = fsr1_create(&fi, why, sizeof(why));
    CHECK(f != NULL, "fsr1_create: %s", why);
    if (a && f) {
        cell_line_art(a, f);
        if (!s_mutant) {
            cell_flat_a4k(a);
            cell_source_a4k(a);
            cell_limits(a);
            cell_alloc_failure();
            cell_cost_a4k(a, f);
        }
    }
    anime4k_destroy(a);
    fsr1_destroy(f);
    gpu_close();
    free(mutant);
    printf(s_fail ? "anime4k-selftest: FAIL\n" : "anime4k-selftest: PASS\n");
    return s_fail ? 1 : 0;
}
