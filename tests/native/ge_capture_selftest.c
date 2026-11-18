// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include "ge_capture.h"
#include "recomp.h"

/* Every production call below sits inside assert(): keep the checks in a build that defines
 * NDEBUG, as the other native tests do (#735). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *g_mem;
volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t address, uint32_t bytes) {
    (void)address; (void)bytes;
}
void sr_ge_vram_write_prepare(uint32_t address, uint32_t bytes) {
    (void)address; (void)bytes;
}
void sr_ge_vram_write_commit(uint32_t address, uint32_t bytes) {
    (void)address; (void)bytes;
}

static uint16_t zbuf[GE_CAPTURE_ZBUF_WORDS];

/* A capture of one list at 0x00123000 with the reads `run` makes; returns end()'s result. */
static int capture_with(const char *path, const GeState *initial, const GeState *final,
                        void (*run)(void)) {
    if (!ge_capture_begin(path, 5, 0x00123000u, initial, zbuf)) return -1;
    run();
    if (!ge_capture_active()) return 0;
    return ge_capture_end(final);
}

/* A list the CPU streams past a stall: the GE reads the words before the stall, the CPU
 * writes the next commands in the same page, and a resumed run reads them. The fixture must
 * hold the words as the GE read them, not the page as it was at first touch. */
static void run_streamed_list(void) {
    SR_HOST(0x08300000u)[0] = 0x11;
    ge_capture_note_memory(0x08300000u, 4);       /* words before the stall */
    SR_HOST(0x08300000u)[8] = 0x22;               /* the CPU writes past the stall */
    ge_capture_note_memory(0x08300008u, 4);       /* the resumed run reads them */
}

/* The CPU rewrites a byte the GE already consumed this frame: one page copy cannot hold both
 * values, so the capture is dropped rather than written with either. */
static void run_rewritten_vertex(void) {
    SR_HOST(0x08310000u)[4] = 0x33;
    ge_capture_note_memory(0x08310004u, 1);
    SR_HOST(0x08310000u)[4] = 0x44;
    ge_capture_note_memory(0x08310004u, 1);
}

/* GE reads through the VRAM mirrors fold onto the eager VRAM copy: neither the swizzled
 * 0x04600000 view nor the linear 0x04400000 view saves a page or stops the capture. */
static void run_vram_mirror_reads(void) {
    ge_capture_note_memory(0x04600000u, 4);
    ge_capture_note_memory(0x04200040u, 4);
    SR_HOST(0x04000010u)[0] = 0x99;                /* a draw before the next read */
    ge_capture_note_memory(0x04400010u, 4);
}

/* The GE's own store (a block transfer into RAM) is replayed, so a later GE read of it is not
 * a CPU change; the saved page keeps the byte from before the store. */
static void run_ge_store_then_read(void) {
    SR_HOST(0x08330000u)[2] = 0x66;
    const uint8_t stored[2] = { 0x77, 0x78 };
    ge_capture_note_write(0x08330002u, stored, 2);
    memcpy(SR_HOST(0x08330002u), stored, 2);
    ge_capture_note_memory(0x08330002u, 2);
}

/* A CPU store to VRAM after begin that no GE work follows does not reach the replay. */
static void run_late_cpu_vram_store(void) {
    ge_capture_note_memory(0x08340000u, 4);
    ge_capture_note_cpu_vram_store(0x04001000u, 4);
}

/* One the GE reads after is not in the VRAM copy: the capture is dropped. */
static void run_cpu_vram_store_then_ge_read(void) {
    ge_capture_note_cpu_vram_store(0x04001000u, 4);
    ge_capture_note_memory(0x08340000u, 4);
}

/* Before the next list the caller drops it and may begin again at that list. */
static void run_cpu_vram_store_then_list(void) {
    ge_capture_note_cpu_vram_store(0x04001000u, 4);
    assert(ge_capture_drop_after_cpu_vram_store());
    assert(!ge_capture_drop_after_cpu_vram_store());
}

static void run_nothing(void) {}

static long file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    return size;
}

static void check_page_fixes(const char *path, const GeState *initial, const GeState *final) {
    GeCaptureFixture fixture;
    assert(capture_with(path, initial, final, run_streamed_list) == 1);
    assert(ge_capture_load(path, &fixture));
    assert(fixture.page_count == 1 && fixture.pages[0].addr == 0x08300000u);
    assert(fixture.pages[0].data[0] == 0x11 && fixture.pages[0].data[8] == 0x22);
    ge_capture_free(&fixture);
    remove(path);

    assert(capture_with(path, initial, final, run_rewritten_vertex) == 0);
    assert(!ge_capture_active() && file_size(path) < 0);

    SR_HOST(0x04000010u)[0] = 0x55;
    assert(capture_with(path, initial, final, run_vram_mirror_reads) == 1);
    assert(ge_capture_load(path, &fixture));
    assert(fixture.page_count == 0 && fixture.initial_vram[0x10] == 0x55);
    ge_capture_free(&fixture);

    assert(capture_with(path, initial, final, run_ge_store_then_read) == 1);
    assert(ge_capture_load(path, &fixture));
    assert(fixture.page_count == 1 && fixture.pages[0].data[2] == 0x66);
    ge_capture_free(&fixture);
    assert(capture_with(path, initial, final, run_late_cpu_vram_store) == 1);
    remove(path);
    assert(capture_with(path, initial, final, run_cpu_vram_store_then_ge_read) == 0);
    assert(file_size(path) < 0);
    assert(capture_with(path, initial, final, run_cpu_vram_store_then_list) == 0);
    assert(file_size(path) < 0);

    /* A fixture whose page lies in a VRAM mirror is refused: replay would write it over the
     * eager VRAM copy. The page record follows the header, both states, the list, the depth
     * buffer and the VRAM. */
    GeState state = *initial;
    assert(ge_capture_begin(path, 5, 0x00123000u, &state, zbuf));
    ge_capture_note_memory(0x08320000u, 4);
    assert(ge_capture_end(final));
    long page_record = 68L + 2L * (long)sizeof(GeState) + 4L +
                       (long)(GE_CAPTURE_ZBUF_WORDS * 2u) + (long)GE_CAPTURE_VRAM_SIZE;
    assert(file_size(path) == page_record + 4L + (long)GE_CAPTURE_PAGE_SIZE);
    FILE *file = fopen(path, "r+b");
    assert(file && fseek(file, page_record, SEEK_SET) == 0);
    const uint32_t mirror_page = 0x04400000u;
    assert(fwrite(&mirror_page, sizeof(mirror_page), 1, file) == 1);
    fclose(file);
    assert(!ge_capture_load(path, &fixture));
    remove(path);
}

