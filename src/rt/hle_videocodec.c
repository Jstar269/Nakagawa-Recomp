// SPDX-License-Identifier: GPL-3.0-or-later AND MIT
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from JPCSP (https://github.com/jpcsp/jpcsp) revision 00ac4d6daedd89c0492093ae83fb0d3d819af414,
// GPL-3.0-or-later: src/jpcsp/HLE/modules/sceVideocodec.java, src/jpcsp/HLE/modules/sceMpegbase.java,
// src/jpcsp/HLE/kernel/types/SceMp4AvcCscStruct.java, src/jpcsp/mediaengine/MEMemory.java and
// src/jpcsp/mediaengine/MEEmulator.java.
// Modified by Nakagawa Recomp contributors, 2026-11-06.
//
// The JPCSP notice of those files, kept as upstream:
//   This file is part of jpcsp.
//   Jpcsp is free software: you can redistribute it and/or modify it under the terms of the GNU
//   General Public License as published by the Free Software Foundation, either version 3 of the
//   License, or (at your option) any later version.
//   Jpcsp is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
//   the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
//   Public License for more details.
//   You should have received a copy of the GNU General Public License along with Jpcsp.  If not,
//   see <http://www.gnu.org/licenses/>.
//
// The sceAudiocodec wrapper checks (argument and codec-range checks, the EDRAM allocation and
// release words and their error codes, the output-size rule) are adapted from uofw
// (https://github.com/uofw/uofw) revision 28306bb5bd4fcdcc0c1d469bf1f3bdb885dde3e9,
// src/kd/avcodec/audiocodec.c, under the MIT licence:
//   Copyright (c) 2011, 2012 The uOFW team
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//   and associated documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//   The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
//   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// The ATRAC3plus work-area words sceAudiocodecCheckNeedMem fills are JPCSP's
// (src/jpcsp/HLE/modules/sceMeAudio.java and sceAudiocodec.java at the JPCSP revision above).

/* The Media Engine (ME) boundary under a game's own disc mpeg.prx (#286): sceVideocodec
 * (8 calls), sceMpegbase (4 calls) and sceAudiocodec (5 calls). When mpeg.prx runs as LLE, these are the calls below it
 * that reach the ME, whose code is firmware the project never carries. Each handler is HLE over
 * a host model:
 *   - ME RAM is a host buffer at ME address 0 with a low-first allocator. The guest sees ME
 *     addresses only as opaque numbers in context words and packet lists.
 *   - sceVideocodecDecode hands the access unit to the host H.264 backend (src/rt/sr_h264.h)
 *     and keeps the decoded picture as host 4:2:0 planes, keyed by the ME address published
 *     as buffer 0 of that picture. The title's mpeg.prx imports no sceMpegBaseYCrCbCopy, so no
 *     guest code reads the PSP's banded picture layout and it is not written here.
 *   - sceMpegBaseCscAvc finds the planes by that address and converts them with the measured
 *     sr_h264_psp_rgb (PSP-MPEG-001) into the guest RGB buffer.
 *
 * derived-translated: the context-word offsets and values, the decode output words, the band
 * buffer allocation, the CSC struct layout, the packet-list layout and the pixel-mode mapping are
 * translated from the JPCSP files named above. Nothing here comes from JPCSP's ME code emulation
 * (MEProcessor, METhread, the ME MMIO handlers or the sceMeVideo driver).
 *
 * Every value marked "synthetic" below is JPCSP's value at 00ac4d6d, not a PSP measurement by this
 * project; PSP request W-LLE-1 measures them. Values marked "measured" come from PSP-VCODEC-001
 * (W-LLE-1 boot 149: a PSP-3000 on firmware 6.60, the firmware's own sceVideocodec and
 * sceAudiocodec on zeroed contexts): Open, GetEDRAM, Init, SetMemory and Stop before any decode,
 * and sceAudiocodecCheckNeedMem. The decode, band, CSC and copy words are not measured yet. Unknown codec types, a context the runtime did not
 * open, ME addresses outside ME RAM, zero-length packets, a CSC of a picture never decoded and a
 * build without an H.264 backend are refused with a named error and one diagnostic line.
 *
 *   - sceAudiocodec decodes the movie's ATRAC3plus frames with the project's ATRAC3plus decoder
 *     (atrac3p_bridge.h), by the same frame rules sceMpegAtracDecode uses in mpeg.c. Its EDRAM
 *     comes from the same ME RAM model. Only codec 0x1000 (ATRAC3plus) is modelled: it is the
 *     one a PSMF movie carries.
 *
 * sceVideocodec and sceMpegbase are reached through a disc mpeg.prx: sceMpeg is a guest-owned
 * library (guest_module_owns_library in hle_kernel_module.c), so a title that ships mpeg.prx runs
 * it and its calls land here. sceAudiocodec is live for every importer: its four calls that
 * returned 0x80010086 before now reach these handlers, and anything other than a PSMF ATRAC3plus
 * frame is refused with a named line. */

#include "hle_internal.h"
#include "atrac3p_bridge.h"

/* ---- named results (all synthetic: the codes a PSP returns for these refusals are not measured;
 * W-LLE-1) ---- */
/* JPCSP sceVideocodecDecode returns -1 for a type other than 0 or 1; type 1 is not modelled here. */
#define VC_ERR_TYPE           0xFFFFFFFFu
/* JPCSP sceVideocodecGetEDRAM returns -1 when its ME allocation fails. */
#define VC_ERR_NO_ME_MEMORY   0xFFFFFFFFu
/* JPCSP sceMpegbase_0530BE4E returns -1 for an unknown pixel mode. */
#define VC_ERR_PIXEL_MODE     0xFFFFFFFFu
/* JPCSP sceMpegBasePESpacketCopy: ERROR_INVALID_SIZE for a zero-length packet. */
#define VC_ERR_ZERO_LENGTH    0x80000104u
/* JPCSP's ERROR_AVC_INVALID_VALUE, used here for every state refusal JPCSP does not check. */
#define VC_ERR_NOT_OPEN       0x806201FEu
#define VC_ERR_STATE          0x806201FEu
#define VC_ERR_NO_PICTURE     0x806201FEu
#define VC_ERR_CSC_GEOMETRY   0x806201FEu
/* JPCSP's ERROR_AVC_VIDEO_FATAL, used here when no host decoder can run the access unit. */
#define VC_ERR_NO_BACKEND     0x80628002u
#define VC_ERR_DECODE         0x80628002u
/* A guest pointer that is null, unaligned or outside guest memory. */
#define VC_ERR_GUEST_ADDRESS  SCE_KERNEL_ERROR_ILLEGAL_ADDR
/* An ME address outside the modelled ME RAM. */
#define VC_ERR_ME_ADDRESS     SCE_KERNEL_ERROR_ILLEGAL_ADDR

/* ---- the ME RAM model (MEMemory.java, MEEmulator.java) ---- */
/* synthetic: 2 MiB, the first-generation PSP's ME RAM in JPCSP MEMemory (later models: 4 MiB);
 * W-LLE-1. */
#define ME_RAM_SIZE           0x200000u
/* synthetic: JPCSP MEEmulator allocates the first 0x10000 bytes at start and allocates low-first
 * with 64-byte alignment. The exact addresses the guest is handed are therefore synthetic too. */
#define ME_RAM_RESERVED_LOW   0x10000u
#define ME_ALLOC_ALIGN        64u
/* Host bookkeeping bound, not a PSP value: each open context holds at most two allocations. */
#define ME_MAX_ALLOCS         32

/* Each allocation records its owner, the guest address of the video context or audio work area
 * it was made for: a call frees or uses only an allocation it owns, whatever address a
 * guest-writable word names. */
typedef enum { ME_OWNER_VIDEO = 1, ME_OWNER_AUDIO = 2 } MeOwnerKind;
typedef struct { uint32_t addr, size, owner; MeOwnerKind kind; } MeAlloc;
static uint8_t *s_me_ram;
static MeAlloc s_me_allocs[ME_MAX_ALLOCS];   /* sorted by address */
static int s_me_alloc_count;

static uint8_t *me_ram(void) {
    if (!s_me_ram) s_me_ram = (uint8_t *)calloc(1u, ME_RAM_SIZE);
    return s_me_ram;
}

/* Low-first allocation of `size` bytes rounded up to the alignment, for `owner`; 0 when nothing
 * fits. */
static uint32_t me_alloc(uint32_t size, MeOwnerKind kind, uint32_t owner) {
    uint32_t rounded;
    if (!me_ram() || s_me_alloc_count >= ME_MAX_ALLOCS ||
        !sr_size_add_ok(size, ME_ALLOC_ALIGN - 1u, &rounded))
        return 0u;
    rounded &= ~(ME_ALLOC_ALIGN - 1u);
    if (rounded == 0u) rounded = ME_ALLOC_ALIGN;
    uint32_t cursor = ME_RAM_RESERVED_LOW;
    int at = 0;
    for (; at < s_me_alloc_count; at++) {
        if (s_me_allocs[at].addr - cursor >= rounded) break;
        cursor = s_me_allocs[at].addr + s_me_allocs[at].size;
    }
    if (cursor > ME_RAM_SIZE || ME_RAM_SIZE - cursor < rounded) return 0u;
    memmove(&s_me_allocs[at + 1], &s_me_allocs[at], (size_t)(s_me_alloc_count - at) * sizeof(MeAlloc));
    s_me_allocs[at].addr = cursor;
    s_me_allocs[at].size = rounded;
    s_me_allocs[at].owner = owner;
    s_me_allocs[at].kind = kind;
    s_me_alloc_count++;
    memset(s_me_ram + cursor, 0, rounded);
    return cursor;
}

/* Frees the allocation at `addr` only when `owner` owns it; 0 otherwise. */
static int me_free(uint32_t addr, MeOwnerKind kind, uint32_t owner) {
    for (int i = 0; i < s_me_alloc_count; i++) {
        if (s_me_allocs[i].addr != addr) continue;
        if (s_me_allocs[i].kind != kind || s_me_allocs[i].owner != owner) return 0;
        memmove(&s_me_allocs[i], &s_me_allocs[i + 1], (size_t)(s_me_alloc_count - i - 1) * sizeof(MeAlloc));
        s_me_alloc_count--;
        return 1;
    }
    return 0;
}

/* The raw address of the one audio allocation a work area owns; 0 when it owns none. */
static uint32_t me_audio_allocation(uint32_t work) {
    for (int i = 0; i < s_me_alloc_count; i++)
        if (s_me_allocs[i].kind == ME_OWNER_AUDIO && s_me_allocs[i].owner == work)
            return s_me_allocs[i].addr;
    return 0u;
}

/* An ME address range to its offset in the host ME RAM. JPCSP MEMemory shows the same RAM at
 * 0x00000000, 0x40000000, 0x80000000 and 0xA0000000 (synthetic: W-LLE-1); any other segment, or
 * a range that leaves ME RAM, is refused. */
static int me_span(uint32_t addr, uint32_t size, uint32_t *offset) {
    const uint32_t segment = addr >> 29;
    if (segment != 0u && segment != 2u && segment != 4u && segment != 5u) return 0;
    const uint32_t off = addr & 0x1FFFFFFFu;
    if (size == 0u || off >= ME_RAM_SIZE || size > ME_RAM_SIZE - off || !me_ram()) return 0;
    *offset = off;
    return 1;
}

static void me_w8(uint32_t off, uint8_t v) { s_me_ram[off] = v; }
static void me_w32(uint32_t off, uint32_t v) {
    s_me_ram[off] = (uint8_t)v;
    s_me_ram[off + 1u] = (uint8_t)(v >> 8);
    s_me_ram[off + 2u] = (uint8_t)(v >> 16);
    s_me_ram[off + 3u] = (uint8_t)(v >> 24);
}

/* ---- refusals ---- */
static uint32_t vc_refuse(const char *call, const char *reason, uint32_t error) {
    fprintf(stderr, "ME_BOUNDARY_REFUSED %s: %s (0x%08x)\n", call, reason, error);
    return error;
}

/* A guest structure the handler reads and writes as words. */
static int vc_guest_words(uint32_t addr, uint32_t bytes) {
    return addr != 0u && (addr & 3u) == 0u && sr_guest_span_writable(addr, bytes);
}

/* ---- sceVideocodec contexts (sceVideocodec.java) ---- */
#define VC_CONTEXT_BYTES      96u          /* JPCSP videocodecBufferSize */
#define VC_TYPE_AVC           0u
/* measured (PSP-VCODEC-001): sceVideocodecOpen writes +0 = 0x05100601 for both codec types; for
 * type 0 it writes +8 = 1 and +24 = 0x8344, the size sceVideocodecGetEDRAM reads, and returns 0.
 * For type 1 it writes +8 = 0x80000000 and +24 = 0x264C and returns 0x80628001. */
#define VC_OPEN_WORD0         0x05100601u
#define VC_OPEN_WORD8         1u
#define VC_OPEN_EDRAM_BYTES   0x8344u
#define VC_TYPE_SECOND        1u
#define VC_OPEN_T1_WORD8      0x80000000u
#define VC_OPEN_T1_EDRAM_BYTES 0x264Cu
#define VC_OPEN_T1_RESULT     0x80628001u
/* Host bookkeeping bound, not a PSP value. */
#define VC_MAX_CONTEXTS       4
/* synthetic: JPCSP allocates 4 sets of 8 band buffers and hands out set (frame count % 3);
 * W-LLE-1 items 2 and 3. */
#define VC_BAND_SETS          4
#define VC_PICTURE_SLOTS      3
/* Host decode scratch: a larger picture is refused, never truncated. Not a PSP value. */
#define VC_MAX_WIDTH          720
#define VC_MAX_HEIGHT         576
/* synthetic: JPCSP sceMpeg.mpegTimestampPerSecond, written into the ME info block and the SEI. */
#define VC_TIMESTAMP_PER_SECOND 90000u

/* Output structures the decode writes, by their highest byte touched (JPCSP videocodecDecodeType0). */
#define VC_BUFFER2_BYTES      40u
#define VC_YUV_BYTES          44u
#define VC_BUFFER3_BYTES      40u
#define VC_SEI_BYTES          36u
#define VC_ME_INFO1_BYTES     36u
#define VC_ME_INFO2_BYTES     32u

typedef struct {
    uint32_t key;        /* the ME address published as buffer 0 of this picture; 0 = empty */
    uint32_t width, height;
    uint8_t *y, *cb, *cr;
} VcPicture;

typedef struct {
    int used;
    uint32_t ctx;        /* guest address of the 96-byte context */
    uint32_t edram;      /* raw ME allocation behind context +92; 0 = none */
    int h264;            /* host decoder instance; -1 = none */
    uint32_t frame_count;
    uint32_t width, height;            /* last decoded picture; 0 before the first */
    uint32_t band_base;                /* ME allocation for the band buffers; 0 = none */
    uint32_t band_width, band_height;  /* the geometry the bands were sized for */
    uint32_t bands[VC_BAND_SETS][8];
    VcPicture pictures[VC_PICTURE_SLOTS];
} VcContext;

static VcContext s_vc[VC_MAX_CONTEXTS];

static VcContext *vc_find(uint32_t ctx) {
    for (int i = 0; i < VC_MAX_CONTEXTS; i++)
        if (s_vc[i].used && s_vc[i].ctx == ctx) return &s_vc[i];
    return NULL;
}

/* The checks every context call shares: codec type, an opened context, a readable context. */
static uint32_t vc_context_for(const char *call, uint32_t ctx, uint32_t type, int check_type,
                               VcContext **out) {
    if (check_type && type != VC_TYPE_AVC)
        return vc_refuse(call, "codec type other than 0 (AVC) is not modelled", VC_ERR_TYPE);
    VcContext *c = vc_find(ctx);
    if (!c) return vc_refuse(call, "context the runtime did not open", VC_ERR_NOT_OPEN);
    if (!vc_guest_words(ctx, VC_CONTEXT_BYTES))
        return vc_refuse(call, "context is not a writable 96-byte guest span", VC_ERR_GUEST_ADDRESS);
    *out = c;
    return 0u;
}

static void vc_release_pictures(VcContext *c) {
    for (int i = 0; i < VC_PICTURE_SLOTS; i++) {
        free(c->pictures[i].y);
        free(c->pictures[i].cb);
        free(c->pictures[i].cr);
        memset(&c->pictures[i], 0, sizeof(c->pictures[i]));
    }
}

static void vc_stop_decoder(VcContext *c) {
    if (c->h264 >= 0) sr_h264_destroy(c->h264);
    c->h264 = -1;
}

/* sceVideocodecOpen(ctx, type) */
static uint32_t h_VideocodecOpen(CpuState *s) {
    const uint32_t ctx = A0, type = A1;
    if (type == VC_TYPE_SECOND && vc_guest_words(ctx, VC_CONTEXT_BYTES) && !vc_find(ctx)) {
        /* measured: the PSP refuses type 1 here after writing three words. The later calls on a
         * type 1 context are not modelled and stay refused. */
        MEM_W32(ctx, VC_OPEN_WORD0);
        MEM_W32(ctx + 8u, VC_OPEN_T1_WORD8);
        MEM_W32(ctx + 24u, VC_OPEN_T1_EDRAM_BYTES);
        return vc_refuse("sceVideocodecOpen", "codec type 1 is refused, as the PSP refuses it", VC_OPEN_T1_RESULT);
    }
    if (type != VC_TYPE_AVC)
        return vc_refuse("sceVideocodecOpen", "codec type other than 0 (AVC) is not modelled", VC_ERR_TYPE);
    if (!vc_guest_words(ctx, VC_CONTEXT_BYTES))
        return vc_refuse("sceVideocodecOpen", "context is not a writable 96-byte guest span", VC_ERR_GUEST_ADDRESS);
    if (vc_find(ctx))
        return vc_refuse("sceVideocodecOpen", "context is already open", VC_ERR_STATE);
    VcContext *c = NULL;
    for (int i = 0; i < VC_MAX_CONTEXTS && !c; i++)
        if (!s_vc[i].used) c = &s_vc[i];
    if (!c) return vc_refuse("sceVideocodecOpen", "no free host context slot", VC_ERR_STATE);
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->ctx = ctx;
    c->h264 = -1;
    MEM_W32(ctx, VC_OPEN_WORD0);
    MEM_W32(ctx + 8u, VC_OPEN_WORD8);
    MEM_W32(ctx + 24u, VC_OPEN_EDRAM_BYTES);
    return 0u;
}

/* sceVideocodecGetEDRAM(ctx, type): allocate ((ctx+24) + 63) | 0x3F bytes of ME RAM;
 * ctx+20 = the 64-byte-aligned address, ctx+92 = the raw address. */
static uint32_t h_VideocodecGetEDRAM(CpuState *s) {
    VcContext *c;
    uint32_t r = vc_context_for("sceVideocodecGetEDRAM", A0, A1, 1, &c);
    if (r) return r;
    if (c->edram)
        return vc_refuse("sceVideocodecGetEDRAM", "context already holds its ME memory", VC_ERR_STATE);
    uint32_t size;
    if (!sr_size_add_ok(MEM_R32(c->ctx + 24u), 63u, &size))
        return vc_refuse("sceVideocodecGetEDRAM", "requested ME size overflows", VC_ERR_NO_ME_MEMORY);
    size |= 0x3Fu;
    const uint32_t addr = me_alloc(size, ME_OWNER_VIDEO, c->ctx);
    if (!addr) return vc_refuse("sceVideocodecGetEDRAM", "ME RAM cannot hold the requested size", VC_ERR_NO_ME_MEMORY);
    c->edram = addr;
    MEM_W32(c->ctx + 20u, (addr + 63u) & ~63u);
    MEM_W32(c->ctx + 92u, addr);
    return 0u;
}

/* sceVideocodecReleaseEDRAM(ctx) */
static uint32_t h_VideocodecReleaseEDRAM(CpuState *s) {
    VcContext *c;
    uint32_t r = vc_context_for("sceVideocodecReleaseEDRAM", A0, 0u, 0, &c);
    if (r) return r;
    MEM_W32(c->ctx + 20u, 0u);
    MEM_W32(c->ctx + 92u, 0u);
    if (c->edram) (void)me_free(c->edram, ME_OWNER_VIDEO, c->ctx);
    c->edram = 0u;
    return 0u;
}

/* sceVideocodecInit(ctx, type): +12 = +20 and +8 = 0 (measured, PSP-VCODEC-001, type 0; JPCSP's
 * +20 + 8 is what the PSP writes for type 1). */
static uint32_t h_VideocodecInit(CpuState *s) {
    VcContext *c;
    uint32_t r = vc_context_for("sceVideocodecInit", A0, A1, 1, &c);
    if (r) return r;
    MEM_W32(c->ctx + 12u, MEM_R32(c->ctx + 20u));
    MEM_W32(c->ctx + 8u, 0u);
    return 0u;
}

/* sceVideocodecSetMemory(ctx, type): JPCSP reads context +64..+76 and writes nothing. The host
 * decoder needs no guest work memory, so the words are not used; no context word changes
 * (synthetic: W-LLE-1 item 1 records every word after the call). */
static uint32_t h_VideocodecSetMemory(CpuState *s) {
    VcContext *c;
    return vc_context_for("sceVideocodecSetMemory", A0, A1, 1, &c);
}

/* synthetic: JPCSP sceVideocodecGetVersion writes this at context +4, citing its own JpcspTrace
 * record of a PSP; W-LLE-1 measures it. */
#define VC_VERSION_WORD       0x78u

/* sceVideocodecGetVersion(ctx, type): context +4 = the version word; nothing else changes. */
static uint32_t h_VideocodecGetVersion(CpuState *s) {
    VcContext *c;
    uint32_t r = vc_context_for("sceVideocodecGetVersion", A0, A1, 1, &c);
    if (r) return r;
    MEM_W32(c->ctx + 4u, VC_VERSION_WORD);
    return 0u;
}

/* sceVideocodecGetSEI(ctx, type): word +28 of the SEI block named by context +80 is cleared, as
 * JPCSP does (synthetic: W-LLE-1). A context with no writable SEI block is refused. */
static uint32_t h_VideocodecGetSEI(CpuState *s) {
    static const char call[] = "sceVideocodecGetSEI";
    VcContext *c;
    uint32_t r = vc_context_for(call, A0, A1, 1, &c);
    if (r) return r;
    const uint32_t sei = MEM_R32(c->ctx + 80u);
    if (!vc_guest_words(sei, VC_SEI_BYTES))
        return vc_refuse(call, "the SEI block at context +80 is not a writable guest span", VC_ERR_GUEST_ADDRESS);
    MEM_W32(sei + 28u, 0u);
    return 0u;
}

/* sceVideocodecStop(ctx, type): JPCSP drops its decoder; the next decode starts a new one. The
 * pictures already decoded stay convertible (synthetic: W-LLE-1). Before any decode the PSP
 * returns 0x806201FE and changes no context word (measured, PSP-VCODEC-001, type 0). */
static uint32_t h_VideocodecStop(CpuState *s) {
    VcContext *c;
    uint32_t r = vc_context_for("sceVideocodecStop", A0, A1, 1, &c);
    if (r) return r;
    if (c->h264 < 0 && c->frame_count == 0u)
        return vc_refuse("sceVideocodecStop", "no access unit was decoded since Open", VC_ERR_STATE);
    vc_stop_decoder(c);
    return 0u;
}

/* sceVideocodecDelete(ctx, type): the decoder, the band buffers, the ME memory and the pictures
 * go, and the context is closed. JPCSP notes from its own PSP traces that the call takes about
 * 40 ms; it returns at once here (synthetic timing: W-LLE-1). */
static uint32_t h_VideocodecDelete(CpuState *s) {
    VcContext *c;
    uint32_t r = vc_context_for("sceVideocodecDelete", A0, A1, 1, &c);
    if (r) return r;
    vc_stop_decoder(c);
    if (c->band_base) (void)me_free(c->band_base, ME_OWNER_VIDEO, c->ctx);
    if (c->edram) (void)me_free(c->edram, ME_OWNER_VIDEO, c->ctx);
    vc_release_pictures(c);
    memset(c, 0, sizeof(*c));
    c->h264 = -1;
    return 0u;
}

/* JPCSP Utilities.alignUp(value, 0x1FF). */
static uint32_t vc_align512(uint32_t v) { return (v + 0x1FFu) & ~0x1FFu; }

/* The band buffers, allocated once in ME RAM for the first picture's geometry, laid out as
 * JPCSP videocodecDecodeType0 does (synthetic: W-LLE-1 items 2 and 3). Only the addresses are
 * used: the bands themselves are not written (see the file comment). */
static int vc_allocate_bands(VcContext *c, uint32_t w, uint32_t h) {
    const uint32_t size_y1 = vc_align512(((w + 16u) >> 5) * (h >> 1) * 16u);
    const uint32_t size_y2 = vc_align512((w >> 5) * (h >> 1) * 16u);
    const uint32_t size_c1 = vc_align512(((w + 16u) >> 5) * (h >> 1) * 8u);
    const uint32_t size_c2 = vc_align512((w >> 5) * (h >> 1) * 8u);
    const uint32_t set_bytes = size_y1 + size_y2 + size_c1 + size_c2;
    const uint32_t step = set_bytes * VC_BAND_SETS;
    const uint32_t base = me_alloc(256u + step * 2u, ME_OWNER_VIDEO, c->ctx);
    if (!base) return 0;
    uint32_t base1 = base + 256u;
    uint32_t base2 = base1 + (size_y1 + size_y2) * VC_BAND_SETS;
    for (int i = 0; i < VC_BAND_SETS; i++) {
        c->bands[i][0] = base1;
        c->bands[i][1] = base1 + step;
        c->bands[i][2] = base1 + size_y1;
        c->bands[i][3] = base1 + size_y1 + step;
        c->bands[i][4] = base2;
        c->bands[i][5] = base2 + step;
        c->bands[i][6] = base2 + size_c1;
        c->bands[i][7] = base2 + size_c1 + step;
        base1 += size_y1 + size_y2;
        base2 += size_c1 + size_c2;
    }
    c->band_base = base;
    c->band_width = w;
    c->band_height = h;
    return 1;
}

static uint8_t s_vc_scratch_y[VC_MAX_WIDTH * VC_MAX_HEIGHT];
static uint8_t s_vc_scratch_cb[(VC_MAX_WIDTH / 2) * (VC_MAX_HEIGHT / 2)];
static uint8_t s_vc_scratch_cr[(VC_MAX_WIDTH / 2) * (VC_MAX_HEIGHT / 2)];

/* Keep the scratch picture as the host planes of picture slot `slot`. */
static int vc_store_picture(VcContext *c, int slot, uint32_t w, uint32_t h) {
    VcPicture *p = &c->pictures[slot];
    const size_t luma = (size_t)w * h, chroma = (size_t)(w / 2u) * (h / 2u);
    if (!p->y || p->width != w || p->height != h) {
        free(p->y); free(p->cb); free(p->cr);
        p->y = (uint8_t *)malloc(luma);
        p->cb = (uint8_t *)malloc(chroma);
        p->cr = (uint8_t *)malloc(chroma);
        if (!p->y || !p->cb || !p->cr) {
            free(p->y); free(p->cb); free(p->cr);
            memset(p, 0, sizeof(*p));
            return 0;
        }
    }
    for (uint32_t row = 0; row < h; row++)
        memcpy(p->y + (size_t)row * w, s_vc_scratch_y + (size_t)row * VC_MAX_WIDTH, w);
    for (uint32_t row = 0; row < h / 2u; row++) {
        memcpy(p->cb + (size_t)row * (w / 2u), s_vc_scratch_cb + (size_t)row * (VC_MAX_WIDTH / 2), w / 2u);
        memcpy(p->cr + (size_t)row * (w / 2u), s_vc_scratch_cr + (size_t)row * (VC_MAX_WIDTH / 2), w / 2u);
    }
    p->width = w;
    p->height = h;
    p->key = c->bands[slot][0];
    return 1;
}

/* sceVideocodecDecode(ctx, type): decode the access unit at ME address ctx+36, ctx+40 bytes.
 * The decode is synchronous here: the call returns after the host decoder ran (synthetic timing;
 * JPCSP blocks the caller about 4 ms on a decoder thread; W-LLE-1 item 2 measures it). */
static uint32_t h_VideocodecDecode(CpuState *s) {
    static const char call[] = "sceVideocodecDecode";
    VcContext *c;
    uint32_t r = vc_context_for(call, A0, A1, 1, &c);
    if (r) return r;
    const uint32_t ctx = c->ctx;
    const uint32_t au_addr = MEM_R32(ctx + 36u), au_size = MEM_R32(ctx + 40u);
    const uint32_t buffer2 = MEM_R32(ctx + 16u), yuv = MEM_R32(ctx + 44u);
    const uint32_t buffer3 = MEM_R32(ctx + 48u), sei = MEM_R32(ctx + 80u);
    uint32_t au_off, info1 = 0u, info2 = 0u, info1_off = 0u, info2_off = 0u;
    if (au_size == 0u) return vc_refuse(call, "zero-length access unit", VC_ERR_ZERO_LENGTH);
    if (!me_span(au_addr, au_size, &au_off))
        return vc_refuse(call, "access unit outside ME RAM", VC_ERR_ME_ADDRESS);
    if (!vc_guest_words(buffer2, VC_BUFFER2_BYTES) || !vc_guest_words(yuv, VC_YUV_BYTES) ||
        !vc_guest_words(buffer3, VC_BUFFER3_BYTES) || (sei && !vc_guest_words(sei, VC_SEI_BYTES)))
        return vc_refuse(call, "an output structure is not a writable guest span", VC_ERR_GUEST_ADDRESS);
    info1 = MEM_R32(yuv + 36u);
    info2 = MEM_R32(yuv + 40u);
    if ((info1 && !me_span(info1, VC_ME_INFO1_BYTES, &info1_off)) ||
        (info2 && !me_span(info2, VC_ME_INFO2_BYTES, &info2_off)))
        return vc_refuse(call, "an ME info block outside ME RAM", VC_ERR_ME_ADDRESS);
    if (!sr_h264_backend_available())
        return vc_refuse(call, SR_H264_UNAVAILABLE_BOUNDARY, VC_ERR_NO_BACKEND);
    if (c->h264 < 0) {
        c->h264 = sr_h264_create();
        if (c->h264 < 0) return vc_refuse(call, SR_H264_UNAVAILABLE_BOUNDARY, VC_ERR_NO_BACKEND);
    }
    if (sr_h264_submit_au_timed(c->h264, s_me_ram + au_off, au_size, -1, 0) < 0)
        return vc_refuse(call, "the H.264 backend rejected the access unit", VC_ERR_DECODE);

    /* Every picture the decoder has ready; the newest is the one handed over. */
    SrH264FrameTarget target;
    SrH264FrameInfo info, pulled;
    memset(&target, 0, sizeof(target));
    memset(&info, 0, sizeof(info));
    target.kind = SR_H264_TARGET_HOST_YUV;
    target.host_buffer = s_vc_scratch_y;
    target.host_width = VC_MAX_WIDTH;
    target.host_height = VC_MAX_HEIGHT;
    target.host_stride = VC_MAX_WIDTH;
    target.host_cb = s_vc_scratch_cb;
    target.host_cr = s_vc_scratch_cr;
    target.host_c_stride = VC_MAX_WIDTH / 2;
    int got = 0;
    for (;;) {
        const int pulled_result = sr_h264_frame_ex(c->h264, 0, &target, &pulled);
        if (pulled_result < 0) return vc_refuse(call, "the H.264 backend failed to deliver a picture", VC_ERR_DECODE);
        if (pulled_result == 0) break;
        info = pulled;
        got = 1;
    }

    int slot = 0;
    if (got) {
        const uint32_t w = (uint32_t)info.width, h = (uint32_t)info.height;
        if (info.width <= 0 || info.height <= 0 || w > VC_MAX_WIDTH || h > VC_MAX_HEIGHT ||
            (w & 15u) || (h & 15u))
            return vc_refuse(call, "decoded picture is not whole macroblocks inside the host scratch", VC_ERR_DECODE);
        if (!c->band_base && !vc_allocate_bands(c, w, h))
            return vc_refuse(call, "ME RAM cannot hold the band buffers", VC_ERR_NO_ME_MEMORY);
        if (w != c->band_width || h != c->band_height)
            return vc_refuse(call, "picture size differs from the size the band buffers were allocated for", VC_ERR_DECODE);
        slot = (int)(c->frame_count % VC_PICTURE_SLOTS);
        if (!vc_store_picture(c, slot, w, h))
            return vc_refuse(call, "no host memory for the decoded picture", VC_ERR_DECODE);
        c->width = w;
        c->height = h;
    }

    /* Output words (JPCSP videocodecDecoderStep and videocodecDecodeType0; synthetic: W-LLE-1
     * item 2). Without a picture only buffer2 changes: its size words keep the last picture's. */
    MEM_W32(ctx + 8u, 0u);
    MEM_W32(buffer2 + 8u, c->width);
    MEM_W32(buffer2 + 12u, c->height);
    MEM_W32(buffer2 + 28u, 1u);
    MEM_W32(buffer2 + 32u, got ? 1u : 0u);
    MEM_W32(buffer2 + 36u, got ? 0u : 1u);
    if (!got) return 0u;

    for (uint32_t i = 0; i < 8u; i++) MEM_W32(yuv + i * 4u, c->bands[slot][i]);
    MEM_W32(yuv + 32u, 1u);
    if (!info1) {
        info1 = c->band_base;
        info1_off = c->band_base;
        MEM_W32(yuv + 36u, info1);
    }
    if (!info2) {
        info2 = c->band_base + 36u;
        info2_off = c->band_base + 36u;
        MEM_W32(yuv + 40u, info2);
    }
    me_w8(info1_off, 0x02u);
    me_w32(info1_off + 8u, VC_TIMESTAMP_PER_SECOND);
    me_w32(info1_off + 16u, VC_TIMESTAMP_PER_SECOND);
    me_w32(info1_off + 24u, c->frame_count * 2u);
    me_w32(info1_off + 28u, 2u);
    me_w8(info1_off + 32u, 0x00u);
    me_w8(info1_off + 33u, 0x01u);
    me_w8(info2_off, 0x00u);
    me_w32(info2_off + 24u, 0u);
    me_w32(info2_off + 28u, 0u);

    MEM_W8(buffer3, 0x01u);
    MEM_W8(buffer3 + 1u, 0xFFu);
    MEM_W32(buffer3 + 4u, 3u);
    MEM_W32(buffer3 + 8u, 4u);
    MEM_W32(buffer3 + 12u, 1u);
    MEM_W8(buffer3 + 16u, 0u);
    MEM_W32(buffer3 + 20u, 0x10000u);
    MEM_W32(buffer3 + 32u, 4004u);
    MEM_W32(buffer3 + 36u, 240000u);

    c->frame_count++;
    if (sei) {
        MEM_W8(sei, 0x02u);
        MEM_W32(sei + 8u, VC_TIMESTAMP_PER_SECOND);
        MEM_W32(sei + 16u, VC_TIMESTAMP_PER_SECOND);
        MEM_W32(sei + 24u, (c->frame_count - 1u) * 2u);
        MEM_W32(sei + 28u, 2u);
        MEM_W8(sei + 32u, 0x00u);
        MEM_W8(sei + 33u, 0x01u);
    }
    return 0u;
}

/* ---- sceMpegbase (sceMpegbase.java) ---- */
/* The GE pixel formats mpeg.c's sceMpegAvcCsc writes: 0 = 5650, 1 = 5551, 2 = 4444, 3 = 8888. */
#define VC_PIXEL_5650 0
#define VC_PIXEL_5551 1
#define VC_PIXEL_4444 2
#define VC_PIXEL_8888 3
static int s_csc_pixel_mode = VC_PIXEL_8888;   /* JPCSP sceMpegbase.start() */
static uint32_t s_csc_default_width;

/* sceMpegBaseCscInit(bufferWidth): the default row width for a CscAvc given width 0. JPCSP also
 * builds a five-word coefficient table that never reaches the guest (W-LLE-1 item 4 measures what
 * the PSP writes); the conversion here is the measured sr_h264_psp_rgb. */
static uint32_t h_MpegBaseCscInit(CpuState *s) {
    s_csc_default_width = A0;
    return 0u;
}

/* sceMpegbase_0530BE4E(mode): the CscAvc pixel mode. JPCSP getPixelMode: 0 = 8888, 1 = 5650,
 * 2 = 5551, 3 = 4444 (synthetic: W-LLE-1 item 4). */
static uint32_t h_Mpegbase_0530BE4E(CpuState *s) {
    static const int k_mode[4] = { VC_PIXEL_8888, VC_PIXEL_5650, VC_PIXEL_5551, VC_PIXEL_4444 };
    if (A0 > 3u) return vc_refuse("sceMpegbase_0530BE4E", "unknown pixel mode", VC_ERR_PIXEL_MODE);
    s_csc_pixel_mode = k_mode[A0];
    return 0u;
}

/* sceMpegBasePESpacketCopy(packetInfo): copy a linked list of 16-byte packets
 * { main-RAM source, ME destination, next packet, length in the low 12 bits } into ME RAM.
 * The whole list is checked before the first byte moves, so a refused list copies nothing
 * (JPCSP copies the packets before a bad one). A list that loops back on itself is refused. */
static uint32_t vc_packet_check(uint32_t packet) {
    static const char call[] = "sceMpegBasePESpacketCopy";
    uint32_t off;
    if ((packet & 3u) || !sr_guest_span_readable(packet, 16u))
        return vc_refuse(call, "packet descriptor is not a readable guest span", VC_ERR_GUEST_ADDRESS);
    const uint32_t src = MEM_R32(packet), dst = MEM_R32(packet + 4u);
    const uint32_t len = MEM_R32(packet + 12u) & 0xFFFu;
    if (len == 0u) return vc_refuse(call, "zero-length packet", VC_ERR_ZERO_LENGTH);
    if (!src || !sr_guest_span_readable(src, len))
        return vc_refuse(call, "packet source is not a readable guest span", VC_ERR_GUEST_ADDRESS);
    if (!me_span(dst, len, &off))
        return vc_refuse(call, "packet destination outside ME RAM", VC_ERR_ME_ADDRESS);
    return 0u;
}

static uint32_t h_MpegBasePESpacketCopy(CpuState *s) {
    const uint32_t head = A0;
    uint32_t slow = head, fast = head;
    for (;;) {   /* Floyd's walk: every packet is checked, a cycle is caught when the two meet */
        for (int step = 0; step < 2 && fast; step++) {
            const uint32_t r = vc_packet_check(fast);
            if (r) return r;
            fast = MEM_R32(fast + 8u);
        }
        if (!fast) break;
        slow = MEM_R32(slow + 8u);
        if (slow == fast)
            return vc_refuse("sceMpegBasePESpacketCopy", "packet list loops back on itself", VC_ERR_GUEST_ADDRESS);
    }
    for (uint32_t packet = head; packet; packet = MEM_R32(packet + 8u)) {
        uint32_t off;
        const uint32_t len = MEM_R32(packet + 12u) & 0xFFFu;
        (void)me_span(MEM_R32(packet + 4u), len, &off);
        if (!sr_guest_copy_in(s_me_ram + off, MEM_R32(packet), len))
            return vc_refuse("sceMpegBasePESpacketCopy", "packet source became unreadable", VC_ERR_GUEST_ADDRESS);
    }
    return 0u;
}

/* The decoded picture whose eight published ME band addresses are `buffers`. */
static const VcPicture *vc_decoded_picture_for(const uint32_t buffers[8]) {
    for (int i = 0; i < VC_MAX_CONTEXTS; i++) {
        const VcContext *c = &s_vc[i];
        if (!c->used) continue;
        for (int slot = 0; slot < VC_PICTURE_SLOTS; slot++) {
            const VcPicture *p = &c->pictures[slot];
            if (!p->key || p->key != buffers[0]) continue;
            for (int b = 1; b < 8; b++)
                if (c->bands[slot][b] != buffers[b]) return NULL;
            return p;
        }
    }
    return NULL;
}

/* sceMpegBaseYCrCbCopy copies a decoded picture's bands out of ME RAM into eight guest buffers.
 * As for the decode, the bands' bytes are not written (see the file comment): the copy is a
 * second host picture keyed by the eight guest addresses, so a CscAvc of those buffers converts
 * it. The two halves of the copy (flags bit 0 and bit 1) are kept apart, exactly as the bands
 * split the picture in JPCSP's layout: bit 0 is the even luma rows and the left eight columns of
 * every 16-column chroma group, bit 1 the odd rows and the right eight. A copy converts only once
 * both halves have arrived. The record table is host bookkeeping, not a PSP value: when it is
 * full, the oldest record is dropped, and a later CscAvc of its buffers is refused by name. */
#define VC_COPY_SLOTS         8
#define VC_COPY_HALF_A        1u
#define VC_COPY_HALF_B        2u
typedef struct {
    uint32_t buffers[8];   /* the guest addresses; buffers[0] == 0 = empty */
    uint32_t halves;       /* VC_COPY_HALF_A | VC_COPY_HALF_B once complete */
    uint32_t age;
    VcPicture picture;     /* key = buffers[0] */
} VcCopy;
static VcCopy s_vc_copies[VC_COPY_SLOTS];
static uint32_t s_vc_copy_clock;

static int vc_copy_matches(const VcCopy *k, const uint32_t buffers[8]) {
    return k->buffers[0] != 0u && memcmp(k->buffers, buffers, sizeof(k->buffers)) == 0;
}

/* The complete copy whose eight guest buffers are `buffers`. */
static const VcPicture *vc_copied_picture_for(const uint32_t buffers[8]) {
    for (int i = 0; i < VC_COPY_SLOTS; i++)
        if (vc_copy_matches(&s_vc_copies[i], buffers))
            return s_vc_copies[i].halves == (VC_COPY_HALF_A | VC_COPY_HALF_B) ? &s_vc_copies[i].picture : NULL;
    return NULL;
}

/* The picture a CscAvc converts: a decoded picture by its ME bands, else a complete copy. */
static const VcPicture *vc_picture_for(const uint32_t buffers[8]) {
    const VcPicture *p = vc_decoded_picture_for(buffers);
    return p ? p : vc_copied_picture_for(buffers);
}

/* The copy record for `buffers`, made ready for a w x h picture: the existing record for these
 * buffers (emptied when its size differs), else a free one, else the oldest. NULL when no host
 * memory is left. */
static VcCopy *vc_copy_record(const uint32_t buffers[8], uint32_t w, uint32_t h) {
    VcCopy *k = NULL;
    for (int i = 0; i < VC_COPY_SLOTS && !k; i++)
        if (vc_copy_matches(&s_vc_copies[i], buffers)) k = &s_vc_copies[i];
    for (int i = 0; i < VC_COPY_SLOTS && !k; i++)
        if (!s_vc_copies[i].buffers[0]) k = &s_vc_copies[i];
    if (!k) {
        k = &s_vc_copies[0];
        for (int i = 1; i < VC_COPY_SLOTS; i++)
            if (s_vc_copies[i].age < k->age) k = &s_vc_copies[i];
        k->buffers[0] = 0u;   /* evicted: rebuilt below */
    }
    VcPicture *p = &k->picture;
    if (!vc_copy_matches(k, buffers) || p->width != w || p->height != h || !p->y) {
        const size_t luma = (size_t)w * h, chroma = (size_t)(w / 2u) * (h / 2u);
        free(p->y); free(p->cb); free(p->cr);
        memset(k, 0, sizeof(*k));
        p->y = (uint8_t *)calloc(luma, 1u);
        p->cb = (uint8_t *)calloc(chroma, 1u);
        p->cr = (uint8_t *)calloc(chroma, 1u);
        if (!p->y || !p->cb || !p->cr) {
            free(p->y); free(p->cb); free(p->cr);
            memset(k, 0, sizeof(*k));
            return NULL;
        }
        p->width = w;
        p->height = h;
        memcpy(k->buffers, buffers, sizeof(k->buffers));
        p->key = buffers[0];
    }
    k->age = ++s_vc_copy_clock;
    return k;
}

/* Copy half `half` (VC_COPY_HALF_A or _B) of picture `src` into `dst` (same size). */
static void vc_copy_half(VcPicture *dst, const VcPicture *src, uint32_t half) {
    const uint32_t w = src->width, h = src->height, cw = w / 2u, ch = h / 2u;
    const uint32_t first_row = half == VC_COPY_HALF_A ? 0u : 1u;
    const uint32_t first_col = half == VC_COPY_HALF_A ? 0u : 8u;
    for (uint32_t row = first_row; row < h; row += 2u)
        memcpy(dst->y + (size_t)row * w, src->y + (size_t)row * w, w);
    for (uint32_t row = 0; row < ch; row++)
        for (uint32_t col = first_col; col < cw; col += 16u) {
            const uint32_t n = cw - col < 8u ? cw - col : 8u;
            const size_t at = (size_t)row * cw + col;
            memcpy(dst->cb + at, src->cb + at, n);
            memcpy(dst->cr + at, src->cr + at, n);
        }
}

/* Read the 48-byte SceMp4AvcCscStruct { height in 16-pixel units, width in 16-pixel units,
 * mode0, mode1, buffer0..7 } at `csc`. */
static int vc_read_csc_struct(uint32_t csc, uint32_t *h16, uint32_t *w16, uint32_t buffers[8]) {
    if ((csc & 3u) || !sr_guest_span_readable(csc, 48u)) return 0;
    *h16 = MEM_R32(csc);
    *w16 = MEM_R32(csc + 4u);
    for (uint32_t i = 0; i < 8u; i++) buffers[i] = MEM_R32(csc + 16u + i * 4u);
    return 1;
}

/* sceMpegBaseYCrCbCopy(dst, src, flags): src names a decoded picture by its ME bands, dst the
 * eight guest buffers it is copied into. JPCSP sizes the copy from the src struct's geometry
 * and ignores the dst struct's. Flags 0 copies nothing. */
static uint32_t h_MpegBaseYCrCbCopy(CpuState *s) {
    static const char call[] = "sceMpegBaseYCrCbCopy";
    const uint32_t flags = A2;
    uint32_t dh16, dw16, sh16, sw16, dst[8], src[8];
    if (!vc_read_csc_struct(A0, &dh16, &dw16, dst) || !vc_read_csc_struct(A1, &sh16, &sw16, src))
        return vc_refuse(call, "a CSC struct is not a readable guest span", VC_ERR_GUEST_ADDRESS);
    if (flags & ~(VC_COPY_HALF_A | VC_COPY_HALF_B))
        return vc_refuse(call, "flags other than bits 0 and 1 are not modelled", VC_ERR_STATE);
    const VcPicture *p = vc_decoded_picture_for(src);
    if (!p) return vc_refuse(call, "copy of a picture this boundary never decoded", VC_ERR_NO_PICTURE);
    if (sw16 > 0xFFFu || sh16 > 0xFFFu || (sw16 << 4) != p->width || (sh16 << 4) != p->height)
        return vc_refuse(call, "source struct size differs from the decoded picture", VC_ERR_CSC_GEOMETRY);
    /* JPCSP's copy sizes, in bytes per band. */
    const uint32_t w = p->width, h = p->height;
    const uint32_t size1 = ((w + 16u) >> 5) * (h >> 1) * 16u, size2 = (w >> 5) * (h >> 1) * 16u;
    const uint32_t bytes[8] = { size1, size2, size1, size2, size1 >> 1, size2 >> 1, size1 >> 1, size2 >> 1 };
    for (uint32_t i = 0; i < 8u; i++) {
        const uint32_t half = (i & 2u) ? VC_COPY_HALF_B : VC_COPY_HALF_A;
        if ((flags & half) && bytes[i] && (!dst[i] || !sr_guest_span_writable_any_view(dst[i], bytes[i])))
            return vc_refuse(call, "a destination buffer is not a writable guest span", VC_ERR_GUEST_ADDRESS);
    }
    if (!flags) return 0u;
    VcCopy *k = vc_copy_record(dst, w, h);
    if (!k) return vc_refuse(call, "no host memory for the copied picture", VC_ERR_DECODE);
    if (flags & VC_COPY_HALF_A) vc_copy_half(&k->picture, p, VC_COPY_HALF_A);
    if (flags & VC_COPY_HALF_B) vc_copy_half(&k->picture, p, VC_COPY_HALF_B);
    k->halves |= flags;
    return 0u;
}

/* Convert the rectangle (rx16, ry16, rw16, rh16), in 16-pixel units, of the picture the CSC
 * struct at `csc` names into `dest`, whose first row is the rectangle's top row; rows are
 * `buffer_width` pixels apart (0 takes CscInit's default). `whole` converts the whole picture. */
static uint32_t vc_csc(const char *call, uint32_t dest, uint32_t buffer_width, uint32_t csc,
                       int whole, uint32_t rx16, uint32_t ry16, uint32_t rw16, uint32_t rh16) {
    uint32_t h16, w16, buffers[8];
    if (!buffer_width) buffer_width = s_csc_default_width;
    if (!vc_read_csc_struct(csc, &h16, &w16, buffers))
        return vc_refuse(call, "CSC struct is not a readable guest span", VC_ERR_GUEST_ADDRESS);
    const VcPicture *p = vc_picture_for(buffers);
    if (!p) return vc_refuse(call, "CSC of a picture this boundary never decoded", VC_ERR_NO_PICTURE);
    if (w16 > 0xFFFu || h16 > 0xFFFu || (w16 << 4) != p->width || (h16 << 4) != p->height)
        return vc_refuse(call, "CSC struct size differs from the decoded picture", VC_ERR_CSC_GEOMETRY);
    if (whole) { rx16 = 0u; ry16 = 0u; rw16 = w16; rh16 = h16; }
    if (!rw16 || !rh16 || rx16 > w16 || rw16 > w16 - rx16 || ry16 > h16 || rh16 > h16 - ry16)
        return vc_refuse(call, "range is empty or leaves the picture", VC_ERR_CSC_GEOMETRY);
    const uint32_t rx = rx16 << 4, ry = ry16 << 4, rw = rw16 << 4, rh = rh16 << 4;
    if (buffer_width < rw)
        return vc_refuse(call, "row width narrower than the converted rectangle", VC_ERR_CSC_GEOMETRY);
    const int mode = s_csc_pixel_mode;
    const uint32_t bpp = mode == VC_PIXEL_8888 ? 4u : 2u;
    SrGuestRectSpan span;
    if (!dest || !sr_guest_rect_geometry(dest, 0u, 0u, buffer_width, rw, rh, bpp, &span) ||
        !sr_guest_span_writable_any_view(span.first, span.total_bytes))
        return vc_refuse(call, "destination rectangle is outside guest memory", VC_ERR_GUEST_ADDRESS);
    uint8_t *out = (uint8_t *)malloc(span.row_bytes);
    if (!out) return vc_refuse(call, "no host scratch row", VC_ERR_CSC_GEOMETRY);
    const uint32_t cw = p->width / 2u;
    for (uint32_t row = 0; row < rh; row++) {
        const uint32_t y = ry + row;
        for (uint32_t col = 0; col < rw; col++) {
            const uint32_t x = rx + col;
            const size_t c_at = (size_t)(y >> 1) * cw + (x >> 1);
            uint8_t r, g, b;
            sr_h264_psp_rgb(p->y[(size_t)y * p->width + x], p->cb[c_at], p->cr[c_at], &r, &g, &b);
            uint16_t v16;
            switch (mode) {
                case VC_PIXEL_5650: v16 = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); break;
                case VC_PIXEL_5551: v16 = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000u); break;
                case VC_PIXEL_4444: v16 = (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000u); break;
                default:            v16 = 0u; break;
            }
            if (bpp == 4u) {   /* 8888: R, G, B and alpha 0, as mpeg.c's sceMpegAvcCsc writes */
                out[col * 4u + 0u] = r;
                out[col * 4u + 1u] = g;
                out[col * 4u + 2u] = b;
                out[col * 4u + 3u] = 0u;
            } else {
                out[col * 2u + 0u] = (uint8_t)v16;
                out[col * 2u + 1u] = (uint8_t)(v16 >> 8);
            }
        }
        if (!sr_guest_copy_out(span.first + row * span.row_pitch, out, span.row_bytes)) {
            free(out);
            return vc_refuse(call, "a destination row could not be written", VC_ERR_GUEST_ADDRESS);
        }
    }
    free(out);
    return 0u;
}

