/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Container-level probe and decrypt orchestration for the built-in PSP
 * decryption boundary (issue #295).  Project-authored from the format
 * rules documented for this project; no key material here.
 *
 * Structural validation runs BEFORE any key lookup so a malformed
 * container reports MALFORMED_INPUT rather than a key diagnostic,
 * and a well-formed one reports exactly the key entry it needs.
 *
 * Firmware is never decrypted (AGENTS.md section 5): a firmware updater
 * archive (PSAR), a raw KIRK block in the IPL form, and a module whose tag
 * is in nk_psp_firmware_tags are refused by name here, before any key
 * lookup or decryption step runs.
 *
 * Two of those tags are also the tags of the user-mode library modules games
 * ship on their own disc (issue #3235).  The tag cannot tell the two apart;
 * the source can, so this file establishes it itself: only
 * nk_container_decrypt_disc_member, which reads a named file out of a game
 * disc image, opens them, for a file under PSP_GAME/USRDIR whose header
 * declares a user module.  It refuses PSP_GAME/SYSDIR/UPDATE, the disc's
 * system software update, by path before reading it.  No caller can say
 * where bytes it holds came from: nk_container_decrypt refuses both tags.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_psp_builtin_keys.h"
#include "nk_psp_container.h"
#include "nk_psp_inflate.h"
#include "nk_psp_kle.h"
#include "nk_psp_prx.h"

#define NK_PSP_HEADER_SIZE 0x150u
#define NK_MAX_WRAPPER_DEPTH 8
#define NK_DECOMP_CAP (64u * 1024u * 1024u)
#define NK_ELF_MIN_SIZE 0x34u

typedef struct View {
    const uint8_t *data;
    size_t size;
    int wrappers;
    int pbp_layers; /* how many of the wrappers were PBP packages */
} View;

/* Where the bytes came from, as this file itself established it. */
typedef enum Origin {
    ORIGIN_UNKNOWN = 0,        /* bytes the caller holds, or any other disc file */
    ORIGIN_DISC_NOT_GAME,      /* PSP_GAME/USRDIR of an image with no game identity */
    ORIGIN_GAME_DISC_USRDIR    /* PSP_GAME/USRDIR of a PSP game disc image */
} Origin;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int has_magic(const uint8_t *d, size_t size, const char *magic,
                     size_t magic_len)
{
    return size >= magic_len && memcmp(d, magic, magic_len) == 0;
}

/* ------------------------- firmware refusal --------------------------- */

#define NK_KIRK_CMD1_MODE_OFFSET 0x60u
/* ~PSP header fields the library-module rule reads: 0x04 module attribute
 * (0x1000 kernel, 0x0800 VSH), 0x7C decrypt mode (4 is a user module). */
#define NK_PSP_ATTR_KERNEL_OR_VSH 0x1800u
#define NK_PSP_DECRYPT_MODE_USER_MODULE 4u

static const char *firmware_tag_what(uint32_t tag)
{
    for (size_t i = 0; i < nk_psp_firmware_tag_count; i++) {
        if (nk_psp_firmware_tags[i].tag == tag) return nk_psp_firmware_tags[i].what;
    }
    return NULL;
}

static int is_disc_library_tag(uint32_t tag)
{
    for (size_t i = 0; i < nk_psp_disc_library_tag_count; i++) {
        if (nk_psp_disc_library_tags[i].tag == tag) return 1;
    }
    return 0;
}

/* v is a validated ~PSP view (at least NK_PSP_HEADER_SIZE bytes). */
static int refuse_firmware_tag(NkPspCtx *ctx, const View *v, uint32_t tag,
                               Origin origin)
{
    const char *what = firmware_tag_what(tag);
    if (is_disc_library_tag(tag)) {
        /* A tag firmware and game discs both carry: the rule is the source. */
        if (what == NULL) what = "user-mode library module";
        if (origin == ORIGIN_DISC_NOT_GAME) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                               "container tag 0x%08X marks a %s; Nakagawa "
                               "never decrypts firmware, and this disc image "
                               "has no PSP_GAME/PARAM.SFO with a DISC_ID, so "
                               "it is not read as a game disc",
                               (unsigned)tag, what);
        }
        if (origin != ORIGIN_GAME_DISC_USRDIR) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                               "container tag 0x%08X marks a %s; Nakagawa "
                               "never decrypts firmware, and opens a game's "
                               "own copy of such a library only when it "
                               "reads it from PSP_GAME/USRDIR of the game's "
                               "disc image",
                               (unsigned)tag, what);
        }
        if (v->pbp_layers != 0 ||
            (rd16(v->data + 0x04) & NK_PSP_ATTR_KERNEL_OR_VSH) != 0u ||
            v->data[0x7C] != NK_PSP_DECRYPT_MODE_USER_MODULE) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                               "container tag 0x%08X marks a %s, and this "
                               "one is not a plain user-mode library module "
                               "(it is in a package, or its header declares "
                               "kernel or VSH mode); Nakagawa never decrypts "
                               "firmware",
                               (unsigned)tag, what);
        }
        return NK_PSP_OK;
    }
    if (what != NULL) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                           "container tag 0x%08X marks a %s; Nakagawa "
                           "never decrypts firmware",
                           (unsigned)tag, what);
    }
    return NK_PSP_OK;
}

