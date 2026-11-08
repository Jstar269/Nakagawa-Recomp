/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * anime4k_pass.c - see anime4k_pass.h.
 *
 * Every present: the shown image is blitted 1:1 into a private RGBA16F image (the stage input,
 * mpv's MAIN), then each stage runs the upscaler's passes as tools/anime4k_port.py generated
 * them: seven convolutions at the input's size and a depth-to-space pass that writes the doubled
 * picture, which is the next stage's input. All of a stage's images stay in GENERAL, read through
 * samplers and written as storage images, with a memory barrier between passes. The images are
 * reused by the frames in flight on one queue; nothing here waits for the GPU except a size change.
 */
#include "anime4k_pass.h"
#include "anime4k_passes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t k_spv_0[] =
#include "anime4k_cnn_x2_m_0_comp.inc"
;
static const uint32_t k_spv_1[] =
#include "anime4k_cnn_x2_m_1_comp.inc"
;
static const uint32_t k_spv_2[] =
#include "anime4k_cnn_x2_m_2_comp.inc"
;
static const uint32_t k_spv_3[] =
#include "anime4k_cnn_x2_m_3_comp.inc"
;
static const uint32_t k_spv_4[] =
#include "anime4k_cnn_x2_m_4_comp.inc"
;
static const uint32_t k_spv_5[] =
#include "anime4k_cnn_x2_m_5_comp.inc"
;
static const uint32_t k_spv_6[] =
#include "anime4k_cnn_x2_m_6_comp.inc"
;
static const uint32_t k_spv_7[] =
#include "anime4k_cnn_x2_m_7_comp.inc"
;
static const uint32_t k_spv_8[] =
#include "anime4k_cnn_x2_m_8_comp.inc"
;

#define PASSES ANIME4K_CNN_X2_M_PASSES
#define GRAPH k_anime4k_cnn_x2_m
static const struct { const uint32_t *code; size_t bytes; } k_spv[PASSES] = {
    { k_spv_0, sizeof(k_spv_0) }, { k_spv_1, sizeof(k_spv_1) }, { k_spv_2, sizeof(k_spv_2) },
    { k_spv_3, sizeof(k_spv_3) }, { k_spv_4, sizeof(k_spv_4) }, { k_spv_5, sizeof(k_spv_5) },
    { k_spv_6, sizeof(k_spv_6) }, { k_spv_7, sizeof(k_spv_7) }, { k_spv_8, sizeof(k_spv_8) },
};

#define FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define BINDINGS (ANIME4K_MAX_BINDS + 1)

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} Img;

typedef struct {
    int w, h;                 /* the stage input's size */
    Img tex[PASSES];          /* pass k writes tex[k]; the last is the doubled picture */
    VkDescriptorSet sets[PASSES];
} Stage;

struct Anime4kPass {
    Anime4kInit init;
    VkDevice dev;
    int w, h, stages;
    uint32_t max_dim;         /* the device's maxImageDimension2D */
    int fail_w, fail_h, fail_stages;  /* the size whose images could not be made */
    unsigned size_builds;     /* how many times the images were built for a new size */
    Img in;                   /* stage 0's input: the shown image, 1:1 */
    Stage stage[ANIME4K_MAX_STAGES];
    VkSampler linear;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkPipelineLayout layout;
    VkShaderModule mods[PASSES];
    VkPipeline pipes[PASSES];

    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
#define A4K_FN(name) PFN_vk##name name
    A4K_FN(CreateImage); A4K_FN(DestroyImage); A4K_FN(GetImageMemoryRequirements);
    A4K_FN(AllocateMemory); A4K_FN(FreeMemory); A4K_FN(BindImageMemory);
    A4K_FN(CreateImageView); A4K_FN(DestroyImageView); A4K_FN(CreateSampler); A4K_FN(DestroySampler);
    A4K_FN(CreateDescriptorSetLayout); A4K_FN(DestroyDescriptorSetLayout);
    A4K_FN(CreateDescriptorPool); A4K_FN(DestroyDescriptorPool); A4K_FN(AllocateDescriptorSets);
    A4K_FN(UpdateDescriptorSets); A4K_FN(CreatePipelineLayout); A4K_FN(DestroyPipelineLayout);
    A4K_FN(CreateShaderModule); A4K_FN(DestroyShaderModule); A4K_FN(CreateComputePipelines);
    A4K_FN(DestroyPipeline); A4K_FN(DeviceWaitIdle);
    A4K_FN(CmdPipelineBarrier); A4K_FN(CmdBlitImage); A4K_FN(CmdBindPipeline);
    A4K_FN(CmdBindDescriptorSets); A4K_FN(CmdPushConstants); A4K_FN(CmdDispatch);
#undef A4K_FN
};

