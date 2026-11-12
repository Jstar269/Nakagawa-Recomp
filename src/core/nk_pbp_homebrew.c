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
                  const uint8_t sfo[HB_SFO_SIZE], char key[10], char identity[65])
{
    static const char hex[] = "0123456789abcdef";
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
    for (size_t i = 0; i < sizeof(digest); ++i) {
        identity[i * 2u] = hex[digest[i] >> 4];
        identity[i * 2u + 1u] = hex[digest[i] & 15u];
    }
    identity[64] = '\0';
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
    rc = hb_key(ctx, f, program_offset, program_size, sfo, out->key, out->identity_sha256);
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
        strcmp(meta->disc_id, hb->key) != 0 ||
        strcmp(meta->identity_sha256, hb->identity_sha256) != 0) {
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
#define HB_MANIFEST_NAME "view.manifest"
/* The path the picked folder really had, one line (nk_pbp_homebrew_other_source). */
#define HB_MANIFEST_HEADER "nakagawa-homebrew-view 1"
#define HB_SEEDED_HEADER "nakagawa-homebrew-seeded 2"
#define HB_TEXT_MAX (8u * 1024u * 1024u)
#define HB_HEX_LEN 64u
#define HB_TEMP_TRIES 10

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

/* Whether a name is one a PSP's Memory Stick (FAT) can hold: no control character,
   none of \ / : * ? " < > |, not "." or "..", and not ending in a space or a dot. */
static bool hb_entry_name_ok(const char *name, size_t length)
{
    if (length == 0u || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20u || c == 0x7Fu || strchr("\\/:*?\"<>|", (int)c) != NULL) return false;
    }
    return name[length - 1u] != ' ' && name[length - 1u] != '.';
}

/* The program folder's own name is also kept to printable ASCII, so the program,
   the launcher's boot argument and every host file system agree on its bytes. */
static bool hb_name_valid(const char *name, size_t length)
{
    if (length > NK_PBP_HOMEBREW_FOLDER_NAME_MAX || !hb_entry_name_ok(name, length)) return false;
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)name[i] > 0x7Eu) return false;
    }
    return true;
}

static bool hb_is_absolute(const char *path)
{
#ifdef _WIN32
    return (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && hb_is_separator(path[2])) ||
           (hb_is_separator(path[0]) && hb_is_separator(path[1]));
#else
    return path[0] == '/';
#endif
}

/* Whether path is folder or lies inside it. */
static bool hb_path_within(const char *path, const char *folder)
{
    size_t length = strlen(folder);
    while (length > 0u && hb_is_separator(folder[length - 1u])) --length; /* "C:\" or "/" */
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

/* The package's folder: its absolute path as picked (whose last part is the name the
   program's folder gets), and the path it really has, every link resolved, which is the
   folder that is checked and copied. */
typedef struct {
    char picked[HB_PATH_MAX];
    size_t name_at;
    char real[HB_PATH_MAX];
} HbSource;

static bool hb_source(const char *pbp_path, HbSource *source)
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
    if (end == 0u || end >= sizeof(source->picked)) return false;
    memcpy(source->picked, absolute, end);
    source->picked[end] = '\0';
    source->name_at = start;
    return nk_platform_real_path(source->picked, source->real, sizeof(source->real));
}

bool nk_pbp_homebrew_folder_name(const char *pbp_path, char *out, size_t out_size,
                                 char *problem, size_t problem_size)
{
    HbSource source;
    size_t length;
    if (out == NULL || out_size == 0u) return false;
    out[0] = '\0';
    if (!hb_source(pbp_path, &source)) {
        hb_problem(problem, problem_size, "%s",
                   "The package's folder could not be found, or where it really is could "
                   "not be read from the system. Put the EBOOT.PBP in its own folder on a "
                   "local drive, as on a PSP's Memory Stick (PSP/GAME/<folder>/EBOOT.PBP).");
        return false;
    }
    length = strlen(source.picked + source.name_at);
    if (!hb_name_valid(source.picked + source.name_at, length) || length >= out_size) {
        hb_problem(problem, problem_size,
                   "The package's folder, \"%.80s\", has no name a PSP's Memory Stick can "
                   "hold. Rename it with up to 64 English letters, digits, spaces and "
                   "punctuation other than \\ / : * ? \" < > |, not ending in a space or a "
                   "dot, or put the EBOOT.PBP in its own folder.",
                   source.picked + source.name_at);
        return false;
    }
    memcpy(out, source.picked + source.name_at, length + 1u);
    return true;
}

bool nk_pbp_homebrew_program_named(const char *pbp_path, char *problem, size_t problem_size)
{
    char real[HB_PATH_MAX];
    const char *name;
    if (pbp_path == NULL || !nk_platform_real_path(pbp_path, real, sizeof(real))) {
        hb_problem(problem, problem_size, "%s",
                   "The package could not be found, or where it really is could not be read "
                   "from the system.");
        return false;
    }
    name = real + strlen(real);
    while (name > real && !hb_is_separator(name[-1])) --name;
    if (strcmp(name, "EBOOT.PBP") != 0) {
        hb_problem(problem, problem_size,
                   "A PSP starts a program only from a file named EBOOT.PBP, in capitals, and "
                   "this package is named \"%.80s\". Rename it to EBOOT.PBP and add it again.",
                   name);
        return false;
    }
    return true;
}

/* iso_path with its ".iso" (or ".ISO") replaced by suffix. */
static bool hb_beside_image(const char *iso_path, const char *suffix, char *out, size_t out_size)
{
    size_t length, suffix_length;
    if (iso_path == NULL || out == NULL || out_size == 0u) return false;
    length = strlen(iso_path);
    suffix_length = strlen(suffix);
    if (length <= 4u || length - 4u + suffix_length >= out_size) return false;
    if (strcmp(iso_path + length - 4u, ".iso") != 0 && strcmp(iso_path + length - 4u, ".ISO") != 0)
        return false;
    memcpy(out, iso_path, length - 4u);
    memcpy(out + length - 4u, suffix, suffix_length + 1u);
    return true;
}

bool nk_pbp_homebrew_view_root(const char *iso_path, char *out, size_t out_size)
{
    return hb_beside_image(iso_path, ".ms0", out, out_size);
}

/* A growing text: the manifest and the seed record are built in one. */
typedef struct {
    char *text;
    size_t length;
    size_t capacity;
    bool failed;
} HbText;

static void hb_text_add(HbText *text, const char *a, const char *b, const char *c)
{
    const char *parts[3] = { a, b, c };
    for (int i = 0; i < 3 && !text->failed; ++i) {
        size_t add = parts[i] != NULL ? strlen(parts[i]) : 0u;
        if (text->length + add + 1u > text->capacity) {
            size_t capacity = text->capacity ? text->capacity : 4096u;
            char *grown;
            while (capacity < text->length + add + 1u) capacity *= 2u;
            if (capacity > HB_TEXT_MAX || (grown = (char *)realloc(text->text, capacity)) == NULL) {
                text->failed = true;
                return;
            }
            text->text = grown;
            text->capacity = capacity;
        }
        if (add > 0u) memcpy(text->text + text->length, parts[i], add);
        text->length += add;
        text->text[text->length] = '\0';
    }
}

/* Read a small text file whole, refusing a link (NULL on any failure). */
static char *hb_read_text(const char *path)
{
    FILE *in = nk_platform_open_regular_file(path);
    int64_t size = nk_platform_get_file_size(path);
    char *text;
    if (in == NULL) return NULL;
    if (size < 0 || (uint64_t)size >= HB_TEXT_MAX ||
        (text = (char *)malloc((size_t)size + 1u)) == NULL) {
        fclose(in);
        return NULL;
    }
    if (fread(text, 1, (size_t)size, in) != (size_t)size || fgetc(in) != EOF) {
        fclose(in);
        free(text);
        return NULL;
    }
    fclose(in);
    text[size] = '\0';
    if (strlen(text) != (size_t)size) {
        free(text);
        return NULL;
    }
    return text;
}