/* sceMpegBaseCscAvc(bufferRGB, unused, bufferWidth, cscStruct). mode0 and mode1 in the struct are
 * unused (as in JPCSP), and so is the second argument. */
static uint32_t h_MpegBaseCscAvc(CpuState *s) {
    return vc_csc("sceMpegBaseCscAvc", A0, A2, A3, 1, 0u, 0u, 0u, 0u);
}

/* sceMpegBaseCscAvcRange(bufferRGB, unused, range, bufferWidth, cscStruct): range is four words
 * { x, y, width, height } in 16-pixel units, and the rectangle's top-left pixel lands at
 * bufferRGB (JPCSP hleMpegBaseCscAvcRange). The fifth argument arrives in t0. */
static uint32_t h_MpegBaseCscAvcRange(CpuState *s) {
    static const char call[] = "sceMpegBaseCscAvcRange";
    const uint32_t range = A2;
    if ((range & 3u) || !sr_guest_span_readable(range, 16u))
        return vc_refuse(call, "range is not a readable guest span", VC_ERR_GUEST_ADDRESS);
    return vc_csc(call, A0, A3, stack_arg(s, 0), 0, MEM_R32(range), MEM_R32(range + 4u),
                  MEM_R32(range + 8u), MEM_R32(range + 12u));
}

