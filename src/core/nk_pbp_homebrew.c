/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Homebrew in an EBOOT.PBP (issue #2143): see nk_pbp_homebrew.h. The disc
 * image is written from ECMA-119 and the PARAM.SFO layout; no third-party
 * code is used.
 */

#include "nk_pbp_homebrew.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_iso.h"
#include "nk_platform.h"

#define HB_PBP_HEADER_SIZE 0x28u
#define HB_SECTOR 2048u
#define HB_CHUNK (64u * 1024u)
#define HB_MAX_ICON_BYTES (1024u * 1024u)
#define HB_SFO_SIZE 240u
#define HB_SFO_TITLE_MAX 127u
/* The sectors before the program: system area (0-15), primary volume
   descriptor (16), terminator (17), root (18), PSP_GAME (19), SYSDIR (20) and
   PARAM.SFO (21). */
#define HB_PROGRAM_LBA 22u
#define HB_PLACEHOLDER_DISC_ID "NKHB00000"

static uint32_t hb_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t hb_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void hb_put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void hb_put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int hb_read_at(FILE *f, uint64_t offset, void *buf, size_t len)
{
    if (nk_fseek64(f, (int64_t)offset, SEEK_SET) != 0) return -1;
    return fread(buf, 1, len, f) == len ? 0 : -1;
}

/* The PARAM.SFO the image carries: CATEGORY, DISC_ID and TITLE, 240 bytes. */
static void hb_build_sfo(const char *title, uint8_t sfo[HB_SFO_SIZE])
{
    static const char keys[] = "CATEGORY\0DISC_ID\0TITLE";
    size_t length = 0;
    char clean[HB_SFO_TITLE_MAX + 1];
    memset(sfo, 0, HB_SFO_SIZE);
    for (; title != NULL && title[length] != '\0' && length < HB_SFO_TITLE_MAX; length++) {
        unsigned char c = (unsigned char)title[length];
        clean[length] = c < 0x20u || c == 0x7Fu ? ' ' : (char)c;
    }
    /* A cut must not leave half of a multi-byte character: drop a trailing lead byte and
       its continuation bytes when the next byte of the title continues them. */
    if (title != NULL && title[length] != '\0' && ((unsigned char)title[length] & 0xC0u) == 0x80u) {
        while (length > 0 && ((unsigned char)clean[length - 1] & 0xC0u) == 0x80u) length--;
        if (length > 0 && ((unsigned char)clean[length - 1] & 0x80u) != 0) length--;
    }
    clean[length] = '\0';
    if (length == 0) {
        memcpy(clean, "Homebrew", 9);
        length = 8;
    }
    memcpy(sfo, "\0PSF", 4);
    hb_put_le32(sfo + 4, 0x00000101u);
    hb_put_le32(sfo + 8, 68u);  /* key table */
    hb_put_le32(sfo + 12, 92u); /* data table */
    hb_put_le32(sfo + 16, 3u);
    /* (key offset, format 0x0204 UTF-8, length, maximum length, data offset) */
    {
        const struct { uint16_t key; uint32_t len, max, data; } rows[3] = {
            { 0u, 3u, 4u, 0u },
            { 9u, (uint32_t)strlen(HB_PLACEHOLDER_DISC_ID) + 1u, 16u, 4u },
            { 17u, (uint32_t)length + 1u, 128u, 20u },
        };
        for (int i = 0; i < 3; i++) {
            uint8_t *row = sfo + 20 + i * 16;
            hb_put_le16(row, rows[i].key);
            hb_put_le16(row + 2, 0x0204u);
            hb_put_le32(row + 4, rows[i].len);
            hb_put_le32(row + 8, rows[i].max);
            hb_put_le32(row + 12, rows[i].data);
        }
    }
    memcpy(sfo + 68, keys, sizeof(keys));
    memcpy(sfo + 92, "UG", 2);
    memcpy(sfo + 92 + 4, HB_PLACEHOLDER_DISC_ID, strlen(HB_PLACEHOLDER_DISC_ID));
    memcpy(sfo + 92 + 20, clean, length);
}

typedef struct {
    uint32_t offsets[8];
    uint64_t size;
} HbPackage;

static int hb_read_package(NkPspCtx *ctx, FILE *f, HbPackage *pkg)
{
    uint8_t header[HB_PBP_HEADER_SIZE];
    int64_t end;
    if (nk_fseek64(f, 0, SEEK_END) != 0 || (end = nk_ftell64(f)) < 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the file's size could not be read");
    }
    pkg->size = (uint64_t)end;
    if (hb_read_at(f, 0, header, sizeof(header)) != 0 || memcmp(header, "\0PBP", 4) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL, "the file is not a PBP package");
    }
    for (int i = 0; i < 8; i++) {
        pkg->offsets[i] = hb_le32(header + 8 + i * 4);
        if (pkg->offsets[i] < HB_PBP_HEADER_SIZE || (uint64_t)pkg->offsets[i] > pkg->size ||
            (i > 0 && pkg->offsets[i] < pkg->offsets[i - 1])) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "the PBP package's section table is malformed");
        }
    }
    return NK_PSP_OK;
}

/* SceModuleInfo is found the way tools/psp_import_table.py finds it, so the player and code
   generation read the same record: the ".rodata.sceModuleInfo" section when the program has a
   section table that names one; otherwise (a stripped program) phdr[0].p_paddr with bit 31
   masked is the record's FILE offset, and it must lie inside a PT_LOAD segment's file bytes.
   A program that declares no record is user mode. A record, section table or program header
   that is declared but lies outside the program is a malformed program (-2), never user mode.
   Bit 31 of p_paddr itself marks a kernel module, as the PRX loader reads it. Imports only
   custom firmware provides are refused later, by the import-table check code generation runs
   on every program. Returns 0, -1 (unreadable) or -2 (malformed). */