/* Write a text file through "<path>.tmp", on the disk before it takes its name. */
static bool hb_write_text(const char *path, const HbText *text)
{
    char temp[HB_PATH_MAX];
    FILE *out;
    bool ok;
    int written = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (text->failed || written <= 0 || (size_t)written >= sizeof(temp)) return false;
    out = nk_platform_fopen_private(temp, "wb");
    if (out == NULL) return false;
    ok = fwrite(text->text, 1, text->length, out) == text->length && nk_platform_sync_file(out);
    ok = fclose(out) == 0 && ok;
    ok = ok && nk_rename_utf8(temp, path) == 0;
    if (!ok) (void)nk_remove_utf8(temp);
    return ok;
}

static void hb_hex(const uint8_t digest[32], char hex[HB_HEX_LEN + 1u])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15u];
    }
    hex[HB_HEX_LEN] = '\0';
}

typedef enum { HB_MISSING, HB_FILE, HB_FOLDER, HB_UNKNOWN } HbState;

/* What is at path; HB_UNKNOWN when the system cannot say (access denied, say). */
static HbState hb_state(const char *path)
{
    if (nk_platform_file_exists(path)) return HB_FILE;
    if (nk_platform_dir_exists(path)) return HB_FOLDER;
    if (nk_platform_path_missing(path)) return HB_MISSING;
    return HB_UNKNOWN;
}

/* Read the regular file from: exactly size bytes (a file that grew or shrank since it
   was counted changed while the import ran), hashed, and written to out when out is not
   NULL. */
static bool hb_read_file(const char *from, uint64_t size, FILE *out, uint8_t digest[32],
                         char *problem, size_t problem_size)
{
    unsigned char *buffer = (unsigned char *)malloc(HB_COPY_CHUNK);
    FILE *in = nk_platform_open_regular_file(from);
    NkSha256 sha;
    uint64_t left = size;
    bool ok = buffer != NULL && in != NULL;
    if (!ok) {
        hb_problem(problem, problem_size,
                   "%.400s could not be read as a plain file.", from);
    }
    nk_sha256_init(&sha);
    while (ok && left > 0u) {
        size_t want = left < HB_COPY_CHUNK ? (size_t)left : HB_COPY_CHUNK;
        size_t got = fread(buffer, 1, want, in);
        if (got != want) {
            hb_problem(problem, problem_size,
                       ferror(in) ? "%.400s could not be read."
                                  : "%.400s changed while it was copied.", from);
            ok = false;
        } else if (out != NULL && fwrite(buffer, 1, got, out) != got) {
            hb_problem(problem, problem_size, "%s",
                       "A file could not be written (is the disk full?).");
            ok = false;
        } else {
            nk_sha256_update(&sha, buffer, got);
            left -= got;
        }
    }
    if (ok && fgetc(in) != EOF) {
        hb_problem(problem, problem_size, "%.400s changed while it was copied.", from);
        ok = false;
    }
    nk_sha256_finish(&sha, digest);
    free(buffer);
    if (in != NULL) fclose(in);
    return ok;
}

/* Copy one file of size bytes to to, through a temporary name beside it that nothing
   holds yet, on the disk before it takes its name (replacing what to held). With expect
   (a manifest digest), bytes that differ from it are not put in place, and *mismatch is
   set. */
static bool hb_copy_file(const char *from, const char *to, uint64_t size, const char *expect,
                         char hex[HB_HEX_LEN + 1u], bool *mismatch, char *problem,
                         size_t problem_size)
{
    char temp[HB_PATH_MAX];
    uint8_t digest[32];
    FILE *out = NULL;
    bool ok;
    for (int attempt = 0; attempt < HB_TEMP_TRIES && out == NULL; ++attempt) {
        int written = attempt == 0 ? snprintf(temp, sizeof(temp), "%s.nkcopy", to)
                                   : snprintf(temp, sizeof(temp), "%s.nkcopy%d", to, attempt);
        if (written <= 0 || (size_t)written >= sizeof(temp)) break;
        if (hb_state(temp) == HB_MISSING) out = nk_platform_fopen_private(temp, "wb");
    }
    if (out == NULL) {
        hb_problem(problem, problem_size, "%.400s could not be written.", to);
        return false;
    }
    ok = hb_read_file(from, size, out, digest, problem, problem_size);
    if (ok && !nk_platform_sync_file(out)) {
        hb_problem(problem, problem_size, "%.400s could not be written to the disk.", to);
        ok = false;
    }
    if (fclose(out) != 0 && ok) {
        hb_problem(problem, problem_size, "%.400s could not be written.", to);
        ok = false;
    }
    hb_hex(digest, hex);
    if (ok && expect != NULL && strcmp(hex, expect) != 0) {
        if (mismatch != NULL) *mismatch = true;
        hb_problem(problem, problem_size,
                   "%.400s is not the file the import copied: the program's folder beside "
                   "its disc image is damaged. Add the program's EBOOT.PBP again.", from);
        ok = false;
    }
    if (ok && nk_rename_utf8(temp, to) != 0) {
        hb_problem(problem, problem_size, "%.400s could not be put in place.", to);
        ok = false;
    }
    if (!ok) (void)nk_remove_utf8(temp);
    return ok;
}

/* A directory listing, read whole and sorted by name before anything under it is
   touched, so a walk of an unchanged folder always gives the same manifest. */
typedef struct {
    char *name;
    bool folder;
} HbListEntry;

typedef struct {
    HbListEntry *items;
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
        HbListEntry *items = (HbListEntry *)realloc(list->items, capacity * sizeof(*items));
        if (items == NULL) {
            list->failed = true;
            return false;
        }
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count].name = (char *)malloc(size);
    if (list->items[list->count].name == NULL) {
        list->failed = true;
        return false;
    }
    memcpy(list->items[list->count].name, name, size);
    list->items[list->count].folder = is_dir;
    list->count++;
    return true;
}

static int hb_list_order(const void *a, const void *b)
{
    return strcmp(((const HbListEntry *)a)->name, ((const HbListEntry *)b)->name);
}

static void hb_list_free(HbList *list)
{
    for (size_t i = 0; i < list->count; ++i) free(list->items[i].name);
    free(list->items);
}

typedef struct {
    size_t entries;
    uint64_t bytes;
    const char *root;    /* the real path of the folder walked: all of it lies within */
    HbText *manifest;    /* NULL, or each entry's line is added (and each file hashed) */
    const NkNpumdimgCallbacks *callbacks;
    char *problem;
    size_t problem_size;
} HbWalk;

/* Walk from (rel its path in the program's folder, "" at the top) within the folder
   bounds; with to, copy each entry there as well. */
