// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// The Media Engine boundary a disc mpeg.prx calls when it runs as LLE (#286): sceVideocodec,
// sceMpegbase and sceAudiocodec in src/rt/hle_videocodec.c. Standalone host executable, no game
// inputs:
//
//   mingw32-make --no-print-directory videocodec-selftest
//
// The handlers are installed by hle_register_videocodec_handlers(), the function sr_hle_init()
// calls, through a recording sr_hle_register double, and every call goes through sr_syscall by
// NID with its arguments in a0-a3. The harness #includes recomp.c and cpu_lle.c (guest memory,
// as tests/native/cpu_lle_selftest.c does) and hle_videocodec.c itself, white-box, so it can read
// the host ME RAM model; a mutation run puts a scratch copy of hle_videocodec.c earlier on the
// include path.
//
// The H.264 stream is generated here: a 64x64 baseline stream whose every macroblock is I_PCM
// with position-dependent samples, so the decoded planes are known byte for byte. The generator
// is a copy of the one in tests/native/psmf_media_selftest.c (static there, so it cannot be
// linked from this executable).
//
// The decode cases run wherever the build links a real H.264 backend (Media Foundation on
// Windows, the FFmpeg subset where H264_FFMPEG=1). Without one, each decode case prints
// "SKIP: videocodec decode: <case>" and is not counted as passed; the fail-closed refusal of a
// decode without a backend runs on every build.
//
// Failing-before evidence: on the base tree src/rt/hle_videocodec.c and the videocodec-selftest
// target do not exist, so this file does not build.
//
// The ATRAC3plus cases build PSMF audio frames around synthetic bodies (the terminator frame of
// tests/native/atrac3p_bridge_selftest.c, and the first xorshift-generated mono body that decodes
// to sound) and compare each block with a reference decode of the same frame by a separate decoder.
//
// Expected stderr noise: one ME_BOUNDARY_REFUSED line per deliberate refusal.

#include "recomp.c"   /* arena, MEM_* backing */
#include "cpu_lle.c"  /* helpers the linked interpreter needs */

/* No GE renderer is linked by this harness. */
volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_video_frame_note(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }

#include "hle_videocodec.c"   /* the boundary under test (white-box, same TU) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stubs for runtime symbols recomp.c references (as tests/native/cpu_lle_selftest.c) ---- */

uint32_t g_sr_debug = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_metadata_watch = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_count = 0;
unsigned g_sr_store_context_limit = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
int g_sr_last_writer_enabled = 0;
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    (void)addr; (void)width; (void)val; (void)pc;
}
CpuState *s_cpu = NULL;
int sr_sched_on = 0;
atomic_int_least32_t sr_timeslice;

uint32_t sched_current_uid(void) { return 0u; }
void sched_exit_current(int32_t status) { (void)status; }
void sched_exit_current_delete(int32_t status) { (void)status; }
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) { (void)uid; (void)arglen; (void)argp; return 0; }
uint32_t sched_terminate_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_delete_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_thread_wakeup(uint32_t uid) { (void)uid; return 0; }
void sched_set_current_join_target(uint32_t uid) { (void)uid; }
void sched_clear_current_join_target(void) {}
int sched_take_current_join_result(uint32_t uid, uint32_t *result_out) { (void)uid; (void)result_out; return 0; }
uint32_t sr_get_ge_status(void) { return 0u; }
uint32_t sr_hle_resolve_late_import(uint32_t nid) { (void)nid; return 0u; }
void sr_yield(CpuState *s) { (void)s; }
int sr_vfpu_interp(CpuState *s, uint32_t op) { (void)s; (void)op; return 0; }
uint64_t SDL_GetTicksNS(void) { return 0u; }

/* ---- results ---------------------------------------------------------------------------- */

static int checks, failures, skips;
#define CHECK(x, text) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL: %s\n", text); } } while (0)
static void skip(const char *name) {
    skips++;
    fprintf(stderr, "SKIP: videocodec decode: %s\n", name);
}

/* ---- the registration path: a recording sr_hle_register and sr_syscall by NID ---------- */

typedef struct { uint32_t nid; const char *name; HleFn fn; } TestRegistration;
static TestRegistration s_regs[32];
static int s_reg_count, s_reg_duplicates;

void sr_hle_register(uint32_t nid, const char *name, HleFn fn) {
    for (int i = 0; i < s_reg_count; i++)
        if (s_regs[i].nid == nid) { s_reg_duplicates++; return; }
    if (s_reg_count < (int)(sizeof(s_regs) / sizeof(s_regs[0]))) {
        s_regs[s_reg_count].nid = nid;
        s_regs[s_reg_count].name = name;
        s_regs[s_reg_count].fn = fn;
        s_reg_count++;
    }
}

uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    for (int i = 0; i < s_reg_count; i++)
        if (s_regs[i].nid == nid) return s_regs[i].fn(s);
    fprintf(stderr, "FAIL: NID 0x%08x is not registered\n", nid);
    failures++;
    return 0xDEADDEADu;
}

static uint32_t call(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = a0; cpu.r[5] = a1; cpu.r[6] = a2; cpu.r[7] = a3;
    return sr_syscall(&cpu, nid);
}

/* A five-argument call: the fifth arrives in t0 (MIPS EABI). */
static uint32_t call5(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[4] = a0; cpu.r[5] = a1; cpu.r[6] = a2; cpu.r[7] = a3; cpu.r[8] = a4;
    return sr_syscall(&cpu, nid);
}

#define NID_OPEN        0xC01EC829u
#define NID_INIT        0x17099F0Au
#define NID_DECODE      0xDBA273FAu
#define NID_STOP        0xA2F0564Eu
#define NID_DELETE      0x307E6E1Cu
#define NID_GETEDRAM    0x2D31F5B1u
#define NID_RELEASE     0x4F160BF4u
#define NID_SETMEMORY   0x745A7B7Au
#define NID_CSCINIT     0x492B5E4Bu
#define NID_CSCAVC      0x91929A21u
#define NID_PESCOPY     0xBEA18F91u
#define NID_PIXELMODE   0x0530BE4Eu
#define NID_GETVERSION  0x26927D19u
#define NID_GETSEI      0x627B7D42u
#define NID_CSCRANGE    0x304882E1u
#define NID_YCRCBCOPY   0x7AC0321Au
#define NID_AC_NEEDMEM  0x9D3F790Cu
#define NID_AC_INIT     0x5B37EB1Du
#define NID_AC_DECODE   0x70A703F8u
#define NID_AC_GETEDRAM 0x3A20A200u
#define NID_AC_RELEASE  0x29681260u

/* ---- guest layout ------------------------------------------------------------------------ */

#define G_CTX      0x08900000u
#define G_CTX2     0x08900100u
#define G_BUF2     0x08900200u
#define G_YUV      0x08900300u
#define G_BUF3     0x08900400u
#define G_SEI      0x08900500u
#define G_CSC      0x08900600u
#define G_PKT      0x08900700u   /* up to 32 packet descriptors */
#define G_SRC      0x08901000u   /* packet payloads */
#define G_RGB      0x08920000u   /* CSC destination */
#define RGB_STRIDE 80u           /* pixels per destination row: 16 more than the picture */
#define G_AU       0x08948000u   /* an access unit in guest memory (16 KiB) */
#define G_NOWHERE  0x0C000000u   /* past the guest arena: neither ME RAM nor guest memory */

static uint8_t *host(uint32_t addr) { return (uint8_t *)SR_HOST(addr); }

static int bytes_are(uint32_t addr, uint32_t size, uint8_t value) {
    for (uint32_t i = 0; i < size; i++) if (host(addr)[i] != value) return 0;
    return 1;
}

static uint32_t me32(uint32_t off) {
    return (uint32_t)s_me_ram[off] | ((uint32_t)s_me_ram[off + 1u] << 8) |
           ((uint32_t)s_me_ram[off + 2u] << 16) | ((uint32_t)s_me_ram[off + 3u] << 24);
}

static void put_packet(uint32_t at, uint32_t src, uint32_t dst, uint32_t next, uint32_t len_word) {
    MEM_W32(at, src);
    MEM_W32(at + 4u, dst);
    MEM_W32(at + 8u, next);
    MEM_W32(at + 12u, len_word);
}

/* ---- source-authored H.264 stream (copied from tests/native/psmf_media_selftest.c) ------ */

typedef struct {
    uint8_t *d; uint32_t cap, n;
    uint32_t acc; int bits;
    int overflow;
} BitW;

static void bw_byte(BitW *w, uint8_t b) {
    if (w->n >= w->cap) { w->overflow = 1; return; }
    w->d[w->n++] = b;
}
static void bw_bit(BitW *w, int b) {
    w->acc = (w->acc << 1) | (uint32_t)(b & 1);
    if (++w->bits == 8) { bw_byte(w, (uint8_t)w->acc); w->acc = 0; w->bits = 0; }
}
static void bw_u(BitW *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) bw_bit(w, (int)((v >> (unsigned)i) & 1u));
}
static void bw_ue(BitW *w, uint32_t v) {
    uint32_t x = v + 1u;
    int n = 0;
    for (uint32_t t = x; t; t >>= 1) n++;
    for (int i = 0; i < n - 1; i++) bw_bit(w, 0);
    bw_u(w, x, n);
}
static void bw_pad(BitW *w) { while (w->bits) bw_bit(w, 0); }
static void bw_trailing(BitW *w) { bw_bit(w, 1); bw_pad(w); }
static void bw_raw(BitW *w, const uint8_t *p, uint32_t n) {
    bw_pad(w);
    if (w->n + n > w->cap) { w->overflow = 1; return; }
    memcpy(w->d + w->n, p, n);
    w->n += n;
}

typedef struct { uint8_t *d; uint32_t cap, n; int overflow; } ByteOut;
static void bo_raw(ByteOut *o, const uint8_t *p, uint32_t n) {
    if (o->n + n > o->cap) { o->overflow = 1; return; }
    memcpy(o->d + o->n, p, n);
    o->n += n;
}
static void bo_byte(ByteOut *o, uint8_t b) { bo_raw(o, &b, 1); }
static void emit_nal(ByteOut *o, uint8_t hdr, const BitW *rbsp) {
    static const uint8_t start_code[3] = { 0, 0, 1 };
    bo_raw(o, start_code, 3u);
    bo_byte(o, hdr);
    int zeros = 0;
    for (uint32_t i = 0; i < rbsp->n; i++) {
        uint8_t b = rbsp->d[i];
        if (zeros >= 2 && b <= 3u) { bo_byte(o, 3u); zeros = 0; }
        bo_byte(o, b);
        zeros = (b == 0) ? zeros + 1 : 0;
    }
}

#define FIX_W_MBS 4u
#define FIX_H_MBS 4u
#define FIX_W     (FIX_W_MBS * 16u)
#define FIX_H     (FIX_H_MBS * 16u)
#define FIX_PICTURES 3
/* The geometry write_sps and write_idr emit: FIX_W_MBS x FIX_H_MBS except in test_band_room. */
static uint32_t s_fix_w_mbs = FIX_W_MBS, s_fix_h_mbs = FIX_H_MBS;
static const uint8_t k_seed[FIX_PICTURES] = { 200u, 60u, 128u };