/* Firmware forms that carry no ~PSP tag: an updater archive, and a bare KIRK
 * command-1 block, which is how the IPL is stored. */
static int refuse_firmware_form(NkPspCtx *ctx, const uint8_t *d, size_t size)
{
    if (has_magic(d, size, "PSAR", 4) || has_magic(d, size, "\0PSAR", 5)) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                           "this is a firmware updater archive (PSAR); "
                           "Nakagawa never decrypts firmware");
    }
    if (size >= NK_KIRK_CMD1_MODE_OFFSET + 4u &&
        rd32(d + NK_KIRK_CMD1_MODE_OFFSET) == 1u &&
        !has_magic(d, size, "~PSP", 4) && !has_magic(d, size, "~SCE", 4) &&
        !has_magic(d, size, "\0PBP", 4) && !has_magic(d, size, "\x7f" "ELF", 4) &&
        !has_magic(d, size, "\x1f\x8b", 2)) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                           "this is a bare KIRK block, the form the boot "
                           "loader (IPL) is stored in; Nakagawa never "
                           "decrypts firmware");
    }
    return NK_PSP_OK;
}

/* ------------------------- validation -------------------------------- */

/* ~PSP header fields read in this file: 0x06 compression attribute
 * (bit 0 = compressed), 0x27 segment count, 0x28 decompressed ELF size, 0x2C total
 * container size, 0x54 up to four segment sizes, 0xD0 tag. */
static int validate_psp(NkPspCtx *ctx, const uint8_t *d, size_t size)
{
    uint32_t psp_size;
    if (size < NK_PSP_HEADER_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container is too small for a ~PSP header "
                           "(%u bytes, need %u); the file is truncated or "
                           "not a module container",
                           (unsigned)size, (unsigned)NK_PSP_HEADER_SIZE);
    }
    psp_size = rd32(d + 0x2C);
    if (psp_size < NK_PSP_HEADER_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container declares an impossible total size "
                           "0x%08X",
                           (unsigned)psp_size);
    }
    if ((size_t)psp_size > size) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container is truncated (declares 0x%08X bytes, "
                           "file carries %u)",
                           (unsigned)psp_size, (unsigned)size);
    }
    if (d[0x27] == 0u || d[0x27] > 4u) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container declares an impossible segment count "
                           "(%u)",
                           (unsigned)d[0x27]);
    }
    {
        uint32_t total_segment_bytes = 0;
        uint8_t segment_count = d[0x27];
        for (uint8_t i = 0; i < segment_count; i++) {
            uint32_t segment_size = rd32(d + 0x54 + i * 4u);
            if (segment_size > (256u * 1024u * 1024u)) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "container segment %u declares an impossible size "
                                   "0x%08X",
                                   (unsigned)i, (unsigned)segment_size);
            }
            if (total_segment_bytes > (256u * 1024u * 1024u) - segment_size) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "container segment sizes overflow aggregate extent");
            }
            total_segment_bytes += segment_size;
        }
    }
    return NK_PSP_OK;
}