/* ---- sceAudiocodec (uofw audiocodec.c for the wrapper, JPCSP for the ME words) ---- */
/* Each call takes (workArea, codecType). The work area words this boundary uses:
 *   +0 version word, +8 error, +12 EDRAM address (64-byte aligned), +16 EDRAM bytes needed,
 *   +24 input, +28 bytes read, +32 output, +36 bytes written, +52/+60/+64 ATRAC3plus words,
 *   +56/+72 the input and output channel words of the output-size rule, +104 the raw EDRAM
 *   allocation (uofw names it allocMem). */
#define AC_CODEC_FIRST          0x1000u
#define AC_CODEC_END            0x1006u     /* uofw: codec < 0x1000 || codec >= 0x1006 */
#define AC_CODEC_AT3PLUS        0x1000u
#define AC_WORK_BYTES           104u        /* uofw pspK1StaBufOk(info, 104) */
#define AC_WORK_BYTES_EDRAM     108u        /* uofw GetEDRAM/ReleaseEDRAM check 108 bytes */
/* uofw error codes. */
#define AC_ERR_NOT_RAM          0x807F0002u
#define AC_ERR_NOT_SUPPORTED    0x80000004u /* SCE_ERROR_NOT_SUPPORTED */
#define AC_ERR_PRIV_REQUIRED    0x80000023u /* SCE_ERROR_PRIV_REQUIRED */
#define AC_ERR_EDRAM            0x807F0003u
#define AC_ERR_EDRAM_NOT_HELD   0x807F0004u
/* synthetic: uofw maps an ME driver result of -1 to 0x807F00FF (g_retValues[3]); JPCSP's
 * driver returns -1 for a decode it cannot run. Used for every ME-side refusal (W-LLE-1). */