#define HB_MODULE_INFO_SIZE 52u
#define HB_MODULE_INFO_SECTION ".rodata.sceModuleInfo"
#define HB_MAX_SECTIONS 256u
#define HB_MAX_SEGMENTS 64u

static int hb_module_is_kernel_mode(FILE *f, uint64_t program_offset, uint32_t program_size,
                                    bool *kernel)
{
    uint8_t elf[0x34];
    uint8_t header[40];
    uint8_t attr[2];
    uint32_t phoff, shoff, str_offset = 0, str_size = 0, paddr_flag = 0;
    uint16_t phentsize, phnum, shentsize, shnum, shstrndx;
    uint64_t record = 0;
    bool found = false;
    *kernel = false;
    if (hb_read_at(f, program_offset, elf, sizeof(elf)) != 0) return -1;
    phoff = hb_le32(elf + 28);
    shoff = hb_le32(elf + 32);
    phentsize = hb_le16(elf + 42);
    phnum = hb_le16(elf + 44);
    shentsize = hb_le16(elf + 46);
    shnum = hb_le16(elf + 48);
    shstrndx = hb_le16(elf + 50);

    if (shnum > 0) {
        if (shnum > HB_MAX_SECTIONS || shentsize != sizeof(header) || shstrndx >= shnum ||
            (uint64_t)shoff + (uint64_t)shnum * sizeof(header) > program_size)
            return -2;
        if (hb_read_at(f, program_offset + shoff + (uint64_t)shstrndx * sizeof(header), header,
                       sizeof(header)) != 0)
            return -1;
        str_offset = hb_le32(header + 16);
        str_size = hb_le32(header + 20);
        if (hb_le32(header + 4) != 3u || (uint64_t)str_offset + str_size > program_size)
            return -2;
        for (uint32_t i = 0; i < shnum && !found; i++) {
            char name[sizeof(HB_MODULE_INFO_SECTION) + 1];
            uint32_t name_at, want;
            if (hb_read_at(f, program_offset + shoff + (uint64_t)i * sizeof(header), header,
                           sizeof(header)) != 0)
                return -1;
            name_at = hb_le32(header);
            if (name_at >= str_size) return -2;
            want = str_size - name_at < sizeof(name) - 1 ? str_size - name_at
                                                          : (uint32_t)sizeof(name) - 1u;
            memset(name, 0, sizeof(name));
            if (hb_read_at(f, program_offset + str_offset + name_at, name, want) != 0) return -1;
            if (strcmp(name, HB_MODULE_INFO_SECTION) != 0) continue;
            if (hb_le32(header + 4) == 8u || hb_le32(header + 20) < HB_MODULE_INFO_SIZE ||
                (uint64_t)hb_le32(header + 16) + HB_MODULE_INFO_SIZE > program_size)
                return -2;
            record = hb_le32(header + 16);
            found = true;
        }
    }
    if (!found && phnum > 0) {
        uint32_t paddr;
        if (phnum > HB_MAX_SEGMENTS || phentsize != 32u ||
            (uint64_t)phoff + (uint64_t)phnum * 32u > program_size)
            return -2;
        if (hb_read_at(f, program_offset + phoff, header, 32) != 0) return -1;
        paddr = hb_le32(header + 12);
        if (paddr != 0u) {
            uint64_t offset = paddr & 0x7FFFFFFFu;
            bool inside = false;
            paddr_flag = paddr & 0x80000000u;
            for (uint32_t i = 0; i < phnum && !inside; i++) {
                if (hb_read_at(f, program_offset + phoff + (uint64_t)i * 32u, header, 32) != 0)
                    return -1;
                if (hb_le32(header) == 1u) {
                    uint64_t seg_offset = hb_le32(header + 4);
                    uint64_t seg_size = hb_le32(header + 16);
                    inside = offset >= seg_offset &&
                             offset + HB_MODULE_INFO_SIZE <= seg_offset + seg_size &&
                             offset + HB_MODULE_INFO_SIZE <= program_size;
                }
            }
            if (!inside) return -2;
            record = offset;
            found = true;
        }
    }
    if (!found) return 0;
    if (hb_read_at(f, program_offset + record, attr, sizeof(attr)) != 0) return -1;
    *kernel = (hb_le16(attr) & 0x1000u) != 0 || paddr_flag != 0;
    return 0;
}

static void hb_u64(NkSha256 *sha, uint64_t value)
{
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    nk_sha256_update(sha, bytes, sizeof(bytes));
}

/* The key nk_iso_inspect gives the finished image: the digest over the domain
   tag (with its terminating NUL, as nk_iso.c hashes it), then PARAM.SFO and
   EBOOT.BIN, each after its length as a little-endian 64-bit integer. */