/* Extract the DATA.PSP slice out of a PBP package (fail closed). */
static int pbp_slice(NkPspCtx *ctx, const uint8_t *d, size_t size,
                     const uint8_t **slice, size_t *slice_size)
{
    uint32_t off_psp;
    uint32_t off_psar;
    size_t end;
    if (size < 0x28u) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "PBP package header is truncated (%u bytes)",
                           (unsigned)size);
    }
    off_psp = rd32(d + 0x20);
    off_psar = rd32(d + 0x24);
    if (off_psp < 0x28u || (size_t)off_psp >= size) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "PBP package has an impossible DATA.PSP offset "
                           "0x%08X",
                           (unsigned)off_psp);
    }
    end = size;
    if ((size_t)off_psar > (size_t)off_psp && (size_t)off_psar <= size) {
        end = (size_t)off_psar;
    }
    *slice = d + off_psp;
    *slice_size = end - (size_t)off_psp;
    return NK_PSP_OK;
}

/* Strip PBP and ~SCE outer layers until an inner module form remains. */
static int view_unwrap(NkPspCtx *ctx, View *v)
{
    int depth;
    for (depth = 0; depth < NK_MAX_WRAPPER_DEPTH; depth++) {
        int rc = refuse_firmware_form(ctx, v->data, v->size);
        if (rc != NK_PSP_OK) return rc;
        if (has_magic(v->data, v->size, "\0PBP", 4)) {
            const uint8_t *slice = NULL;
            size_t slice_size = 0;
            rc = pbp_slice(ctx, v->data, v->size, &slice, &slice_size);
            if (rc != NK_PSP_OK) return rc;
            v->data = slice;
            v->size = slice_size;
            v->wrappers++;
            v->pbp_layers++;
            continue;
        }
        if (has_magic(v->data, v->size, "~SCE", 4)) {
            uint32_t header_size;
            if (v->size < 8u) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "~SCE wrapper header is truncated");
            }
            header_size = rd32(v->data + 4);
            if (header_size < 8u || (size_t)header_size >= v->size) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "~SCE wrapper declares an impossible "
                                   "header size 0x%08X",
                                   (unsigned)header_size);
            }
            v->data += header_size;
            v->size -= header_size;
            v->wrappers++;
            continue;
        }
        break;
    }
    if (depth == NK_MAX_WRAPPER_DEPTH) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container nesting is too deep");
    }
    return NK_PSP_OK;
}

static NkContainerKind classify(const uint8_t *d, size_t size)
{
    if (has_magic(d, size, "~PSP", 4)) return NK_CTR_PSP;
    if (has_magic(d, size, "\x7f"
                           "ELF", 4)) return NK_CTR_PLAIN_ELF;
    if (has_magic(d, size, "\x1f"
                           "\x8b", 2)) return NK_CTR_GZIP;
    if (has_magic(d, size, "~SCE", 4)) return NK_CTR_SCE_WRAPPER;
    if (has_magic(d, size, "\0PBP", 4)) return NK_CTR_PBP;
    return NK_CTR_UNKNOWN;
}

/* ---------------------------- probe ---------------------------------- */

/* Classify and validate a view view_unwrap has already unwrapped; out is
   zeroed by the caller. decrypt_from calls this on its own unwrapped
   view, so the wrappers are stripped and the firmware forms refused once. */
static int probe_view(NkPspCtx *ctx, const View *view, Origin origin,
                      NkContainerInfo *out)
{
    View v = *view;
    int rc;
    out->wrappers_skipped = v.wrappers;
    out->kind = classify(v.data, v.size);
    switch (out->kind) {
    case NK_CTR_PSP:
        rc = validate_psp(ctx, v.data, v.size);
        if (rc != NK_PSP_OK) {
            out->kind = NK_CTR_UNKNOWN;
            return rc;
        }
        out->tag = rd32(v.data + 0xD0);
        out->compressed = (rd16(v.data + 0x06) & 1u) != 0u;
        return refuse_firmware_tag(ctx, &v, out->tag, origin);
    case NK_CTR_PLAIN_ELF:
    case NK_CTR_GZIP:
        return NK_PSP_OK;
    default:
        out->kind = NK_CTR_UNKNOWN;
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "unrecognized container magic "
                           "(expected ~PSP, ~SCE, PBP, gzip or ELF)");
    }
}