static bool hb_walk(const char *from, const char *rel, const char *to, unsigned depth,
                    HbWalk *walk)
{
    HbList list;
    size_t skipped = 0;
    bool ok = true;
    char real[HB_PATH_MAX];
    memset(&list, 0, sizeof(list));
    if (depth > NK_PBP_HOMEBREW_FOLDER_MAX_DEPTH) {
        hb_problem(walk->problem, walk->problem_size, "%s",
                   "The package's folder has more than 16 levels of folders inside it.");
        return false;
    }
    /* A folder swapped for a link since it was listed would lead out of the folder. */
    if (!nk_platform_real_path(from, real, sizeof(real)) || !hb_path_within(real, walk->root)) {
        hb_problem(walk->problem, walk->problem_size, "%.400s changed while it was read.",
                   from);
        return false;
    }
    if (!nk_platform_list_dir_counted(from, hb_list_add, &list, &skipped) || list.failed) {
        hb_problem(walk->problem, walk->problem_size, "%.400s could not be read.", from);
        ok = false;
    } else if (skipped > 0u) {
        hb_problem(walk->problem, walk->problem_size,
                   "%.400s holds something other than plain files and folders (a link, a "
                   "junction, a file kept in the cloud and not on this computer, or an "
                   "entry that could not be read), which the import would leave out.", from);
        ok = false;
    } else if (list.count > 1u) {
        qsort(list.items, list.count, sizeof(list.items[0]), hb_list_order);
    }
    for (size_t i = 0; ok && i < list.count; ++i) {
        const char *name = list.items[i].name;
        char source[HB_PATH_MAX];
        char target[HB_PATH_MAX];
        char child[HB_PATH_MAX];
        int written = rel[0] ? snprintf(child, sizeof(child), "%s/%s", rel, name)
                             : snprintf(child, sizeof(child), "%s", name);
        if (written <= 0 || (size_t)written >= HB_PATH_MAX / 2u ||
            !hb_join(source, sizeof(source), from, name) ||
            (to != NULL && !hb_join(target, sizeof(target), to, name))) {
            hb_problem(walk->problem, walk->problem_size, "%s",
                       "A path in the package's folder is too long.");
            ok = false;
        } else if (!hb_entry_name_ok(name, strlen(name))) {
            hb_problem(walk->problem, walk->problem_size,
                       "%.400s has a name a PSP's Memory Stick cannot hold (a control "
                       "character, one of \\ / : * ? \" < > |, or a space or a dot at its "
                       "end).", source);
            ok = false;
        } else if (++walk->entries > NK_PBP_HOMEBREW_FOLDER_MAX_ENTRIES) {
            hb_problem(walk->problem, walk->problem_size, "%s",
                       "The package's folder holds more than 4096 files and folders. Put "
                       "the EBOOT.PBP in its own folder with only the files it uses.");
            ok = false;
        } else if (list.items[i].folder) {
            if (walk->manifest != NULL) hb_text_add(walk->manifest, "D ", child, "\n");
            if (to != NULL && !nk_platform_mkdir_p_private(target)) {
                hb_problem(walk->problem, walk->problem_size, "%.400s could not be made.",
                           target);
                ok = false;
            } else {
                ok = hb_walk(source, child, to != NULL ? target : NULL, depth + 1u, walk);
            }
        } else {
            int64_t size = nk_platform_get_file_size(source);
            char hex[HB_HEX_LEN + 1u] = "";
            char line[96];
            uint8_t digest[32];
            if (size < 0) {
                hb_problem(walk->problem, walk->problem_size, "%.400s could not be read.",
                           source);
                ok = false;
            } else if ((walk->bytes += (uint64_t)size) > NK_PBP_HOMEBREW_FOLDER_MAX_BYTES) {
                hb_problem(walk->problem, walk->problem_size, "%s",
                           "The package's folder holds more than 1 GiB. Put the EBOOT.PBP in "
                           "its own folder with only the files it uses.");
                ok = false;
            } else if (to != NULL && walk->callbacks != NULL &&
                       walk->callbacks->is_cancelled != NULL &&
                       walk->callbacks->is_cancelled(walk->callbacks->user)) {
                hb_problem(walk->problem, walk->problem_size, "%s", "The import was cancelled.");
                ok = false;
            } else if (to != NULL) {
                ok = hb_copy_file(source, target, (uint64_t)size, NULL, hex, NULL,
                                  walk->problem, walk->problem_size);
            } else if (walk->manifest != NULL) {
                ok = hb_read_file(source, (uint64_t)size, NULL, digest, walk->problem,
                                  walk->problem_size);
                hb_hex(digest, hex);
            }
            if (ok && walk->manifest != NULL) {
                snprintf(line, sizeof(line), "F %s %llu ", hex, (unsigned long long)size);
                hb_text_add(walk->manifest, line, child, "\n");
            }
        }
    }
    if (ok && walk->manifest != NULL && walk->manifest->failed) {
        hb_problem(walk->problem, walk->problem_size, "%s",
                   "The package's folder lists too much to keep track of.");
        ok = false;
    }
    if (ok && to != NULL && !nk_platform_sync_dir(to)) {
        hb_problem(walk->problem, walk->problem_size,
                   "%.400s could not be written to the disk.", to);
        ok = false;
    }
    hb_list_free(&list);
    return ok;
}

/* The real form of a path whose last parts may not exist yet. */
static bool hb_real_maybe_missing(const char *path, char *out, size_t out_size)
{
    char head[HB_PATH_MAX];
    size_t length = strlen(path);
    size_t cut = length;
    if (length >= sizeof(head)) return false;
    memcpy(head, path, length + 1u);
    while (!nk_platform_real_path(head, out, out_size)) {
        while (cut > 0u && !hb_is_separator(head[cut - 1u])) --cut;
        while (cut > 0u && hb_is_separator(head[cut - 1u])) --cut;
        if (cut == 0u || (cut == 2u && head[1] == ':')) return false;
        head[cut] = '\0';
    }
    if (cut < length) {
        size_t used = strlen(out);
        if (used + (length - cut) >= out_size) return false;
        memcpy(out + used, path + cut, length - cut + 1u);
    }
    return true;
}

/* Whether folder (a real path) is, or holds, the folder at other. */
static bool hb_holds_folder(const char *folder, const char *other)
{
    char resolved[HB_PATH_MAX];
    return other != NULL && other[0] != '\0' &&
           nk_platform_real_path(other, resolved, sizeof(resolved)) &&
           hb_path_within(resolved, folder);
}

/* Whether folder (a real path) is, or holds, one of the user's personal folders: the
   home folder, the Desktop, Documents, Downloads, Music, Pictures or Videos (where the
   system says they are, so a folder moved into a cloud folder is still found), or a
   cloud-sync root the system names. A package picked from one of those does not bring
   the folder along: its files are the user's, not the program's. */
static bool hb_holds_personal_folder(const char *folder)
{
    /* The sync client's roots, by the environment names it sets on Windows. */
    static const char *const sync_roots[] = { "OneDriveConsumer", "OneDriveCommercial" };
    char known[HB_PATH_MAX];
    for (int which = 0; which < (int)NK_PERSONAL_COUNT; ++which) {
        if (nk_platform_get_personal_dir((NkPersonalDir)which, known, sizeof(known)) &&
            hb_holds_folder(folder, known))
            return true;
    }
    static const char *const english[] = {
        "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos"
    };
    const char *home = nk_platform_env_home_dir();
    if (hb_holds_folder(folder, home)) return true;
    for (size_t i = 0; home != NULL && home[0] && i < sizeof(english) / sizeof(english[0]); ++i) {
        if (hb_join(known, sizeof(known), home, english[i]) && hb_holds_folder(folder, known))
            return true;
    }
    for (size_t i = 0; i < sizeof(sync_roots) / sizeof(sync_roots[0]); ++i) {
        if (hb_holds_folder(folder, getenv(sync_roots[i]))) return true;
    }
    return false;
}