static int hb_key(NkPspCtx *ctx, FILE *f, uint64_t program_offset, uint32_t program_size,
                  const uint8_t sfo[HB_SFO_SIZE], char key[10])
{
    static const char domain[] = "nakagawa-non-retail-disc-key-v1";
    NkSha256 sha;
    uint8_t digest[32];
    uint8_t *buffer = (uint8_t *)malloc(HB_CHUNK);
    uint32_t done = 0;
    if (buffer == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
    nk_sha256_init(&sha);
    nk_sha256_update(&sha, (const uint8_t *)domain, sizeof(domain));
    hb_u64(&sha, HB_SFO_SIZE);
    nk_sha256_update(&sha, sfo, HB_SFO_SIZE);
    hb_u64(&sha, program_size);
    while (done < program_size) {
        uint32_t count = program_size - done < HB_CHUNK ? program_size - done : HB_CHUNK;
        if (hb_read_at(f, program_offset + done, buffer, count) != 0) {
            free(buffer);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the package's program could not be read");
        }
        nk_sha256_update(&sha, buffer, count);
        done += count;
    }
    free(buffer);
    nk_sha256_finish(&sha, digest);
    nk_non_retail_disc_key(digest, key);
    return NK_PSP_OK;
}

int nk_pbp_homebrew_inspect_file(NkPspCtx *ctx, const char *pbp_path, const NkPbpInfo *info,
                                 NkPbpHomebrew *out)
{
    FILE *f;
    HbPackage pkg;
    uint8_t head[0x34];
    uint8_t sfo[HB_SFO_SIZE];
    uint8_t png[8];
    uint32_t program_size;
    uint64_t program_offset;
    bool kernel = false;
    int rc;

    if (ctx == NULL || pbp_path == NULL || info == NULL || out == NULL) return NK_PSP_ERR_INTERNAL;
    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "%s", info->title);
    f = nk_fopen_utf8(pbp_path, "rb");
    if (f == NULL) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the file could not be opened");
    }
    rc = hb_read_package(ctx, f, &pkg);
    if (rc != NK_PSP_OK) {
        fclose(f);
        return rc;
    }
    program_offset = pkg.offsets[6];
    program_size = pkg.offsets[7] - pkg.offsets[6];
    out->program_size = program_size;
    if (program_size > NK_PBP_HOMEBREW_MAX_PROGRAM_BYTES) {
        out->kind = NK_PBP_HB_TOO_LARGE;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program is larger than Nakagawa reads");
    }
    if (program_size >= sizeof(head)) {
        if (hb_read_at(f, program_offset, head, sizeof(head)) != 0) {
            fclose(f);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the package's program could not be read");
        }
    } else {
        memset(head, 0, sizeof(head));
        if (program_size > 0 && hb_read_at(f, program_offset, head, program_size) != 0) {
            fclose(f);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the package's program could not be read");
        }
    }
    if (program_size >= 4 && memcmp(head, "~PSP", 4) == 0) {
        out->kind = NK_PBP_HB_ENCRYPTED;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program is encrypted");
    }
    if (program_size < sizeof(head) || memcmp(head, "\x7f" "ELF", 4) != 0 || head[4] != 1 ||
        head[5] != 1 || hb_le16(head + 18) != 8u ||
        (hb_le16(head + 16) != 2u && hb_le16(head + 16) != 0xFFA0u)) {
        out->kind = NK_PBP_HB_NOT_A_PROGRAM;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program is not a PSP program");
    }
    switch (hb_module_is_kernel_mode(f, program_offset, program_size, &kernel)) {
    case 0:
        break;
    case -2:
        out->kind = NK_PBP_HB_NOT_A_PROGRAM;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program has malformed module information");
    default:
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "the package's program could not be read");
    }
    if (kernel) {
        out->kind = NK_PBP_HB_KERNEL_MODE;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program is kernel-mode homebrew");
    }
    {
        uint32_t icon = pkg.offsets[2] - pkg.offsets[1];
        if (icon >= sizeof(png) && icon <= HB_MAX_ICON_BYTES &&
            hb_read_at(f, pkg.offsets[1], png, sizeof(png)) == 0 &&
            memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0) {
            out->icon_size = icon;
        }
    }
    hb_build_sfo(info->title, sfo);
    rc = hb_key(ctx, f, program_offset, program_size, sfo, out->key);
    fclose(f);
    out->kind = NK_PBP_HB_PROGRAM;
    return rc;
}

/* ECMA-119 9.1 directory record, both-endian extent and size. */
static size_t hb_dir_record(uint8_t *record, const uint8_t *name, size_t name_len, uint32_t lba,
                            uint32_t size, bool is_dir)
{
    size_t length = 33 + name_len + ((name_len & 1u) == 0 ? 1u : 0u);
    memset(record, 0, length);
    record[0] = (uint8_t)length;
    for (int i = 0; i < 4; i++) {
        record[2 + i] = (uint8_t)(lba >> (8 * i));
        record[9 - i] = (uint8_t)(lba >> (8 * i));
        record[10 + i] = (uint8_t)(size >> (8 * i));
        record[17 - i] = (uint8_t)(size >> (8 * i));
    }
    record[25] = is_dir ? 0x02 : 0x00;
    record[28] = 1;
    record[31] = 1;
    record[32] = (uint8_t)name_len;
    memcpy(record + 33, name, name_len);
    return length;
}

static size_t hb_dir_start(uint8_t *sector, uint32_t self, uint32_t parent)
{
    static const uint8_t dot[1] = { 0 };
    static const uint8_t dotdot[1] = { 1 };
    size_t used = hb_dir_record(sector, dot, 1, self, HB_SECTOR, true);
    return used + hb_dir_record(sector + used, dotdot, 1, parent, HB_SECTOR, true);
}

static uint32_t hb_sectors(uint32_t bytes)
{
    return (bytes + HB_SECTOR - 1u) / HB_SECTOR;
}

