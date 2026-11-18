/* SPDX-License-Identifier: GPL-3.0-or-later
 *
  Draan proudly presents:

  With huge help from community:
  coyotebean, Davee, hitchhikr, kgsws, liquidzigong, Mathieulh, Proxima,
  SilverSpring

  ******************** KIRK-ENGINE ********************
  An Open-Source implementation of KIRK (PSP crypto engine) algorithms.
  Includes also additional routines for hash forging.

  ********************

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  ------------------------------------------------------------------
  SPDX-License-Identifier: GPL-3.0-or-later
  Ported from libkirk/kirk_engine.c of John-K/pspdecrypt
  (https://github.com/John-K/pspdecrypt), commit
  c156627db7634d395c380c0a9589130f603307fc, licensed GPL-3.0-or-later.

  Modified for this project:
    - every command takes an NkPspCtx and fetches key material from the
      run-time KeyStore; the embedded key vault, KIRK CMD1/CMD16 keys,
      XOR pads and fuse-ID state are removed entirely;
    - ECDSA verification goes through the project's own nk_psp_ec
      (upstream ec.c/bn.c are GPL-2.0-only and were not ported);
    - all buffer arithmetic is bounds-checked before any bulk copy
      (upstream assumed well-formed sizes);
    - key generation (0x0C), PRNG (0x0E), signing (0x10) and content
      encryption (0x00) refuse fail-closed because they would require
      private material this boundary never holds;
    - 2026-11-18: kirk4 and kirk7 refuse an empty pass as well as an
      unaligned one.
    - 2026-11-18: the CMD1 wrapping key is expanded from the caller's
      context for each command and wiped after it (no process-wide AES
      schedule); CMD1 reuses the header keys its CMAC check unwrapped;
      CMD1, CMD4, CMD7 and CMD11 refuse an output size smaller than what
      they write; CMD4/CMD7 and kirk4/kirk7 share one CBC routine each.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_psp_aes.h"
#include "nk_psp_ec.h"
#include "nk_psp_kirk.h"
#include "nk_psp_keystore.h"
#include "nk_psp_sha1.h"

static int kirk_keyvault_key(NkPspCtx *ctx, int keyslot, u8 key_out[16])
{
    char name[32];
    const uint8_t *value = NULL;
    size_t len = 0;
    if (keyslot < 0 || keyslot >= 0x80) return KIRK_INVALID_SEED_CODE;
    snprintf(name, sizeof(name), "kirk.keyvault.%d", keyslot);
    if (nk_psp_need(ctx, name, &value, &len) != NK_PSP_OK)
        return KIRK_INVALID_SEED_CODE;
    if (len != 16) {
        return nk_psp_fail(ctx, NK_PSP_ERR_KEYFILE, name,
                           "key entry %s must be 16 bytes", name);
    }
    memcpy(key_out, value, 16);
    return KIRK_OPERATION_SUCCESS;
}

/* Expand the KIRK CMD1 wrapping key held by ctx's KeyStore. The schedule
 * lives in the caller's frame for one command, so two contexts never share
 * it and the caller wipes it after use. */
static int kirk_cmd1_key(NkPspCtx *ctx, AES_ctx *aes)
{
    const uint8_t *value = NULL;
    size_t len = 0;
    if (nk_psp_need(ctx, "kirk.cmd1.key", &value, &len) != NK_PSP_OK)
        return KIRK_NOT_INITIALIZED;
    if (len != 16) {
        return (int)nk_psp_fail(ctx, NK_PSP_ERR_KEYFILE, "kirk.cmd1.key",
                                "key entry kirk.cmd1.key must be 16 bytes");
    }
    AES_set_key(aes, value, 128);
    return KIRK_OPERATION_SUCCESS;
}

int kirk_init(NkPspCtx *ctx)
{
    AES_ctx aes;
    int rc = kirk_cmd1_key(ctx, &aes);
    nk_secure_zero(&aes, sizeof(aes));
    return rc;
}

/* ------------------------- HELPERS ------------------------------- */