/* The loader takes GeState from the file: a cursor or size the GE indexes or shifts by must
 * hold a value its command could have set, or the fixture is refused. */
static void check_state_validation(const char *path, const GeState *initial, const GeState *final) {
    GeCaptureFixture fixture;
    GeState bad = *initial;
    bad.bone_num = -100000;
    assert(capture_with(path, &bad, final, run_nothing) == 1);
    assert(!ge_capture_load(path, &fixture));
    bad = *final;
    bad.tex_mip_wlog2[0] = 31;
    assert(capture_with(path, initial, &bad, run_nothing) == 1);
    assert(!ge_capture_load(path, &fixture));
    bad = *initial;
    bad.tex_w = 3;
    assert(capture_with(path, &bad, final, run_nothing) == 1);
    assert(!ge_capture_load(path, &fixture));
    assert(capture_with(path, initial, final, run_nothing) == 1);
    assert(ge_capture_load(path, &fixture));
    ge_capture_free(&fixture);
    remove(path);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    assert(arena);
    g_mem = arena + 0x08000000u;
    GeState initial, final;
    memset(&initial, 0, sizeof(initial));
    memset(&final, 0, sizeof(final));
    for (uint32_t i = 0; i < GE_CAPTURE_ZBUF_WORDS; i++) zbuf[i] = (uint16_t)(i ^ 0x5a5au);
    initial.fbp = GE_CAPTURE_VRAM_BASE + 0x4000u;
    initial.fbw = 512;
    initial.fbfmt = 2;
    final = initial;
    final.vaddr = 0x08123456u;
    final.iaddr = 0x08124560u;
    final.fbw = 256;
    SR_HOST(GE_CAPTURE_VRAM_BASE)[17] = 0x6d;
    SR_HOST(0x00123000u)[9] = 0xa1;
    SR_HOST(0x08145000u)[11] = 0xb2;

    /* The start address is seeded by begin(), including for a runtime capture
     * that begins on a resumed stall PC rather than a fresh list enqueue. */
    assert(ge_capture_begin(argv[1], 77, 0x00123008u, &initial, zbuf));
    assert(ge_capture_add_list(0x00124008u));
    ge_capture_note_memory(0x00123009u, 4);
    ge_capture_note_memory(0x0814500bu, 1);
    ge_capture_note_memory(0x08145fffu, 2);  /* crosses into a third sparse page */
    ge_capture_note_memory(GE_CAPTURE_VRAM_BASE + 17u, 1);  /* covered eagerly */
    SR_HOST(0x00123000u)[9] = 0xff;
    SR_HOST(GE_CAPTURE_VRAM_BASE)[17] = 0xee;
    assert(ge_capture_end(&final));

    GeCaptureFixture fixture;
    assert(ge_capture_load(argv[1], &fixture));
    assert(fixture.frame == 77 && fixture.list_count == 2);
    assert(fixture.list_addrs[0] == 0x00123008u && fixture.list_addrs[1] == 0x00124008u);
    assert(fixture.page_count == 3);
    assert(!memcmp(&fixture.final_state, &final, sizeof(final)));   /* end()'s state, not begin()'s */
    memset(SR_HOST(GE_CAPTURE_VRAM_BASE), 0, GE_CAPTURE_VRAM_SIZE);
    memset(SR_HOST(0x00123000u), 0, GE_CAPTURE_PAGE_SIZE);
    memset(SR_HOST(0x08145000u), 0, GE_CAPTURE_PAGE_SIZE * 2u);
    memset(zbuf, 0, sizeof(zbuf));
    GeState restored;
    assert(ge_capture_apply(&fixture, &restored, zbuf));
    assert(!memcmp(&restored, &initial, sizeof(initial)));
    assert(SR_HOST(GE_CAPTURE_VRAM_BASE)[17] == 0x6d);
    assert(SR_HOST(0x00123000u)[9] == 0xa1);
    assert(SR_HOST(0x08145000u)[11] == 0xb2);
    assert(zbuf[12345] == (uint16_t)(12345u ^ 0x5a5au));
    ge_capture_free(&fixture);
    remove(argv[1]);
    check_page_fixes(argv[1], &initial, &final);
    check_state_validation(argv[1], &initial, &final);
    free(arena);
    puts("ge capture selftest: OK");
    return 0;
}