static void write_sps(BitW *w) {
    bw_u(w, 66u, 8);          /* profile_idc: baseline */
    bw_u(w, 0u, 8);           /* constraint set flags  */
    bw_u(w, 30u, 8);          /* level_idc 3.0         */
    bw_ue(w, 0u);             /* seq_parameter_set_id */
    bw_ue(w, 0u);             /* log2_max_frame_num_minus4 */
    bw_ue(w, 0u);             /* pic_order_cnt_type */
    bw_ue(w, 0u);             /* log2_max_pic_order_cnt_lsb_minus4 */
    bw_ue(w, 1u);             /* max_num_ref_frames */
    bw_bit(w, 0);             /* gaps_in_frame_num_value_allowed_flag */
    bw_ue(w, s_fix_w_mbs - 1u); /* pic_width_in_mbs_minus1 */
    bw_ue(w, s_fix_h_mbs - 1u); /* pic_height_in_map_units_minus1 */
    bw_bit(w, 1);             /* frame_mbs_only_flag */
    bw_bit(w, 1);             /* direct_8x8_inference_flag */
    bw_bit(w, 0);             /* frame_cropping_flag */
    bw_bit(w, 0);             /* vui_parameters_present_flag */
    bw_trailing(w);
}

static void write_pps(BitW *w) {
    bw_ue(w, 0u);             /* pic_parameter_set_id */
    bw_ue(w, 0u);             /* seq_parameter_set_id */
    bw_bit(w, 0);             /* entropy_coding_mode_flag: CAVLC */
    bw_bit(w, 0);             /* bottom_field_pic_order_in_frame_present_flag */
    bw_ue(w, 0u);             /* num_slice_groups_minus1 */
    bw_ue(w, 0u);             /* num_ref_idx_l0_default_active_minus1 */
    bw_ue(w, 0u);             /* num_ref_idx_l1_default_active_minus1 */
    bw_bit(w, 0);             /* weighted_pred_flag */
    bw_u(w, 0u, 2);           /* weighted_bipred_idc */
    bw_ue(w, 0u);             /* pic_init_qp_minus26 (se(0)) */
    bw_ue(w, 0u);             /* pic_init_qs_minus26 (se(0)) */
    bw_ue(w, 0u);             /* chroma_qp_index_offset (se(0)) */
    bw_bit(w, 0);             /* deblocking_filter_control_present_flag */
    bw_bit(w, 0);             /* constrained_intra_pred_flag */
    bw_bit(w, 0);             /* redundant_pic_cnt_present_flag */
    bw_trailing(w);
}

/* The I_PCM sample i (0..383: 256 luma, 64 Cb, 64 Cr) of macroblock mb in picture `seed`. */
static uint8_t pcm_sample(uint8_t seed, uint32_t mb, uint32_t i) {
    return (uint8_t)(1u + (seed + i * 7u + mb * 29u) % 254u);
}

/* One IDR picture, every macroblock I_PCM with position-dependent samples. */
static void write_idr(BitW *w, int picture, uint8_t seed) {
    uint8_t pcm[384];
    bw_ue(w, 0u);                       /* first_mb_in_slice */
    bw_ue(w, 2u);                       /* slice_type: I */
    bw_ue(w, 0u);                       /* pic_parameter_set_id */
    bw_u(w, (uint32_t)(picture % 16), 4);   /* frame_num */
    bw_ue(w, (uint32_t)(picture % 2));      /* idr_pic_id */
    bw_u(w, 0u, 4);                     /* pic_order_cnt_lsb */
    bw_bit(w, 0);                       /* dec_ref_pic_marking.no_output_of_prior_pics_flag */
    bw_bit(w, 0);                       /* dec_ref_pic_marking.long_term_reference_flag */
    bw_ue(w, 0u);                       /* slice_qp_delta (se(0)) */
    for (uint32_t mb = 0; mb < s_fix_w_mbs * s_fix_h_mbs; mb++) {
        for (uint32_t i = 0; i < 384u; i++) pcm[i] = pcm_sample(seed, mb, i);
        bw_ue(w, 25u);                  /* mb_type: I_PCM */
        bw_raw(w, pcm, 384u);
    }
    bw_trailing(w);
}

/* One access unit: delimiter, parameter sets on the first picture, then the IDR picture. */
static uint32_t build_au(int picture, uint8_t *dst, uint32_t cap) {
    static const uint8_t aud[5] = { 0, 0, 1, 0x09, 0x10 };
    uint8_t scratch[8192];
    ByteOut out;
    BitW w;
    out.d = dst; out.cap = cap; out.n = 0; out.overflow = 0;
    bo_raw(&out, aud, sizeof(aud));
    if (picture == 0) {
        w.d = scratch; w.cap = sizeof(scratch); w.n = 0; w.acc = 0; w.bits = 0; w.overflow = 0;
        write_sps(&w);
        emit_nal(&out, 0x67, &w);
        w.n = 0; w.acc = 0; w.bits = 0; w.overflow = 0;
        write_pps(&w);
        emit_nal(&out, 0x68, &w);
    }
    w.d = scratch; w.cap = sizeof(scratch); w.n = 0; w.acc = 0; w.bits = 0; w.overflow = 0;
    write_idr(&w, picture, k_seed[picture % FIX_PICTURES]);
    CHECK(!w.overflow, "fixture: the IDR picture fits the scratch buffer");
    emit_nal(&out, 0x65, &w);
    CHECK(!out.overflow, "fixture: the access unit fits its buffer");
    return out.n;
}

/* The decoded planes picture `picture` must produce. */
static void expected_planes(int picture, uint8_t *y, uint8_t *cb, uint8_t *cr) {
    for (uint32_t mb = 0; mb < FIX_W_MBS * FIX_H_MBS; mb++) {
        const uint32_t mbx = mb % FIX_W_MBS, mby = mb / FIX_W_MBS;
        for (uint32_t i = 0; i < 384u; i++) {
            const uint8_t v = pcm_sample(k_seed[picture], mb, i);
            if (i < 256u) {
                y[(mby * 16u + i / 16u) * FIX_W + mbx * 16u + i % 16u] = v;
            } else {
                const uint32_t j = (i - 256u) % 64u;
                uint8_t *plane = i < 320u ? cb : cr;
                plane[(mby * 8u + j / 8u) * (FIX_W / 2u) + mbx * 8u + j % 8u] = v;
            }
        }
    }
}

/* The band bytes of a FIX_W x FIX_H picture, by a per-pixel rule written apart from the
 * runtime's walker: luma (x, y) is band ((x >> 4) & 1) | ((y & 1) << 1), at strip x >> 5, row
 * y >> 1, byte x & 15; chroma (x, y) is band 4 + (y & 1) + 2 * ((x >> 3) & 1), at group x >> 4,
 * row y >> 1, pair x & 7, cb then cr. Bands 0, 1, 4 and 5 come from picture `a`, the rest from
 * picture `b`. */
#define BAND_LUMA   (((FIX_W + 16u) >> 5) * (FIX_H / 2u) * 16u)   /* 1024 for 64 x 64 */
#define BAND_CHROMA (BAND_LUMA / 2u)
static const uint32_t k_band_bytes[8] = { BAND_LUMA, BAND_LUMA, BAND_LUMA, BAND_LUMA,
                                          BAND_CHROMA, BAND_CHROMA, BAND_CHROMA, BAND_CHROMA };
static uint8_t s_want[8][BAND_LUMA];

static void want_bands(int a, int b) {
    static uint8_t y[2][FIX_W * FIX_H], cb[2][(FIX_W / 2) * (FIX_H / 2)], cr[2][(FIX_W / 2) * (FIX_H / 2)];
    expected_planes(a, y[0], cb[0], cr[0]);
    expected_planes(b, y[1], cb[1], cr[1]);
    memset(s_want, 0, sizeof(s_want));
    for (uint32_t yy = 0; yy < FIX_H; yy++)
        for (uint32_t x = 0; x < FIX_W; x++) {
            const uint32_t band = ((x >> 4) & 1u) | ((yy & 1u) << 1);
            const int from = (band == 0u || band == 1u) ? 0 : 1;
            s_want[band][(x >> 5) * (FIX_H / 2u) * 16u + (yy >> 1) * 16u + (x & 15u)] = y[from][yy * FIX_W + x];
        }
    for (uint32_t yy = 0; yy < FIX_H / 2u; yy++)
        for (uint32_t x = 0; x < FIX_W / 2u; x++) {
            const uint32_t band = 4u + (yy & 1u) + 2u * ((x >> 3) & 1u);
            const int from = (band == 4u || band == 5u) ? 0 : 1;
            const uint32_t at = (x >> 4) * (FIX_H / 4u) * 16u + (yy >> 1) * 16u + (x & 7u) * 2u;
            s_want[band][at] = cb[from][yy * (FIX_W / 2u) + x];
            s_want[band][at + 1u] = cr[from][yy * (FIX_W / 2u) + x];
        }
}

/* Band `band` at `addr` (in ME RAM below the ME RAM size, else guest memory) is s_want's. */
static int band_is_wanted(uint32_t addr, int band) {
    const uint8_t *at = addr < ME_RAM_SIZE ? s_me_ram + addr : host(addr);
    return memcmp(at, s_want[band], k_band_bytes[band]) == 0;
}

/* ---- cases -------------------------------------------------------------------------------- */

static void test_registration(void) {
    static const struct { uint32_t nid; const char *name; } want[] = {
        { NID_OPEN, "sceVideocodecOpen" }, { NID_INIT, "sceVideocodecInit" },
        { NID_DECODE, "sceVideocodecDecode" }, { NID_STOP, "sceVideocodecStop" },
        { NID_DELETE, "sceVideocodecDelete" }, { NID_GETEDRAM, "sceVideocodecGetEDRAM" },
        { NID_RELEASE, "sceVideocodecReleaseEDRAM" }, { NID_SETMEMORY, "sceVideocodecSetMemory" },
        { NID_CSCINIT, "sceMpegBaseCscInit" }, { NID_CSCAVC, "sceMpegBaseCscAvc" },
        { NID_PESCOPY, "sceMpegBasePESpacketCopy" }, { NID_PIXELMODE, "sceMpegbase_0530BE4E" },
        { NID_GETVERSION, "sceVideocodecGetVersion" }, { NID_GETSEI, "sceVideocodecGetSEI" },
        { NID_CSCRANGE, "sceMpegBaseCscAvcRange" }, { NID_YCRCBCOPY, "sceMpegBaseYCrCbCopy" },
        { NID_AC_NEEDMEM, "sceAudiocodecCheckNeedMem" }, { NID_AC_INIT, "sceAudiocodecInit" },
        { NID_AC_DECODE, "sceAudiocodecDecode" }, { NID_AC_GETEDRAM, "sceAudiocodecGetEDRAM" },
        { NID_AC_RELEASE, "sceAudiocodecReleaseEDRAM" },
    };
    hle_register_videocodec_handlers();
    CHECK(s_reg_count == 21 && s_reg_duplicates == 0, "registration: exactly the 21 boundary NIDs, once each");
    for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        int found = 0;
        for (int i = 0; i < s_reg_count; i++)
            if (s_regs[i].nid == want[k].nid && strcmp(s_regs[i].name, want[k].name) == 0) found = 1;
        CHECK(found, "registration: each NID carries its library name");
    }
}