/* The image's first HB_PROGRAM_LBA sectors. */
static void hb_build_head(uint8_t *head, const uint8_t sfo[HB_SFO_SIZE], uint32_t program_size,
                          uint32_t icon_size, uint32_t total_sectors)
{
    uint32_t icon_lba = HB_PROGRAM_LBA + hb_sectors(program_size);
    uint8_t *pvd = head + 16u * HB_SECTOR;
    uint8_t *root = head + 18u * HB_SECTOR;
    uint8_t *game = head + 19u * HB_SECTOR;
    uint8_t *sysdir = head + 20u * HB_SECTOR;
    size_t used;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memset(pvd + 8, ' ', 64); /* system and volume identifiers */
    memcpy(pvd + 40, "NAKAGAWA_HOMEBREW", 17);
    hb_put_le32(pvd + 80, total_sectors);
    for (int i = 0; i < 4; i++) pvd[87 - i] = (uint8_t)(total_sectors >> (8 * i));
    hb_put_le16(pvd + 120, 1u);
    pvd[123] = 1;
    hb_put_le16(pvd + 124, 1u);
    pvd[127] = 1;
    hb_put_le16(pvd + 128, HB_SECTOR);
    pvd[131] = (uint8_t)(HB_SECTOR & 0xFFu);
    pvd[130] = (uint8_t)(HB_SECTOR >> 8);
    {
        static const uint8_t dot[1] = { 0 };
        hb_dir_record(pvd + 156, dot, 1, 18u, HB_SECTOR, true);
    }
    pvd[881] = 1;
    {
        uint8_t *terminator = head + 17u * HB_SECTOR;
        terminator[0] = 255;
        memcpy(terminator + 1, "CD001", 5);
        terminator[6] = 1;
    }
    used = hb_dir_start(root, 18u, 18u);
    hb_dir_record(root + used, (const uint8_t *)"PSP_GAME", 8, 19u, HB_SECTOR, true);
    used = hb_dir_start(game, 19u, 18u);
    if (icon_size > 0) {
        used += hb_dir_record(game + used, (const uint8_t *)"ICON0.PNG", 9, icon_lba, icon_size,
                              false);
    }
    used += hb_dir_record(game + used, (const uint8_t *)"PARAM.SFO", 9, 21u, HB_SFO_SIZE, false);
    hb_dir_record(game + used, (const uint8_t *)"SYSDIR", 6, 20u, HB_SECTOR, true);
    used = hb_dir_start(sysdir, 20u, 19u);
    hb_dir_record(sysdir + used, (const uint8_t *)"EBOOT.BIN", 9, HB_PROGRAM_LBA, program_size,
                  false);
    memcpy(head + 21u * HB_SECTOR, sfo, HB_SFO_SIZE);
}

static int hb_write_padded(FILE *out, size_t written_in_sector)
{
    static const uint8_t zeros[HB_SECTOR] = { 0 };
    size_t pad = (HB_SECTOR - written_in_sector % HB_SECTOR) % HB_SECTOR;
    return pad == 0 || fwrite(zeros, 1, pad, out) == pad ? 0 : -1;
}

/* Copy count bytes of the package to out, padded to a sector. A package that changed or was
   truncated since it was inspected fails here or at the read-back key check, never half-kept. */
static int hb_copy_section(NkPspCtx *ctx, FILE *in, uint64_t offset, uint32_t count, FILE *out,
                           const NkNpumdimgCallbacks *callbacks, uint64_t *done, uint64_t total)
{
    uint8_t *buffer = (uint8_t *)malloc(HB_CHUNK);
    uint32_t copied = 0;
    if (buffer == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
    while (copied < count) {
        uint32_t step = count - copied < HB_CHUNK ? count - copied : HB_CHUNK;
        if (callbacks != NULL && callbacks->is_cancelled != NULL &&
            callbacks->is_cancelled(callbacks->user)) {
            free(buffer);
            return nk_psp_fail(ctx, NK_PSP_ERR_CANCELLED, NULL,
                               "the conversion was cancelled; the partial disc image was "
                               "removed");
        }
        if (hb_read_at(in, offset + copied, buffer, step) != 0) {
            free(buffer);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the package's program could not be read");
        }
        if (fwrite(buffer, 1, step, out) != step) {
            free(buffer);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the disc image could not be written (is the disk full?)");
        }
        copied += step;
        *done += step;
        if (callbacks != NULL && callbacks->on_progress != NULL)
            callbacks->on_progress(callbacks->user, *done, total);
    }
    free(buffer);
    if (hb_write_padded(out, count) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "the disc image could not be written (is the disk full?)");
    }
    return NK_PSP_OK;
}

int nk_pbp_homebrew_convert_file(NkPspCtx *ctx, const char *pbp_path, const NkPbpInfo *info,
                                 const char *iso_path, const NkNpumdimgCallbacks *callbacks,
                                 NkPbpHomebrew *out)
{
    NkPbpHomebrew local;
    NkPbpHomebrew *hb = out != NULL ? out : &local;
    HbPackage pkg;
    FILE *in = NULL;
    FILE *img = NULL;
    uint8_t *head = NULL;
    uint8_t sfo[HB_SFO_SIZE];
    char *temp = NULL;
    size_t temp_len;
    uint32_t total_sectors;
    uint64_t done = 0, total;
    NkIsoMetadata *meta = NULL;
    int rc;

    if (ctx == NULL || pbp_path == NULL || info == NULL || iso_path == NULL)
        return NK_PSP_ERR_INTERNAL;
    rc = nk_pbp_homebrew_inspect_file(ctx, pbp_path, info, hb);
    if (rc != NK_PSP_OK) return rc;
    in = nk_fopen_utf8(pbp_path, "rb");
    if (in == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the file could not be opened");
    rc = hb_read_package(ctx, in, &pkg);
    if (rc != NK_PSP_OK) {
        fclose(in);
        return rc;
    }
    hb_build_sfo(info->title, sfo);
    total_sectors = HB_PROGRAM_LBA + hb_sectors(hb->program_size) + hb_sectors(hb->icon_size);
    total = (uint64_t)hb->program_size + hb->icon_size;
    head = (uint8_t *)calloc(HB_PROGRAM_LBA, HB_SECTOR);
    temp_len = strlen(iso_path) + sizeof(".converting");
    temp = (char *)malloc(temp_len);
    meta = (NkIsoMetadata *)calloc(1, sizeof(*meta));
    if (head == NULL || temp == NULL || meta == NULL) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
        goto done;
    }
    snprintf(temp, temp_len, "%s.converting", iso_path);
    (void)nk_remove_utf8(temp);
    hb_build_head(head, sfo, hb->program_size, hb->icon_size, total_sectors);
    img = nk_fopen_utf8(temp, "wb");
    if (img == NULL) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the disc image could not be created");
        goto done;
    }
    if (fwrite(head, 1, (size_t)HB_PROGRAM_LBA * HB_SECTOR, img) !=
        (size_t)HB_PROGRAM_LBA * HB_SECTOR) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be written (is the disk full?)");
        goto done;
    }
    rc = hb_copy_section(ctx, in, pkg.offsets[6], hb->program_size, img, callbacks, &done, total);
    if (rc == NK_PSP_OK && hb->icon_size > 0) {
        rc = hb_copy_section(ctx, in, pkg.offsets[1], hb->icon_size, img, callbacks, &done,
                             total);
    }
    if (rc != NK_PSP_OK) goto done;
    if (!nk_platform_sync_file(img) || fclose(img) != 0) {
        img = NULL;
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be written (is the disk full?)");
        goto done;
    }
    img = NULL;
    /* The finished image must be the game the inspection named. */
    if (nk_iso_inspect(temp, meta) != NK_OK || !meta->non_retail ||
        strcmp(meta->disc_id, hb->key) != 0) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                         "the disc image written for this package was not read back as "
                         "the same game");
        goto done;
    }
    if (nk_rename_utf8(temp, iso_path) != 0) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be moved into place");
        goto done;
    }