/* The checks nk_pbp_homebrew_check_folder makes; source is filled in. */
static bool hb_check_source(const char *pbp_path, const char *view_root, HbSource *source,
                            char *name, size_t name_size, char *problem, size_t problem_size)
{
    char view[HB_PATH_MAX];
    HbWalk walk;
    if (!nk_pbp_homebrew_folder_name(pbp_path, name, name_size, problem, problem_size))
        return false;
    if (!hb_source(pbp_path, source) || view_root == NULL ||
        !hb_real_maybe_missing(view_root, view, sizeof(view))) {
        hb_problem(problem, problem_size, "%s", "The package's folder could not be found.");
        return false;
    }
    /* The record of where a view came from is a line of text (review of #2426, N3). */
    for (const char *c = source->real; *c != '\0'; ++c) {
        if ((unsigned char)*c < 0x20u || *c == 0x7f) {
            hb_problem(problem, problem_size,
                       "The path of the folder \"%.80s\" holds a control character. Rename "
                       "the folder, or the folder it is in, and add the program again.",
                       source->picked + source->name_at);
            return false;
        }
    }
    if (hb_path_within(view, source->real) || hb_path_within(source->real, view)) {
        hb_problem(problem, problem_size,
                   "The folder \"%.80s\" also holds Nakagawa's data folder, or lies inside it. "
                   "Put the EBOOT.PBP in its own folder, as on a PSP's Memory Stick "
                   "(PSP/GAME/<folder>/).", source->picked + source->name_at);
        return false;
    }
    if (hb_holds_personal_folder(source->real)) {
        hb_problem(problem, problem_size,
                   "The EBOOT.PBP is in your \"%.80s\" folder, which holds your own files, and "
                   "the program's folder is copied with it. Put the EBOOT.PBP in its own folder, "
                   "as on a PSP's Memory Stick (PSP/GAME/<folder>/).",
                   source->picked + source->name_at);
        return false;
    }
    memset(&walk, 0, sizeof(walk));
    walk.root = source->real;
    walk.problem = problem;
    walk.problem_size = problem_size;
    return hb_walk(source->real, "", NULL, 0u, &walk);
}

bool nk_pbp_homebrew_check_folder(const char *pbp_path, const char *view_root,
                                  char *problem, size_t problem_size)
{
    char name[NK_PBP_HOMEBREW_FOLDER_NAME_MAX + 1u];
    HbSource source;
    return hb_check_source(pbp_path, view_root, &source, name, sizeof(name), problem,
                           problem_size);
}

/* <root>/PSP/GAME. */
static bool hb_game_dir(const char *root, char *out, size_t out_size)
{
    char psp[HB_PATH_MAX];
    return hb_join(psp, sizeof(psp), root, "PSP") && hb_join(out, out_size, psp, "GAME");
}

/* The manifest of the folder source holds now (hashing every file), for folder name. */
static bool hb_build_manifest(const HbSource *source, const char *name, HbText *manifest,
                              const char *to, const NkNpumdimgCallbacks *callbacks,
                              char *problem, size_t problem_size)
{
    HbWalk walk;
    hb_text_add(manifest, HB_MANIFEST_HEADER "\nfolder ", name, "\n");
    memset(&walk, 0, sizeof(walk));
    walk.root = source->real;
    walk.manifest = manifest;
    walk.callbacks = callbacks;
    walk.problem = problem;
    walk.problem_size = problem_size;
    return hb_walk(source->real, "", to, 0u, &walk);
}

/* The record of the folder a view was copied from (review of #2426, S2), beside the image:
   view_root with its ".ms0" replaced by ".source". It holds two lines: the path the folder
   really had and the name it had as picked, which is the program's folder name on the
   Memory Stick. It is written before the copy starts and is no part of the view, so a
   damaged view, a view removed part way and a copy left in staging by a crash all still
   have it. */
static bool hb_record_path(const char *view_root, char *out, size_t out_size)
{
    size_t length = view_root != NULL ? strlen(view_root) : 0u;
    if (length <= 4u || strcmp(view_root + length - 4u, ".ms0") != 0 || length + 3u >= out_size)
        return false;
    memcpy(out, view_root, length - 4u);
    memcpy(out + length - 4u, ".source", 8u);
    return true;
}

typedef enum { HB_RECORD_NONE, HB_RECORD_READ, HB_RECORD_BAD } HbRecordState;

/* Read the record at path. HB_RECORD_READ points real and name into *text, which the caller
   frees; HB_RECORD_BAD is a record that is there but cannot be read as two lines of text. */
static HbRecordState hb_record_read(const char *path, char **text, const char **real,
                                    const char **name)
{
    HbState state = hb_state(path);
    char *first;
    char *second;
    *text = NULL;
    if (state == HB_MISSING) return HB_RECORD_NONE;
    if (state != HB_FILE || (*text = hb_read_text(path)) == NULL) return HB_RECORD_BAD;
    for (const char *c = *text; *c != '\0'; ++c) {
        if (((unsigned char)*c < 0x20u && *c != '\n') || *c == 0x7f) return HB_RECORD_BAD;
    }
    first = strchr(*text, '\n');
    if (first == NULL || first == *text) return HB_RECORD_BAD;
    second = strchr(first + 1, '\n');
    if (second == NULL || second == first + 1 || second[1] != '\0') return HB_RECORD_BAD;
    *first = '\0';
    *second = '\0';
    *real = *text;
    *name = first + 1;
    return HB_RECORD_READ;
}

/* The parent folder of path, for syncing its entries. */
static bool hb_parent(const char *path, char *out, size_t out_size)
{
    size_t end = strlen(path);
    while (end > 0u && hb_is_separator(path[end - 1u])) --end;
    while (end > 0u && !hb_is_separator(path[end - 1u])) --end;
    while (end > 1u && hb_is_separator(path[end - 1u])) --end;
    if (end == 0u || end >= out_size) return false;
    memcpy(out, path, end);
    out[end] = '\0';
    return true;
}

/* A failed import's staging copy goes; then record, when given, which no copy outlives. */
static void hb_drop_staging(const char *staging, const char *record)
{
    if (nk_platform_remove_tree(staging, NULL) && record != NULL) (void)nk_remove_utf8(record);
}

bool nk_pbp_homebrew_write_view(const char *pbp_path, const char *view_root,
                                const NkNpumdimgCallbacks *callbacks,
                                char *problem, size_t problem_size)
{
    char name[NK_PBP_HOMEBREW_FOLDER_NAME_MAX + 1u];
    char staging[HB_PATH_MAX];
    char game[HB_PATH_MAX];
    char target[HB_PATH_MAX];
    char manifest_path[HB_PATH_MAX];
    char record_path[HB_PATH_MAX];
    char parent[HB_PATH_MAX];
    HbSource source;
    HbText manifest;
    HbText record;
    bool ok;
    bool first;
    int written;
    if (!hb_check_source(pbp_path, view_root, &source, name, sizeof(name), problem,
                         problem_size))
        return false;
    written = snprintf(staging, sizeof(staging), "%s.importing", view_root);
    if (written <= 0 || (size_t)written >= sizeof(staging) ||
        !hb_game_dir(staging, game, sizeof(game)) || !hb_join(target, sizeof(target), game, name) ||
        !hb_join(manifest_path, sizeof(manifest_path), staging, HB_MANIFEST_NAME) ||
        !hb_record_path(view_root, record_path, sizeof(record_path)) ||
        !hb_parent(view_root, parent, sizeof(parent))) {
        hb_problem(problem, problem_size, "%s", "The path to Nakagawa's data folder is too long.");
        return false;
    }
    /* The record first: whatever view or staging copy is on the disk from here on came from
       the folder it names. A first import that fails takes it away again with its staging
       copy, so nothing new is kept. */
    first = hb_state(record_path) == HB_MISSING && hb_state(view_root) == HB_MISSING;
    memset(&record, 0, sizeof(record));
    hb_text_add(&record, source.real, "\n", name);
    hb_text_add(&record, "\n", NULL, NULL);
    ok = hb_write_text(record_path, &record) && nk_platform_sync_dir(parent);
    free(record.text);
    if (!ok) {
        hb_problem(problem, problem_size, "%.400s could not be written.", record_path);
        return false;
    }
    if (!nk_platform_remove_tree(staging, NULL) || !nk_platform_mkdir_p_private(target)) {
        hb_problem(problem, problem_size, "%.400s could not be made.", target);
        hb_drop_staging(staging, first ? record_path : NULL);
        return false;
    }
    memset(&manifest, 0, sizeof(manifest));
    ok = hb_build_manifest(&source, name, &manifest, target, callbacks, problem, problem_size);
    if (ok && (!hb_write_text(manifest_path, &manifest) || !nk_platform_sync_dir(staging))) {
        hb_problem(problem, problem_size, "%.400s could not be written.", staging);
        ok = false;
    }
    free(manifest.text);
    if (!ok) {
        hb_drop_staging(staging, first ? record_path : NULL);
        return false;
    }
    /* Between these two steps there is no view: a launch then stops by name
       (NK_PBP_HB_VIEW_MISSING) rather than run the program without its folder. */
    if (!nk_platform_remove_tree(view_root, NULL) || nk_rename_utf8(staging, view_root) != 0 ||
        !nk_platform_sync_dir(parent)) {
        hb_drop_staging(staging, first ? record_path : NULL);
        hb_problem(problem, problem_size, "%.400s could not be put in place.", view_root);
        return false;
    }
    return true;
}