int nk_container_probe(NkPspCtx *ctx, const uint8_t *data, size_t size,
                       NkContainerInfo *out)
{
    View v;
    int rc;
    if (out == NULL) return NK_PSP_ERR_INTERNAL;
    memset(out, 0, sizeof(*out));
    if (data == NULL || size == 0u) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "input is empty");
    }
    v.data = data;
    v.size = size;
    v.wrappers = 0;
    v.pbp_layers = 0;
    rc = view_unwrap(ctx, &v);
    if (rc != NK_PSP_OK) return rc;
    return probe_view(ctx, &v, ORIGIN_UNKNOWN, out);
}

/* ------------------------ key-entry listing --------------------------- */

int nk_container_key_entries(const NkContainerInfo *info,
                             const NkKeystore *ks,
                             char entries[][80], int max_entries)
{
    int count = 0;
    if (info == NULL || entries == NULL || max_entries <= 0) return 0;
    if (info->kind != NK_CTR_PSP) return 0;

    snprintf(entries[count], (size_t)80, "prx.tag.0x%08X", info->tag);
    count++;
    if (count >= max_entries) return count;
    snprintf(entries[count], (size_t)80, "kirk.cmd1.key");
    count++;
    if (count >= max_entries) return count;
    if (ks != NULL) {
        NkPrxTagEntry recipe;
        if (nk_keystore_get_prx_tag(ks, info->tag, &recipe) == 0 &&
            recipe.have_code && recipe.code >= 0 && recipe.code <= 127) {
            snprintf(entries[count], (size_t)80, "kirk.keyvault.%d",
                     recipe.code);
            count++;
        }
    }
    return count;
}

/* --------------------------- decrypt ---------------------------------- */

/* Decompress a decrypted ~PSP payload (gzip or KL4E/KL3E by magic). */
static int decompress_payload(NkPspCtx *ctx, const uint8_t *payload,
                              size_t payload_size, int compressed,
                              uint32_t elf_size, uint8_t **out,
                              size_t *out_size)
{
    if (payload_size >= 2u && payload[0] == 0x1Fu && payload[1] == 0x8Bu) {
        char detail[128];
        uint8_t *d = NULL;
        size_t dlen = 0;
        int rc = nk_psp_inflate(payload, payload_size, &d, &dlen,
                                (size_t)elf_size, NK_DECOMP_CAP, detail,
                                sizeof(detail));
        if (rc != NK_INFLATE_OK) {
            return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                               "gzip payload failed to decompress: %s",
                               detail[0] != '\0' ? detail : "malformed stream");
        }
        *out = d;
        *out_size = dlen;
        return NK_PSP_OK;
    }
    if (compressed) {
        int is_kl4e = 0;
        if (has_magic(payload, payload_size, "KL4E", 4)) {
            is_kl4e = 1;
        } else if (!has_magic(payload, payload_size, "KL3E", 4)) {
            return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                               "compressed payload uses an unsupported "
                               "compression scheme");
        }
        if (elf_size < NK_ELF_MIN_SIZE || elf_size > NK_DECOMP_CAP) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "container declares an impossible compressed "
                               "size 0x%08X",
                               (unsigned)elf_size);
        }
        {
            uint8_t *d = (uint8_t *)malloc((size_t)elf_size);
            void *end = NULL;
            int rc;
            if (d == NULL) {
                return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                                   "out of memory decompressing payload");
            }
            /* The magic is 4 bytes; the stream may not read past the
             * decrypted payload we actually have.  decompress_kle returns the
             * decoded byte count (> 0) on success, a negative SCE error
             * (SCE_ERROR_INVALID_SIZE 0x80000104 or SCE_ERROR_INVALID_FORMAT
             * 0x80000108) on a malformed or truncated stream, and 0 for an
             * empty stored stream; only a positive count is a payload. */
            rc = decompress_kle(d, (int)elf_size,
                                (uint8_t *)(void *)(payload + 4),
                                (int)(payload_size - 4u), &end, is_kl4e);
            if (rc <= 0) {
                free(d);
                return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                                   "%s payload stream is malformed",
                                   is_kl4e ? "KL4E" : "KL3E");
            }
            *out = d;
            *out_size = (size_t)rc;
            return NK_PSP_OK;
        }
    }
    /* Uncompressed payload: take it as-is (caller owns a copy). */
    {
        uint8_t *d = (uint8_t *)malloc(payload_size);
        if (d == NULL) {
            return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                               "out of memory copying payload");
        }
        memcpy(d, payload, payload_size);
        *out = d;
        *out_size = payload_size;
        return NK_PSP_OK;
    }
}