done:
    if (img != NULL) fclose(img);
    if (rc != NK_PSP_OK && temp != NULL) (void)nk_remove_utf8(temp);
    free(meta);
    free(temp);
    free(head);
    fclose(in);
    return rc;
}

/* ---- The program's Memory Stick folder (see nk_pbp_homebrew.h) ---------------------------- */

#define HB_PATH_MAX 4096u
#define HB_COPY_CHUNK (64u * 1024u)

static void hb_problem(char *problem, size_t problem_size, const char *fmt, const char *arg)
{
    if (problem != NULL && problem_size > 0) snprintf(problem, problem_size, fmt, arg);
}

static bool hb_is_separator(char c)
{
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

static bool hb_join(char *out, size_t out_size, const char *left, const char *right)
{
    int written = snprintf(out, out_size, "%s%c%s", left, nk_platform_path_separator(), right);
    return written > 0 && (size_t)written < out_size;
}

/* A name a PSP's Memory Stick holds, kept to printable ASCII so the program, the
   launcher's boot argument and every host file system agree on its bytes. */
static bool hb_name_valid(const char *name, size_t length)
{
    if (length == 0u || length > NK_PBP_HOMEBREW_FOLDER_NAME_MAX) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20u || c > 0x7Eu || strchr("\\/:*?\"<>|", (int)c) != NULL) return false;
    }
    return name[length - 1u] != ' ' && name[length - 1u] != '.';
}

/* The absolute path of the folder pbp_path is in, and where its name starts in it. */
static bool hb_source_folder(const char *pbp_path, char *folder, size_t folder_size,
                             size_t *name_at)
{
    char absolute[HB_PATH_MAX];
    size_t end, start;
    if (pbp_path == NULL || !nk_platform_absolute_path(pbp_path, absolute, sizeof(absolute)))
        return false;
    end = strlen(absolute);
    while (end > 0u && !hb_is_separator(absolute[end - 1u])) --end;  /* drop the file name */
    while (end > 0u && hb_is_separator(absolute[end - 1u])) --end;   /* and its separator */
    start = end;
    while (start > 0u && !hb_is_separator(absolute[start - 1u])) --start;
    if (end == 0u || end >= folder_size) return false;
    memcpy(folder, absolute, end);
    folder[end] = '\0';
    *name_at = start;
    return true;
}

bool nk_pbp_homebrew_folder_name(const char *pbp_path, char *out, size_t out_size,
                                 char *problem, size_t problem_size)
{
    char folder[HB_PATH_MAX];
    size_t name_at = 0, length;
    if (out == NULL || out_size == 0u) return false;
    out[0] = '\0';
    if (!hb_source_folder(pbp_path, folder, sizeof(folder), &name_at)) {
        hb_problem(problem, problem_size, "%s",
                   "The package's folder could not be found. Put the EBOOT.PBP in its own "
                   "folder, as on a PSP's Memory Stick (PSP/GAME/<folder>/EBOOT.PBP).");
        return false;
    }
    length = strlen(folder + name_at);
    if (!hb_name_valid(folder + name_at, length) || length >= out_size) {
        hb_problem(problem, problem_size,
                   "The package's folder, \"%.80s\", has no name a PSP's Memory Stick can "
                   "hold. Rename it with up to 64 English letters, digits, spaces and . _ - "
                   "( ) characters, not ending in a space or a dot, or put the EBOOT.PBP in "
                   "its own folder.", folder + name_at);
        return false;
    }
    memcpy(out, folder + name_at, length + 1u);
    return true;
}

bool nk_pbp_homebrew_view_root(const char *iso_path, char *out, size_t out_size)
{
    size_t length;
    if (iso_path == NULL || out == NULL || out_size == 0u) return false;
    length = strlen(iso_path);
    if (length <= 4u || length >= out_size) return false;
    if (strcmp(iso_path + length - 4u, ".iso") != 0 && strcmp(iso_path + length - 4u, ".ISO") != 0)
        return false;
    memcpy(out, iso_path, length - 4u);
    memcpy(out + length - 4u, ".ms0", 5u);
    return true;
}