bool nk_pbp_homebrew_view_current(const char *pbp_path, const char *view_root)
{
    char name[NK_PBP_HOMEBREW_FOLDER_NAME_MAX + 1u];
    char manifest_path[HB_PATH_MAX];
    char problem[8];
    HbSource source;
    HbText manifest;
    char *stored;
    bool current;
    if (!hb_check_source(pbp_path, view_root, &source, name, sizeof(name), problem,
                         sizeof(problem)) ||
        !hb_join(manifest_path, sizeof(manifest_path), view_root, HB_MANIFEST_NAME) ||
        (stored = hb_read_text(manifest_path)) == NULL)
        return false;
    memset(&manifest, 0, sizeof(manifest));
    current = hb_build_manifest(&source, name, &manifest, NULL, NULL, problem, sizeof(problem)) &&
              !manifest.failed && strcmp(manifest.text, stored) == 0;
    free(manifest.text);
    free(stored);
    return current;
}

/* ---- Reading a manifest back ---- */

typedef struct {
    char kind;                 /* 'D' or 'F' */
    char hex[HB_HEX_LEN + 1u]; /* F: the file's SHA-256 */
    uint64_t size;             /* F: its bytes */
    const char *rel;           /* "/"-separated, checked */
} HbEntry;

typedef struct {
    char *text; /* the entries point into it */
    HbEntry *items;
    size_t count;
    char folder[NK_PBP_HOMEBREW_FOLDER_NAME_MAX + 1u];
} HbManifest;

static void hb_manifest_free(HbManifest *manifest)
{
    free(manifest->text);
    free(manifest->items);
    memset(manifest, 0, sizeof(*manifest));
}

static bool hb_is_hex(const char *text)
{
    for (size_t i = 0; i < HB_HEX_LEN; ++i) {
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    }
    return true;
}

/* A relative path a manifest may name: "/"-separated names a Memory Stick holds. */
static bool hb_rel_ok(const char *rel)
{
    const char *part = rel;
    if (strlen(rel) >= HB_PATH_MAX / 2u) return false;
    for (;;) {
        const char *end = strchr(part, '/');
        size_t length = end != NULL ? (size_t)(end - part) : strlen(part);
        char name[HB_PATH_MAX / 2u];
        memcpy(name, part, length);
        name[length] = '\0';
        if (!hb_entry_name_ok(name, length)) return false;
        if (end == NULL) return true;
        part = end + 1;
    }
}

/* Whether the folder holding rel (all of rel up to its last '/') is an earlier D entry: the
   seed checks each folder for links before it puts anything in it, so a file entry may not reach
   a folder the list never named (review of #2426, M2). A top-level entry has no such folder. */
static bool hb_manifest_parent_listed(const HbManifest *manifest, const char *rel)
{
    const char *slash = strrchr(rel, '/');
    size_t length;
    if (slash == NULL) return true;
    length = (size_t)(slash - rel);
    for (size_t i = 0; i < manifest->count; ++i) {
        const HbEntry *entry = &manifest->items[i];
        if (entry->kind == 'D' && strlen(entry->rel) == length &&
            memcmp(entry->rel, rel, length) == 0)
            return true;
    }
    return false;
}

/* Parse a manifest, splitting text into lines in place. A list longer than the import writes
   (two header lines and NK_PBP_HOMEBREW_FOLDER_MAX_ENTRIES entries), or whose files add up to
   more than NK_PBP_HOMEBREW_FOLDER_MAX_BYTES, is refused before its table is made (review of
   #2426, m3). */
static bool hb_manifest_parse(char *text, HbManifest *manifest)
{
    char *line = text;
    size_t lines = 0, capacity = 0;
    uint64_t total = 0;
    for (const char *c = text; *c; ++c) capacity += *c == '\n';
    if (capacity > NK_PBP_HOMEBREW_FOLDER_MAX_ENTRIES + 2u) return false;
    manifest->items = (HbEntry *)calloc(capacity + 1u, sizeof(HbEntry));
    if (manifest->items == NULL) return false;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end == NULL) return false; /* every line ends in a newline */
        *end = '\0';
        if (lines == 0u) {
            if (strcmp(line, HB_MANIFEST_HEADER) != 0) return false;
        } else if (lines == 1u) {
            if (strncmp(line, "folder ", 7) != 0 || !hb_name_valid(line + 7, strlen(line + 7)))
                return false;
            memcpy(manifest->folder, line + 7, strlen(line + 7) + 1u);
        } else {
            HbEntry *entry = &manifest->items[manifest->count];
            if (line[0] == 'D' && line[1] == ' ') {
                entry->kind = 'D';
                entry->rel = line + 2;
            } else if (line[0] == 'F' && line[1] == ' ' && strlen(line) > 2u + HB_HEX_LEN + 1u &&
                       hb_is_hex(line + 2) && line[2 + HB_HEX_LEN] == ' ') {
                char *size_end;
                const char *size_text = line + 3 + HB_HEX_LEN;
                unsigned long long size;
                if (*size_text < '0' || *size_text > '9') return false;
                size = strtoull(size_text, &size_end, 10);
                if (*size_end != ' ' || size > NK_PBP_HOMEBREW_FOLDER_MAX_BYTES - total)
                    return false;
                total += (uint64_t)size;
                entry->kind = 'F';
                memcpy(entry->hex, line + 2, HB_HEX_LEN);
                entry->hex[HB_HEX_LEN] = '\0';
                entry->size = (uint64_t)size;
                entry->rel = size_end + 1;
            } else {
                return false;
            }
            if (!hb_rel_ok(entry->rel) ||
                manifest->count >= NK_PBP_HOMEBREW_FOLDER_MAX_ENTRIES ||
                !hb_manifest_parent_listed(manifest, entry->rel))
                return false;
            manifest->count++;
        }
        lines++;
        line = end + 1;
    }
    return lines >= 2u;
}

static bool hb_manifest_read(const char *view_root, HbManifest *manifest)
{
    char path[HB_PATH_MAX];
    memset(manifest, 0, sizeof(*manifest));
    if (!hb_join(path, sizeof(path), view_root, HB_MANIFEST_NAME) ||
        (manifest->text = hb_read_text(path)) == NULL)
        return false;
    if (!hb_manifest_parse(manifest->text, manifest)) {
        hb_manifest_free(manifest);
        return false;
    }
    return true;
}