static int checked_round16(u32 value, u32 *out)
{
    if (value > 0xFFFFFFE0u) return -1; /* would overflow the rounding */
    *out = (value + 15u) & ~15u;
    return 0;
}

/* Bounds-check a CMD1-style layout: header + data_offset + chk_size. */
static int cmd1_bounds(int size, const KIRK_CMD1_HEADER *header, u32 *chk_size)
{
    if (size < (int)sizeof(KIRK_CMD1_HEADER)) return -1;
    if (checked_round16(header->data_size, chk_size) != 0) return -1;
    u64 total = (u64)sizeof(KIRK_CMD1_HEADER) +
                (u64)header->data_offset + (u64)*chk_size;
    return total <= (u64)size ? 0 : -1;
}

/* CMAC check of a CMD1 header + payload under the wrapping key cmd1. On
 * success header_keys holds the unwrapped AES and CMAC keys, so CMD1 does
 * not unwrap them a second time. */
static int cmd10_verify(AES_ctx *cmd1, u8 *inbuff, int insize,
                        u8 header_keys[32])
{
    KIRK_CMD1_HEADER *header = (KIRK_CMD1_HEADER *)inbuff;
    u8 cmac_header_hash[16];
    u8 cmac_data_hash[16];
    AES_ctx cmac_key;
    u32 chk_size;
    int rc;

    if (!(header->mode == KIRK_MODE_CMD1 || header->mode == KIRK_MODE_CMD2 ||
          header->mode == KIRK_MODE_CMD3)) return KIRK_INVALID_MODE;
    if (header->data_size == 0) return KIRK_DATA_SIZE_ZERO;
    if (cmd1_bounds(insize, header, &chk_size) != 0) return KIRK_INVALID_SIZE;
    if (header->mode != KIRK_MODE_CMD1)
        return KIRK_SIG_CHECK_INVALID; /* checks for commands 2 & 3 not enabled */

    AES_cbc_decrypt(cmd1, inbuff, header_keys, 32);
    AES_set_key(&cmac_key, header_keys + 16, 128);
    AES_CMAC(&cmac_key, inbuff + 0x60, 0x30, cmac_header_hash);
    AES_CMAC(&cmac_key, inbuff + 0x60,
             0x30 + (int)chk_size + header->data_offset, cmac_data_hash);
    nk_secure_zero(&cmac_key, sizeof(cmac_key));

    if (memcmp(cmac_header_hash, header->CMAC_header_hash, 16) != 0)
        rc = KIRK_HEADER_HASH_INVALID;
    else if (memcmp(cmac_data_hash, header->CMAC_data_hash, 16) != 0)
        rc = KIRK_DATA_HASH_INVALID;
    else
        rc = KIRK_OPERATION_SUCCESS;
    return rc;
}

/* KIRK command 0x0A: CMAC integrity check of a CMD1 header + payload. */
int kirk_CMD10(NkPspCtx *ctx, u8 *inbuff, int insize)
{
    u8 header_keys[32];
    AES_ctx cmd1;
    int rc = kirk_cmd1_key(ctx, &cmd1);
    if (rc == KIRK_OPERATION_SUCCESS)
        rc = cmd10_verify(&cmd1, inbuff, insize, header_keys);
    nk_secure_zero(&cmd1, sizeof(cmd1));
    nk_secure_zero(header_keys, sizeof(header_keys));
    return rc;
}

/* Load curve parameters (and optionally a public point) from the KeyStore.
 * Both KIRK curves share the prime and a coefficient, so those entries carry
 * no curve suffix (kirk.ecdsa.p, kirk.ecdsa.a); b, the order and the base
 * point are per curve (kirk.ecdsa.b1 ... kirk.ecdsa.gy2), matching the names
 * the KeyStore accepts. */