/* A directory listing, read whole before anything under it is touched. */
typedef struct {
    char **names;
    bool *folders;
    size_t count;
    size_t capacity;
    bool failed;
} HbList;

static bool hb_list_add(const char *name, bool is_dir, void *ctx)
{
    HbList *list = (HbList *)ctx;
    size_t size = strlen(name) + 1u;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2u : 16u;
        char **names = (char **)realloc(list->names, capacity * sizeof(*names));
        bool *folders;
        if (names == NULL) {
            list->failed = true;
            return false;
        }
        list->names = names;
        folders = (bool *)realloc(list->folders, capacity * sizeof(*folders));
        if (folders == NULL) {
            list->failed = true;
            return false;
        }
        list->folders = folders;
        list->capacity = capacity;
    }
    list->names[list->count] = (char *)malloc(size);
    if (list->names[list->count] == NULL) {
        list->failed = true;
        return false;
    }
    memcpy(list->names[list->count], name, size);
    list->folders[list->count] = is_dir;
    list->count++;
    return true;
}

static void hb_list_free(HbList *list)
{
    for (size_t i = 0; i < list->count; ++i) free(list->names[i]);
    free(list->names);
    free(list->folders);
}

typedef struct {
    size_t entries;
    uint64_t bytes;
    bool keep_existing; /* a seed: what the target holds already stays as it is */
    const NkNpumdimgCallbacks *callbacks;
    char *problem;
    size_t problem_size;
} HbWalk;

/* Copy one file through "<to>.nkcopy", on the disk before it takes its name. */
static bool hb_copy_file(const char *from, const char *to, HbWalk *walk)
{
    char temp[HB_PATH_MAX];
    unsigned char *buffer;
    FILE *in, *out;
    bool ok = true;
    int written = snprintf(temp, sizeof(temp), "%s.nkcopy", to);
    if (written <= 0 || (size_t)written >= sizeof(temp)) {
        hb_problem(walk->problem, walk->problem_size, "%s", "A path in the folder is too long.");
        return false;
    }
    buffer = (unsigned char *)malloc(HB_COPY_CHUNK);
    in = nk_fopen_utf8(from, "rb");
    if (buffer == NULL || in == NULL) {
        free(buffer);
        if (in != NULL) fclose(in);
        hb_problem(walk->problem, walk->problem_size, "%.400s could not be read.", from);
        return false;
    }
    out = nk_fopen_utf8(temp, "wb");
    if (out == NULL) {
        free(buffer);
        fclose(in);
        hb_problem(walk->problem, walk->problem_size, "%.400s could not be written.", to);
        return false;
    }
    for (;;) {
        size_t got = fread(buffer, 1, HB_COPY_CHUNK, in);
        if (got == 0) break;
        if (fwrite(buffer, 1, got, out) != got) {
            hb_problem(walk->problem, walk->problem_size,
                       "%.400s could not be written (is the disk full?).", to);
            ok = false;
            break;
        }
    }
    if (ok && ferror(in)) {
        hb_problem(walk->problem, walk->problem_size, "%.400s could not be read.", from);
        ok = false;
    }
    if (ok && !nk_platform_sync_file(out)) {
        hb_problem(walk->problem, walk->problem_size,
                   "%.400s could not be written to the disk.", to);
        ok = false;
    }
    free(buffer);
    fclose(in);
    if (fclose(out) != 0 && ok) {
        hb_problem(walk->problem, walk->problem_size, "%.400s could not be written.", to);
        ok = false;
    }
    if (ok && nk_rename_utf8(temp, to) != 0) {
        hb_problem(walk->problem, walk->problem_size, "%.400s could not be put in place.", to);
        ok = false;
    }
    if (!ok) (void)nk_remove_utf8(temp);
    return ok;
}

/* Walk from within the folder bounds; with to, copy each entry there as well. */
static bool hb_walk(const char *from, const char *to, unsigned depth, HbWalk *walk)
{
    HbList list;
    size_t skipped = 0;
    bool ok = true;
    memset(&list, 0, sizeof(list));
    if (depth > NK_PBP_HOMEBREW_FOLDER_MAX_DEPTH) {
        hb_problem(walk->problem, walk->problem_size, "%s",
                   "The package's folder has more than 16 levels of folders inside it.");
        return false;
    }
    if (!nk_platform_list_dir_counted(from, hb_list_add, &list, &skipped) || list.failed) {
        hb_problem(walk->problem, walk->problem_size, "%.400s could not be read.", from);
        ok = false;
    } else if (skipped > 0u) {
        hb_problem(walk->problem, walk->problem_size,
                   "%.400s holds a link, a junction or a file kept in the cloud and not on "
                   "this computer, which the import would leave out.", from);
        ok = false;
    }
    for (size_t i = 0; ok && i < list.count; ++i) {
        char source[HB_PATH_MAX];
        char target[HB_PATH_MAX];
        if (!hb_join(source, sizeof(source), from, list.names[i]) ||
            (to != NULL && !hb_join(target, sizeof(target), to, list.names[i]))) {
            hb_problem(walk->problem, walk->problem_size, "%s",
                       "A path in the package's folder is too long.");
            ok = false;
        } else if (++walk->entries > NK_PBP_HOMEBREW_FOLDER_MAX_ENTRIES) {
            hb_problem(walk->problem, walk->problem_size, "%s",
                       "The package's folder holds more than 4096 files and folders. Put "
                       "the EBOOT.PBP in its own folder with only the files it uses.");
            ok = false;
        } else if (list.folders[i]) {
            if (to == NULL) {
                ok = hb_walk(source, NULL, depth + 1u, walk);
            } else if (walk->keep_existing && nk_platform_file_exists(target)) {
                /* the program put a file where the folder has a folder: the file stays */
            } else if (!nk_platform_mkdir_p_private(target)) {
                hb_problem(walk->problem, walk->problem_size, "%.400s could not be made.",
                           target);
                ok = false;
            } else {
                ok = hb_walk(source, target, depth + 1u, walk);
            }
        } else {
            int64_t size = nk_platform_get_file_size(source);
            if (size < 0) {
                hb_problem(walk->problem, walk->problem_size, "%.400s could not be read.",
                           source);
                ok = false;
            } else if ((walk->bytes += (uint64_t)size) > NK_PBP_HOMEBREW_FOLDER_MAX_BYTES) {
                hb_problem(walk->problem, walk->problem_size, "%s",
                           "The package's folder holds more than 1 GiB. Put the EBOOT.PBP in "
                           "its own folder with only the files it uses.");
                ok = false;
            } else if (to == NULL ||
                       (walk->keep_existing && !nk_platform_path_missing(target))) {
                /* counted only, or already on the Memory Stick */
            } else if (walk->callbacks != NULL && walk->callbacks->is_cancelled != NULL &&
                       walk->callbacks->is_cancelled(walk->callbacks->user)) {
                hb_problem(walk->problem, walk->problem_size, "%s", "The import was cancelled.");
                ok = false;
            } else {
                ok = hb_copy_file(source, target, walk);
            }
        }
    }
    if (ok && to != NULL && !nk_platform_sync_dir(to)) {
        hb_problem(walk->problem, walk->problem_size,
                   "%.400s could not be written to the disk.", to);
        ok = false;
    }
    hb_list_free(&list);
    return ok;
}