/* 1 when the image at iso_path is one this import wrote (its volume identifier),
   0 when it is another image, -1 when it cannot be read. */
static int hb_image_is_homebrew(const char *iso_path)
{
    uint8_t pvd[64];
    FILE *f = nk_fopen_utf8(iso_path, "rb");
    int result;
    if (f == NULL) return -1;
    result = hb_read_at(f, 16u * HB_SECTOR, pvd, sizeof(pvd)) != 0 ? -1 :
             (pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0 &&
              memcmp(pvd + 40, "NAKAGAWA_HOMEBREW", 17) == 0) ? 1 : 0;
    fclose(f);
    return result;
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

/* Whether path, every link resolved, is folder_real or lies inside it. */
static bool hb_real_within(const char *path, const char *folder_real)
{
    char real[HB_PATH_MAX];
    return nk_platform_real_path(path, real, sizeof(real)) && hb_path_within(real, folder_real);
}

/* Whether the image lies in the data folder's discs/, where only the import writes images
   (review of #2426, N1): the volume identifier alone is a label any image can carry. */
static bool hb_image_in_data(const char *iso_path)
{
    char data[HB_PATH_MAX];
    char discs[HB_PATH_MAX];
    char discs_real[HB_PATH_MAX];
    return nk_platform_get_app_data_dir(data, sizeof(data)) &&
           hb_join(discs, sizeof(discs), data, "discs") &&
           nk_platform_real_path(discs, discs_real, sizeof(discs_real)) &&
           hb_real_within(iso_path, discs_real);
}

NkPbpHomebrewView nk_pbp_homebrew_find_view(const char *iso_path, char *folder,
                                            size_t folder_size)
{
    char root[HB_PATH_MAX];
    char game[HB_PATH_MAX];
    char program_dir[HB_PATH_MAX];
    char program[HB_PATH_MAX];
    HbViewScan scan;
    HbManifest manifest;
    size_t skipped = 0;
    bool same;
    if (folder != NULL && folder_size > 0u) folder[0] = '\0';
    if (!nk_pbp_homebrew_view_root(iso_path, root, sizeof(root)) ||
        hb_image_is_homebrew(iso_path) != 1 || !hb_image_in_data(iso_path))
        return NK_PBP_HB_VIEW_NONE;
    if (nk_platform_path_missing(root)) return NK_PBP_HB_VIEW_MISSING;
    memset(&scan, 0, sizeof(scan));
    if (folder == NULL || !hb_game_dir(root, game, sizeof(game)) ||
        !nk_platform_list_dir_counted(game, hb_view_scan, &scan, &skipped) || skipped != 0u ||
        scan.folders != 1u || scan.files != 0u || scan.name[0] == '\0' ||
        strlen(scan.name) >= folder_size ||
        !hb_join(program_dir, sizeof(program_dir), game, scan.name) ||
        !hb_join(program, sizeof(program), program_dir, "EBOOT.PBP") ||
        !nk_platform_file_exists(program) || !hb_manifest_read(root, &manifest))
        return NK_PBP_HB_VIEW_BROKEN;
    same = strcmp(manifest.folder, scan.name) == 0;
    hb_manifest_free(&manifest);
    if (!same) return NK_PBP_HB_VIEW_BROKEN;
    memcpy(folder, scan.name, strlen(scan.name) + 1u);
    return NK_PBP_HB_VIEW_FOUND;
}

static bool hb_count_entry(const char *name, bool is_dir, void *ctx)
{
    (void)name;
    (void)is_dir;
    ++*(size_t *)ctx;
    return true;
}

typedef enum { HB_CONTENT_NONE, HB_CONTENT_NAMED, HB_CONTENT_OTHER } HbContent;

/* What dir, a view or its staging copy, holds, told by the program's folder name alone:
   nothing (missing or empty), one folder PSP/GAME/<name>, or anything else, including what
   cannot be listed. A folder of another name goes into other. */
static HbContent hb_view_content(const char *dir, const char *name, char *other,
                                 size_t other_size)
{
    char game[HB_PATH_MAX];
    HbViewScan scan;
    size_t entries = 0, skipped = 0;
    HbState state = hb_state(dir);
    if (state == HB_MISSING) return HB_CONTENT_NONE;
    if (state != HB_FOLDER || !nk_platform_list_dir_counted(dir, hb_count_entry, &entries,
                                                            &skipped) || skipped != 0u)
        return HB_CONTENT_OTHER;
    if (entries == 0u) return HB_CONTENT_NONE;
    memset(&scan, 0, sizeof(scan));
    skipped = 0;
    if (!hb_game_dir(dir, game, sizeof(game)) || hb_state(game) != HB_FOLDER ||
        !nk_platform_list_dir_counted(game, hb_view_scan, &scan, &skipped) || skipped != 0u ||
        scan.files != 0u || scan.folders != 1u || scan.name[0] == '\0')
        return HB_CONTENT_OTHER;
    if (strcmp(scan.name, name) != 0) {
        snprintf(other, other_size, "%s", scan.name);
        return HB_CONTENT_OTHER;
    }
    return HB_CONTENT_NAMED;
}

bool nk_pbp_homebrew_other_source(const char *pbp_path, const char *iso_path, char *other,
                                  size_t other_size)
{
    char name[NK_PBP_HOMEBREW_FOLDER_NAME_MAX + 1u];
    char root[HB_PATH_MAX];
    char staging[HB_PATH_MAX];
    char record_path[HB_PATH_MAX];
    HbSource source;
    char *text = NULL;
    const char *stored_real = NULL;
    const char *stored_name = NULL;
    HbRecordState record;
    bool differs;
    int written;
    if (other == NULL || other_size == 0u) return true;
    other[0] = '\0';
    /* What cannot be told is another folder's: the import stops. */
    if (!hb_source(pbp_path, &source) ||
        !nk_pbp_homebrew_folder_name(pbp_path, name, sizeof(name), NULL, 0u) ||
        !nk_pbp_homebrew_view_root(iso_path, root, sizeof(root)) ||
        !hb_record_path(root, record_path, sizeof(record_path)))
        return true;
    written = snprintf(staging, sizeof(staging), "%s.importing", root);
    if (written <= 0 || (size_t)written >= sizeof(staging)) return true;
    record = hb_record_read(record_path, &text, &stored_real, &stored_name);
    if (record == HB_RECORD_BAD) {
        free(text);
        return true;
    }
    if (record == HB_RECORD_READ) {
        /* The same folder, by the path it really has (on Windows as hb_path_within compares)
           and by the name it is picked by, which names its folder on the Memory Stick
           (review of #2426, N2). Every view and staging copy came from it. */
        differs = !hb_path_within(source.real, stored_real) ||
                  !hb_path_within(stored_real, source.real) || strcmp(stored_name, name) != 0;
        if (differs) snprintf(other, other_size, "%s", stored_real);
        free(text);
        return differs;
    }
    /* No record: a view is compared by its folder's name; a staging copy cannot be told to
       be this import's own, so any is another folder's. */
    if (hb_view_content(root, name, other, other_size) == HB_CONTENT_OTHER) return true;
    return hb_view_content(staging, name, other, other_size) != HB_CONTENT_NONE;
}

bool nk_pbp_homebrew_delete_import(const char *iso_path, char *problem, size_t problem_size)
{
    char root[HB_PATH_MAX];
    char staging[HB_PATH_MAX];
    char seeded[HB_PATH_MAX];
    char converting[HB_PATH_MAX];
    char record[HB_PATH_MAX];
    char record_temp[HB_PATH_MAX];
    char folder[HB_PATH_MAX];
    HbState image;
    if (!nk_pbp_homebrew_view_root(iso_path, root, sizeof(root)) ||
        !hb_beside_image(iso_path, ".seeded", seeded, sizeof(seeded)) ||
        !hb_record_path(root, record, sizeof(record)) ||
        !hb_parent(iso_path, folder, sizeof(folder)) ||
        snprintf(staging, sizeof(staging), "%s.importing", root) >= (int)sizeof(staging) ||
        snprintf(converting, sizeof(converting), "%s.converting", iso_path) >=
            (int)sizeof(converting) ||
        snprintf(record_temp, sizeof(record_temp), "%s.tmp", record) >= (int)sizeof(record_temp)) {
        hb_problem(problem, problem_size, "%s", "The path to Nakagawa's data folder is too long.");
        return false;
    }
    if (nk_platform_path_missing(folder)) return true;
    /* Only the import's own files, in the data folder's discs/, and never a disc image
       that is not a program's. */
    image = hb_state(iso_path);
    if (!hb_image_in_data(folder) || image == HB_FOLDER || image == HB_UNKNOWN ||
        (image == HB_FILE && hb_image_is_homebrew(iso_path) != 1)) {
        hb_problem(problem, problem_size,
                   "%.400s is not a program's import, so nothing was deleted.", iso_path);
        return false;
    }
    {
        /* The record last: until everything else is gone, it still stops another folder. */
        const char *const paths[] = { root, staging, seeded, converting, iso_path, record_temp,
                                      record };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
            if (!nk_platform_remove_tree(paths[i], NULL)) {
                hb_problem(problem, problem_size,
                           "%.400s could not be deleted. Close any program using it and try "
                           "again.", paths[i]);
                return false;
            }
        }
    }
    return true;
}

