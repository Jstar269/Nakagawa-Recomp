/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * nk_decrypt -- command-line entry for the built-in PSP decryption
 * boundary (issue #295).  The Python tooling (tools/nk_core) calls this
 * binary.  It decrypts with the built-in game tables;
 * --key-file replaces them with a JSON key file, which the
 * synthetic tests use.  A missing entry fails closed naming the exact
 * entry required, and firmware is refused before any key lookup.
 *
 * Subcommands:
 *   kat    --vector {aes-ecb|aes-cbc|aes-cmac|sha1}
 *          Print the published known-answer vector as JSON (exit 0).
 *   probe  --in FILE [--key-file FILE]
 *          Report which key entries the container needs (exit 0).
 *   decrypt [--key-file FILE] --in FILE --out FILE
 *          Run the full production boundary; the output file is written
 *          only on success.  A failure prints its nk_psp_status_name word
 *          (KEY_REQUIRED followed by the entry, MALFORMED_INPUT,
 *          INTEGRITY_FAILURE and so on; docs/FAILURE_CODES.md lists them)
 *          and exits with that status's code.
 *   npumdimg [--key-file FILE] --in EBOOT.PBP --out FILE.iso
 *          Turn a PSN release of a UMD game (a PBP whose DATA.PSAR is an
 *          NPUMDIMG image) back into its ISO: written beside FILE.iso,
 *          checked (ISO9660 volume descriptor, PARAM.SFO DISC_ID equal to the
 *          package's), then renamed into place.  A PSOne Classic prints
 *          PSONE_CLASSIC_UNSUPPORTED and an updater FIRMWARE_OUT_OF_SCOPE
 *          before any key lookup; console-bound DRM prints CONTEXT_REQUIRED.
 */

/* This program has no platform layer (tools/nk_core/decrypt_tool.py builds it
 * from the crypto sources alone), so the host calls it needs are local: the
 * UTF-8 command line on Windows, and the removal, replacing rename and private
 * open of the output file. POSIX needs the feature-test macro before any
 * system header, as strict -std=c99 hides open() flags and fdopen(). */
#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_platform.h"
#include "nk_psp_aes.h"
#include "nk_psp_container.h"
#include "nk_psp_crypto.h"
#include "nk_psp_keystore.h"
#include "nk_psp_npumdimg.h"
#include "nk_psp_sha1.h"
#include "nk_shared_helpers.h"

/* --------------------------- helpers --------------------------------- */

static void to_hex(const u8 *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0Fu];
    }
    out[n * 2] = '\0';
}

/* The whole input file, 1 byte to 512 MiB. */
static int read_file(const char *path, u8 **data, size_t *size)
{
    unsigned char *buf = NULL;
    size_t len = 0;
    if (nk_read_file_bounded(path, 512u * 1024u * 1024u, &buf, &len) != NK_READ_FILE_OK)
        return -1;
    if (len == 0) {
        free(buf);
        return -1;
    }
    *data = buf;
    *size = len;
    return 0;
}

/* Paths are UTF-8; Win32 converts strictly, as nk_fopen_utf8 does. A new
 * output file is one only the user can read: POSIX creates it 0600 (O_EXCL,
 * so no file or symlink already there is reused), as nk_platform_fopen_private
 * does for other per-title private data; Win32 inherits the per-user data
 * folder's ACLs. */