static int load_curve(NkPspCtx *ctx, const char *suffix, NkEcParams *params)
{
    static const struct { const char *field; size_t offset; int per_curve; } fields[] = {
        {"p", offsetof(NkEcParams, p), 0},
        {"a", offsetof(NkEcParams, a), 0},
        {"b", offsetof(NkEcParams, b), 1},
        {"n", offsetof(NkEcParams, n), 1},
        {"gx", offsetof(NkEcParams, gx), 1},
        {"gy", offsetof(NkEcParams, gy), 1},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        char name[40];
        const uint8_t *value = NULL;
        size_t len = 0;
        snprintf(name, sizeof(name), "kirk.ecdsa.%s%s", fields[i].field,
                 fields[i].per_curve ? suffix : "");
        if (nk_psp_need(ctx, name, &value, &len) != NK_PSP_OK)
            return NK_PSP_ERR_MISSING_KEY;
        uint8_t *dst = (uint8_t *)params + fields[i].offset;
        if (len == NK_EC_BYTES) {
            memcpy(dst, value, NK_EC_BYTES);
        } else if (len == NK_EC_BYTES + 1 && value[0] == 0) {
            memcpy(dst, value + 1, NK_EC_BYTES); /* tolerate a leading zero */
        } else {
            return (int)nk_psp_fail(ctx, NK_PSP_ERR_KEYFILE, name,
                                    "key entry %s must be %u bytes",
                                    name, (unsigned)NK_EC_BYTES);
        }
    }
    return NK_PSP_OK;
}

/* ECDSA check of a CMD1 header + payload signed with the curve-1 key. */
static int cmd1_ecdsa_verify(NkPspCtx *ctx, u8 *inbuff, int size)
{
    KIRK_CMD1_ECDSA_HEADER *eheader = (KIRK_CMD1_ECDSA_HEADER *)inbuff;
    NkEcParams params;
    u8 pub[40];
    u8 header_hash[20];
    u8 data_hash[20];
    SHA_CTX sha;
    const uint8_t *px = NULL;
    const uint8_t *py = NULL;
    size_t plen = 0;
    int rc = load_curve(ctx, "1", &params);
    if (rc != NK_PSP_OK) return KIRK_INVALID_OPERATION;
    if (nk_psp_need(ctx, "kirk.ecdsa.px1", &px, &plen) != NK_PSP_OK ||
        nk_psp_need(ctx, "kirk.ecdsa.py1", &py, &plen) != NK_PSP_OK ||
        plen != NK_EC_BYTES) {
        return KIRK_INVALID_OPERATION;
    }
    memcpy(pub, px, NK_EC_BYTES);
    memcpy(pub + NK_EC_BYTES, py, NK_EC_BYTES);

    SHAInit(&sha);
    SHAUpdate(&sha, (u8 *)eheader + 0x60, 0x30);
    SHAFinal(header_hash, &sha);
    if (nk_ec_verify(&params, pub, pub + NK_EC_BYTES, header_hash,
                     eheader->header_sig_r, eheader->header_sig_s) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INTEGRITY, NULL,
                           "KIRK header signature did not verify "
                           "(integrity check failed; refusing to decrypt)");
    }
    SHAInit(&sha);
    SHAUpdate(&sha, (u8 *)eheader + 0x60, size - 0x60);
    SHAFinal(data_hash, &sha);
    if (nk_ec_verify(&params, pub, pub + NK_EC_BYTES, data_hash,
                     eheader->data_sig_r, eheader->data_sig_s) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INTEGRITY, NULL,
                           "KIRK data signature did not verify "
                           "(integrity check failed; refusing to decrypt)");
    }
    return KIRK_OPERATION_SUCCESS;
}

/* KIRK command 0x01: private decryption of a wrapped CMD1 container. The
 * payload decrypts to round16(data_size) bytes, which outsize must hold. */