static uint32_t s_edram_raw, s_edram;

static void test_context_words(void) {
    memset(host(G_CTX), 0, 96u);
    CHECK(call(NID_OPEN, G_CTX, 1u, 0, 0) == 0x80628001u && MEM_R32(G_CTX) == 0x05100601u &&
              MEM_R32(G_CTX + 8u) == 0x80000000u && MEM_R32(G_CTX + 24u) == 0x264Cu && !vc_find(G_CTX),
          "open: codec type 1 returns 0x80628001 after writing +0, +8 and +24, and stays unopened (unrecorded read)");
    CHECK(call(NID_OPEN, G_CTX, 2u, 0, 0) == VC_ERR_TYPE, "open: codec type 2 is refused");
    memset(host(G_CTX), 0, 96u);
    memset(host(G_CTX2), 0, 96u);
    CHECK(call(NID_OPEN, G_CTX, 0u, 0, 0) == 0u, "open: type 0 opens the context");
    CHECK(MEM_R32(G_CTX) == 0x05100601u && MEM_R32(G_CTX + 8u) == 1u && MEM_R32(G_CTX + 24u) == 0x8344u,
          "open: +0 = 0x05100601, +8 = 1 and +24 = 0x8344 (unrecorded read)");
    CHECK(bytes_are(G_CTX + 4u, 4u, 0u) && bytes_are(G_CTX + 12u, 12u, 0u) && bytes_are(G_CTX + 28u, 68u, 0u),
          "open: no other context word changes");
    CHECK(call(NID_OPEN, G_CTX, 0u, 0, 0) == VC_ERR_STATE, "open: an open context cannot be opened again");
    CHECK(call(NID_INIT, G_CTX2, 0u, 0, 0) == VC_ERR_NOT_OPEN, "init: a context the runtime did not open is refused");
    CHECK(call(NID_GETEDRAM, G_CTX2, 0u, 0, 0) == VC_ERR_NOT_OPEN, "getedram: an unopened context is refused");
    CHECK(call(NID_SETMEMORY, G_CTX, 1u, 0, 0) == VC_ERR_TYPE, "setmemory: codec type 1 is refused");

    MEM_W32(G_CTX + 24u, 0x4000u);
    CHECK(call(NID_GETEDRAM, G_CTX, 0u, 0, 0) == 0u, "getedram: ME memory is allocated");
    s_edram_raw = MEM_R32(G_CTX + 92u);
    s_edram = MEM_R32(G_CTX + 20u);
    CHECK(s_edram_raw == 0x10000u, "getedram: the first allocation is the lowest past the reserved 0x10000 bytes");
    CHECK(s_edram == ((s_edram_raw + 63u) & ~63u), "getedram: +20 is +92 aligned to 64 bytes");
    CHECK(s_me_alloc_count == 1 && s_me_allocs[0].addr == s_edram_raw && s_me_allocs[0].size == 0x4040u,
          "getedram: the ME allocation is ((+24) + 63) | 0x3F rounded to 64 bytes");
    CHECK(call(NID_GETEDRAM, G_CTX, 0u, 0, 0) == VC_ERR_STATE && MEM_R32(G_CTX + 92u) == s_edram_raw,
          "getedram: a second allocation for the same context is refused");

    CHECK(call(NID_INIT, G_CTX, 0u, 0, 0) == 0u && MEM_R32(G_CTX + 12u) == s_edram && MEM_R32(G_CTX + 8u) == 0u,
          "init: +12 = +20 and +8 = 0 (unrecorded read)");

    for (uint32_t i = 0; i < 4u; i++) MEM_W32(G_CTX + 64u + i * 4u, 0x11111111u * (i + 1u));
    uint8_t before[96];
    memcpy(before, host(G_CTX), 96u);
    CHECK(call(NID_SETMEMORY, G_CTX, 0u, 0, 0) == 0u && memcmp(before, host(G_CTX), 96u) == 0,
          "setmemory: accepted and no context word changes");
    CHECK(call(NID_STOP, G_CTX, 0u, 0, 0) == VC_ERR_STATE && memcmp(before, host(G_CTX), 96u) == 0 &&
              vc_find(G_CTX),
          "stop: before any decode it returns 0x806201FE and changes nothing (unrecorded read)");
}

/* sceVideocodecGetVersion and sceVideocodecGetSEI: the words JPCSP writes (synthetic, W-LLE-1). */
static void test_version_sei(void) {
    uint8_t before[96];
    memcpy(before, host(G_CTX), 96u);
    CHECK(call(NID_GETVERSION, G_CTX2, 0u, 0, 0) == VC_ERR_NOT_OPEN, "getversion: an unopened context is refused");
    CHECK(call(NID_GETVERSION, G_CTX, 1u, 0, 0) == VC_ERR_TYPE && memcmp(before, host(G_CTX), 96u) == 0,
          "getversion: codec type 1 is refused and the context is untouched");
    CHECK(call(NID_GETVERSION, G_CTX, 0u, 0, 0) == 0u && MEM_R32(G_CTX + 4u) == 0x78u,
          "getversion: context +4 is 0x78 (JPCSP, synthetic)");
    before[4] = 0x78u; before[5] = 0u; before[6] = 0u; before[7] = 0u;
    CHECK(memcmp(before, host(G_CTX), 96u) == 0, "getversion: no other context word changes");

    MEM_W32(G_CTX + 80u, 0u);
    CHECK(call(NID_GETSEI, G_CTX, 0u, 0, 0) == VC_ERR_GUEST_ADDRESS, "getsei: a context with no SEI block is refused");
    memset(host(G_SEI), 0xA5, 36u);
    MEM_W32(G_CTX + 80u, G_SEI);
    CHECK(call(NID_GETSEI, G_CTX2, 0u, 0, 0) == VC_ERR_NOT_OPEN, "getsei: an unopened context is refused");
    CHECK(call(NID_GETSEI, G_CTX, 0u, 0, 0) == 0u && MEM_R32(G_SEI + 28u) == 0u &&
              bytes_are(G_SEI, 28u, 0xA5u) && bytes_are(G_SEI + 32u, 4u, 0xA5u),
          "getsei: SEI block +28 is cleared and nothing else changes");
}

static void test_packet_copy(void) {
    const uint32_t off = s_edram;
    for (uint32_t i = 0; i < 0x200u; i++) host(G_SRC)[i] = (uint8_t)(0x40u + i * 3u);
    /* Two packets; the length word's upper bits are not part of the length. */
    put_packet(G_PKT, G_SRC, s_edram, G_PKT + 16u, 0xABCDE000u | 100u);
    put_packet(G_PKT + 16u, G_SRC + 0x100u, s_edram + 100u, 0u, 50u);
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == 0u, "pescopy: a two-packet list is copied");
    CHECK(memcmp(s_me_ram + off, host(G_SRC), 100u) == 0,
          "pescopy: the first packet's bytes land at its ME address");
    CHECK(memcmp(s_me_ram + off + 100u, host(G_SRC + 0x100u), 50u) == 0,
          "pescopy: the second packet's bytes land at its ME address");
    CHECK(s_me_ram[off + 150u] == 0u, "pescopy: nothing is written past the last packet");

    put_packet(G_PKT, G_SRC, s_edram + 0x800u, G_PKT + 16u, 64u);
    put_packet(G_PKT + 16u, G_SRC, s_edram + 0x900u, 0u, 0xFFFFF000u);
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == VC_ERR_ZERO_LENGTH, "pescopy: a zero-length packet is refused");
    CHECK(s_me_ram[off + 0x800u] == 0u, "pescopy: a refused list copies none of its packets");

    put_packet(G_PKT, G_SRC, ME_RAM_SIZE - 8u, 0u, 16u);
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == VC_ERR_ME_ADDRESS, "pescopy: a destination leaving ME RAM is refused");
    put_packet(G_PKT, G_SRC, G_NOWHERE, 0u, 16u);
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == VC_ERR_GUEST_ADDRESS,
          "pescopy: a destination in neither ME RAM nor guest memory is refused");
    memset(host(G_AU), 0xA5, 0x200u);
    put_packet(G_PKT, G_SRC, G_AU, G_PKT + 16u, 100u);
    put_packet(G_PKT + 16u, G_SRC + 0x100u, s_edram + 0xA00u, 0u, 8u);
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == 0u && memcmp(host(G_AU), host(G_SRC), 100u) == 0 &&
              bytes_are(G_AU + 100u, 0x100u, 0xA5u) && memcmp(s_me_ram + off + 0xA00u, host(G_SRC + 0x100u), 8u) == 0,
          "pescopy: a destination past ME RAM is guest memory (the ME memory view), and one list mixes both");
    memset(s_me_ram + off + 0xA00u, 0, 8u);
    put_packet(G_PKT, G_SRC, 0x20000000u | (s_edram + 0xA00u), 0u, 8u);
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == 0u && s_me_ram[off + 0xA00u] == 0u,
          "pescopy: segment 0x20000000 is not an ME RAM view, so ME RAM is untouched");
    put_packet(G_PKT, G_SRC, 0x40000000u | (s_edram + 0xA00u), 0u, 8u);
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == 0u && memcmp(s_me_ram + off + 0xA00u, host(G_SRC), 8u) == 0,
          "pescopy: the 0x40000000 view of ME RAM reaches the same bytes");
    put_packet(G_PKT, G_SRC, s_edram + 0xB00u, G_PKT + 16u, 8u);
    put_packet(G_PKT + 16u, G_SRC, s_edram + 0xB00u, G_PKT, 8u);
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == VC_ERR_GUEST_ADDRESS && s_me_ram[off + 0xB00u] == 0u,
          "pescopy: a list that loops back is refused and copies nothing");
    memset(s_me_ram + off, 0, 0x1000u);
}

/* Copy access unit `picture` through sceMpegBasePESpacketCopy, in packets of at most 2048 bytes,
 * into ME RAM at the EDRAM address, or into guest memory at G_AU when `in_guest` (the ME memory
 * view), and point the context at it. */
static void load_au(int picture, int in_guest) {
    static uint8_t au[16384];
    const uint32_t len = build_au(picture, au, sizeof(au));
    const uint32_t to = in_guest ? G_AU : s_edram;
    memcpy(host(G_SRC), au, len);
    uint32_t done = 0, pkt = G_PKT;
    while (done < len) {
        const uint32_t n = len - done > 2048u ? 2048u : len - done;
        put_packet(pkt, G_SRC + done, to + done, done + n < len ? pkt + 16u : 0u, n);
        done += n;
        pkt += 16u;
    }
    CHECK(call(NID_PESCOPY, G_PKT, 0, 0, 0) == 0u &&
              memcmp(in_guest ? host(G_AU) : s_me_ram + s_edram, au, len) == 0,
          "decode: the access unit reaches its buffer through PESpacketCopy");
    MEM_W32(G_CTX + 36u, to);
    MEM_W32(G_CTX + 40u, len);
}