/* The boundary for bytes of the given origin. Only this file names an origin
   other than ORIGIN_UNKNOWN, for bytes it read from a disc image itself. */
static int decrypt_from(NkPspCtx *ctx, Origin origin, const uint8_t *data,
                        size_t size, uint8_t **out, size_t *out_size)
{
    View v;
    NkContainerInfo info;
    int rc;

    if (out == NULL || out_size == NULL) return NK_PSP_ERR_INTERNAL;
    *out = NULL;
    *out_size = 0;
    if (data == NULL || size == 0u) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "input is empty");
    }
    if (ctx == NULL) return NK_PSP_ERR_INTERNAL;

    v.data = data;
    v.size = size;
    v.wrappers = 0;
    v.pbp_layers = 0;
    rc = view_unwrap(ctx, &v);
    if (rc != NK_PSP_OK) return rc;

    memset(&info, 0, sizeof(info));
    rc = probe_view(ctx, &v, origin, &info);
    if (rc != NK_PSP_OK) return rc;

    if (info.kind == NK_CTR_PLAIN_ELF) {
        uint8_t *copy = (uint8_t *)malloc(v.size);
        if (copy == NULL) {
            return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                               "out of memory copying ELF image");
        }
        memcpy(copy, v.data, v.size);
        *out = copy;
        *out_size = v.size;
        return NK_PSP_OK;
    }
    if (info.kind == NK_CTR_GZIP) {
        char detail[128];
        uint8_t *d = NULL;
        size_t dlen = 0;
        rc = nk_psp_inflate(v.data, v.size, &d, &dlen, 0u, NK_DECOMP_CAP,
                            detail, sizeof(detail));
        if (rc != NK_INFLATE_OK) {
            return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                               "gzip input failed to decompress: %s",
                               detail[0] != '\0' ? detail : "malformed stream");
        }
        /* The same ELF check the ~PSP path makes: a gzip of anything else (an
           image, an archive, a still-encrypted module) is not a decryption. */
        if (dlen < 4u || memcmp(d, "\x7f" "ELF", 4) != 0) {
            free(d);
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "decompressed gzip input is not an ELF image");
        }
        *out = d;
        *out_size = dlen;
        return NK_PSP_OK;
    }
    if (info.kind != NK_CTR_PSP) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "unrecognized container magic "
                           "(expected ~PSP, ~SCE, PBP, gzip or ELF)");
    }

    /* Encrypted (or plain) ~PSP module: validate, then decrypt. */
    {
        uint32_t psp_size = rd32(v.data + 0x2C);
        uint32_t elf_size = rd32(v.data + 0x28);
        int compressed = info.compressed;
        uint8_t *work;
        int decrypted_len;
        uint8_t *result = NULL;
        size_t result_size = 0;

        work = (uint8_t *)malloc((size_t)psp_size);
        if (work == NULL) {
            return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                               "out of memory decrypting container");
        }
        decrypted_len = pspDecryptPRX(ctx, v.data, work, psp_size, NULL);
        if (decrypted_len < 0) {
            free(work);
            return decrypted_len;
        }
        rc = decompress_payload(ctx, work, (size_t)decrypted_len, compressed,
                                elf_size, &result, &result_size);
        free(work);
        if (rc != NK_PSP_OK) return rc;

        if (result_size < 4u || memcmp(result, "\x7f"
                                                "ELF", 4) != 0) {
            free(result);
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "decrypted payload is not an ELF image");
        }
        *out = result;
        *out_size = result_size;
        return NK_PSP_OK;
    }
}