/* The absolute form of a path whose last parts may not exist yet. */
static bool hb_absolute_maybe_missing(const char *path, char *out, size_t out_size)
{
    char head[HB_PATH_MAX];
    size_t length = strlen(path);
    size_t cut = length;
    if (length >= sizeof(head)) return false;
    memcpy(head, path, length + 1u);
    while (!nk_platform_absolute_path(head, out, out_size)) {
        while (cut > 0u && !hb_is_separator(head[cut - 1u])) --cut;
        while (cut > 0u && hb_is_separator(head[cut - 1u])) --cut;
        if (cut == 0u) return false;
        head[cut] = '\0';
    }
    if (cut < length) {
        size_t used = strlen(out);
        if (used + (length - cut) >= out_size) return false;
        memcpy(out + used, path + cut, length - cut + 1u);
    }
    return true;
}

/* Whether path is folder or lies inside it. */
static bool hb_path_within(const char *path, const char *folder)
{
    size_t length = strlen(folder);
    for (size_t i = 0; i < length; ++i) {
        char a = path[i], b = folder[i];
        if (a == '\0') return false;
#ifdef _WIN32
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
#endif
        if (a != b) return false;
    }
    return path[length] == '\0' || hb_is_separator(path[length]);
}

/* Whether folder is the user's home folder, Desktop, or one of the personal folders in the
   home folder: a package picked from one of those does not bring the folder along, since it
   holds the user's own files, not the program's. */
static bool hb_is_personal_folder(const char *folder)
{
    static const char *const personal[] = {
        "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", "OneDrive"
    };
    char known[HB_PATH_MAX];
    char resolved[HB_PATH_MAX];
    const char *home = nk_platform_env_home_dir();
    if (nk_platform_get_desktop_dir(known, sizeof(known)) &&
        nk_platform_absolute_path(known, resolved, sizeof(resolved)) &&
        hb_path_within(folder, resolved) && hb_path_within(resolved, folder))
        return true;
    if (home == NULL || home[0] == '\0' ||
        !nk_platform_absolute_path(home, resolved, sizeof(resolved)))
        return false;
    if (hb_path_within(folder, resolved) && hb_path_within(resolved, folder)) return true;
    for (size_t i = 0; i < sizeof(personal) / sizeof(personal[0]); ++i) {
        if (hb_join(known, sizeof(known), resolved, personal[i]) &&
            hb_path_within(folder, known) && hb_path_within(known, folder))
            return true;
    }
    return false;
}

bool nk_pbp_homebrew_check_folder(const char *pbp_path, const char *view_root,
                                  char *problem, size_t problem_size)
{
    char name[NK_PBP_HOMEBREW_FOLDER_NAME_MAX + 1u];
    char folder[HB_PATH_MAX];
    char view[HB_PATH_MAX];
    size_t name_at = 0;
    HbWalk walk;
    if (!nk_pbp_homebrew_folder_name(pbp_path, name, sizeof(name), problem, problem_size))
        return false;
    if (!hb_source_folder(pbp_path, folder, sizeof(folder), &name_at) || view_root == NULL ||
        !hb_absolute_maybe_missing(view_root, view, sizeof(view))) {
        hb_problem(problem, problem_size, "%s", "The package's folder could not be found.");
        return false;
    }
    if (hb_path_within(view, folder)) {
        hb_problem(problem, problem_size,
                   "The folder \"%.80s\" also holds Nakagawa's data folder. Put the EBOOT.PBP "
                   "in its own folder, as on a PSP's Memory Stick (PSP/GAME/<folder>/).",
                   folder + name_at);
        return false;
    }
    if (hb_is_personal_folder(folder)) {
        hb_problem(problem, problem_size,
                   "The EBOOT.PBP is in your \"%.80s\" folder, which holds your own files, and "
                   "the program's folder is copied with it. Put the EBOOT.PBP in its own folder, "
                   "as on a PSP's Memory Stick (PSP/GAME/<folder>/).", folder + name_at);
        return false;
    }
    memset(&walk, 0, sizeof(walk));
    walk.problem = problem;
    walk.problem_size = problem_size;
    return hb_walk(folder, NULL, 0u, &walk);
}

