/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Container-level probe and decrypt orchestration for the built-in PSP
 * decryption boundary (issue #295).  Project-authored from the format
 * rules the pipeline meets (see docs/SETUP.md); no key material here.
 *
 * Forms handled:
 *   - "~PSP" executables (EBOOT.BIN/PRX), encrypted or plain;
 *   - "~SCE" outer wrappers (u32 header size at +4, inner module follows);
 *   - PBP packages (DATA.PSP extracted at offset 0x20);
 *   - gzip-compressed payloads after decryption (and KL4E/KL3E streams);
 *   - plain ELF32/MIPS images (pass-through).
 *
 * Every key, seed or XOR pad arrives through the run-time KeyStore; a
 * missing entry fails closed naming the exact entry name required.
 * Firmware (an updater PSAR archive, a bare KIRK block in the IPL form, a
 * module whose tag is in nk_psp_firmware_tags) is refused with
 * NK_PSP_ERR_FIRMWARE before any key lookup.  Two of those tags are also the
 * tags of library modules games ship on their own disc; only
 * nk_container_decrypt_disc_member, which reads the file out of the game's
 * disc image itself, opens them (issue #3235).
 */

#ifndef NK_PSP_CONTAINER_H
#define NK_PSP_CONTAINER_H

#include "nk_psp_crypto.h"
#include "nk_psp_keystore.h"
#include "nk_psp_npumdimg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NkContainerKind {
    NK_CTR_UNKNOWN = 0,
    NK_CTR_PLAIN_ELF,   /* 7f 45 4c 46 */
    NK_CTR_PSP,         /* "~PSP" executable/PRX */
    NK_CTR_SCE_WRAPPER, /* "~SCE" outer wrapper */
    NK_CTR_PBP,         /* "\0PBP" package */
    NK_CTR_GZIP         /* 1f 8b gzip stream */
} NkContainerKind;

typedef struct NkContainerInfo {
    NkContainerKind kind; /* innermost recognizable form */
    unsigned tag;         /* ~PSP tag at 0xD0 (0 when not a ~PSP) */
    int compressed;       /* ~PSP comp_attribute bit 0 */
    int wrappers_skipped; /* how many PBP/~SCE layers were unwrapped */
} NkContainerInfo;

/*
 * Identify a container WITHOUT needing any keys: unwraps PBP/~SCE
 * layers and validates structure.  Returns NK_PSP_OK, or a fail-closed
 * code (NK_PSP_ERR_FORMAT for a structurally malformed container) with
 * ctx->message filled when ctx is non-NULL.
 */
int nk_container_probe(NkPspCtx *ctx, const uint8_t *data, size_t size,
                       NkContainerInfo *out);

/*
 * Append the canonical key-entry names every ~PSP container needs into
 * entries (one NUL-terminated name per slot, at most max_entries).  The
 * list is the minimum, not complete: it always names the ~PSP tag entry
 * and "kirk.cmd1.key", and when the KeyStore already holds the tag recipe
 * the "code" also resolves the matching "kirk.keyvault.<code>" entry.  A
 * container whose header only decryption reveals to be ECDSA-signed
 * (type 6) also needs the curve-1 "kirk.ecdsa.*" entries, which are not
 * listed; a missing one is named by decrypt (ctx->missing_entry).
 * Returns the number of names written.
 */
int nk_container_key_entries(const NkContainerInfo *info,
                             const NkKeystore *ks,
                             char entries[][80], int max_entries);

/*
 * Full production boundary: unwrap PBP/~SCE, validate and decrypt the
 * ~PSP container, decompress (gzip or KL4E/KL3E) and verify the result
 * is an ELF32 image.  On success returns NK_PSP_OK and hands the caller
 * a freshly malloc'd buffer (*out, *out_size; caller frees).  On
 * failure returns a negative NK_PSP_ERR_* code with ctx filled in
 * (missing entry name and message) and *out untouched.
 */
int nk_container_decrypt(NkPspCtx *ctx, const uint8_t *data, size_t size,
                         uint8_t **out, size_t *out_size);

/* The largest program file the boundary reads out of a disc image. */
#define NK_DISC_MEMBER_MAX_SIZE (64u * 1024u * 1024u)

/*
 * The same boundary for one file of a disc image, which this call reads
 * itself: member_path is the file's path inside the image, '/'-separated,
 * below PSP_GAME/SYSDIR or PSP_GAME/USRDIR (letters compare without case;
 * any other path is NK_PSP_ERR_FORMAT, a file that is not there
 * NK_PSP_ERR_INPUT).  Because the call knows where the bytes came from:
 *   - anything at or below PSP_GAME/SYSDIR/UPDATE, the disc's system software
 *     update, is NK_PSP_ERR_FIRMWARE by path, before the image is read;
 *   - a file below PSP_GAME/USRDIR of an image that has a PSP_GAME/PARAM.SFO
 *     with a DISC_ID may carry a tag of nk_psp_disc_library_tags when its
 *     ~PSP header declares a user module (no kernel or VSH attribute bit,
 *     decrypt mode 4) and it is not inside a PBP package.  The KeyStore must
 *     hold those recipes (nk_keystore_load_builtin_disc_libraries).
 * Every other firmware form and tag is refused exactly as
 * nk_container_decrypt refuses it.
 */
int nk_container_decrypt_disc_member(NkPspCtx *ctx, const NkPsarSource *disc,
                                     const char *member_path, uint8_t **out,
                                     size_t *out_size);
/* The file form: iso_path is a UTF-8 path to a plain ISO9660 image. */
int nk_container_decrypt_disc_member_file(NkPspCtx *ctx, const char *iso_path,
                                          const char *member_path, uint8_t **out,
                                          size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_CONTAINER_H */