#define AC_ERR_ME               0x807F00FFu
/* measured (PSP-VCODEC-001): sceAudiocodecCheckNeedMem for ATRAC3plus reads the halfword at +40,
 * which sceMpegCreate leaves at 0x5C28 or 0x5C5C. For each it returns 0 and writes +16 (the ME
 * memory the codec needs), +52 (the sample rate), +60 and +64. For 0 it returns 0x807F00FD and
 * writes +8, +52 and +64. Any other halfword is not measured and is refused. */
#define AC_AT3P_PARAM_OFF       40u
#define AC_AT3P_ZERO_RESULT     0x807F00FDu
#define AC_AT3P_ZERO_WORD8      0x20Fu
#define AC_AT3P_ZERO_WORD52     0x7D00u
#define AC_AT3P_ZERO_WORD64     8u
#define AC_AT3P_WORD64          0x2E8u
#define AC_FRAME_SAMPLES        2048u
/* Host bookkeeping bound, not a PSP value. */
#define AC_MAX_CONTEXTS         4

typedef struct {
    int used;
    uint32_t work;        /* guest address of the work area */
    Atrac3pBridge *at3p;  /* created at the first decode: the frame names its size and layout */
    int align, channels;
} AcContext;

static AcContext s_ac[AC_MAX_CONTEXTS];
static int16_t s_ac_pcm[2u * AC_FRAME_SAMPLES];