/* <root>/PSP/GAME. */
static bool hb_game_dir(const char *root, char *out, size_t out_size)
{
    char psp[HB_PATH_MAX];
    return hb_join(psp, sizeof(psp), root, "PSP") && hb_join(out, out_size, psp, "GAME");
}

bool nk_pbp_homebrew_write_view(const char *pbp_path, const char *view_root,
                                const NkNpumdimgCallbacks *callbacks,
                                char *problem, size_t problem_size)
{
    char name[NK_PBP_HOMEBREW_FOLDER_NAME_MAX + 1u];
    char folder[HB_PATH_MAX];
    char staging[HB_PATH_MAX];
    char game[HB_PATH_MAX];
    char target[HB_PATH_MAX];
    size_t name_at = 0;
    HbWalk walk;
    int written;
    if (!nk_pbp_homebrew_check_folder(pbp_path, view_root, problem, problem_size) ||
        !nk_pbp_homebrew_folder_name(pbp_path, name, sizeof(name), problem, problem_size) ||
        !hb_source_folder(pbp_path, folder, sizeof(folder), &name_at))
        return false;
    written = snprintf(staging, sizeof(staging), "%s.importing", view_root);
    if (written <= 0 || (size_t)written >= sizeof(staging) ||
        !hb_game_dir(staging, game, sizeof(game)) || !hb_join(target, sizeof(target), game, name)) {
        hb_problem(problem, problem_size, "%s", "The path to Nakagawa's data folder is too long.");
        return false;
    }
    if (!nk_platform_remove_tree(staging, NULL) || !nk_platform_mkdir_p_private(target)) {
        hb_problem(problem, problem_size, "%.400s could not be made.", target);
        return false;
    }
    memset(&walk, 0, sizeof(walk));
    walk.callbacks = callbacks;
    walk.problem = problem;
    walk.problem_size = problem_size;
    if (!hb_walk(folder, target, 0u, &walk)) {
        (void)nk_platform_remove_tree(staging, NULL);
        return false;
    }
    if (!nk_platform_remove_tree(view_root, NULL) || nk_rename_utf8(staging, view_root) != 0) {
        (void)nk_platform_remove_tree(staging, NULL);
        hb_problem(problem, problem_size, "%.400s could not be put in place.", view_root);
        return false;
    }
    return true;
}

typedef struct {
    char name[NK_PBP_HOMEBREW_FOLDER_NAME_MAX + 1u];
    size_t folders;
    size_t files;
} HbViewScan;

static bool hb_view_scan(const char *name, bool is_dir, void *ctx)
{
    HbViewScan *scan = (HbViewScan *)ctx;
    if (!is_dir) {
        scan->files++;
    } else if (++scan->folders == 1u) {
        size_t length = strlen(name);
        if (hb_name_valid(name, length)) memcpy(scan->name, name, length + 1u);
    }
    return true;
}

NkPbpHomebrewView nk_pbp_homebrew_find_view(const char *iso_path, char *folder,
                                            size_t folder_size)
{
    char root[HB_PATH_MAX];
    char game[HB_PATH_MAX];
    char program_dir[HB_PATH_MAX];
    char program[HB_PATH_MAX];
    HbViewScan scan;
    if (folder != NULL && folder_size > 0u) folder[0] = '\0';
    if (!nk_pbp_homebrew_view_root(iso_path, root, sizeof(root)) ||
        nk_platform_path_missing(root))
        return NK_PBP_HB_VIEW_NONE;
    memset(&scan, 0, sizeof(scan));
    if (folder == NULL || !hb_game_dir(root, game, sizeof(game)) ||
        !nk_platform_list_dir(game, hb_view_scan, &scan) || scan.folders != 1u ||
        scan.files != 0u || scan.name[0] == '\0' || strlen(scan.name) >= folder_size ||
        !hb_join(program_dir, sizeof(program_dir), game, scan.name) ||
        !hb_join(program, sizeof(program), program_dir, "EBOOT.PBP") ||
        !nk_platform_file_exists(program))
        return NK_PBP_HB_VIEW_BROKEN;
    memcpy(folder, scan.name, strlen(scan.name) + 1u);
    return NK_PBP_HB_VIEW_FOUND;
}

bool nk_pbp_homebrew_seed_memstick(const char *iso_path, const char *folder,
                                   const char *memstick_root,
                                   char *problem, size_t problem_size)
{
    char root[HB_PATH_MAX];
    char game[HB_PATH_MAX];
    char source[HB_PATH_MAX];
    char stick_game[HB_PATH_MAX];
    char target[HB_PATH_MAX];
    HbWalk walk;
    if (folder == NULL || memstick_root == NULL ||
        !nk_pbp_homebrew_view_root(iso_path, root, sizeof(root)) ||
        !hb_game_dir(root, game, sizeof(game)) || !hb_join(source, sizeof(source), game, folder) ||
        !hb_game_dir(memstick_root, stick_game, sizeof(stick_game)) ||
        !hb_join(target, sizeof(target), stick_game, folder)) {
        hb_problem(problem, problem_size, "%s",
                   "The program's Memory Stick folder has a path too long to use.");
        return false;
    }
    if (nk_platform_file_exists(target) || !nk_platform_mkdir_p_private(target)) {
        hb_problem(problem, problem_size, "%.400s could not be made on the Memory Stick.",
                   target);
        return false;
    }
    memset(&walk, 0, sizeof(walk));
    walk.keep_existing = true;
    walk.problem = problem;
    walk.problem_size = problem_size;
    return hb_walk(source, target, 0u, &walk);
}