int nk_container_decrypt(NkPspCtx *ctx, const uint8_t *data, size_t size,
                         uint8_t **out, size_t *out_size)
{
    return decrypt_from(ctx, ORIGIN_UNKNOWN, data, size, out, out_size);
}

/* ----------------------- a file of a game disc image -------------------- */

#define NK_DISC_SECTOR 2048u
#define NK_DISC_PVD_OFFSET (16u * NK_DISC_SECTOR)
#define NK_DISC_MAX_DIR_SIZE (1024u * 1024u)
#define NK_DISC_MAX_SFO_SIZE (64u * 1024u)
#define NK_DISC_PATH_MAX 1200u
#define NK_DISC_PATH_MAX_PARTS 16

typedef struct DiscPath {
    const char *part[NK_DISC_PATH_MAX_PARTS];
    size_t len[NK_DISC_PATH_MAX_PARTS];
    int count;
} DiscPath;

static char ascii_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* An ISO9660 name against a path part: letters compare without case. */
static int name_is(const char *name, size_t name_len, const char *want)
{
    size_t want_len = strlen(want);
    if (name_len != want_len) return 0;
    for (size_t i = 0; i < name_len; i++) {
        if (ascii_upper(name[i]) != ascii_upper(want[i])) return 0;
    }
    return 1;
}

static int part_is(const DiscPath *path, int index, const char *want)
{
    return index < path->count && name_is(path->part[index], path->len[index], want);
}

/* Split "PSP_GAME/USRDIR/..." into its parts. A path that could name anything
   but one file below PSP_GAME/SYSDIR or PSP_GAME/USRDIR is refused: an empty,
   "." or ".." part, a backslash, a version suffix, a control character. */
static int disc_path_parse(NkPspCtx *ctx, const char *member_path, DiscPath *path)
{
    size_t total = member_path != NULL ? strlen(member_path) : 0u;
    const char *start = member_path;
    path->count = 0;
    if (total == 0u || total > NK_DISC_PATH_MAX) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the disc file path is empty or too long");
    }
    for (const char *p = member_path;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - start);
            if (len == 0u || len > 255u || (len == 1u && start[0] == '.') ||
                (len == 2u && start[0] == '.' && start[1] == '.') ||
                path->count >= NK_DISC_PATH_MAX_PARTS) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "the disc file path is not a plain path "
                                   "below PSP_GAME");
            }
            path->part[path->count] = start;
            path->len[path->count] = len;
            path->count++;
            if (*p == '\0') break;
            start = p + 1;
        } else if (*p == '\\' || *p == ';' || (unsigned char)*p < 0x20u ||
                   (unsigned char)*p == 0x7Fu) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "the disc file path is not a plain path below "
                               "PSP_GAME");
        }
    }
    if (path->count < 3 || !part_is(path, 0, "PSP_GAME") ||
        !(part_is(path, 1, "SYSDIR") || part_is(path, 1, "USRDIR"))) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the disc file path is not below PSP_GAME/SYSDIR "
                           "or PSP_GAME/USRDIR");
    }
    return NK_PSP_OK;
}

static int disc_read(NkPspCtx *ctx, const NkPsarSource *disc, uint64_t offset,
                     void *buf, size_t len)
{
    if (offset > disc->size || (uint64_t)len > disc->size - offset ||
        disc->read_at(disc->user, offset, buf, len) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "the disc image could not be read (it is "
                           "truncated or unreadable)");
    }
    return NK_PSP_OK;
}

/* Find one name in a directory extent. NK_PSP_OK with *found set (and the
   record's extent and kind) or a failure code. */