static AcContext *ac_find(uint32_t work) {
    for (int i = 0; i < AC_MAX_CONTEXTS; i++)
        if (s_ac[i].used && s_ac[i].work == work) return &s_ac[i];
    return NULL;
}

static void ac_drop(AcContext *c) {
    atrac3p_bridge_destroy(c->at3p);
    memset(c, 0, sizeof(*c));
}

/* The checks every sceAudiocodec call starts with (uofw): a work area in RAM, a codec in range,
 * then, here, the one codec this boundary models. */
static uint32_t ac_check(const char *call, uint32_t work, uint32_t codec, uint32_t bytes) {
    if (!vc_guest_words(work, bytes))
        return vc_refuse(call, "work area is not a writable guest span", AC_ERR_NOT_RAM);
    if (codec < AC_CODEC_FIRST || codec >= AC_CODEC_END)
        return vc_refuse(call, "codec type out of range", AC_ERR_NOT_SUPPORTED);
    if (codec != AC_CODEC_AT3PLUS)
        return vc_refuse(call, "codec type other than 0x1000 (ATRAC3plus) is not modelled", AC_ERR_NOT_SUPPORTED);
    return 0u;
}

/* sceAudiocodecCheckNeedMem(workArea, codec) */
static uint32_t h_AudiocodecCheckNeedMem(CpuState *s) {
    static const char call[] = "sceAudiocodecCheckNeedMem";
    static const struct {
        uint16_t param;
        uint32_t needed, rate, word60;
    } k_at3p[] = {
        { 0x5C28u, 0x7BC0u, 44100u, 2u },
        { 0x5C5Cu, 0x1F6C0u, 48000u, 7u },
    };
    const uint32_t work = A0;
    const uint32_t r = ac_check(call, work, A1, AC_WORK_BYTES);
    if (r) return r;
    const uint16_t param = MEM_R16(work + AC_AT3P_PARAM_OFF);
    if (param == 0u) {
        MEM_W32(work, VC_OPEN_WORD0);
        MEM_W32(work + 8u, AC_AT3P_ZERO_WORD8);
        MEM_W32(work + 52u, AC_AT3P_ZERO_WORD52);
        MEM_W32(work + 64u, AC_AT3P_ZERO_WORD64);
        return vc_refuse(call, "the ATRAC3plus parameter halfword at +40 is 0, as the PSP refuses it",
                         AC_AT3P_ZERO_RESULT);
    }
    for (size_t i = 0; i < sizeof(k_at3p) / sizeof(k_at3p[0]); i++) {
        if (param != k_at3p[i].param) continue;
        MEM_W32(work, VC_OPEN_WORD0);
        MEM_W32(work + 16u, k_at3p[i].needed);
        MEM_W32(work + 52u, k_at3p[i].rate);
        MEM_W32(work + 60u, k_at3p[i].word60);
        MEM_W32(work + 64u, AC_AT3P_WORD64);
        return 0u;
    }
    return vc_refuse(call, "an ATRAC3plus parameter halfword at +40 other than 0x5C28 or 0x5C5C is not measured",
                     AC_ERR_ME);
}