/* rel ("/"-separated) under base, in the host's separators. */
static bool hb_join_rel(char *out, size_t out_size, const char *base, const char *rel)
{
    if (!hb_join(out, out_size, base, rel)) return false;
    for (char *c = out + strlen(base); *c; ++c) {
        if (*c == '/') *c = nk_platform_path_separator();
    }
    return true;
}

/* The digest the seed record holds for rel, or NULL. Lines: "<hex> <rel>". */
static const char *hb_seeded_digest(char *const *lines, size_t count, const char *rel)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(lines[i] + HB_HEX_LEN + 1u, rel) == 0) return lines[i];
    }
    return NULL;
}

/* Read the seed record for the stick folder whose real path is stick: NULL lines and true
   when there is none yet, or when it describes another Memory Stick (then nothing on this
   one counts as placed, and the seed adopts by content). */
static bool hb_seeded_read(const char *path, const char *stick, char **text, char ***lines,
                           size_t *count)
{
    size_t capacity = 0, n = 0;
    char *line;
    *text = NULL;
    *lines = NULL;
    *count = 0;
    switch (hb_state(path)) {
    case HB_MISSING: return true;
    case HB_FILE: break;
    default: return false;
    }
    if ((*text = hb_read_text(path)) == NULL) return false;
    for (const char *c = *text; *c; ++c) capacity += *c == '\n';
    if ((*lines = (char **)calloc(capacity + 1u, sizeof(char *))) == NULL) return false;
    line = *text;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end == NULL) return false;
        *end = '\0';
        if (n == 0u) {
            if (strcmp(line, HB_SEEDED_HEADER) != 0) return false;
        } else if (n == 1u) {
            if (strncmp(line, "stick ", 6) != 0) return false;
            if (strcmp(line + 6, stick) != 0) {
                *count = 0;
                return true;
            }
        } else {
            if (strlen(line) <= HB_HEX_LEN + 1u || !hb_is_hex(line) || line[HB_HEX_LEN] != ' ')
                return false;
            (*lines)[(*count)++] = line;
        }
        n++;
        line = end + 1;
    }
    return n >= 2u;
}

/* Whether rel lies under one of the kept prefixes (a folder of the view the program
   replaced with a file of its own). */
static bool hb_under(const char *rel, const char *const *prefixes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        size_t length = strlen(prefixes[i]);
        if (strncmp(rel, prefixes[i], length) == 0 && rel[length] == '/') return true;
    }
    return false;
}

/* The seed record for one Memory Stick: "<image>.seeded/<first 128 bits of the SHA-256 of
   the stick folder's real path>", so each stick a title runs on keeps its own. */
static bool hb_seeded_record_path(const char *dir, const char *stick, char *out,
                                  size_t out_size)
{
    NkSha256 sha;
    uint8_t digest[32];
    char hex[HB_HEX_LEN + 1u];
    nk_sha256_init(&sha);
    nk_sha256_update(&sha, (const uint8_t *)stick, strlen(stick));
    nk_sha256_finish(&sha, digest);
    hb_hex(digest, hex);
    hex[32] = '\0';
    return hb_join(out, out_size, dir, hex);
}

static void hb_record(HbText *record, const char *hex, const char *rel)
{
    hb_text_add(record, hex, " ", rel);
    hb_text_add(record, "\n", NULL, NULL);
}

/* Make path, a folder below root, one missing folder at a time: each folder on the way, made or
   found, must really be inside root_real before the next is made in it, so a link or junction
   on the way is refused before anything is made through it (review of #2426, m1). */