int kirk_CMD1(NkPspCtx *ctx, u8 *outbuff, int outsize, u8 *inbuff, int size)
{
    KIRK_CMD1_HEADER *header = (KIRK_CMD1_HEADER *)inbuff;
    u8 header_keys[32];
    AES_ctx cmd1;
    AES_ctx k1;
    u32 chk_size;
    int rc;

    if (size < (int)sizeof(KIRK_CMD1_HEADER)) return KIRK_INVALID_SIZE;
    rc = kirk_cmd1_key(ctx, &cmd1);
    if (rc != KIRK_OPERATION_SUCCESS) return rc;

    if (header->mode != KIRK_MODE_CMD1) {
        rc = KIRK_INVALID_MODE;
    } else if (cmd1_bounds(size, header, &chk_size) != 0) {
        rc = KIRK_INVALID_SIZE;
    } else if (header->data_size == 0) {
        rc = KIRK_DATA_SIZE_ZERO;
    } else if (outsize < 0 || (u64)chk_size > (u64)outsize) {
        rc = KIRK_INVALID_SIZE;
    } else if (header->ecdsa_hash == 1) {
        rc = cmd1_ecdsa_verify(ctx, inbuff, size);
        if (rc == KIRK_OPERATION_SUCCESS)
            AES_cbc_decrypt(&cmd1, inbuff, header_keys, 32);
    } else {
        rc = cmd10_verify(&cmd1, inbuff, size, header_keys);
        if (rc == KIRK_HEADER_HASH_INVALID || rc == KIRK_DATA_HASH_INVALID) {
            rc = (int)nk_psp_fail(ctx, NK_PSP_ERR_INTEGRITY, NULL,
                                  "KIRK header CMAC did not verify "
                                  "(integrity check failed; refusing to decrypt)");
        }
    }

    if (rc == KIRK_OPERATION_SUCCESS) {
        AES_set_key(&k1, header_keys, 128);
        AES_cbc_decrypt(&k1, inbuff + sizeof(KIRK_CMD1_HEADER) + header->data_offset,
                        outbuff, (int)header->data_size);
        nk_secure_zero(&k1, sizeof(k1));
    }
    nk_secure_zero(&cmd1, sizeof(cmd1));
    nk_secure_zero(header_keys, sizeof(header_keys));
    return rc;
}

/* KIRK commands 0x04 and 0x07: AES-128-CBC under a keyvault slot. CMD4
 * (mode 4) encrypts and writes after a header-sized gap; CMD7 (mode 5)
 * decrypts to the start of outbuff. Either way outsize must hold it. */
static int kirk_cbc_command(NkPspCtx *ctx, u8 *outbuff, int outsize,
                            u8 *inbuff, int size, int encrypt)
{
    KIRK_AES128CBC_HEADER *header = (KIRK_AES128CBC_HEADER *)inbuff;
    const size_t out_at = encrypt ? sizeof(KIRK_AES128CBC_HEADER) : 0u;
    u8 key[16];
    AES_ctx aesKey;

    if (size < (int)sizeof(KIRK_AES128CBC_HEADER)) return KIRK_INVALID_SIZE;
    if (header->mode != (encrypt ? KIRK_MODE_ENCRYPT_CBC : KIRK_MODE_DECRYPT_CBC))
        return KIRK_INVALID_MODE;
    if (header->data_size == 0) return KIRK_DATA_SIZE_ZERO;
    if (header->data_size < 0 ||
        (u64)sizeof(KIRK_AES128CBC_HEADER) + (u64)header->data_size > (u64)size)
        return KIRK_INVALID_SIZE;
    if ((header->data_size & 15) != 0) return KIRK_INVALID_SIZE;
    if (outsize < 0 || (u64)out_at + (u64)header->data_size > (u64)outsize)
        return KIRK_INVALID_SIZE;
    if (kirk_keyvault_key(ctx, header->keyseed, key) != KIRK_OPERATION_SUCCESS)
        return KIRK_INVALID_SEED_CODE;
    AES_set_key(&aesKey, key, 128);
    if (encrypt) {
        AES_cbc_encrypt(&aesKey, inbuff + sizeof(KIRK_AES128CBC_HEADER),
                        outbuff + out_at, header->data_size);
    } else {
        AES_cbc_decrypt(&aesKey, inbuff + sizeof(KIRK_AES128CBC_HEADER),
                        outbuff, header->data_size);
    }
    nk_secure_zero(key, sizeof(key));
    nk_secure_zero(&aesKey, sizeof(aesKey));
    return KIRK_OPERATION_SUCCESS;
}