#if defined(_WIN32) || defined(_WIN64)
static wchar_t *utf8_to_wide_alloc(const char *path)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide;
    if (n <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static void remove_file(const char *path)
{
    wchar_t *wide = utf8_to_wide_alloc(path);
    if (wide != NULL) (void)DeleteFileW(wide);
    free(wide);
}

static int replace_file(const char *from, const char *to)
{
    wchar_t *wfrom = utf8_to_wide_alloc(from);
    wchar_t *wto = utf8_to_wide_alloc(to);
    int rc = -1;
    if (wfrom != NULL && wto != NULL &&
        MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        rc = 0;
    free(wfrom);
    free(wto);
    return rc;
}

static FILE *open_private_new(const char *path)
{
    return nk_fopen_utf8(path, "wb");
}
#else
static void remove_file(const char *path)
{
    (void)unlink(path);
}

static int replace_file(const char *from, const char *to)
{
    return rename(from, to);
}

static FILE *open_private_new(const char *path)
{
    int flags = O_WRONLY | O_CREAT | O_EXCL;
    int fd;
    FILE *f;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    f = fdopen(fd, "wb");
    if (f == NULL) close(fd);
    return f;
}
#endif

/* The decrypted module is written beside `path` and renamed into place only
 * once every byte was written and the file closed, so a short write (a full
 * disk) or a failed close never leaves a truncated module at `path`, where
 * the tooling would keep it as a finished copy. The temporary file is
 * removed on any failure. */
static int write_file(const char *path, const u8 *data, size_t size)
{
    static const char suffix[] = ".tmp";
    size_t len = strlen(path);
    char *temp;
    FILE *f;
    int ok;
    if (len > (size_t)-1 - sizeof(suffix)) return -1;
    temp = (char *)malloc(len + sizeof(suffix));
    if (temp == NULL) return -1;
    memcpy(temp, path, len);
    memcpy(temp + len, suffix, sizeof(suffix));
    remove_file(temp); /* one a killed run left behind */
    f = open_private_new(temp);
    if (f == NULL) {
        free(temp);
        return -1;
    }
    ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    if (ok && replace_file(temp, path) != 0) ok = 0;
    if (!ok) remove_file(temp);
    free(temp);
    return ok ? 0 : -1;
}

static int file_present(const char *path)
{
    FILE *f = nk_fopen_utf8(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* --------------------------- diagnostics ------------------------------ */

/* Prints the status word for `rc` (plus the context's message when there is
 * one) and returns the matching process exit code. Callers return that value
 * from main(), so the word and the code are what the Python tooling sees. The
 * word is nk_psp_status_name's, the one name the failure-code taxonomy
 * (docs/FAILURE_CODES.md) gives each result; the exit codes are the contract
 * tools/nk_core/decrypt_tool.py reads. */
static int print_diagnostic(int rc, const NkPspCtx *ctx)
{
    const char *entry = (ctx != NULL && ctx->missing_entry[0] != '\0')
                            ? ctx->missing_entry
                            : NULL;
    const char *msg = (ctx != NULL && ctx->message[0] != '\0')
                          ? ctx->message
                          : NULL;
    int exit_code;
    switch (rc) {
    case NK_PSP_ERR_INPUT: exit_code = 1; break;
    case NK_PSP_ERR_MISSING_KEY: exit_code = 3; break;
    case NK_PSP_ERR_FORMAT: exit_code = 4; break;
    case NK_PSP_ERR_INTEGRITY: exit_code = 5; break;
    case NK_PSP_ERR_KEYFILE: exit_code = 6; break;
    case NK_PSP_ERR_UNSUPPORTED: exit_code = 7; break;
    case NK_PSP_ERR_DECOMPRESS: exit_code = 8; break;
    case NK_PSP_ERR_FIRMWARE: exit_code = 10; break;
    case NK_PSP_ERR_CONTEXT: exit_code = 11; break;
    case NK_PSP_ERR_PSONE: exit_code = 12; break;
    case NK_PSP_ERR_CANCELLED: exit_code = 13; break;
    default: rc = NK_PSP_ERR_INTERNAL; exit_code = 9; break;
    }
    if (rc == NK_PSP_ERR_MISSING_KEY) {
        printf("%s %s\n", nk_psp_status_name(rc), entry != NULL ? entry : "(unknown)");
        printf("this executable needs key entry %s\n",
               entry != NULL ? entry : "(unknown)");
    } else {
        printf("%s\n", nk_psp_status_name(rc));
    }
    if (msg != NULL) printf("%s\n", msg);
    return exit_code;
}

/* ------------------------------ kat ----------------------------------- */

/*
 * Published known-answer vectors: FIPS 197 appendix C.1 (AES-128),
 * RFC 4493 section 4 (AES-CMAC), RFC 3174 (SHA-1).  These are standard
 * public test vectors used to prove the primitives; they are not key
 * material for any protected content.
 */
static const u8 kat_aes_plain[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
static const u8 kat_aes_k[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
/* Public SP 800-38A F.2.1 plaintext (two blocks). The aes-cbc vector encrypts it
 * under kat_cmac_k, the public F.2.1 key, from an all-zero starting block. The
 * output is zero-IV CBC, not the F.2.1 ciphertext (see tools/test_psp_decrypt.py). */
static const u8 kat_cbc_plain[32] = {
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
    0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
    0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51};
static const u8 kat_cmac_k[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
static const u8 kat_cmac_msg[16] = {
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
    0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};

static int cmd_kat(const char *vector)
{
    char hex[129];
    if (vector == NULL) {
        fprintf(stderr, "kat: --vector is required\n");
        return 2;
    }
    if (strcmp(vector, "aes-ecb") == 0) {
        AES_ctx ctx;
        u8 cipher[16];
        u8 back[16];
        AES_set_key(&ctx, kat_aes_k, 128);
        AES_encrypt(&ctx, kat_aes_plain, cipher);
        AES_decrypt(&ctx, cipher, back);
        to_hex(cipher, 16, hex);
        printf("{\"cipher\":\"%s\",", hex);
        to_hex(back, 16, hex);
        printf("\"roundtrip\":\"%s\"}\n", hex);
        return 0;
    }
    if (strcmp(vector, "aes-cbc") == 0) {
        AES_ctx ctx;
        u8 cipher[32];
        u8 back[32];
        AES_set_key(&ctx, kat_cmac_k, 128);
        AES_cbc_encrypt(&ctx, kat_cbc_plain, cipher, 32);
        AES_cbc_decrypt(&ctx, cipher, back, 32);
        to_hex(cipher, 32, hex);
        printf("{\"cipher\":\"%s\",", hex);
        to_hex(back, 32, hex);
        printf("\"roundtrip\":\"%s\"}\n", hex);
        return 0;
    }
    if (strcmp(vector, "aes-cmac") == 0) {
        AES_ctx ctx;
        u8 msg[16];
        u8 mac[16];
        AES_set_key(&ctx, kat_cmac_k, 128);
        /* RFC 4493 section 4, example 1 (empty message). */
        AES_CMAC(&ctx, msg, 0, mac);
        to_hex(mac, 16, hex);
        printf("{\"mac\":\"%s\",", hex);
        /* RFC 4493 section 4, example 2 (one full block). */
        memcpy(msg, kat_cmac_msg, 16);
        AES_CMAC(&ctx, msg, 16, mac);
        to_hex(mac, 16, hex);
        printf("\"mac_msg16\":\"%s\"}\n", hex);
        return 0;
    }
    if (strcmp(vector, "sha1") == 0) {
        SHA_CTX sha;
        u8 digest[20];
        BYTE msg[3];
        msg[0] = (BYTE)'a';
        msg[1] = (BYTE)'b';
        msg[2] = (BYTE)'c';
        SHAInit(&sha);
        SHAUpdate(&sha, msg, 3);
        SHAFinal(digest, &sha);
        to_hex(digest, 20, hex);
        printf("{\"digest\":\"%s\"}\n", hex);
        return 0;
    }
    fprintf(stderr, "kat: unknown vector '%s'\n", vector);
    return 2;
}

/* Fill a KeyStore: the built-in game tables, or only the given key file.
 * An absent key file leaves the store empty, so a container then names the
 * entry it needs.  Returns 0, or the exit code after printing why. */
static int load_keys(NkKeystore *ks, const char *key_path)
{
    char key_err[256];
    if (key_path == NULL) {
        if (nk_keystore_load_builtin(ks, key_err, sizeof(key_err)) != NK_PSP_OK) {
            printf("INTERNAL_ERROR %s\n", key_err);
            return 9;
        }
        return 0;
    }
    if (!file_present(key_path)) {
        printf("note: no key file at %s; continuing with an empty key "
               "store\n", key_path);
        return 0;
    }
    if (nk_keystore_load_file(ks, key_path, key_err, sizeof(key_err)) != NK_PSP_OK) {
        printf("KEYFILE_INVALID %s\n", key_err);
        return 6;
    }
    return 0;
}

/* ----------------------------- probe ---------------------------------- */

static const char *kind_name(NkContainerKind kind)
{
    switch (kind) {
    case NK_CTR_PLAIN_ELF: return "elf";
    case NK_CTR_PSP: return "psp";
    case NK_CTR_SCE_WRAPPER: return "sce-wrapper";
    case NK_CTR_PBP: return "pbp";
    case NK_CTR_GZIP: return "gzip";
    default: return "unknown";
    }
}

static int cmd_probe(const char *in_path, const char *key_path)
{
    u8 *data = NULL;
    size_t size = 0;
    NkPspCtx ctx;
    NkContainerInfo info;
    NkKeystore *ks = NULL;
    char entries[8][80];
    int entry_count;
    int i;
    int rc;

    if (in_path == NULL) {
        fprintf(stderr, "probe: --in is required\n");
        return 2;
    }
    if (read_file(in_path, &data, &size) != 0) {
        printf("IO_FAILURE cannot read input file %s\n", in_path);
        return 1;
    }
    nk_psp_ctx_init(&ctx, NULL);
    rc = nk_container_probe(&ctx, data, size, &info);
    if (rc != NK_PSP_OK) {
        free(data);
        return print_diagnostic(rc, &ctx);
    }

    /* The key store enriches the listing (keyvault slot). */
    ks = nk_keystore_create();
    if (ks == NULL) {
        free(data);
        printf("INTERNAL_ERROR out of memory\n");
        return 9;
    }
    rc = load_keys(ks, key_path);
    if (rc != 0) {
        nk_keystore_free(ks);
        free(data);
        return rc;
    }

    printf("container %s\n", kind_name(info.kind));
    entry_count = nk_container_key_entries(&info, ks, entries, 8);
    for (i = 0; i < entry_count; i++) {
        printf("%s\n", entries[i]);
    }
    if (entry_count == 0) {
        printf("(no key entries needed)\n");
    }
    nk_keystore_free(ks);
    free(data);
    return 0;
}

/* ---------------------------- decrypt ---------------------------------- */

static int cmd_decrypt(const char *key_path, const char *in_path,
                       const char *out_path)
{
    u8 *data = NULL;
    size_t size = 0;
    u8 *plain = NULL;
    size_t plain_size = 0;
    NkKeystore *ks = NULL;
    NkPspCtx ctx;
    int rc;

    if (in_path == NULL || out_path == NULL) {
        fprintf(stderr, "decrypt: --in and --out are required\n");
        return 2;
    }
    if (read_file(in_path, &data, &size) != 0) {
        printf("IO_FAILURE cannot read input file %s\n", in_path);
        return 1;
    }

    ks = nk_keystore_create();
    if (ks == NULL) {
        free(data);
        printf("INTERNAL_ERROR out of memory\n");
        return 9;
    }
    rc = load_keys(ks, key_path);
    if (rc != 0) {
        nk_keystore_free(ks);
        free(data);
        return rc;
    }

    nk_psp_ctx_init(&ctx, ks);
    rc = nk_container_decrypt(&ctx, data, size, &plain, &plain_size);
    if (rc != NK_PSP_OK) {
        int code = print_diagnostic(rc, &ctx);
        nk_keystore_free(ks);
        free(data);
        return code;
    }

    /* Success: only now is the output file created. */
    if (write_file(out_path, plain, plain_size) != 0) {
        printf("IO_FAILURE cannot write %s\n", out_path);
        free(plain);
        nk_keystore_free(ks);
        free(data);
        return 1;
    }
    printf("DECRYPT_OK %u bytes\n", (unsigned)plain_size);
    free(plain);
    nk_keystore_free(ks);
    free(data);
    return 0;
}

/* ---------------------------- npumdimg --------------------------------- */

static int cmd_npumdimg(const char *key_path, const char *in_path,
                        const char *out_path)
{
    NkKeystore *ks = NULL;
    NkPspCtx ctx;
    NkPbpInfo info;
    int rc;

    if (in_path == NULL || out_path == NULL) {
        fprintf(stderr, "npumdimg: --in and --out are required\n");
        return 2;
    }
    ks = nk_keystore_create();
    if (ks == NULL) {
        printf("INTERNAL_ERROR out of memory\n");
        return 9;
    }
    rc = load_keys(ks, key_path);
    if (rc != 0) {
        nk_keystore_free(ks);
        return rc;
    }
    nk_psp_ctx_init(&ctx, ks);
    rc = nk_npumdimg_convert_file(&ctx, in_path, out_path, NULL, &info);
    nk_keystore_free(ks);
    if (rc != NK_PSP_OK) return print_diagnostic(rc, &ctx);
    printf("NPUMDIMG_OK disc_id=%s\n", info.disc_id);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: nk_decrypt kat --vector {aes-ecb|aes-cbc|aes-cmac|sha1}\n"
            "       nk_decrypt probe --in FILE [--key-file FILE]\n"
            "       nk_decrypt decrypt [--key-file FILE] --in FILE --out FILE\n"
            "       nk_decrypt npumdimg [--key-file FILE] --in EBOOT.PBP --out FILE.iso\n");
}

static int run(int argc, char **argv)
{
    const char *cmd;
    const char *vector = NULL;
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *key_path = NULL;
    int i;

    if (argc < 2) {
        usage();
        return 2;
    }
    cmd = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--vector") == 0 && i + 1 < argc) {
            vector = argv[++i];
        } else if (strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
            in_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--key-file") == 0 && i + 1 < argc) {
            key_path = argv[++i];
        } else {
            fprintf(stderr, "nk_decrypt: unknown argument '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (strcmp(cmd, "kat") == 0) return cmd_kat(vector);
    if (strcmp(cmd, "probe") == 0) return cmd_probe(in_path, key_path);
    if (strcmp(cmd, "decrypt") == 0)
        return cmd_decrypt(key_path, in_path, out_path);
    if (strcmp(cmd, "npumdimg") == 0)
        return cmd_npumdimg(key_path, in_path, out_path);

    fprintf(stderr, "nk_decrypt: unknown command '%s'\n", cmd);
    usage();
    return 2;
}

/* The paths arrive as UTF-16 on Windows, and the narrow argv the C runtime
 * builds from them is in the ANSI code page, which turns a profile folder name
 * with an accented or CJK letter into bytes nk_fopen_utf8 refuses. The
 * arguments are rebuilt as UTF-8 from the wide command line, as nk_disc_expand
 * does. */
int main(int argc, char **argv)
{
#if defined(_WIN32) || defined(_WIN64)
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **utf8;
    int i;
    int rc;
    (void)argc;
    (void)argv;
    if (wargv == NULL) {
        fprintf(stderr, "nk_decrypt: cannot read the command line\n");
        return 2;
    }
    utf8 = (char **)calloc((size_t)wargc + 1u, sizeof(char *));
    if (utf8 == NULL) {
        LocalFree(wargv);
        printf("INTERNAL_ERROR out of memory\n");
        return 9;
    }
    for (i = 0; i < wargc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        utf8[i] = len > 0 ? (char *)malloc((size_t)len) : NULL;
        if (utf8[i] == NULL ||
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, utf8[i], len, NULL, NULL) <= 0) {
            int j;
            for (j = 0; j <= i; j++) free(utf8[j]);
            free(utf8);
            LocalFree(wargv);
            fprintf(stderr, "nk_decrypt: cannot convert the command line to UTF-8\n");
            return 2;
        }
    }
    LocalFree(wargv);
    rc = run(wargc, utf8);
    for (i = 0; i < wargc; i++) free(utf8[i]);
    free(utf8);
    return rc;
#else
    return run(argc, argv);
#endif
}