static void test_decode_refusals(void) {
    MEM_W32(G_CTX + 16u, G_BUF2);
    MEM_W32(G_CTX + 44u, G_YUV);
    MEM_W32(G_CTX + 48u, G_BUF3);
    MEM_W32(G_CTX + 80u, G_SEI);
    memset(host(G_YUV), 0, 48u);
    CHECK(call(NID_DECODE, G_CTX, 1u, 0, 0) == VC_ERR_TYPE, "decode: codec type 1 is refused");
    CHECK(call(NID_DECODE, G_CTX2, 0u, 0, 0) == VC_ERR_NOT_OPEN, "decode: an unopened context is refused");
    MEM_W32(G_CTX + 36u, ME_RAM_SIZE - 8u);
    MEM_W32(G_CTX + 40u, 16u);
    CHECK(call(NID_DECODE, G_CTX, 0u, 0, 0) == VC_ERR_ME_ADDRESS, "decode: an access unit leaving ME RAM is refused");
    MEM_W32(G_CTX + 36u, G_NOWHERE);
    CHECK(call(NID_DECODE, G_CTX, 0u, 0, 0) == VC_ERR_GUEST_ADDRESS,
          "decode: an access unit in neither ME RAM nor guest memory is refused");
    MEM_W32(G_CTX + 36u, s_edram);
    MEM_W32(G_CTX + 40u, 0u);
    CHECK(call(NID_DECODE, G_CTX, 0u, 0, 0) == VC_ERR_ZERO_LENGTH, "decode: a zero-length access unit is refused");
    MEM_W32(G_CTX + 40u, 16u);
    MEM_W32(G_CTX + 16u, 0u);
    CHECK(call(NID_DECODE, G_CTX, 0u, 0, 0) == VC_ERR_GUEST_ADDRESS, "decode: a null output structure is refused");
    MEM_W32(G_CTX + 16u, G_BUF2);

    /* No backend selected: the decode fails closed with the shared unavailable wording. */
    CHECK(sr_h264_select_backend("null") == 1, "decode: the null backend can be selected");
    CHECK(call(NID_DECODE, G_CTX, 0u, 0, 0) == VC_ERR_NO_BACKEND,
          "decode: without an H.264 backend the decode is refused, never reported as done");
    CHECK(sr_h264_select_backend("auto") == 1, "decode: the build's own backend is selected again");
}

static const char *const k_decode_cases[] = {
    "Decode writes the authored I_PCM samples into the ME bands in the band layout",
    "Decode reads an access unit from guest memory through the ME memory view",
    "Decode writes buffer2, the YUV struct, the ME info blocks, buffer3 and the SEI",
    "CscAvc writes the measured 8888 conversion pixel for pixel",
    "CscAvc writes the measured 5650 conversion pixel for pixel",
    "YCrCbCopy writes each half's bands into guest buffers byte for byte",
    "CscAvc reads a copied band from guest memory",
    "YCrCbCopy keeps the two halves of two pictures apart",
    "CscAvcRange converts only the requested rectangle",
    "A copy outlives the context that decoded it",
};

static int s_have_picture;
static int s_last_picture;
static int s_pictures;

static void check_decode_words(const VcContext *c, int picture) {
    const uint32_t slot = (uint32_t)picture % 3u, base = c->band_base, step = (1024u + 1024u + 512u + 512u) * 4u;
    const uint32_t b0 = base + 256u + slot * 2048u, c0 = base + 256u + 8192u + slot * 1024u;
    const uint32_t want[8] = { b0, b0 + step, b0 + 1024u, b0 + 1024u + step,
                               c0, c0 + step, c0 + 512u, c0 + 512u + step };
    int bands = 1;
    for (uint32_t i = 0; i < 8u; i++) if (MEM_R32(G_YUV + i * 4u) != want[i]) bands = 0;
    CHECK(base >= ME_RAM_RESERVED_LOW && (base & 63u) == 0u && base + 256u + step * 2u <= ME_RAM_SIZE,
          "decode: the band buffers are one 64-byte-aligned ME allocation");
    CHECK(bands, "decode: the YUV struct holds slot (frame % 3) of the JPCSP band layout for 64x64");
    CHECK(MEM_R32(G_YUV + 32u) == 1u, "decode: YUV struct +32 is 1");
    CHECK(MEM_R32(G_YUV + 36u) == base && MEM_R32(G_YUV + 40u) == base + 36u,
          "decode: the ME info block pointers default into the band allocation");
    CHECK(s_me_ram[base] == 0x02u && me32(base + 8u) == 90000u && me32(base + 16u) == 90000u &&
              me32(base + 24u) == (uint32_t)picture * 2u && me32(base + 28u) == 2u &&
              s_me_ram[base + 32u] == 0u && s_me_ram[base + 33u] == 1u,
          "decode: the first ME info block carries the JPCSP timing words");
    CHECK(s_me_ram[base + 36u] == 0u && me32(base + 36u + 24u) == 0u && me32(base + 36u + 28u) == 0u,
          "decode: the second ME info block carries the JPCSP words");
    CHECK(MEM_R32(G_BUF2 + 8u) == FIX_W && MEM_R32(G_BUF2 + 12u) == FIX_H && MEM_R32(G_BUF2 + 28u) == 1u &&
              MEM_R32(G_BUF2 + 32u) == 1u && MEM_R32(G_BUF2 + 36u) == 0u,
          "decode: buffer2 holds the size, 1, and the image-present pair");
    CHECK(host(G_BUF3)[0] == 0x01u && host(G_BUF3)[1] == 0xFFu && MEM_R32(G_BUF3 + 4u) == 3u &&
              MEM_R32(G_BUF3 + 8u) == 4u && MEM_R32(G_BUF3 + 12u) == 1u && host(G_BUF3)[16] == 0u &&
              MEM_R32(G_BUF3 + 20u) == 0x10000u && MEM_R32(G_BUF3 + 32u) == 4004u &&
              MEM_R32(G_BUF3 + 36u) == 240000u,
          "decode: buffer3 carries the JPCSP words");
    CHECK(host(G_SEI)[0] == 0x02u && MEM_R32(G_SEI + 8u) == 90000u && MEM_R32(G_SEI + 16u) == 90000u &&
              MEM_R32(G_SEI + 24u) == (uint32_t)picture * 2u && MEM_R32(G_SEI + 28u) == 2u &&
              host(G_SEI)[32] == 0u && host(G_SEI)[33] == 1u,
          "decode: the SEI block carries the JPCSP words");
    CHECK(MEM_R32(G_CTX + 8u) == 0u, "decode: context +8 is cleared");
}

static void test_decode(void) {
    if (!sr_h264_backend_available()) {
        for (size_t i = 0; i < sizeof(k_decode_cases) / sizeof(k_decode_cases[0]); i++) skip(k_decode_cases[i]);
        return;
    }
    int pictures = 0;
    for (int au = 0; au < FIX_PICTURES; au++) {
        load_au(au, au == 1);   /* AU 1 from guest memory: its picture arrives in a later call and is compared */
        memset(host(G_BUF2), 0xA5, 40u);
        memset(host(G_BUF3), 0xA5, 40u);
        memset(host(G_SEI), 0xA5, 36u);
        MEM_W32(G_CTX + 8u, 0xA5A5A5A5u);
        CHECK(call(NID_DECODE, G_CTX, 0u, 0, 0) == 0u, "decode: an access unit decodes");
        const VcContext *c = vc_find(G_CTX);
        if (!c) { CHECK(0, "decode: the context stays open"); return; }
        if (MEM_R32(G_BUF2 + 32u) != 1u) {
            CHECK(MEM_R32(G_BUF2 + 32u) == 0u && MEM_R32(G_BUF2 + 36u) == 1u && MEM_R32(G_BUF2 + 28u) == 1u,
                  "decode: a decode with no picture ready says so in buffer2");
            continue;
        }
        const int picture = pictures++;
        int published = 1, held = 1;
        want_bands(picture, picture);
        for (int b = 0; b < 8; b++) {
            if (MEM_R32(G_YUV + (uint32_t)b * 4u) != c->bands[picture % 3][b]) published = 0;
            if (!band_is_wanted(c->bands[picture % 3][b], b)) held = 0;
        }
        CHECK(published, "decode: the eight band addresses of the picture's slot are published");
        CHECK(held, "decode: the eight ME bands hold the authored I_PCM samples in the band layout, byte for byte");
        check_decode_words(c, picture);
        s_last_picture = picture;
    }
    fprintf(stderr, "videocodec: %d of %d access units delivered a picture in their own decode call\n",
            pictures, FIX_PICTURES);
    CHECK(pictures >= 1, "decode: the synchronous decode delivers a picture");
    s_have_picture = pictures >= 1;
    s_pictures = pictures;
}

static void expected_rgb(int picture, uint32_t x, uint32_t y, uint8_t *r, uint8_t *g, uint8_t *b) {
    static uint8_t ey[FIX_W * FIX_H], ecb[(FIX_W / 2) * (FIX_H / 2)], ecr[(FIX_W / 2) * (FIX_H / 2)];
    static int cached = -1;
    if (cached != picture) { expected_planes(picture, ey, ecb, ecr); cached = picture; }
    const uint32_t ci = (y / 2u) * (FIX_W / 2u) + x / 2u;
    sr_h264_psp_rgb(ey[y * FIX_W + x], ecb[ci], ecr[ci], r, g, b);
}

static void put_csc(uint32_t h16, uint32_t w16) {
    MEM_W32(G_CSC, h16);
    MEM_W32(G_CSC + 4u, w16);
    MEM_W32(G_CSC + 8u, 0u);
    MEM_W32(G_CSC + 12u, 0u);
    for (uint32_t i = 0; i < 8u; i++) MEM_W32(G_CSC + 16u + i * 4u, MEM_R32(G_YUV + i * 4u));
}