int kirk_CMD4(NkPspCtx *ctx, u8 *outbuff, int outsize, u8 *inbuff, int size)
{
    return kirk_cbc_command(ctx, outbuff, outsize, inbuff, size, 1);
}

int kirk_CMD7(NkPspCtx *ctx, u8 *outbuff, int outsize, u8 *inbuff, int size)
{
    return kirk_cbc_command(ctx, outbuff, outsize, inbuff, size, 0);
}

/* A raw AES-CBC pass under a keyvault slot (kirk4 encrypts, kirk7 decrypts). */
static int kirk_cbc_pass(NkPspCtx *ctx, u8 *outbuff, const u8 *inbuff,
                         size_t size, int keyId, int encrypt)
{
    u8 key[16];
    AES_ctx aesKey;
    if (kirk_keyvault_key(ctx, keyId, key) != KIRK_OPERATION_SUCCESS)
        return KIRK_INVALID_SEED_CODE;
    /* An empty pass is refused with the unaligned ones: the CBC routines
     * handle one block before they look at the length. */
    if (size == 0 || (size & 15u) != 0) {
        nk_secure_zero(key, sizeof(key));
        return (int)nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                                "empty or unaligned KIRK pass (%u bytes)",
                                (unsigned)size);
    }
    AES_set_key(&aesKey, key, 128);
    if (encrypt) {
        AES_cbc_encrypt(&aesKey, inbuff, outbuff, (int)size);
    } else {
        AES_cbc_decrypt(&aesKey, inbuff, outbuff, (int)size);
    }
    nk_secure_zero(key, sizeof(key));
    nk_secure_zero(&aesKey, sizeof(aesKey));
    return KIRK_OPERATION_SUCCESS;
}

int kirk4(NkPspCtx *ctx, u8 *outbuff, const u8 *inbuff, size_t size, int keyId)
{
    return kirk_cbc_pass(ctx, outbuff, inbuff, size, keyId, 1);
}

int kirk7(NkPspCtx *ctx, u8 *outbuff, const u8 *inbuff, size_t size, int keyId)
{
    return kirk_cbc_pass(ctx, outbuff, inbuff, size, keyId, 0);
}

/* KIRK command 0x0B: SHA-1 over the payload following the 4-byte header;
 * the 20-byte digest must fit in outsize. */
int kirk_CMD11(NkPspCtx *ctx, u8 *outbuff, int outsize, u8 *inbuff, int size)
{
    KIRK_SHA1_HEADER *header = (KIRK_SHA1_HEADER *)inbuff;
    SHA_CTX sha;
    (void)ctx;
    if (size < (int)sizeof(KIRK_SHA1_HEADER)) return KIRK_INVALID_SIZE;
    if (header->data_size == 0 || size == 0) return KIRK_DATA_SIZE_ZERO;
    if ((u64)sizeof(KIRK_SHA1_HEADER) + (u64)header->data_size > (u64)size)
        return KIRK_INVALID_SIZE;
    if (outsize < 20) return KIRK_INVALID_SIZE;
    SHAInit(&sha);
    SHAUpdate(&sha, inbuff + sizeof(KIRK_SHA1_HEADER), (int)header->data_size);
    SHAFinal(outbuff, &sha);
    return KIRK_OPERATION_SUCCESS;
}

/* Commands that need private/embedded material refuse fail-closed. */
static int refuse_private(NkPspCtx *ctx, const char *what)
{
    return (int)nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                            "%s is not part of the built-in decryption "
                            "boundary; only verification and decryption "
                            "paths are enabled", what);
}

int kirk_CMD12(NkPspCtx *ctx, u8 *outbuff, int outsize)
{
    (void)outbuff; (void)outsize;
    return refuse_private(ctx, "KIRK command 0x0C (ECDSA key generation)");
}

int kirk_CMD14(NkPspCtx *ctx, u8 *outbuff, int outsize)
{
    (void)outbuff; (void)outsize;
    return refuse_private(ctx, "KIRK command 0x0E (pseudo-random generation)");
}