static int disc_dir_find(NkPspCtx *ctx, const NkPsarSource *disc, uint32_t dir_lba,
                         uint32_t dir_size, const char *name, size_t name_len,
                         int *found, uint32_t *lba, uint32_t *size, int *is_dir)
{
    uint8_t *dir;
    int rc;
    *found = 0;
    if (dir_size == 0u || dir_size > NK_DISC_MAX_DIR_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "a directory of the disc image has an impossible size");
    }
    dir = (uint8_t *)malloc(dir_size);
    if (dir == NULL) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                           "out of memory reading the disc image");
    }
    rc = disc_read(ctx, disc, (uint64_t)dir_lba * NK_DISC_SECTOR, dir, dir_size);
    if (rc != NK_PSP_OK) {
        free(dir);
        return rc;
    }
    for (uint32_t pos = 0; pos < dir_size;) {
        uint32_t rec_len = dir[pos];
        const uint8_t *r = dir + pos;
        uint32_t nlen;
        if (rec_len == 0u) { /* records do not cross a sector */
            pos = (pos / NK_DISC_SECTOR + 1u) * NK_DISC_SECTOR;
            continue;
        }
        if (rec_len < 34u || rec_len > dir_size - pos) break;
        nlen = r[32];
        /* An associated file (flag 0x04) shares a name with its file and is
           not it. The first other record of the name is the one taken. */
        if (33u + nlen <= rec_len && (r[25] & 0x04u) == 0u) {
            const char *n = (const char *)r + 33;
            const char *semi = (const char *)memchr(n, ';', nlen);
            size_t cmp_len = semi != NULL ? (size_t)(semi - n) : (size_t)nlen;
            if (cmp_len == name_len) {
                size_t k = 0;
                while (k < cmp_len && ascii_upper(n[k]) == ascii_upper(name[k])) k++;
                if (k == cmp_len) {
                    if ((r[25] & 0x80u) != 0u) { /* one record of a multi-extent file */
                        free(dir);
                        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                           "the file on the disc is stored in "
                                           "several extents, which a program "
                                           "file is not");
                    }
                    *lba = rd32(r + 2);
                    *size = rd32(r + 10);
                    *is_dir = (r[25] & 0x02u) != 0;
                    *found = 1;
                    break;
                }
            }
        }
        pos += rec_len;
    }
    free(dir);
    return NK_PSP_OK;
}

/* The root directory of an ISO9660 image. */
static int disc_root(NkPspCtx *ctx, const NkPsarSource *disc, uint32_t *lba,
                     uint32_t *size)
{
    uint8_t pvd[NK_DISC_SECTOR];
    int rc;
    if (disc->size < (uint64_t)NK_DISC_PVD_OFFSET + NK_DISC_SECTOR) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the disc image is too small to hold a volume descriptor");
    }
    rc = disc_read(ctx, disc, NK_DISC_PVD_OFFSET, pvd, sizeof(pvd));
    if (rc != NK_PSP_OK) return rc;
    if (pvd[0] != 1u || memcmp(pvd + 1, "CD001", 5) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the disc image has no ISO9660 primary volume descriptor");
    }
    *lba = rd32(pvd + 156 + 2);
    *size = rd32(pvd + 156 + 10);
    return NK_PSP_OK;
}

/* Resolve the parts of a path from the root: the last part must be a file,
   every other part a directory. *found is 0 when a part is not there. */
static int disc_lookup(NkPspCtx *ctx, const NkPsarSource *disc,
                       const char *const *part, const size_t *len, int count,
                       int *found, uint32_t *lba, uint32_t *size)
{
    uint32_t at_lba = 0, at_size = 0;
    int rc = disc_root(ctx, disc, &at_lba, &at_size);
    *found = 0;
    if (rc != NK_PSP_OK) return rc;
    for (int i = 0; i < count; i++) {
        int is_dir = 0, here = 0;
        rc = disc_dir_find(ctx, disc, at_lba, at_size, part[i], len[i], &here,
                           &at_lba, &at_size, &is_dir);
        if (rc != NK_PSP_OK) return rc;
        if (!here || is_dir != (i + 1 < count)) return NK_PSP_OK;
    }
    *lba = at_lba;
    *size = at_size;
    *found = 1;
    return NK_PSP_OK;
}

/* Whether the image identifies itself as a PSP game disc: PSP_GAME/PARAM.SFO
   with a DISC_ID. Anything short of that is "no", never a failure. */