static bool hb_mkdirs_within(const char *root, const char *root_real, const char *path)
{
    char prefix[HB_PATH_MAX];
    size_t root_length = strlen(root);
    size_t length = strlen(path);
    if (length >= sizeof(prefix) || length <= root_length || strncmp(path, root, root_length) != 0)
        return false;
    for (size_t end = root_length + 1u; end <= length; ++end) {
        HbState state;
        if (end < length && path[end] != '/' && path[end] != '\\') continue;
        memcpy(prefix, path, end);
        prefix[end] = '\0';
        state = hb_state(prefix);
        if (state == HB_FILE || state == HB_UNKNOWN) return false;
        if (state == HB_MISSING && !nk_platform_mkdir_p_private(prefix)) return false;
        if (!hb_real_within(prefix, root_real)) return false;
    }
    return true;
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
    char target_real[HB_PATH_MAX];
    char stick_real[HB_PATH_MAX];
    char seeded_dir[HB_PATH_MAX];
    char seeded_path[HB_PATH_MAX];
    char manifest_path[HB_PATH_MAX];
    HbManifest manifest;
    HbText record;
    char *seeded_text = NULL;
    char **seeded = NULL;
    size_t seeded_count = 0;
    const char **kept = NULL;
    size_t kept_count = 0;
    size_t done = 0;
    bool ok = true;
    bool record_ok = false;
    bool view_damaged = false;
    if (memstick_root == NULL || memstick_root[0] == '\0' || !hb_is_absolute(memstick_root)) {
        hb_problem(problem, problem_size, "%s",
                   "The title has no Memory Stick folder to put the program's folder on.");
        return false;
    }
    if (folder == NULL || !nk_pbp_homebrew_view_root(iso_path, root, sizeof(root)) ||
        !hb_beside_image(iso_path, ".seeded", seeded_dir, sizeof(seeded_dir)) ||
        !hb_join(manifest_path, sizeof(manifest_path), root, HB_MANIFEST_NAME) ||
        !hb_game_dir(root, game, sizeof(game)) || !hb_join(source, sizeof(source), game, folder) ||
        !hb_game_dir(memstick_root, stick_game, sizeof(stick_game)) ||
        !hb_join(target, sizeof(target), stick_game, folder)) {
        hb_problem(problem, problem_size, "%s",
                   "The program's Memory Stick folder has a path too long to use.");
        return false;
    }
    if (!hb_manifest_read(root, &manifest) || strcmp(manifest.folder, folder) != 0) {
        if (manifest.text != NULL) hb_manifest_free(&manifest);
        hb_problem(problem, problem_size, "%s",
                   "The list of the program's files beside its disc image is damaged. Add "
                   "the program's EBOOT.PBP again.");
        return false;
    }
    if (hb_state(target) == HB_FILE || hb_state(target) == HB_UNKNOWN ||
        (hb_state(memstick_root) == HB_MISSING && !nk_platform_mkdir_p_private(memstick_root))) {
        hb_problem(problem, problem_size, "%.400s could not be made on the Memory Stick.",
                   target);
        ok = false;
    } else if (!nk_platform_real_path(memstick_root, stick_real, sizeof(stick_real))) {
        hb_problem(problem, problem_size,
                   "Where %.400s really is could not be read from the system.", memstick_root);
        ok = false;
    } else if (!hb_mkdirs_within(memstick_root, stick_real, target)) {
        hb_problem(problem, problem_size,
                   "%.400s could not be made on the Memory Stick, or leads outside it (a link "
                   "or junction is in the way); nothing was put there.", target);
        ok = false;
    } else if (!nk_platform_real_path(target, target_real, sizeof(target_real))) {
        hb_problem(problem, problem_size,
                   "Where %.400s really is could not be read from the system.", target);
        ok = false;
    } else if (!hb_path_within(target_real, stick_real)) {
        hb_problem(problem, problem_size,
                   "%.400s leads outside the title's Memory Stick (a link or junction is in "
                   "the way); nothing was put there.", target);
        ok = false;
    } else if (!hb_seeded_record_path(seeded_dir, target_real, seeded_path,
                                      sizeof(seeded_path))) {
        hb_problem(problem, problem_size, "%s",
                   "The program's Memory Stick folder has a path too long to use.");
        ok = false;
    } else if (!hb_seeded_read(seeded_path, target_real, &seeded_text, &seeded, &seeded_count)) {
        hb_problem(problem, problem_size,
                   "%.400s, the record of what was put on the Memory Stick, could not be "
                   "read. Remove it and start the program again; nothing on the Memory Stick "
                   "is replaced without it.", seeded_path);
        ok = false;
    } else {
        record_ok = true;
    }
    kept = (const char **)calloc(manifest.count + 1u, sizeof(*kept));
    memset(&record, 0, sizeof(record));
    if (record_ok) hb_text_add(&record, HB_SEEDED_HEADER "\nstick ", target_real, "\n");
    if (kept == NULL) {
        hb_problem(problem, problem_size, "%s", "Nakagawa ran out of memory.");
        ok = false;
    }
    for (size_t i = 0; ok && i < manifest.count; ++i) {
        const HbEntry *entry = &manifest.items[i];
        char stick[HB_PATH_MAX];
        char view[HB_PATH_MAX];
        char hex[HB_HEX_LEN + 1u];
        HbState state;
        HbState view_state;
        int64_t view_size = -1;
        done = i;
        if (hb_under(entry->rel, kept, kept_count)) continue;
        if (!hb_join_rel(stick, sizeof(stick), target, entry->rel) ||
            !hb_join_rel(view, sizeof(view), source, entry->rel)) {
            hb_problem(problem, problem_size, "%s",
                       "The program's Memory Stick folder has a path too long to use.");
            ok = false;
            break;
        }
        state = hb_state(stick);
        if (state == HB_UNKNOWN) {
            hb_problem(problem, problem_size, "%.400s could not be read.", stick);
            ok = false;
        } else if (entry->kind == 'D') {
            if (state == HB_FILE) {
                kept[kept_count++] = entry->rel; /* the program's file stays */
            } else if (state == HB_MISSING && !nk_platform_mkdir_p_private(stick)) {
                hb_problem(problem, problem_size, "%.400s could not be made.", stick);
                ok = false;
            } else if (!hb_real_within(stick, target_real)) {
                hb_problem(problem, problem_size,
                           "%.400s leads outside the program's Memory Stick folder (a link or "
                           "junction is in the way); nothing was put there.", stick);
                ok = false;
            }
        } else if (state == HB_FOLDER) {
            /* a folder where the view has a file: the program's, it stays */
        } else if ((view_state = hb_state(view)) == HB_UNKNOWN ||
                   (view_state == HB_FILE &&
                    (view_size = nk_platform_get_file_size(view)) < 0)) {
            /* not damage: the view stays, and the next start tries again */
            hb_problem(problem, problem_size, "%.400s could not be read.", view);
            ok = false;
        } else if (view_state != HB_FILE || view_size != (int64_t)entry->size) {
            view_damaged = true;
            ok = false;
        } else if (state == HB_MISSING) {
            ok = hb_copy_file(view, stick, entry->size, entry->hex, hex, &view_damaged, problem,
                              problem_size);
            if (ok) hb_record(&record, entry->hex, entry->rel);
        } else {
            /* A file the stick has: the seed's while it holds what a seed put there (or the
               view's own bytes); replaced only then, when the view brings others. Anything
               else is the program's and stays. */
            const char *placed = hb_seeded_digest(seeded, seeded_count, entry->rel);
            int64_t size = nk_platform_get_file_size(stick);
            uint8_t digest[32];
            if (placed != NULL && strncmp(placed, entry->hex, HB_HEX_LEN) == 0) {
                hb_record(&record, entry->hex, entry->rel);
            } else if (size < 0 ||
                       !hb_read_file(stick, (uint64_t)size, NULL, digest, problem, problem_size)) {
                if (size < 0) hb_problem(problem, problem_size, "%.400s could not be read.", stick);
                ok = false;
            } else {
                hb_hex(digest, hex);
                if (strcmp(hex, entry->hex) == 0) {
                    hb_record(&record, entry->hex, entry->rel);
                } else if (placed != NULL && strncmp(placed, hex, HB_HEX_LEN) == 0) {
                    ok = hb_copy_file(view, stick, entry->size, entry->hex, hex, &view_damaged,
                                      problem, problem_size);
                    if (ok) hb_record(&record, entry->hex, entry->rel);
                }
            }
        }
    }
    if (ok) done = manifest.count;
    if (view_damaged) {
        /* Without its manifest the view is BROKEN and no longer current, so adding the
           program again writes it afresh rather than keep failing here. */
        if (nk_remove_utf8(manifest_path) != 0 && !nk_platform_path_missing(manifest_path)) {
            /* Kept, the view would still pass as current and the next add say "ready"
               (review of #2426, N9). */
            hb_problem(problem, problem_size,
                       "The copy of the program's folder is damaged, and %.300s could not be "
                       "deleted. Close any program using it and start the title again.",
                       manifest_path);
        } else {
            hb_problem(problem, problem_size,
                       "The copy of the program's folder beside %.300s is damaged. Add the "
                       "program's EBOOT.PBP again.", iso_path);
        }
    }
    if (record_ok) {
        /* What a seed stopped part way did place is recorded too, and what it did not reach
           keeps its earlier record, so a later release still reaches it. */
        for (size_t i = done; i < manifest.count; ++i) {
            const char *placed = manifest.items[i].kind == 'F'
                ? hb_seeded_digest(seeded, seeded_count, manifest.items[i].rel) : NULL;
            if (placed != NULL) hb_text_add(&record, placed, "\n", NULL);
        }
        if (!nk_platform_sync_dir(target) || !nk_platform_mkdir_p_private(seeded_dir) ||
            !hb_write_text(seeded_path, &record)) {
            if (ok) hb_problem(problem, problem_size, "%.400s could not be written.", seeded_path);
            ok = false;
        }
    }
    free(record.text);
    free(kept);
    free(seeded);
    free(seeded_text);
    hb_manifest_free(&manifest);
    return ok;
}