int kirk_CMD16(NkPspCtx *ctx, u8 *outbuff, int outsize, u8 *inbuff, int insize)
{
    (void)outbuff; (void)outsize; (void)inbuff; (void)insize;
    return refuse_private(ctx, "KIRK command 0x10 (ECDSA signing)");
}

/* KIRK command 0x0D: scalar multiplication of a public point (curve 2). */
int kirk_CMD13(NkPspCtx *ctx, u8 *outbuff, int outsize, u8 *inbuff, int insize)
{
    NkEcParams params;
    KIRK_CMD13_BUFFER *input = (KIRK_CMD13_BUFFER *)inbuff;
    int rc;

    if (outsize != 0x28) return KIRK_INVALID_SIZE;
    if (insize != 0x3C) return KIRK_INVALID_SIZE;
    rc = load_curve(ctx, "2", &params);
    if (rc != NK_PSP_OK) return KIRK_INVALID_OPERATION;
    if (nk_ec_point_mul(&params, input->multiplier,
                        input->public_key.x, input->public_key.y,
                        outbuff, outbuff + NK_EC_BYTES) != 0) {
        return (int)nk_psp_fail(ctx, NK_PSP_ERR_INTEGRITY, NULL,
                                "KIRK point multiplication input was invalid");
    }
    return KIRK_OPERATION_SUCCESS;
}

/* KIRK command 0x11: ECDSA verification (curve 2, public key in input). */
int kirk_CMD17(NkPspCtx *ctx, u8 *inbuff, int insize)
{
    NkEcParams params;
    KIRK_CMD17_BUFFER *sig = (KIRK_CMD17_BUFFER *)inbuff;
    int rc;

    if (insize != 0x64) return KIRK_INVALID_SIZE;
    rc = load_curve(ctx, "2", &params);
    if (rc != NK_PSP_OK) return KIRK_INVALID_OPERATION;
    if (nk_ec_verify(&params, sig->public_key.x, sig->public_key.y,
                     sig->message_hash, sig->signature.r,
                     sig->signature.s) != 0) {
        return KIRK_SIG_CHECK_INVALID;
    }
    return KIRK_OPERATION_SUCCESS;
}

/* sce-like dispatch (ctx-threaded port of sceUtilsBufferCopyWithRange). */
int sceUtilsBufferCopyWithRange(NkPspCtx *ctx, u8 *outbuff, int outsize,
                                u8 *inbuff, int insize, int cmd)
{
    switch (cmd) {
    case KIRK_CMD_DECRYPT_PRIVATE:
        return kirk_CMD1(ctx, outbuff, outsize, inbuff, insize);
    case KIRK_CMD_ENCRYPT_IV_0:
        return kirk_CMD4(ctx, outbuff, outsize, inbuff, insize);
    case KIRK_CMD_DECRYPT_IV_0:
        return kirk_CMD7(ctx, outbuff, outsize, inbuff, insize);
    case KIRK_CMD_PRIV_SIGN_CHECK: return kirk_CMD10(ctx, inbuff, insize);
    case KIRK_CMD_SHA1_HASH:
        return kirk_CMD11(ctx, outbuff, outsize, inbuff, insize);
    case KIRK_CMD_ECDSA_GEN_KEYS: return kirk_CMD12(ctx, outbuff, outsize);
    case KIRK_CMD_ECDSA_MULTIPLY_POINT:
        return kirk_CMD13(ctx, outbuff, outsize, inbuff, insize);
    case KIRK_CMD_PRNG: return kirk_CMD14(ctx, outbuff, outsize);
    case KIRK_CMD_ECDSA_SIGN:
        return kirk_CMD16(ctx, outbuff, outsize, inbuff, insize);
    case KIRK_CMD_ECDSA_VERIFY: return kirk_CMD17(ctx, inbuff, insize);
    default:
        return (int)nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                                "unknown KIRK command 0x%02X", (unsigned)cmd);
    }
}