static int disc_is_game(const NkPsarSource *disc)
{
    static const char *const part[] = {"PSP_GAME", "PARAM.SFO"};
    static const size_t len[] = {8u, 9u};
    NkPspCtx scratch;
    uint32_t lba = 0, size = 0;
    int found = 0, game = 0;
    uint8_t *sfo;
    char disc_id[16];
    nk_psp_ctx_init(&scratch, NULL);
    if (disc_lookup(&scratch, disc, part, len, 2, &found, &lba, &size) != NK_PSP_OK ||
        !found || size == 0u || size > NK_DISC_MAX_SFO_SIZE) {
        return 0;
    }
    sfo = (uint8_t *)malloc(size);
    if (sfo == NULL) return 0;
    if (disc_read(&scratch, disc, (uint64_t)lba * NK_DISC_SECTOR, sfo, size) == NK_PSP_OK &&
        nk_psp_sfo_string(sfo, size, "DISC_ID", disc_id, sizeof(disc_id)) == 0 &&
        disc_id[0] != '\0') {
        game = 1;
    }
    free(sfo);
    return game;
}

/* The path rules, which need no image: a plain path below the two program
   folders, and never the disc's own system software update, which is refused
   by path, whatever the file is, before one byte of the image is read. */
static int disc_path_admit(NkPspCtx *ctx, const char *member_path, DiscPath *path)
{
    int rc = disc_path_parse(ctx, member_path, path);
    if (rc != NK_PSP_OK) return rc;
    if (part_is(path, 1, "SYSDIR") && part_is(path, 2, "UPDATE")) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                           "PSP_GAME/SYSDIR/UPDATE holds the disc's system "
                           "software update; Nakagawa never decrypts firmware");
    }
    return NK_PSP_OK;
}

int nk_container_decrypt_disc_member(NkPspCtx *ctx, const NkPsarSource *disc,
                                     const char *member_path, uint8_t **out,
                                     size_t *out_size)
{
    DiscPath path;
    Origin origin = ORIGIN_UNKNOWN;
    uint32_t lba = 0, size = 0;
    uint8_t *data;
    int found = 0;
    int rc;

    if (out == NULL || out_size == NULL) return NK_PSP_ERR_INTERNAL;
    *out = NULL;
    *out_size = 0;
    if (ctx == NULL || disc == NULL || disc->read_at == NULL) return NK_PSP_ERR_INTERNAL;

    rc = disc_path_admit(ctx, member_path, &path);
    if (rc != NK_PSP_OK) return rc;
    rc = disc_lookup(ctx, disc, path.part, path.len, path.count, &found, &lba, &size);
    if (rc != NK_PSP_OK) return rc;
    if (!found) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "the disc image has no file at that path");
    }
    if (size == 0u || size > NK_DISC_MEMBER_MAX_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the file on the disc is %u bytes, outside what a "
                           "program file can be", (unsigned)size);
    }
    if (part_is(&path, 1, "USRDIR")) {
        origin = disc_is_game(disc) ? ORIGIN_GAME_DISC_USRDIR : ORIGIN_DISC_NOT_GAME;
    }
    data = (uint8_t *)malloc(size);
    if (data == NULL) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                           "out of memory reading the disc file");
    }
    rc = disc_read(ctx, disc, (uint64_t)lba * NK_DISC_SECTOR, data, size);
    if (rc == NK_PSP_OK) rc = decrypt_from(ctx, origin, data, size, out, out_size);
    free(data);
    return rc;
}

int nk_container_decrypt_disc_member_file(NkPspCtx *ctx, const char *iso_path,
                                          const char *member_path, uint8_t **out,
                                          size_t *out_size)
{
    NkPsarSource disc;
    DiscPath path;
    FILE *file = NULL;
    int rc;
    if (out == NULL || out_size == NULL) return NK_PSP_ERR_INTERNAL;
    *out = NULL;
    *out_size = 0;
    if (ctx == NULL || iso_path == NULL) return NK_PSP_ERR_INTERNAL;
    rc = disc_path_admit(ctx, member_path, &path); /* before the image is opened */
    if (rc != NK_PSP_OK) return rc;
    rc = nk_psar_source_open_file(ctx, iso_path, &file, &disc);
    if (rc != NK_PSP_OK) return rc;
    rc = nk_container_decrypt_disc_member(ctx, &disc, member_path, out, out_size);
    fclose(file);
    return rc;
}