static void test_csc(void) {
    const uint32_t dest_bytes = FIX_H * RGB_STRIDE * 4u;
    /* Refusals that need no picture. */
    memset(host(G_RGB), 0xA5, dest_bytes);
    MEM_W32(G_CSC, 4u);
    MEM_W32(G_CSC + 4u, 4u);
    for (uint32_t i = 0; i < 8u; i++) MEM_W32(G_CSC + 16u + i * 4u, G_NOWHERE + i * 0x1000u);
    CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_CSC) == VC_ERR_GUEST_ADDRESS && bytes_are(G_RGB, dest_bytes, 0xA5u),
          "cscavc: band addresses in neither ME RAM nor guest memory are refused and write nothing");
    for (uint32_t i = 0; i < 8u; i++) MEM_W32(G_CSC + 16u + i * 4u, 0x20000u + i * 0x1000u);
    MEM_W32(G_CSC + 16u, ME_RAM_SIZE - 16u);
    CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_CSC) == VC_ERR_ME_ADDRESS && bytes_are(G_RGB, dest_bytes, 0xA5u),
          "cscavc: a band that runs past the end of ME RAM is refused and writes nothing");
    MEM_W32(G_CSC, 0u);
    CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_CSC) == VC_ERR_CSC_GEOMETRY,
          "cscavc: a struct of height 0 is refused");
    MEM_W32(G_CSC, VC_MAX_HEIGHT / 16u + 1u);
    CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_CSC) == VC_ERR_CSC_GEOMETRY,
          "cscavc: a struct taller than 576 lines is refused");
    CHECK(call(NID_PIXELMODE, 0u, 0, 0, 0) == 0u && s_csc_pixel_mode == VC_PIXEL_8888, "0530be4e: mode 0 is 8888");
    CHECK(call(NID_PIXELMODE, 4u, 0, 0, 0) == VC_ERR_PIXEL_MODE && s_csc_pixel_mode == VC_PIXEL_8888,
          "0530be4e: an unknown mode is refused and the mode is kept");
    CHECK(call(NID_CSCINIT, 64u, 0, 0, 0) == 0u, "cscinit: accepted");

    if (!s_have_picture) return;   /* the SKIP lines were printed by test_decode */
    const int picture = s_last_picture;
    put_csc(FIX_H / 16u, FIX_W / 16u);
    CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_CSC) == 0u, "cscavc: the decoded picture converts in 8888");
    int exact = 1, margin = 1;
    for (uint32_t y = 0; y < FIX_H; y++) {
        for (uint32_t x = 0; x < FIX_W; x++) {
            uint8_t r, g, b;
            expected_rgb(picture, x, y, &r, &g, &b);
            const uint8_t *px = host(G_RGB + (y * RGB_STRIDE + x) * 4u);
            if (px[0] != r || px[1] != g || px[2] != b || px[3] != 0u) exact = 0;
        }
        if (!bytes_are(G_RGB + (y * RGB_STRIDE + FIX_W) * 4u, (RGB_STRIDE - FIX_W) * 4u, 0xA5u)) margin = 0;
    }
    CHECK(exact, "cscavc: every 8888 pixel is sr_h264_psp_rgb of the authored samples, alpha 0");
    CHECK(margin, "cscavc: the columns past the picture keep their bytes");

    CHECK(call(NID_PIXELMODE, 1u, 0, 0, 0) == 0u && s_csc_pixel_mode == VC_PIXEL_5650, "0530be4e: mode 1 is 5650");
    memset(host(G_RGB), 0xA5, dest_bytes);
    CHECK(call(NID_CSCAVC, G_RGB, 0u, 0u, G_CSC) == 0u, "cscavc: width 0 takes CscInit's default width");
    exact = 1;
    for (uint32_t y = 0; y < FIX_H; y++)
        for (uint32_t x = 0; x < FIX_W; x++) {
            uint8_t r, g, b;
            expected_rgb(picture, x, y, &r, &g, &b);
            const uint16_t want = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
            const uint8_t *px = host(G_RGB + (y * 64u + x) * 2u);
            if ((uint16_t)(px[0] | (px[1] << 8)) != want) exact = 0;
        }
    CHECK(exact, "cscavc: every 5650 pixel is sr_h264_psp_rgb of the authored samples");

    put_csc(FIX_H / 16u, FIX_W / 16u);
}

/* ---- sceMpegBaseYCrCbCopy and sceMpegBaseCscAvcRange -------------------------------------- */
#define G_DCSC     0x08900740u   /* destination CSC struct (48 bytes) */
#define G_SCSC2    0x08900780u   /* a second source CSC struct */
#define G_DCSC3    0x089007C0u   /* a second destination CSC struct */
#define G_YCC      0x08930000u   /* eight guest band buffers, 0x800 bytes apart */
#define G_YCC3     0x08934000u   /* eight more */
#define YCC_STEP   0x800u

static void put_dst_csc(uint32_t at, uint32_t base) {
    MEM_W32(at, FIX_H / 16u);
    MEM_W32(at + 4u, FIX_W / 16u);
    MEM_W32(at + 8u, 0u);
    MEM_W32(at + 12u, 0u);
    for (uint32_t i = 0; i < 8u; i++) MEM_W32(at + 16u + i * 4u, base + i * YCC_STEP);
}

/* Band b of the eight buffers at `base` holds s_want's bytes when `half` covers it and is still
 * all 0xA5 otherwise; past each band's size every byte is still 0xA5. */
static int guest_bands_are(uint32_t base, uint32_t halves) {
    for (uint32_t b = 0; b < 8u; b++) {
        const uint32_t at = base + b * YCC_STEP, half = (b == 0u || b == 1u || b == 4u || b == 5u) ? 1u : 2u;
        if ((halves & half) ? !band_is_wanted(at, (int)b) : !bytes_are(at, k_band_bytes[b], 0xA5u)) return 0;
        if (!bytes_are(at + k_band_bytes[b], YCC_STEP - k_band_bytes[b], 0xA5u)) return 0;
    }
    return 1;
}

/* The 8888 pixel of a copy whose half A came from picture `a` and half B from picture `b`. */
static void expected_mixed_rgb(int a, int b, uint32_t x, uint32_t y, uint8_t *r, uint8_t *g, uint8_t *bl) {
    static uint8_t ya[FIX_W * FIX_H], cba[(FIX_W / 2) * (FIX_H / 2)], cra[(FIX_W / 2) * (FIX_H / 2)];
    static uint8_t yb[FIX_W * FIX_H], cbb[(FIX_W / 2) * (FIX_H / 2)], crb[(FIX_W / 2) * (FIX_H / 2)];
    static int ca = -1, cb = -1;
    if (ca != a) { expected_planes(a, ya, cba, cra); ca = a; }
    if (cb != b) { expected_planes(b, yb, cbb, crb); cb = b; }
    const uint32_t ci = (y / 2u) * (FIX_W / 2u) + x / 2u;
    const int chroma_a = ((x / 2u) % 16u) < 8u;
    sr_h264_psp_rgb((y % 2u) == 0u ? ya[y * FIX_W + x] : yb[y * FIX_W + x],
                    chroma_a ? cba[ci] : cbb[ci], chroma_a ? cra[ci] : crb[ci], r, g, bl);
}

static int rgb_matches(int a, int b, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t stride) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, bl;
            expected_mixed_rgb(a, b, x0 + x, y0 + y, &r, &g, &bl);
            const uint8_t *px = host(G_RGB + (y * stride + x) * 4u);
            if (px[0] != r || px[1] != g || px[2] != bl || px[3] != 0u) return 0;
        }
    return 1;
}

static void test_copy_range(void) {
    const uint32_t dest_bytes = FIX_H * RGB_STRIDE * 4u;
    if (!s_have_picture) return;   /* the SKIP lines were printed by test_decode */
    const int picture = s_last_picture;
    CHECK(call(NID_PIXELMODE, 0u, 0, 0, 0) == 0u, "ycrcbcopy: CSC back in 8888");
    put_csc(FIX_H / 16u, FIX_W / 16u);   /* the source: the last decoded picture's ME bands */
    put_dst_csc(G_DCSC, G_YCC);
    memset(host(G_YCC), 0xA5, 8u * YCC_STEP);

    /* Refusals write nothing. */
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 4u, 0) == VC_ERR_STATE && bytes_are(G_YCC, 8u * YCC_STEP, 0xA5u),
          "ycrcbcopy: an unknown flag bit is refused and writes nothing");
    MEM_W32(G_DCSC + 16u + 5u * 4u, 0u);
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 1u, 0) == VC_ERR_GUEST_ADDRESS && bytes_are(G_YCC, 8u * YCC_STEP, 0xA5u),
          "ycrcbcopy: a null destination buffer is refused before any band moves");
    MEM_W32(G_DCSC + 16u + 5u * 4u, 0x00100000u);
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 1u, 0) == VC_ERR_GUEST_ADDRESS && bytes_are(G_YCC, 8u * YCC_STEP, 0xA5u),
          "ycrcbcopy: a destination in ME RAM is refused (the copy writes guest memory)");
    memset(host(G_NOWHERE - 16u), 0xA5, 16u);
    MEM_W32(G_DCSC + 16u + 5u * 4u, G_NOWHERE - 16u);   /* band 5 needs BAND_CHROMA bytes, 16 are guest memory */
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 1u, 0) == VC_ERR_GUEST_ADDRESS && bytes_are(G_YCC, 8u * YCC_STEP, 0xA5u) &&
              bytes_are(G_NOWHERE - 16u, 16u, 0xA5u),
          "ycrcbcopy: a destination that leaves guest memory part-way is refused before any band moves (review S6)");
    put_dst_csc(G_DCSC, G_YCC);
    MEM_W32(G_SCSC2, FIX_H / 16u);
    MEM_W32(G_SCSC2 + 4u, FIX_W / 16u);
    for (uint32_t i = 0; i < 8u; i++) MEM_W32(G_SCSC2 + 16u + i * 4u, MEM_R32(G_CSC + 16u + i * 4u));
    MEM_W32(G_SCSC2 + 16u + 2u * 4u, G_NOWHERE);
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_SCSC2, 3u, 0) == VC_ERR_GUEST_ADDRESS && bytes_are(G_YCC, 8u * YCC_STEP, 0xA5u),
          "ycrcbcopy: an unreadable source band is refused before any band moves");
    MEM_W32(G_CSC + 4u, 0u);
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 3u, 0) == VC_ERR_CSC_GEOMETRY && bytes_are(G_YCC, 8u * YCC_STEP, 0xA5u),
          "ycrcbcopy: a source struct of width 0 is refused");
    MEM_W32(G_CSC + 4u, FIX_W / 16u);
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 0u, 0) == 0u && bytes_are(G_YCC, 8u * YCC_STEP, 0xA5u),
          "ycrcbcopy: flags 0 copies nothing");

    /* The bytes: half A, then half B, each band exactly its size. */
    want_bands(picture, picture);
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 1u, 0) == 0u && guest_bands_are(G_YCC, 1u),
          "ycrcbcopy: flags 1 writes bands 0, 1, 4 and 5 into the guest buffers, byte for byte, and nothing else");
    CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 2u, 0) == 0u && guest_bands_are(G_YCC, 3u),
          "ycrcbcopy: flags 2 adds bands 2, 3, 6 and 7");
    memset(host(G_RGB), 0xA5, dest_bytes);
    CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_DCSC) == 0u &&
              rgb_matches(picture, picture, 0u, 0u, FIX_W, FIX_H, RGB_STRIDE),
          "ycrcbcopy: the guest copy converts to the source picture pixel for pixel");

    /* CscAvc reads the guest bytes: luma (0, 0) is band 0's first byte. */
    {
        uint8_t *first = host(G_YCC);
        const uint8_t kept = *first;
        *first = (uint8_t)(kept ^ 0x80u);
        uint8_t r, g, bl;
        sr_h264_psp_rgb(*first, s_want[4][0], s_want[4][1], &r, &g, &bl);
        CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_DCSC) == 0u && host(G_RGB)[0] == r && host(G_RGB)[1] == g &&
                  host(G_RGB)[2] == bl,
              "cscavc: a byte the guest changes in a copied band changes the converted pixel");
        *first = kept;
    }

    /* A guest copy is a source too: JPCSP's ME memory view falls through to guest memory. */
    put_dst_csc(G_DCSC3, G_YCC3);
    memset(host(G_YCC3), 0xA5, 8u * YCC_STEP);
    CHECK(call(NID_YCRCBCOPY, G_DCSC3, G_DCSC, 3u, 0) == 0u && guest_bands_are(G_YCC3, 3u),
          "ycrcbcopy: guest buffers copy into other guest buffers byte for byte");

    /* Two pictures: half A from one, half B from another, so each half lands exactly where the
     * band split puts it. */
    const VcContext *c = vc_find(G_CTX);
    if (c && s_pictures >= 2) {
        const int other = s_last_picture - 1;
        const uint32_t other_slot = (uint32_t)other % 3u;
        for (uint32_t i = 0; i < 8u; i++) MEM_W32(G_SCSC2 + 16u + i * 4u, c->bands[other_slot][i]);
        memset(host(G_YCC), 0xA5, 8u * YCC_STEP);
        want_bands(picture, other);
        CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 1u, 0) == 0u && call(NID_YCRCBCOPY, G_DCSC, G_SCSC2, 2u, 0) == 0u &&
                  guest_bands_are(G_YCC, 3u),
              "ycrcbcopy: halves from two decoded pictures land in their own bands");
        memset(host(G_RGB), 0xA5, dest_bytes);
        CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_DCSC) == 0u &&
                  rgb_matches(picture, other, 0u, 0u, FIX_W, FIX_H, RGB_STRIDE),
              "ycrcbcopy: even luma rows and left chroma halves are half A's, the rest half B's");
        CHECK(call(NID_YCRCBCOPY, G_DCSC, G_CSC, 3u, 0) == 0u, "ycrcbcopy: the copy is one picture again");
    } else {
        skip("YCrCbCopy keeps the two halves of two pictures apart");
    }

    /* CscAvcRange: the rectangle (16, 16) 32 x 32 lands at the destination's top-left. */
    MEM_W32(G_PKT, 1u); MEM_W32(G_PKT + 4u, 1u); MEM_W32(G_PKT + 8u, 2u); MEM_W32(G_PKT + 12u, 2u);
    memset(host(G_RGB), 0xA5, dest_bytes);
    CHECK(call5(NID_CSCRANGE, G_RGB, 0u, G_PKT, RGB_STRIDE, G_CSC) == 0u, "cscavcrange: a rectangle converts");
    int margin = 1;
    for (uint32_t y = 0; y < FIX_H; y++)
        if (!bytes_are(G_RGB + (y * RGB_STRIDE + (y < 32u ? 32u : 0u)) * 4u, (RGB_STRIDE - (y < 32u ? 32u : 0u)) * 4u, 0xA5u))
            margin = 0;
    CHECK(rgb_matches(picture, picture, 16u, 16u, 32u, 32u, RGB_STRIDE),
          "cscavcrange: every pixel is the picture's pixel at the rectangle's offset");
    CHECK(margin, "cscavcrange: nothing outside the rectangle's rows and columns is written");
    CHECK(call5(NID_CSCRANGE, G_RGB, 0u, G_PKT, RGB_STRIDE, G_DCSC) == 0u &&
              rgb_matches(picture, picture, 16u, 16u, 32u, 32u, RGB_STRIDE),
          "cscavcrange: a copy converts as its source does");
    MEM_W32(G_PKT, 3u);
    memset(host(G_RGB), 0xA5, dest_bytes);
    CHECK(call5(NID_CSCRANGE, G_RGB, 0u, G_PKT, RGB_STRIDE, G_CSC) == VC_ERR_CSC_GEOMETRY &&
              bytes_are(G_RGB, dest_bytes, 0xA5u),
          "cscavcrange: a rectangle leaving the picture is refused and writes nothing");
    MEM_W32(G_PKT, 0u); MEM_W32(G_PKT + 8u, 0u);
    CHECK(call5(NID_CSCRANGE, G_RGB, 0u, G_PKT, RGB_STRIDE, G_CSC) == VC_ERR_CSC_GEOMETRY,
          "cscavcrange: an empty rectangle is refused");
    CHECK(call5(NID_CSCRANGE, G_RGB, 0u, 0x12u, RGB_STRIDE, G_CSC) == VC_ERR_GUEST_ADDRESS,
          "cscavcrange: an unreadable range is refused");
}