int anime4k_parse_movie_mode(const char *value) {
    if (!value || !value[0] || strcmp(value, "off") == 0) return MOVIE_UPSCALE_OFF;
    if (strcmp(value, "fsr1") == 0) return MOVIE_UPSCALE_FSR1;
    if (strcmp(value, "anime4k") == 0) return MOVIE_UPSCALE_ANIME4K;
    return -1;
}

int anime4k_stages_for(int w, int h, int out_w, int out_h) {
    int stages = 0;
    if (w <= 0 || h <= 0) return 0;
    while (stages < ANIME4K_MAX_STAGES && (double)out_w > 1.2 * (double)(w << stages) &&
           (double)out_h > 1.2 * (double)(h << stages))
        stages++;
    return stages;
}

static void set_error(char *error, size_t cap, const char *text) {
    if (error && cap) snprintf(error, cap, "%s", text);
}

static uint32_t memory_type(Anime4kPass *p, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp;
    p->GetPhysicalDeviceMemoryProperties(p->init.physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    }
    return UINT32_MAX;
}

static int img_make(Anime4kPass *p, Img *im, int w, int h, VkImageUsageFlags usage) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = FORMAT;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (p->CreateImage(p->dev, &ii, NULL, &im->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    p->GetImageMemoryRequirements(p->dev, im->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(p, mr.memoryTypeBits) };
    if (mai.memoryTypeIndex == UINT32_MAX ||
        p->AllocateMemory(p->dev, &mai, NULL, &im->memory) != VK_SUCCESS ||
        p->BindImageMemory(p->dev, im->image, im->memory, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = FORMAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return p->CreateImageView(p->dev, &vi, NULL, &im->view) == VK_SUCCESS;
}

static void img_free(Anime4kPass *p, Img *im) {
    if (im->view) p->DestroyImageView(p->dev, im->view, NULL);
    if (im->image) p->DestroyImage(p->dev, im->image, NULL);
    if (im->memory) p->FreeMemory(p->dev, im->memory, NULL);
    memset(im, 0, sizeof(*im));
}

static void sized_free(Anime4kPass *p) {
    img_free(p, &p->in);
    for (int s = 0; s < ANIME4K_MAX_STAGES; s++) {
        for (int k = 0; k < PASSES; k++) img_free(p, &p->stage[s].tex[k]);
        p->stage[s].w = p->stage[s].h = 0;
    }
    p->w = p->h = p->stages = 0;
}

/* The view of texture `index` of stage s: 0 the stage input, 1 + k pass k's output. */
static VkImageView tex_view(Anime4kPass *p, int s, int index) {
    if (index > 0) return p->stage[s].tex[index - 1].view;
    return s == 0 ? p->in.view : p->stage[s - 1].tex[PASSES - 1].view;
}

static void sets_write(Anime4kPass *p, int s) {
    for (int k = 0; k < PASSES; k++) {
        VkDescriptorImageInfo info[BINDINGS];
        VkWriteDescriptorSet w[BINDINGS];
        memset(w, 0, sizeof(w));
        for (int b = 0; b < BINDINGS; b++) {
            int output = b == ANIME4K_OUTPUT_BINDING;
            /* Bindings the pass does not declare get the stage input, never read. */
            int index = !output && b < GRAPH[k].binds ? GRAPH[k].bind[b] : 0;
            info[b].sampler = output ? VK_NULL_HANDLE : p->linear;
            info[b].imageView = output ? p->stage[s].tex[k].view : tex_view(p, s, index);
            info[b].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = p->stage[s].sets[k];
            w[b].dstBinding = (uint32_t)b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = output ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[b].pImageInfo = &info[b];
        }
        p->UpdateDescriptorSets(p->dev, BINDINGS, w, 0, NULL);
    }
}

/* The largest image is the last stage's output, w << stages wide: past the device's limit a size
 * is invalid usage, so it is refused before anything is made. */
static int size_within_limit(const Anime4kPass *p, int w, int h, int stages) {
    return (uint32_t)w <= (p->max_dim >> stages) && (uint32_t)h <= (p->max_dim >> stages);
}

static int sized_make(Anime4kPass *p, int w, int h, int stages) {
    p->size_builds++;
    p->DeviceWaitIdle(p->dev);
    sized_free(p);
    if (!img_make(p, &p->in, w, h, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return 0;
    for (int s = 0; s < stages; s++) {
        Stage *st = &p->stage[s];
        st->w = w << s;
        st->h = h << s;
        for (int k = 0; k < PASSES; k++) {
            int ow = st->w * GRAPH[k].scale, oh = st->h * GRAPH[k].scale;
            VkImageUsageFlags use = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (k == PASSES - 1) use |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (!img_make(p, &st->tex[k], ow, oh, use)) return 0;
        }
    }
    for (int s = 0; s < stages; s++) sets_write(p, s);
    p->w = w;
    p->h = h;
    p->stages = stages;
    return 1;
}

static void image_barrier(Anime4kPass *p, VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                          VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    p->CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

unsigned anime4k_size_builds(const Anime4kPass *p) {
    return p ? p->size_builds : 0;
}

VkImage anime4k_record(Anime4kPass *p, VkCommandBuffer cmd, VkImage src, int w, int h, int stages) {
    if (!p || !cmd || !src || w <= 0 || h <= 0 || stages < 1 || stages > ANIME4K_MAX_STAGES)
        return VK_NULL_HANDLE;
    if (w != p->w || h != p->h || stages != p->stages) {
        /* A size whose images could not be made is not tried again: no device wait per present. */
        if (w == p->fail_w && h == p->fail_h && stages == p->fail_stages) return VK_NULL_HANDLE;
        const char *why = NULL;
        if (!size_within_limit(p, w, h, stages)) {
            /* Refused before anything is touched: the last good size's images may still be in
             * flight, and they stay (no wait, no free, no rebuild if the caller goes back to it). */
            why = "past the device's image limit";
        } else if (!sized_make(p, w, h, stages)) {
            sized_free(p);  /* sized_make has waited for the device */
            why = "a Vulkan image or its memory could not be made";
        }
        if (why) {
            p->fail_w = w;
            p->fail_h = h;
            p->fail_stages = stages;
            fprintf(stderr, "sdl3vk: Anime4K cannot make its images for %dx%d at %d stage%s: %s "
                            "(the device's limit is %u)\n",
                    w, h, stages, stages == 1 ? "" : "s", why, p->max_dim);
            return VK_NULL_HANDLE;
        }
    }
    const VkPipelineStageFlags earlier = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    /* The shown image, 1:1, into stage 0's input; the previous present's reads come first. */
    image_barrier(p, cmd, p->in.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT, earlier, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit;
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1].x = w; blit.srcOffsets[1].y = h; blit.srcOffsets[1].z = 1;
    blit.dstOffsets[1] = blit.srcOffsets[1];
    p->CmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->in.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    image_barrier(p, cmd, p->in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    for (int s = 0; s < stages; s++)
        for (int k = 0; k < PASSES; k++)
            image_barrier(p, cmd, p->stage[s].tex[k].image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, earlier,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    VkMemoryBarrier between;
    memset(&between, 0, sizeof(between));
    between.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    between.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    between.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    for (int s = 0; s < stages; s++) {
        Stage *st = &p->stage[s];
        for (int k = 0; k < PASSES; k++) {
            int32_t size[2] = { st->w * GRAPH[k].scale, st->h * GRAPH[k].scale };
            p->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipes[k]);
            p->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &st->sets[k],
                                     0, NULL);
            p->CmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
            p->CmdDispatch(cmd, ((uint32_t)size[0] + 7u) / 8u, ((uint32_t)size[1] + 7u) / 8u, 1);
            p->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &between, 0, NULL, 0, NULL);
        }
    }
    VkImage out = p->stage[stages - 1].tex[PASSES - 1].image;
    image_barrier(p, cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    return out;
}

static VkShaderModule shader_make(Anime4kPass *p, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    return p->CreateShaderModule(p->dev, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static int fixed_make(Anime4kPass *p) {
    VkSamplerCreateInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p->CreateSampler(p->dev, &si, NULL, &p->linear) != VK_SUCCESS) return 0;

    VkDescriptorSetLayoutBinding b[BINDINGS];
    memset(b, 0, sizeof(b));
    for (uint32_t i = 0; i < BINDINGS; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i == ANIME4K_OUTPUT_BINDING ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                          : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo dl;
    memset(&dl, 0, sizeof(dl));
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = BINDINGS;
    dl.pBindings = b;
    if (p->CreateDescriptorSetLayout(p->dev, &dl, NULL, &p->dsl) != VK_SUCCESS) return 0;

    const uint32_t sets = ANIME4K_MAX_STAGES * PASSES;
    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * ANIME4K_MAX_BINDS },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets },
    };
    VkDescriptorPoolCreateInfo dp;
    memset(&dp, 0, sizeof(dp));
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = sets;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (p->CreateDescriptorPool(p->dev, &dp, NULL, &p->pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout layouts[ANIME4K_MAX_STAGES * PASSES];
    for (uint32_t i = 0; i < sets; i++) layouts[i] = p->dsl;
    VkDescriptorSet all[ANIME4K_MAX_STAGES * PASSES];
    VkDescriptorSetAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = p->pool;
    ai.descriptorSetCount = sets;
    ai.pSetLayouts = layouts;
    if (p->AllocateDescriptorSets(p->dev, &ai, all) != VK_SUCCESS) return 0;
    for (int s = 0; s < ANIME4K_MAX_STAGES; s++)
        for (int k = 0; k < PASSES; k++) p->stage[s].sets[k] = all[s * PASSES + k];

    VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 2u * (uint32_t)sizeof(int32_t) };
    VkPipelineLayoutCreateInfo pl;
    memset(&pl, 0, sizeof(pl));
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p->dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (p->CreatePipelineLayout(p->dev, &pl, NULL, &p->layout) != VK_SUCCESS) return 0;
    for (int k = 0; k < PASSES; k++) {
        int mutant = p->init.mutant_spirv && p->init.mutant_spirv_bytes && p->init.mutant_pass == k;
        p->mods[k] = mutant ? shader_make(p, p->init.mutant_spirv, p->init.mutant_spirv_bytes)
                            : shader_make(p, k_spv[k].code, k_spv[k].bytes);
        if (!p->mods[k]) return 0;
        VkComputePipelineCreateInfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = p->mods[k];
        pi.stage.pName = "main";
        pi.layout = p->layout;
        if (p->CreateComputePipelines(p->dev, VK_NULL_HANDLE, 1, &pi, NULL, &p->pipes[k]) != VK_SUCCESS)
            return 0;
    }
    return 1;
}

Anime4kPass *anime4k_create(const Anime4kInit *init, char *error, size_t error_size) {
    if (!init || !init->get_instance_proc_addr || !init->device) {
        set_error(error, error_size, "no Vulkan device");
        return NULL;
    }
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)init->get_instance_proc_addr(
            init->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetDeviceProcAddr dev_addr =
        (PFN_vkGetDeviceProcAddr)init->get_instance_proc_addr(init->instance, "vkGetDeviceProcAddr");
    if (!families || !dev_addr) {
        set_error(error, error_size, "the Vulkan loader is incomplete");
        return NULL;
    }
    VkQueueFamilyProperties qf[32];
    uint32_t nq = 32;
    families(init->physical_device, &nq, qf);
    if (init->queue_family >= nq || !(qf[init->queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        set_error(error, error_size, "this GPU's queue has no compute");
        return NULL;
    }
    Anime4kPass *p = (Anime4kPass *)calloc(1, sizeof(*p));
    if (!p) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    p->init = *init;
    p->dev = init->device;
    p->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
        init->get_instance_proc_addr(init->instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceProperties props_fn = (PFN_vkGetPhysicalDeviceProperties)
        init->get_instance_proc_addr(init->instance, "vkGetPhysicalDeviceProperties");
    if (props_fn) {
        VkPhysicalDeviceProperties props;
        props_fn(init->physical_device, &props);
        p->max_dim = props.limits.maxImageDimension2D;
    }
    if (!p->max_dim) p->max_dim = 4096u;  /* Vulkan's required minimum */
#define DEV(name) p->name = (PFN_vk##name)dev_addr(p->dev, "vk" #name)
    DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements); DEV(AllocateMemory);
    DEV(FreeMemory); DEV(BindImageMemory); DEV(CreateImageView); DEV(DestroyImageView);
    DEV(CreateSampler); DEV(DestroySampler); DEV(CreateDescriptorSetLayout);
    DEV(DestroyDescriptorSetLayout); DEV(CreateDescriptorPool); DEV(DestroyDescriptorPool);
    DEV(AllocateDescriptorSets); DEV(UpdateDescriptorSets); DEV(CreatePipelineLayout);
    DEV(DestroyPipelineLayout); DEV(CreateShaderModule); DEV(DestroyShaderModule);
    DEV(CreateComputePipelines); DEV(DestroyPipeline); DEV(DeviceWaitIdle);
    DEV(CmdPipelineBarrier); DEV(CmdBlitImage); DEV(CmdBindPipeline); DEV(CmdBindDescriptorSets);
    DEV(CmdPushConstants); DEV(CmdDispatch);
#undef DEV
    if (!fixed_make(p)) {
        set_error(error, error_size, "a Vulkan object could not be made");
        anime4k_destroy(p);
        return NULL;
    }
    return p;
}

void anime4k_destroy(Anime4kPass *p) {
    if (!p) return;
    p->DeviceWaitIdle(p->dev);
    sized_free(p);
    for (int k = 0; k < PASSES; k++) {
        if (p->pipes[k]) p->DestroyPipeline(p->dev, p->pipes[k], NULL);
        if (p->mods[k]) p->DestroyShaderModule(p->dev, p->mods[k], NULL);
    }
    if (p->layout) p->DestroyPipelineLayout(p->dev, p->layout, NULL);
    if (p->pool) p->DestroyDescriptorPool(p->dev, p->pool, NULL);
    if (p->dsl) p->DestroyDescriptorSetLayout(p->dev, p->dsl, NULL);
    if (p->linear) p->DestroySampler(p->dev, p->linear, NULL);
    free(p);
}