/* sceAudiocodecGetEDRAM(workArea, codec): uofw allocates ((+16) + 0x3F) | 0x3F bytes of ME
 * memory, keeps the raw address at +104 and publishes it 64-byte aligned at +12.
 * uofw does not check for an earlier allocation: it allocates again and overwrites +104,
 * orphaning the first, with no error of its own. Here the ME RAM is shared with every video
 * context, so the orphan would starve them:
 *   - a work area whose +104 still names the allocation it owns is refused (0x807F0003, a
 *     synthetic choice like sceVideocodecGetEDRAM's: W-LLE-1);
 *   - a work area that owns an allocation +104 no longer names was recycled without a release
 *     (cleared or reused by the title): that stale allocation and its host decoder go, and a
 *     new allocation is made, as on the PSP. */
static uint32_t h_AudiocodecGetEDRAM(CpuState *s) {
    static const char call[] = "sceAudiocodecGetEDRAM";
    const uint32_t work = A0;
    const uint32_t r = ac_check(call, work, A1, AC_WORK_BYTES_EDRAM);
    if (r) return r;
    const uint32_t held = me_audio_allocation(work);
    if (held) {
        if (MEM_R32(work + 104u) == held)
            return vc_refuse(call, "work area already holds its ME memory (sceAudiocodecReleaseEDRAM first)",
                             AC_ERR_EDRAM);
        (void)me_free(held, ME_OWNER_AUDIO, work);
        AcContext *stale = ac_find(work);
        if (stale) ac_drop(stale);
    }
    uint32_t size;
    uint32_t addr = 0u;
    if (sr_size_add_ok(MEM_R32(work + 16u), 0x3Fu, &size)) addr = me_alloc(size | 0x3Fu, ME_OWNER_AUDIO, work);
    if (!addr) {
        MEM_W32(work + 104u, 0u);
        MEM_W32(work + 12u, 0u);
        return vc_refuse(call, "ME RAM cannot hold the requested size", AC_ERR_EDRAM);
    }
    MEM_W32(work + 104u, addr);
    MEM_W32(work + 12u, (addr + 0x3Fu) & ~0x3Fu);
    return 0u;
}