static void test_teardown(void) {
    /* Without an H.264 backend no access unit decoded, and Stop answers as before any decode. */
    const uint32_t stop_want = vc_find(G_CTX) && vc_find(G_CTX)->frame_count == 0u && vc_find(G_CTX)->h264 < 0
                                   ? VC_ERR_STATE : 0u;
    CHECK(call(NID_STOP, G_CTX, 0u, 0, 0) == stop_want && vc_find(G_CTX) && vc_find(G_CTX)->h264 < 0,
          "stop: the host decoder is released");
    const int allocs = s_me_alloc_count;
    CHECK(call(NID_RELEASE, G_CTX, 0, 0, 0) == 0u && MEM_R32(G_CTX + 20u) == 0u && MEM_R32(G_CTX + 92u) == 0u &&
              s_me_alloc_count == allocs - 1,
          "releaseedram: +20 and +92 are cleared and the ME memory is freed");
    CHECK(call(NID_DELETE, G_CTX, 0u, 0, 0) == 0u && s_me_alloc_count == 0 && !vc_find(G_CTX),
          "delete: the band buffers go and the context closes");
    CHECK(call(NID_INIT, G_CTX, 0u, 0, 0) == VC_ERR_NOT_OPEN, "delete: a deleted context is refused");
    if (s_have_picture) {
        memset(host(G_RGB), 0xA5, FIX_H * RGB_STRIDE * 4u);
        CHECK(call(NID_CSCAVC, G_RGB, 0u, RGB_STRIDE, G_DCSC) == 0u &&
                  rgb_matches(s_last_picture, s_last_picture, 0u, 0u, FIX_W, FIX_H, RGB_STRIDE),
              "delete: a copy in guest buffers still converts");
    }
}

/* ---- sceAudiocodec: the ATRAC3plus half --------------------------------------------------- */

#define G_AC       0x08900800u   /* 108-byte work area */
#define G_AC2      0x08900880u   /* a second work area that borrows G_AC's EDRAM */
#define G_AC3      0x08900900u   /* a work area that is never initialised */
#define G_AIN      0x08940000u   /* one PSMF audio frame */
#define G_AOUT     0x08942000u   /* one 0x2000-byte block, then a guard */
#define AC_CODEC   0x1000u

/* A PSMF audio frame: the 8-byte 0x0FD0 header (channel id in bits 2-4 of byte 2; the 10-bit
 * size code in bits 0-1 of byte 2 and all of byte 3, in 8-byte units; data = code * 8 + 8 bytes),
 * then the body, zero-padded. Returns the data size. The expected size is written out here, not
 * taken from sr_psmf_audio_frame_bytes, so the test checks the helper too. */
static uint32_t put_audio_frame_sized(uint32_t at, int channels, uint32_t code, const uint8_t *body,
                                      uint32_t body_len) {
    const uint32_t data = code * 8u + 8u;
    uint8_t *p = host(at);
    memset(p, 0, 8u + data);
    p[0] = 0x0Fu; p[1] = 0xD0u;
    p[2] = (uint8_t)((channels << 2) | ((code >> 8) & 3u));
    p[3] = (uint8_t)code;
    memcpy(p + 8, body, body_len);
    return data;
}

static uint32_t put_audio_frame(uint32_t at, int channels, const uint8_t *body, uint32_t body_len) {
    return put_audio_frame_sized(at, channels, 1u, body, body_len);
}

/* The block a separate decoder makes from the same frame, as interleaved stereo bytes. */
static int reference_block(int channels, const uint8_t *frame, uint32_t data, uint8_t *out) {
    Atrac3pBridge *ref = NULL;
    static int16_t pcm[2 * 2048];
    int samples = 0;
    if (atrac3p_bridge_create(channels, (int)data, &ref) != 0 || !ref) return 0;
    const int ok = atrac3p_bridge_decode(ref, frame, (int)data, pcm, &samples) == 0 && samples == 2048;
    atrac3p_bridge_destroy(ref);
    if (!ok) return 0;
    for (int i = 0; i < 2048; i++)
        for (int ch = 0; ch < 2; ch++) {
            const uint16_t v = (uint16_t)pcm[channels == 1 ? i : i * 2 + ch];
            out[(i * 2 + ch) * 2] = (uint8_t)v;
            out[(i * 2 + ch) * 2 + 1] = (uint8_t)(v >> 8);
        }
    return 1;
}

/* A 16-byte mono frame body that a fresh decoder turns into a block that is not silence, so
 * the mono-to-stereo duplication is visible. The candidates are xorshift32 bytes from a fixed
 * seed with the frame's first byte kept to the mono channel-unit form of the bridge
 * selftest's fixture (start bit 0, unit id 0); the first that decodes without error to a
 * block with a non-zero sample is used. Deterministic: the same body every run. */
static int find_loud_mono_body(uint8_t body[16]) {
    uint32_t state = 0x2545F491u;
    for (unsigned candidate = 0; candidate < 20000u; candidate++) {
        for (int i = 0; i < 16; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            body[i] = (uint8_t)state;
        }
        body[0] = (uint8_t)(body[0] & 0x1Fu);
        Atrac3pBridge *b = NULL;
        static int16_t pcm[2 * 2048];
        int samples = 0;
        if (atrac3p_bridge_create(1, 16, &b) != 0 || !b) return 0;
        const int ok = atrac3p_bridge_decode(b, body, 16, pcm, &samples) == 0 && samples == 2048;
        atrac3p_bridge_destroy(b);
        if (!ok) continue;
        for (int i = 0; i < 2048; i++)
            if (pcm[i] != 0) return 1;
    }
    return 0;
}

static void ac_set_io(uint32_t w56, uint32_t w72) {
    MEM_W32(G_AC + 24u, G_AIN);
    MEM_W32(G_AC + 32u, G_AOUT);
    MEM_W32(G_AC + 56u, w56);
    MEM_W32(G_AC + 72u, w72);
    memset(host(G_AOUT), 0xAA, 0x2000u + 64u);
}