/* sceAudiocodecReleaseEDRAM(workArea): uofw frees +104 and clears +104 and +12; a work area that
 * holds no ME allocation is refused. +104 is guest-writable, so it must name the allocation
 * this work area owns: any other address (another work area's, a video context's) is refused
 * with the same code and nothing is freed. The host decoder for the work area goes too
 * (JPCSP). */
static uint32_t h_AudiocodecReleaseEDRAM(CpuState *s) {
    static const char call[] = "sceAudiocodecReleaseEDRAM";
    const uint32_t work = A0;
    if (!vc_guest_words(work, AC_WORK_BYTES_EDRAM))
        return vc_refuse(call, "work area is not a writable guest span", AC_ERR_PRIV_REQUIRED);
    const uint32_t alloc = MEM_R32(work + 104u);
    if (!alloc || (alloc & 0x1FFFFFFFu) > 0x3FFFFFu)
        return vc_refuse(call, "work area holds no ME allocation of this boundary", AC_ERR_EDRAM_NOT_HELD);
    if (alloc != me_audio_allocation(work) || !me_free(alloc, ME_OWNER_AUDIO, work))
        return vc_refuse(call, "+104 names ME memory this work area does not own", AC_ERR_EDRAM_NOT_HELD);
    MEM_W32(work + 104u, 0u);
    MEM_W32(work + 12u, 0u);
    AcContext *c = ac_find(work);
    if (c) ac_drop(c);
    return 0u;
}

/* sceAudiocodecInit(workArea, codec): the work area becomes a decoding context. A context
 * initialised again starts from a clean decoder (JPCSP releases the old one). */
static uint32_t h_AudiocodecInit(CpuState *s) {
    static const char call[] = "sceAudiocodecInit";
    const uint32_t work = A0;
    const uint32_t r = ac_check(call, work, A1, AC_WORK_BYTES);
    if (r) return r;
    const uint32_t held = me_audio_allocation(work);
    if (!held)
        return vc_refuse(call, "work area holds no EDRAM (sceAudiocodecGetEDRAM first)", AC_ERR_ME);
    if (MEM_R32(work + 12u) != ((held + 0x3Fu) & ~0x3Fu))
        return vc_refuse(call, "+12 is not the EDRAM sceAudiocodecGetEDRAM published", AC_ERR_ME);
    AcContext *c = ac_find(work);
    if (c) {
        if (c->at3p) (void)atrac3p_bridge_reset(c->at3p);
    } else {
        for (int i = 0; i < AC_MAX_CONTEXTS && !c; i++)
            if (!s_ac[i].used) c = &s_ac[i];
        if (!c) return vc_refuse(call, "no free host context slot", AC_ERR_ME);
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->work = work;
    }
    MEM_W32(work, VC_OPEN_WORD0);
    MEM_W32(work + 8u, 0u);
    return 0u;
}

/* sceAudiocodecDecode(workArea, codec): one ATRAC3plus frame from +24 to PCM at +32.
 * The input is a PSMF audio frame: an 8-byte 0x0FD0 header whose bytes 2-3 name the size and
 * channel layout, then the frame (the rule mpeg.c's sceMpegAtracDecode applies). The output size
 * follows uofw's rule: 0x2000 when +56 is 1 and +72 is not, else +72 << 12. Mono input written to
 * a stereo output is duplicated into both channels. Synchronous (synthetic timing: JPCSP delays
 * the caller 2.3 ms; W-LLE-1). */
static uint32_t h_AudiocodecDecode(CpuState *s) {
    static const char call[] = "sceAudiocodecDecode";
    const uint32_t work = A0;
    const uint32_t r = ac_check(call, work, A1, AC_WORK_BYTES);
    if (r) return r;
    AcContext *c = ac_find(work);
    if (!c) return vc_refuse(call, "work area the runtime did not initialise", AC_ERR_ME);
    MEM_W32(work + 8u, 0u);
    const uint32_t in = MEM_R32(work + 24u), out = MEM_R32(work + 32u);
    const uint32_t w56 = MEM_R32(work + 56u), w72 = MEM_R32(work + 72u);
    const uint32_t out_bytes = (w56 == 1u && w72 != 1u) ? 0x2000u : (w72 << 12);
    if (out_bytes != 0x1000u && out_bytes != 0x2000u)
        return vc_refuse(call, "output layout other than mono or stereo 16-bit is not modelled", AC_ERR_ME);
    const int out_channels = (int)(out_bytes >> 12);
    uint8_t hdr[8];
    if (!sr_guest_copy_in(hdr, in, sizeof(hdr)))
        return vc_refuse(call, "input is not a readable guest span", AC_ERR_PRIV_REQUIRED);
    if (hdr[0] != 0x0Fu || hdr[1] != 0xD0u)
        return vc_refuse(call, "input without the PSMF ATRAC3plus frame header is not modelled", AC_ERR_ME);
    /* The same expression, precedence included, as mpeg.c's mpeg_audio_decode_frame
     * (sceMpegAtracDecode): the two must agree. */
    const uint32_t frame_bytes = (uint32_t)(((hdr[2] & 3u) << 8) | ((uint32_t)hdr[3] * 8u)) + 0x10u;
    const int data = (int)(frame_bytes - 8u);
    const int channels = (hdr[2] >> 2) & 7;
    if (channels < 1 || channels > 2)
        return vc_refuse(call, "the frame header names no mono or stereo layout", AC_ERR_ME);
    if (channels > out_channels)
        return vc_refuse(call, "a stereo frame into a mono output (downmix) is not modelled", AC_ERR_ME);
    if (!sr_guest_span_readable(in, frame_bytes))
        return vc_refuse(call, "input frame is not a readable guest span", AC_ERR_PRIV_REQUIRED);
    if (!out || !sr_guest_span_writable(out, out_bytes))
        return vc_refuse(call, "output is not a writable guest span", AC_ERR_PRIV_REQUIRED);
    if (!c->at3p || c->align != data || c->channels != channels) {
        atrac3p_bridge_destroy(c->at3p);
        c->at3p = NULL;
        if (atrac3p_bridge_create(channels, data, &c->at3p) != 0) {
            c->at3p = NULL;
            return vc_refuse(call, "no ATRAC3plus decoder for this frame size", AC_ERR_ME);
        }
        c->align = data;
        c->channels = channels;
    }
    uint8_t *frame = (uint8_t *)malloc((size_t)data);
    if (!frame) return vc_refuse(call, "no host memory for the frame", AC_ERR_ME);
    int samples = 0;
    const int ok = sr_guest_copy_in(frame, in + 8u, (uint32_t)data) &&
                   atrac3p_bridge_decode(c->at3p, frame, data, s_ac_pcm, &samples) == 0 &&
                   samples == (int)AC_FRAME_SAMPLES;
    free(frame);
    if (!ok) return vc_refuse(call, "the ATRAC3plus frame did not decode to one block", AC_ERR_ME);
    if (channels == 1 && out_channels == 2)
        for (int i = (int)AC_FRAME_SAMPLES - 1; i >= 0; i--) {
            s_ac_pcm[i * 2] = s_ac_pcm[i];
            s_ac_pcm[i * 2 + 1] = s_ac_pcm[i];
        }
    uint8_t *bytes = (uint8_t *)malloc(out_bytes);
    if (!bytes) return vc_refuse(call, "no host memory for the block", AC_ERR_ME);
    for (uint32_t i = 0; i < out_bytes / 2u; i++) {   /* PSP memory is little-endian */
        bytes[i * 2u] = (uint8_t)((uint16_t)s_ac_pcm[i]);
        bytes[i * 2u + 1u] = (uint8_t)((uint16_t)s_ac_pcm[i] >> 8);
    }
    const int copied = sr_guest_copy_out(out, bytes, out_bytes);
    free(bytes);
    if (!copied) return vc_refuse(call, "the output block could not be written", AC_ERR_PRIV_REQUIRED);
    /* synthetic (W-LLE-1): JPCSP writes the frame bytes its decoder read at +28 and the output
     * size at +36; here +28 is the frame without its 8-byte header. */
    MEM_W32(work + 28u, (uint32_t)data);
    MEM_W32(work + 36u, out_bytes);
    return 0u;
}

void hle_register_videocodec_handlers(void) {
    sr_hle_register(0xc01ec829u, "sceVideocodecOpen", h_VideocodecOpen);
    sr_hle_register(0x17099f0au, "sceVideocodecInit", h_VideocodecInit);
    sr_hle_register(0xdba273fau, "sceVideocodecDecode", h_VideocodecDecode);
    sr_hle_register(0xa2f0564eu, "sceVideocodecStop", h_VideocodecStop);
    sr_hle_register(0x307e6e1cu, "sceVideocodecDelete", h_VideocodecDelete);
    sr_hle_register(0x2d31f5b1u, "sceVideocodecGetEDRAM", h_VideocodecGetEDRAM);
    sr_hle_register(0x4f160bf4u, "sceVideocodecReleaseEDRAM", h_VideocodecReleaseEDRAM);
    sr_hle_register(0x745a7b7au, "sceVideocodecSetMemory", h_VideocodecSetMemory);
    sr_hle_register(0x26927d19u, "sceVideocodecGetVersion", h_VideocodecGetVersion);
    sr_hle_register(0x627b7d42u, "sceVideocodecGetSEI", h_VideocodecGetSEI);
    sr_hle_register(0x492b5e4bu, "sceMpegBaseCscInit", h_MpegBaseCscInit);
    sr_hle_register(0x91929a21u, "sceMpegBaseCscAvc", h_MpegBaseCscAvc);
    sr_hle_register(0xbea18f91u, "sceMpegBasePESpacketCopy", h_MpegBasePESpacketCopy);
    sr_hle_register(0x0530be4eu, "sceMpegbase_0530BE4E", h_Mpegbase_0530BE4E);
    sr_hle_register(0x304882e1u, "sceMpegBaseCscAvcRange", h_MpegBaseCscAvcRange);
    sr_hle_register(0x7ac0321au, "sceMpegBaseYCrCbCopy", h_MpegBaseYCrCbCopy);
    sr_hle_register(0x9d3f790cu, "sceAudiocodecCheckNeedMem", h_AudiocodecCheckNeedMem);
    sr_hle_register(0x5b37eb1du, "sceAudiocodecInit", h_AudiocodecInit);
    sr_hle_register(0x70a703f8u, "sceAudiocodecDecode", h_AudiocodecDecode);
    sr_hle_register(0x3a20a200u, "sceAudiocodecGetEDRAM", h_AudiocodecGetEDRAM);
    sr_hle_register(0x29681260u, "sceAudiocodecReleaseEDRAM", h_AudiocodecReleaseEDRAM);
}