static void test_audiocodec(void) {
    static const uint8_t k_terminator[] = { 0x60 };
    static uint8_t expect[0x2000];
    memset(host(G_AC), 0, 0x100u);

    CHECK(call(NID_AC_NEEDMEM, G_AC, 0x0FFFu, 0, 0) == AC_ERR_NOT_SUPPORTED &&
              call(NID_AC_NEEDMEM, G_AC, 0x1006u, 0, 0) == AC_ERR_NOT_SUPPORTED,
          "audiocodec: a codec type outside 0x1000-0x1005 is refused as not supported");
    CHECK(call(NID_AC_NEEDMEM, G_AC, 0x1001u, 0, 0) == AC_ERR_NOT_MODELLED && MEM_R32(G_AC) == 0u,
          "audiocodec: an in-range codec other than ATRAC3plus is refused (project-chosen; the unrecorded read returned 0) "
          "and the work area is untouched");
    CHECK(call(NID_AC_NEEDMEM, 0u, AC_CODEC, 0, 0) == AC_ERR_NOT_RAM &&
              call(NID_AC_NEEDMEM, G_AC + 2u, AC_CODEC, 0, 0) == AC_ERR_NOT_RAM,
          "audiocodec: a null or unaligned work area is refused");

    /* The halfword at +40 selects the words (synthetic, unrecorded read: PSP-VCODEC-001). */
    CHECK(call(NID_AC_NEEDMEM, G_AC, AC_CODEC, 0, 0) == 0x807F00FDu && MEM_R32(G_AC) == 0x05100601u &&
              MEM_R32(G_AC + 8u) == 0x20Fu && MEM_R32(G_AC + 16u) == 0u && MEM_R32(G_AC + 52u) == 0x7D00u &&
              MEM_R32(G_AC + 60u) == 0u && MEM_R32(G_AC + 64u) == 8u,
          "checkneedmem: a zero halfword at +40 returns 0x807F00FD with +8, +52 and +64 written");
    memset(host(G_AC), 0, 0x100u);
    MEM_W16(G_AC + 40u, 0x5C5Cu);
    CHECK(call(NID_AC_NEEDMEM, G_AC, AC_CODEC, 0, 0) == 0u && MEM_R32(G_AC) == 0x05100601u &&
              MEM_R32(G_AC + 8u) == 0u && MEM_R32(G_AC + 16u) == 0x1F6C0u && MEM_R32(G_AC + 52u) == 48000u &&
              MEM_R32(G_AC + 60u) == 7u && MEM_R32(G_AC + 64u) == 0x2E8u,
          "checkneedmem: halfword 0x5C5C at +40: the 48 kHz words");
    memset(host(G_AC), 0, 0x100u);
    MEM_W16(G_AC + 40u, 0x1234u);
    CHECK(call(NID_AC_NEEDMEM, G_AC, AC_CODEC, 0, 0) == AC_ERR_ME && MEM_R32(G_AC) == 0u &&
              MEM_R32(G_AC + 16u) == 0u,
          "checkneedmem: an unmeasured halfword at +40 is refused and the work area is untouched");
    memset(host(G_AC), 0, 0x100u);
    MEM_W16(G_AC + 40u, 0x5C28u);
    CHECK(call(NID_AC_NEEDMEM, G_AC, AC_CODEC, 0, 0) == 0u && MEM_R32(G_AC) == 0x05100601u &&
              MEM_R32(G_AC + 16u) == 0x7BC0u && MEM_R32(G_AC + 52u) == 44100u &&
              MEM_R32(G_AC + 60u) == 2u && MEM_R32(G_AC + 64u) == 0x2E8u,
          "checkneedmem: halfword 0x5C28 at +40: the 44.1 kHz words");
    CHECK(call(NID_AC_INIT, G_AC, AC_CODEC, 0, 0) == AC_ERR_ME && !ac_find(G_AC),
          "init: a work area without EDRAM is refused");

    const int allocs = s_me_alloc_count;
    CHECK(call(NID_AC_GETEDRAM, G_AC, AC_CODEC, 0, 0) == 0u && s_me_alloc_count == allocs + 1,
          "getedram: one ME allocation");
    const uint32_t raw = MEM_R32(G_AC + 104u), edram = MEM_R32(G_AC + 12u);
    CHECK(raw >= ME_RAM_RESERVED_LOW && raw < ME_RAM_SIZE && edram == ((raw + 0x3Fu) & ~0x3Fu) &&
              edram + 0x7BC0u <= ME_RAM_SIZE,
          "getedram: +104 holds the raw ME address and +12 the 64-byte-aligned one");

    /* A second GetEDRAM for the same work area is refused and changes nothing: uofw would
     * allocate again and orphan the first allocation (review S2). */
    CHECK(call(NID_AC_GETEDRAM, G_AC, AC_CODEC, 0, 0) == AC_ERR_EDRAM && s_me_alloc_count == allocs + 1 &&
              MEM_R32(G_AC + 104u) == raw && MEM_R32(G_AC + 12u) == edram,
          "getedram: a second allocation for the same work area is refused");

    /* Init takes a +12 that is an EDRAM GetEDRAM published, with +16 bytes inside it. As in
     * uofw, the work area need not be the one that allocated it: a disc mpeg.prx initialises
     * another (#286). */
    MEM_W32(G_AC + 12u, edram + 0x40u);
    CHECK(call(NID_AC_INIT, G_AC, AC_CODEC, 0, 0) == AC_ERR_ME && !ac_find(G_AC),
          "init: a +12 other than an EDRAM GetEDRAM published is refused");
    MEM_W32(G_AC + 12u, edram);
    MEM_W32(G_AC + 16u, 0x7BC0u + 0x100u);   /* the allocation is 0x7C40 bytes */
    CHECK(call(NID_AC_INIT, G_AC, AC_CODEC, 0, 0) == AC_ERR_ME && !ac_find(G_AC),
          "init: a +16 larger than the EDRAM allocation is refused");
    MEM_W32(G_AC + 16u, 0x7BC0u);
    memset(host(G_AC2), 0, 0x80u);
    MEM_W32(G_AC2 + 12u, edram);
    MEM_W32(G_AC2 + 16u, 0x7BC0u);
    CHECK(call(NID_AC_INIT, G_AC2, AC_CODEC, 0, 0) == 0u && ac_find(G_AC2) && MEM_R32(G_AC2) == 0x05100601u,
          "init: another work area naming that EDRAM at +12 initialises (uofw checks no owner)");

    /* ReleaseEDRAM frees only the work area's own allocation (review S1): a +104 naming a video
     * context's ME memory, or another work area's, is refused and nothing is freed. */
    memset(host(G_CTX2), 0, 96u);
    MEM_W32(G_CTX2 + 24u, 0x1000u);
    CHECK(call(NID_OPEN, G_CTX2, 0u, 0, 0) == 0u && call(NID_GETEDRAM, G_CTX2, 0u, 0, 0) == 0u,
          "releaseedram: a video context holds ME memory");
    const uint32_t video_raw = MEM_R32(G_CTX2 + 92u);
    const int held = s_me_alloc_count;
    MEM_W32(G_AC2 + 104u, video_raw);
    CHECK(call(NID_AC_RELEASE, G_AC2, 0, 0, 0) == AC_ERR_EDRAM_NOT_HELD && s_me_alloc_count == held,
          "releaseedram: +104 naming a video context's ME memory is refused and nothing is freed");
    MEM_W32(G_AC2 + 104u, raw);
    CHECK(call(NID_AC_RELEASE, G_AC2, 0, 0, 0) == AC_ERR_EDRAM_NOT_HELD && s_me_alloc_count == held &&
              MEM_R32(G_AC + 104u) == raw,
          "releaseedram: +104 naming another work area's ME memory is refused and nothing is freed");
    CHECK(call(NID_DELETE, G_CTX2, 0u, 0, 0) == 0u && s_me_alloc_count == held - 1 && !vc_find(G_CTX2),
          "releaseedram: the video context still frees its own ME memory");

    CHECK(call(NID_AC_DECODE, G_AC, AC_CODEC, 0, 0) == AC_ERR_ME, "decode: before init it is refused");
    memset(host(G_AC3), 0, 0x80u);
    CHECK(call(NID_AC_DECODE, G_AC3, AC_CODEC, 0, 0) == AC_ERR_ME,
          "decode: a work area the runtime never initialised is refused");
    MEM_W32(G_AC + 8u, 0x1234u);
    CHECK(call(NID_AC_INIT, G_AC, AC_CODEC, 0, 0) == 0u && ac_find(G_AC) && MEM_R32(G_AC + 8u) == 0u,
          "init: the work area becomes a context and its error word is cleared");

    /* A mono frame into a stereo block (+56 = 1, +72 = 2: uofw's 0x2000-byte rule). */
    uint8_t loud[16];
    CHECK(find_loud_mono_body(loud), "decode: a mono fixture frame that decodes to sound exists");
    uint32_t data = put_audio_frame(G_AIN, 1, loud, sizeof(loud));
    ac_set_io(1u, 2u);
    CHECK(reference_block(1, host(G_AIN + 8u), data, expect), "decode: the reference decoder takes the mono frame");
    CHECK(call(NID_AC_DECODE, G_AC, AC_CODEC, 0, 0) == 0u, "decode: a mono ATRAC3plus frame decodes");
    CHECK(memcmp(host(G_AOUT), expect, 0x2000u) == 0,
          "decode: the mono block is the reference decode, duplicated into both channels");
    CHECK(!bytes_are(G_AOUT, 0x2000u, 0x00u), "decode: the mono fixture block is not silence");
    CHECK(bytes_are(G_AOUT + 0x2000u, 64u, 0xAAu), "decode: nothing is written past the block");
    CHECK(MEM_R32(G_AC + 36u) == 0x2000u && MEM_R32(G_AC + 28u) == data,
          "decode: +36 is the block size and +28 the frame size");

    /* A stereo terminator frame: a block of silence over a filled buffer. */
    data = put_audio_frame(G_AIN, 2, k_terminator, sizeof(k_terminator));
    ac_set_io(2u, 2u);
    CHECK(reference_block(2, host(G_AIN + 8u), data, expect), "decode: the reference decoder takes the stereo frame");
    CHECK(call(NID_AC_DECODE, G_AC, AC_CODEC, 0, 0) == 0u && memcmp(host(G_AOUT), expect, 0x2000u) == 0 &&
              bytes_are(G_AOUT, 0x2000u, 0x00u) && MEM_R32(G_AC + 36u) == 0x2000u,
          "decode: a stereo frame writes the reference block");

    /* The +56/+72 size rule fills only +36: the block is stereo whatever it gives (JPCSP's
     * sceAudiocodecInit output), as a disc mpeg.prx needs (#286). */
    ac_set_io(0u, 1u);
    CHECK(call(NID_AC_DECODE, G_AC, AC_CODEC, 0, 0) == 0u && memcmp(host(G_AOUT), expect, 0x2000u) == 0 &&
              bytes_are(G_AOUT + 0x2000u, 64u, 0xAAu) && MEM_R32(G_AC + 36u) == 0x1000u,
          "decode: +72 = 1 still writes the stereo block, and +36 holds the rule's 0x1000");
    ac_set_io(0u, 0u);
    CHECK(call(NID_AC_DECODE, G_AC, AC_CODEC, 0, 0) == 0u && memcmp(host(G_AOUT), expect, 0x2000u) == 0 &&
              MEM_R32(G_AC + 36u) == 0u,
          "decode: words that give a size of 0 still write the stereo block, and +36 holds 0");

    /* Frames over 2 KiB set the size code's high bits in byte 2 (#2205). The code is 8-byte units,
     * so it is ORed together before the multiply: code 0x101 is a 0x810-byte data part (0x818 with
     * the header), where multiplying byte 3 before the OR would give 0x110. */
    data = put_audio_frame_sized(G_AIN, 2, 0x101u, k_terminator, sizeof(k_terminator));
    CHECK(data == 0x810u && host(G_AIN)[2] == 0x09u && host(G_AIN)[3] == 0x01u,
          "decode: the over-2-KiB stereo fixture sets bits 0-1 of byte 2");
    ac_set_io(2u, 2u);
    CHECK(reference_block(2, host(G_AIN + 8u), data, expect),
          "decode: the reference decoder takes the over-2-KiB stereo frame");
    CHECK(call(NID_AC_DECODE, G_AC, AC_CODEC, 0, 0) == 0u && memcmp(host(G_AOUT), expect, 0x2000u) == 0 &&
              bytes_are(G_AOUT + 0x2000u, 64u, 0xAAu),
          "decode: an over-2-KiB stereo frame writes the reference block");
    CHECK(MEM_R32(G_AC + 28u) == 0x810u,
          "decode: +28 is the over-2-KiB frame's size, the size code ORed before the multiply");

    /* Refusals leave the output untouched. */
    ac_set_io(2u, 2u);
    host(G_AIN)[1] = 0x00u;
    CHECK(call(NID_AC_DECODE, G_AC, AC_CODEC, 0, 0) == AC_ERR_ME && bytes_are(G_AOUT, 0x2000u, 0xAAu),
          "decode: input without the 0x0FD0 frame header is refused");

    CHECK(call(NID_AC_RELEASE, G_AC, 0, 0, 0) == 0u && MEM_R32(G_AC + 104u) == 0u && MEM_R32(G_AC + 12u) == 0u &&
              s_me_alloc_count == allocs && !ac_find(G_AC),
          "releaseedram: the ME memory and the decoder go and the words are cleared");
    CHECK(!ac_find(G_AC2) && call(NID_AC_DECODE, G_AC2, AC_CODEC, 0, 0) == AC_ERR_ME,
          "releaseedram: the work area that borrowed the EDRAM goes with it (review S5)");
    CHECK(call(NID_AC_RELEASE, G_AC, 0, 0, 0) == AC_ERR_EDRAM_NOT_HELD,
          "releaseedram: a second release is refused");

    /* A work area recycled without a release (the title clears or reuses it, so +104 no longer
     * names its allocation) is not wedged: GetEDRAM frees the stale allocation and makes a new
     * one, and the new one releases (delta review S-new-1). */
    CHECK(call(NID_AC_NEEDMEM, G_AC, AC_CODEC, 0, 0) == 0u && call(NID_AC_GETEDRAM, G_AC, AC_CODEC, 0, 0) == 0u &&
              call(NID_AC_INIT, G_AC, AC_CODEC, 0, 0) == 0u && s_me_alloc_count == allocs + 1,
          "recycle: a work area holds EDRAM and a decoder");
    memset(host(G_AC2), 0, 0x80u);
    MEM_W32(G_AC2 + 12u, MEM_R32(G_AC + 12u));
    MEM_W32(G_AC2 + 16u, 0x7BC0u);
    CHECK(call(NID_AC_INIT, G_AC2, AC_CODEC, 0, 0) == 0u && ac_find(G_AC2),
          "recycle: another work area borrows that EDRAM");
    memset(host(G_AC), 0, AC_WORK_BYTES_EDRAM);
    MEM_W16(G_AC + 40u, 0x5C28u);   /* the parameter halfword sceMpegCreate leaves */
    CHECK(call(NID_AC_NEEDMEM, G_AC, AC_CODEC, 0, 0) == 0u && call(NID_AC_GETEDRAM, G_AC, AC_CODEC, 0, 0) == 0u &&
              s_me_alloc_count == allocs + 1 && MEM_R32(G_AC + 104u) != 0u && !ac_find(G_AC) && !ac_find(G_AC2),
          "recycle: GetEDRAM on the cleared work area frees the stale allocation, and every context on it, "
          "and makes a new one");
    CHECK(call(NID_AC_RELEASE, G_AC, 0, 0, 0) == 0u && s_me_alloc_count == allocs,
          "recycle: the new allocation releases");
}

/* LoadExec: hle_videocodec_guest_reset leaves no context, ME allocation or CSC state behind, so
 * the next program can open a context at the same guest address. */
static void test_guest_reset(void) {
    memset(host(G_CTX), 0, 96u);
    memset(host(G_AC), 0, 0x100u);
    MEM_W16(G_AC + 40u, 0x5C28u);
    CHECK(call(NID_OPEN, G_CTX, 0u, 0, 0) == 0u && call(NID_GETEDRAM, G_CTX, 0u, 0, 0) == 0u,
          "reset: a context is open and holds ME memory");
    CHECK(call(NID_AC_NEEDMEM, G_AC, AC_CODEC, 0, 0) == 0u && call(NID_AC_GETEDRAM, G_AC, AC_CODEC, 0, 0) == 0u &&
              call(NID_AC_INIT, G_AC, AC_CODEC, 0, 0) == 0u && ac_find(G_AC),
          "reset: an audio work area is initialised and holds ME memory");
    CHECK(call(NID_PIXELMODE, 1u, 0, 0, 0) == 0u && call(NID_CSCINIT, 48u, 0, 0, 0) == 0u,
          "reset: the CSC pixel mode and default width are set");
    me_ram()[0x10000u] = 0x5Au;
    hle_videocodec_guest_reset();
    int clear = !vc_find(G_CTX) && !ac_find(G_AC) && s_me_alloc_count == 0 && me_ram()[0x10000u] == 0u;
    for (int i = 0; i < VC_MAX_CONTEXTS; i++) if (s_vc[i].used || s_vc[i].h264 != -1) clear = 0;
    for (int i = 0; i < AC_MAX_CONTEXTS; i++) if (s_ac[i].used || s_ac[i].at3p) clear = 0;
    CHECK(clear, "reset: no video context, audio context or ME allocation is left, and ME RAM is zeroed");
    CHECK(s_csc_pixel_mode == VC_PIXEL_8888 && s_csc_default_width == 0u,
          "reset: the CSC state is a fresh boot's (8888, no default width)");
    memset(host(G_CTX), 0, 96u);
    CHECK(call(NID_OPEN, G_CTX, 0u, 0, 0) == 0u, "reset: the same guest address opens again");
    CHECK(call(NID_GETEDRAM, G_CTX, 0u, 0, 0) == 0u && MEM_R32(G_CTX + 92u) == 0x10000u,
          "reset: the ME allocator starts again at its lowest address");
    CHECK(call(NID_RELEASE, G_CTX, 0u, 0, 0) == 0u && call(NID_DELETE, G_CTX, 0u, 0, 0) == 0u &&
              s_me_alloc_count == 0,
          "reset: the reopened context closes");
}

/* Band room (review B1). Every band of every set has room for its vc_band_bytes() count inside
 * the band allocation, apart from every other band, at each geometry the ME RAM model holds. At
 * w % 32 == 16, band 2 (the x % 32 == 0 strips, y1 bytes) is longer than JPCSP's size_y2 room. */
static void test_band_room(void) {
    int geometries = 0, roomy = 1;
    for (uint32_t w = 16u; w <= VC_MAX_WIDTH && roomy; w += 16u)
        for (uint32_t h = 16u; h <= VC_MAX_HEIGHT && roomy; h += 16u) {
            VcContext c;
            memset(&c, 0, sizeof(c));
            c.ctx = G_CTX2;
            if (!vc_allocate_bands(&c, w, h)) continue;
            uint32_t size = 0u, bytes[8];
            for (int i = 0; i < s_me_alloc_count; i++)
                if (s_me_allocs[i].addr == c.band_base) size = s_me_allocs[i].size;
            vc_band_bytes(w, h, bytes);
            geometries++;
            for (int i = 0; i < VC_BAND_SETS * 8 && roomy; i++) {
                const uint32_t a = c.bands[i / 8][i % 8], an = bytes[i % 8];
                if (!an) continue;
                if (!size || a < c.band_base + 256u || a - c.band_base > size || an > size - (a - c.band_base))
                    roomy = 0;
                for (int j = i + 1; j < VC_BAND_SETS * 8 && roomy; j++) {
                    const uint32_t b = c.bands[j / 8][j % 8], bn = bytes[j % 8];
                    if (bn && a < b + bn && b < a + an) roomy = 0;
                }
            }
            if (!roomy) fprintf(stderr, "videocodec: band overlap at %ux%u\n", (unsigned)w, (unsigned)h);
            (void)me_free(c.band_base, ME_OWNER_VIDEO, c.ctx);
        }
    CHECK(geometries > 700 && roomy,
          "bands: at every geometry ME RAM holds, each band has room for its count, apart from every other band");

    /* A 48 x 64 movie (y1 = 1024, y2 = 512): decoding the next picture leaves the previous
     * picture's bands as they were. */
    static const char k_case[] = "Decode keeps a picture's bands when the next picture decodes, at w % 32 == 16";
    if (!sr_h264_backend_available()) { skip(k_case); return; }
    s_fix_w_mbs = 3u;
    s_fix_h_mbs = 4u;
    memset(host(G_CTX), 0, 96u);
    CHECK(call(NID_OPEN, G_CTX, 0u, 0, 0) == 0u && call(NID_GETEDRAM, G_CTX, 0u, 0, 0) == 0u &&
              call(NID_INIT, G_CTX, 0u, 0, 0) == 0u,
          "bands: a 48 x 64 context opens");
    MEM_W32(G_CTX + 16u, G_BUF2);
    MEM_W32(G_CTX + 44u, G_YUV);
    MEM_W32(G_CTX + 48u, G_BUF3);
    MEM_W32(G_CTX + 80u, G_SEI);
    uint32_t bytes[8];
    vc_band_bytes(48u, 64u, bytes);
    static uint8_t prev[8][1024];
    uint32_t prev_at[8];
    int delivered = 0, kept = 1;
    for (int au = 0; au < 4; au++) {
        load_au(au, 1);
        CHECK(call(NID_DECODE, G_CTX, 0u, 0, 0) == 0u, "bands: a 48 x 64 access unit decodes");
        if (MEM_R32(G_BUF2 + 32u) != 1u) continue;
        if (delivered)
            for (int b = 0; b < 8; b++)
                if (memcmp(s_me_ram + prev_at[b], prev[b], bytes[b]) != 0) kept = 0;
        for (int b = 0; b < 8; b++) {
            prev_at[b] = MEM_R32(G_YUV + (uint32_t)b * 4u);
            memcpy(prev[b], s_me_ram + prev_at[b], bytes[b]);
        }
        delivered++;
    }
    CHECK(delivered >= 2 && kept, "bands: each 48 x 64 picture's bands survive the next decode byte for byte");
    CHECK(call(NID_DELETE, G_CTX, 0u, 0, 0) == 0u && !vc_find(G_CTX), "bands: the 48 x 64 context closes");
    s_fix_w_mbs = FIX_W_MBS;
    s_fix_h_mbs = FIX_H_MBS;
}

int main(void) {
    sr_mem_init();
    test_registration();
    test_context_words();
    test_version_sei();
    test_packet_copy();
    test_decode_refusals();
    test_decode();
    test_csc();
    test_copy_range();
    test_teardown();
    test_audiocodec();
    test_guest_reset();
    test_band_room();
    if (failures) {
        fprintf(stderr, "videocodec selftest: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    if (skips)
        printf("videocodec selftest: OK, %d checks passed; %d decode cases SKIP (no H.264 backend in this build)\n",
               checks, skips);
    else
        printf("videocodec selftest: OK, %d checks passed\n", checks);
    return 0;
}
