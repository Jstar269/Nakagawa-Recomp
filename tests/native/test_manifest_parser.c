/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_title_manifest.h"
#include "nk_launch.h"
#include "nk_platform.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The manifest parser test links the PGF object file but not the runtime GE. */
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
void sr_oor(uint32_t address, uint32_t value, int store) {
    (void)address; (void)value; (void)store;
    abort();
}

static const char * const CANONICAL_VALID_BASE =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"id\": \"size-boundary-test\",\n"
    "  \"display_name\": \"Size Boundary Test\",\n"
    "  \"kind\": \"synthetic\",\n"
    "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
    "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
    "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
    "  \"hle_profile\": \"standard\",\n"
    "  \"feature_requirements\": [\"allegrex\"],\n"
    "  \"verification_profile\": \"smoke\"\n"
    "}\n";

static void test_manifest_size_boundary(void) {
    printf("[MANIFEST_TEST] Testing 256 KB size boundary (LIMIT_CLASS = %s)...\n", NK_MANIFEST_LIMIT_CLASS);

    size_t base_len = strlen(CANONICAL_VALID_BASE);
    assert(base_len < NK_MANIFEST_MAX_BYTES);

    /* 1. Exactly NK_MANIFEST_MAX_BYTES (padded with whitespace) -> MUST ACCEPT */
    char *max_accepted = (char *)malloc(NK_MANIFEST_MAX_BYTES + 1);
    assert(max_accepted != NULL);
    memcpy(max_accepted, CANONICAL_VALID_BASE, base_len);
    memset(max_accepted + base_len, ' ', NK_MANIFEST_MAX_BYTES - base_len);
    max_accepted[NK_MANIFEST_MAX_BYTES] = '\0';

    char err_buf[512] = {0};
    NkTitleEntry entry;
    bool ok_max = nk_title_manifest_parse_buffer(max_accepted, NK_MANIFEST_MAX_BYTES, false, &entry, err_buf, sizeof(err_buf));
    if (!ok_max) {
        fprintf(stderr, "[MANIFEST_TEST] Max accepted failed: %s\n", err_buf);
    }
    assert(ok_max);
    assert(strcmp(entry.id, "size-boundary-test") == 0);
    free(max_accepted);

    /* 2. Exactly NK_MANIFEST_MAX_BYTES + 1 -> MUST REJECT WITH PRODUCT_SECURITY_POLICY */
    size_t over_len = NK_MANIFEST_MAX_BYTES + 1;
    char *over_limit = (char *)malloc(over_len + 1);
    assert(over_limit != NULL);
    memcpy(over_limit, CANONICAL_VALID_BASE, base_len);
    memset(over_limit + base_len, ' ', over_len - base_len);
    over_limit[over_len] = '\0';

    memset(err_buf, 0, sizeof(err_buf));
    bool ok_over = nk_title_manifest_parse_buffer(over_limit, over_len, false, &entry, err_buf, sizeof(err_buf));
    assert(!ok_over);
    assert(strstr(err_buf, NK_MANIFEST_LIMIT_CLASS) != NULL || strstr(err_buf, "limit") != NULL);
    free(over_limit);

    printf("[MANIFEST_TEST] Size boundary test PASSED! (Exact 256KB accepted, 256KB+1 rejected)\n");
}

static void test_manifest_unicode_and_surrogates(void) {
    printf("[MANIFEST_TEST] Testing Unicode escape decoding, UTF-16 surrogate pairs, and UTF-8 emission...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Japanese escaped \u vs UTF-8 */
    const char *jp_escaped =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"unicode-jp-test\",\n"
        "  \"display_name\": \"\\u65e5\\u672c\\u8a9e\\u30bf\\u30a4\\u30c8\\u30eb\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    assert(nk_title_manifest_parse_buffer(jp_escaped, strlen(jp_escaped), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.display_name, "日本語タイトル") == 0);
    assert(entry.has_executable_base == true);
    assert(entry.executable_base == 0);
    assert(entry.has_executable_entry == true);

    /* 2. UTF-16 surrogate pair for supplementary plane codepoint U+1F600 (GRINNING FACE 😀) */
    const char *surrogate_json =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"surrogate-test\",\n"
        "  \"display_name\": \"Emoji \\uD83D\\uDE00 Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    assert(nk_title_manifest_parse_buffer(surrogate_json, strlen(surrogate_json), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.display_name, "Emoji \xF0\x9F\x98\x80 Test") == 0);

    /* 3. Lone high surrogate -> MUST REJECT */
    const char *lone_high =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"lone-high-test\",\n"
        "  \"display_name\": \"Lone \\uD83D High\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(lone_high, strlen(lone_high), false, &entry, err, sizeof(err)));

    /* 4. Lone low surrogate -> MUST REJECT */
    const char *lone_low =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"lone-low-test\",\n"
        "  \"display_name\": \"Lone \\uDE00 Low\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(lone_low, strlen(lone_low), false, &entry, err, sizeof(err)));

    printf("[MANIFEST_TEST] Unicode and surrogate tests PASSED!\n");
}

static void test_manifest_numbers_and_addresses(void) {
    printf("[MANIFEST_TEST] Testing integer validation, float rejection, and address parsing...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Fractional number (1.5) for integer field -> MUST REJECT */
    const char *float_entry =
        "{\n"
        "  \"schema_version\": 1.5,\n"
        "  \"id\": \"float-test\",\n"
        "  \"display_name\": \"Float Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(float_entry, strlen(float_entry), false, &entry, err, sizeof(err)));

    /* 2. Number with exponent (1e5) for integer field -> MUST REJECT */
    const char *exp_entry =
        "{\n"
        "  \"schema_version\": 1e0,\n"
        "  \"id\": \"exp-test\",\n"
        "  \"display_name\": \"Exp Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(exp_entry, strlen(exp_entry), false, &entry, err, sizeof(err)));

    /* 3. A well-formed "0x" plus 8-hex-digit address string -> MUST REJECT: addresses are JSON
       integers in assets/title_manifest.schema.json and tools/title_manifest.py uint(), and the
       native parser accepts exactly what they accept (audit 2026-11-17). */
    const char *hex_addr =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"hex-addr-test\",\n"
        "  \"display_name\": \"Hex Address Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": \"0x08800000\", \"entry\": \"0x08804000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(hex_addr, strlen(hex_addr), false, &entry, err, sizeof(err)));
    assert(strstr(err, "$.executable") != NULL);

    /* 3b. The same addresses as integers -> MUST ACCEPT */
    const char *int_addr =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"int-addr-test\",\n"
        "  \"display_name\": \"Integer Address Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 142606336, \"entry\": 142622720, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(nk_title_manifest_parse_buffer(int_addr, strlen(int_addr), false, &entry, err, sizeof(err)));
    assert(entry.executable_base == 0x08800000U);
    assert(entry.executable_entry == 0x08804000U);

    /* 3c. Address strings are refused everywhere an address is read: a module load address, a
       runtime binding and a dispatch alias. */
    const char *hex_load_address =
        "{\"schema_version\": 1, \"id\": \"hex-load-test\", \"display_name\": \"T\", \"kind\": \"synthetic\", "
        "\"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, "
        "\"modules\": [{\"name\": \"a.prx\", \"load_address\": \"0x08900000\", \"required\": true, \"role\": \"guest-prx\"}], "
        "\"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, "
        "\"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(hex_load_address, strlen(hex_load_address), false, &entry, err, sizeof(err)));
    assert(strstr(err, "load_address") != NULL);
    const char *hex_binding =
        "{\"schema_version\": 1, \"id\": \"hex-binding-test\", \"display_name\": \"T\", \"kind\": \"synthetic\", "
        "\"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, "
        "\"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, "
        "\"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\", "
        "\"runtime_bindings\": {\"schema_version\": 1, \"fallback_entry\": \"0x08801000\"}}";
    assert(!nk_title_manifest_parse_buffer(hex_binding, strlen(hex_binding), false, &entry, err, sizeof(err)));
    assert(strstr(err, "fallback_entry") != NULL);
    const char *hex_alias =
        "{\"schema_version\": 1, \"id\": \"hex-alias-test\", \"display_name\": \"T\", \"kind\": \"synthetic\", "
        "\"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, "
        "\"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, "
        "\"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\", "
        "\"runtime_bindings\": {\"schema_version\": 1, \"dispatch_aliases\": [{\"from\": \"0x08802000\", \"to\": 142610432}]}}";
    assert(!nk_title_manifest_parse_buffer(hex_alias, strlen(hex_alias), false, &entry, err, sizeof(err)));
    assert(strstr(err, ".from") != NULL);

    /* 4. Overlong address string ("0x088000000" -> 9 hex digits) -> MUST REJECT */
    const char *overlong_hex =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"bad-hex-test\",\n"
        "  \"display_name\": \"Bad Hex Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": \"0x088000000\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(overlong_hex, strlen(overlong_hex), false, &entry, err, sizeof(err)));

    /* 5. Short address string ("0x1") -> MUST REJECT */
    const char *short_hex =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"bad-hex-test\",\n"
        "  \"display_name\": \"Bad Hex Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": \"0x1\", \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(short_hex, strlen(short_hex), false, &entry, err, sizeof(err)));

    printf("[MANIFEST_TEST] Numbers and addresses tests PASSED!\n");
}

static void test_manifest_identifiers_and_disc_ids(void) {
    printf("[MANIFEST_TEST] Testing canonical identifier regex and disc ID validation...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Identifier with uppercase -> MUST REJECT (canonical is strictly lowercase) */
    const char *upper_id =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"UPPERCASE-id\",\n"
        "  \"display_name\": \"Upper ID Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(upper_id, strlen(upper_id), false, &entry, err, sizeof(err)));

    /* 2. Identifier starting with symbol -> MUST REJECT */
    const char *sym_id =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"_bad_start\",\n"
        "  \"display_name\": \"Sym ID Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(sym_id, strlen(sym_id), false, &entry, err, sizeof(err)));

    /* 3. Valid lowercase identifier with digits, '.', '_', '-' -> MUST ACCEPT */
    const char *valid_id =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"title.v1_alpha-2\",\n"
        "  \"display_name\": \"Valid ID Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(nk_title_manifest_parse_buffer(valid_id, strlen(valid_id), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.id, "title.v1_alpha-2") == 0);

    /* 4. Retail disc ID with invalid format (lowercase) -> MUST REJECT */
    const char *bad_disc =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"retail-bad-disc\",\n"
        "  \"display_name\": \"Retail Bad Disc\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"ucus98701\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(bad_disc, strlen(bad_disc), false, &entry, err, sizeof(err)));

    /* 5. Retail disc ID with compatible_revisions containing primary disc ID -> MUST REJECT */
    const char *compat_dup_primary =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"retail-compat-dup\",\n"
        "  \"display_name\": \"Retail Compat Dup Primary\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCUS98701\", \"region\": \"NA\", \"revision_policy\": \"explicit-compatible-revisions\", \"compatible_revisions\": [\"UCUS98701\"]},\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";
    assert(!nk_title_manifest_parse_buffer(compat_dup_primary, strlen(compat_dup_primary), false, &entry, err, sizeof(err)));

    printf("[MANIFEST_TEST] Identifiers and disc ID tests PASSED!\n");
}

static void test_manifest_mutations(void) {
    printf("[MANIFEST_TEST] Testing schema mutations and hostile edge cases...\n");
    char err[512];
    NkTitleEntry entry;

    /* 1. Missing required field (display_name missing) */
    const char *no_display = "{\"schema_version\": 1, \"id\": \"test\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(no_display, strlen(no_display), false, &entry, err, sizeof(err)));
    assert(strstr(err, "missing required field") != NULL);

    /* 2. Unknown field */
    const char *unknown_field = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"unknown_key\": 123, \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(unknown_field, strlen(unknown_field), false, &entry, err, sizeof(err)));
    assert(strstr(err, "unknown field") != NULL);

    /* 3. Duplicate JSON object key */
    const char *dup_key = "{\"schema_version\": 1, \"id\": \"test\", \"id\": \"test2\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_key, strlen(dup_key), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate JSON object key") != NULL);

    /* 4. Wrong type for schema_version */
    const char *wrong_type = "{\"schema_version\": \"1\", \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(wrong_type, strlen(wrong_type), false, &entry, err, sizeof(err)));

    /* 5. Unsupported schema_version */
    const char *bad_sv = "{\"schema_version\": 2, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(bad_sv, strlen(bad_sv), false, &entry, err, sizeof(err)));

    /* 6. Negative/overflow integer */
    const char *neg_entry = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": -5, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(neg_entry, strlen(neg_entry), false, &entry, err, sizeof(err)));

    /* 7. Malformed hex */
    const char *bad_hex = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": \"0xZZZZ\", \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(bad_hex, strlen(bad_hex), false, &entry, err, sizeof(err)));

    /* 8. Duplicate module binding */
    /* The bases must lie in user RAM: with load_address 1/2 the guest-module window rule fires
       first, so the fixture would test that rule rather than the duplicate-name comparison it
       is named for. Both parsers reject a guest-prx base of 1 for the same reason. */
    const char *dup_mod = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [{\"name\": \"a.prx\", \"load_address\": 143654912, \"required\": true, \"role\": \"guest-prx\"}, {\"name\": \"A.PRX\", \"load_address\": 145752064, \"required\": true, \"role\": \"guest-prx\"}], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_mod, strlen(dup_mod), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate module name") != NULL);

    /* 9. Duplicate feature requirement */
    const char *dup_feat = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [\"allegrex\", \"allegrex\"], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(dup_feat, strlen(dup_feat), false, &entry, err, sizeof(err)));
    assert(strstr(err, "duplicate feature requirement") != NULL);

    /* 10. Trailing garbage */
    const char *trailing = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"} GARBAGE";
    assert(!nk_title_manifest_parse_buffer(trailing, strlen(trailing), false, &entry, err, sizeof(err)));
    assert(strstr(err, "Trailing garbage") != NULL);

    /* 11. Invalid UTF-8 */
    const char invalid_utf8[] = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"\xFF\xFE\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(invalid_utf8, sizeof(invalid_utf8) - 1, false, &entry, err, sizeof(err)));
    assert(strstr(err, "UTF-8") != NULL);

    /* 12. Excessively nested JSON (> 16 levels) */
    const char *deep_nest = "{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":{\"a\":1}}}}}}}}}}}}}}}}}}";
    assert(!nk_title_manifest_parse_buffer(deep_nest, strlen(deep_nest), false, &entry, err, sizeof(err)));
    assert(strstr(err, "nesting exceeds") != NULL);

    /* 13. Reserved VFPU target range rejection */
    const char *vfpu_target = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\", \"runtime_bindings\": {\"schema_version\": 1, \"dispatch_aliases\": [{\"from\": 1073745920, \"to\": 142610432}]}}";
    assert(!nk_title_manifest_parse_buffer(vfpu_target, strlen(vfpu_target), false, &entry, err, sizeof(err)));
    assert(strstr(err, "core VFPU dispatch-target range") != NULL);

    /* 14. Non-retail kind with disc object -> REJECT */
    const char *synth_with_disc = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"disc\": {\"id\": \"UCUS98701\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"}, \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(synth_with_disc, strlen(synth_with_disc), false, &entry, err, sizeof(err)));
    assert(strstr(err, "disc object is forbidden") != NULL);

    /* 15. Retail kind without disc object -> REJECT */
    const char *retail_no_disc = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"retail\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(retail_no_disc, strlen(retail_no_disc), false, &entry, err, sizeof(err)));
    assert(strstr(err, "retail kind requires disc object") != NULL);

    /* 16. Trailing comma in an object -> REJECT
     *
     * Python's json module rejects {"a":1,}. The native parser consumed the
     * comma, returned to the top of its loop and accepted the closing brace,
     * so a manifest could exist that only the native overlay loader would
     * take -- a differential-parser divergence, which is exactly what the
     * two implementations are meant not to have. */
    const char *obj_trailing_comma = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\",}";
    assert(!nk_title_manifest_parse_buffer(obj_trailing_comma, strlen(obj_trailing_comma), false, &entry, err, sizeof(err)));
    assert(strstr(err, "Trailing comma") != NULL);

    /* 17. Trailing comma in an array -> REJECT (same state transition) */
    const char *arr_trailing_comma = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\",]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(arr_trailing_comma, strlen(arr_trailing_comma), false, &entry, err, sizeof(err)));
    assert(strstr(err, "Trailing comma") != NULL);

    /* 18. Guest module base outside user RAM -> REJECT
     *
     * A guest module's code AND data live at load_address, so a base outside user RAM lets
     * translated code run while every data write is dropped (the late-PRX bases once sat at
     * 0x322xxxxx). The Python parser has always rejected this; the native parser accepted it,
     * which is precisely the differential-parser divergence the two implementations must not
     * have. */
    const char *guest_base_outside_ram = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [{\"name\": \"late.prx\", \"load_address\": 840957952, \"required\": false, \"role\": \"optional-guest-prx\"}], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(guest_base_outside_ram, strlen(guest_base_outside_ram), false, &entry, err, sizeof(err)));
    assert(strstr(err, "guest module base must lie in user RAM") != NULL);

    /* 19. Guest module with guest_path + load_address_evidence -> ACCEPT
     *
     * Both are schema v1 module declarations, and the Python parser validates them. Rejecting
     * them as unknown fields made the native parser refuse any manifest that names where a
     * module image lives -- including the operator's own retail title manifest. */
    const char *guest_declared = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [{\"name\": \"libfont.prx\", \"load_address\": 146800640, \"required\": false, \"role\": \"optional-guest-prx\", \"guest_path\": \"disc0:/PSP_GAME/USRDIR/module/libfont.prx\", \"load_address_evidence\": \"measured-hw\"}], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(nk_title_manifest_parse_buffer(guest_declared, strlen(guest_declared), false, &entry, err, sizeof(err)));

    /* 20. guest_path that is not an absolute PSP device path -> REJECT */
    const char *bad_guest_path = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [{\"name\": \"libfont.prx\", \"load_address\": 146800640, \"required\": false, \"role\": \"optional-guest-prx\", \"guest_path\": \"C:/PSP_GAME/USRDIR/module/libfont.prx\"}], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(bad_guest_path, strlen(bad_guest_path), false, &entry, err, sizeof(err)));
    assert(strstr(err, "absolute PSP device path") != NULL);

    /* 21. load_address_evidence outside the published classes -> REJECT */
    const char *bad_evidence = "{\"schema_version\": 1, \"id\": \"test\", \"display_name\": \"T\", \"kind\": \"synthetic\", \"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, \"modules\": [{\"name\": \"libfont.prx\", \"load_address\": 146800640, \"required\": false, \"role\": \"optional-guest-prx\", \"load_address_evidence\": \"trust-me\"}], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, \"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"}";
    assert(!nk_title_manifest_parse_buffer(bad_evidence, strlen(bad_evidence), false, &entry, err, sizeof(err)));
    assert(strstr(err, "unsupported evidence class") != NULL);

    printf("[MANIFEST_TEST] Schema mutation tests PASSED!\n");
}

static void test_executable_entry_vs_fallback(void) {
    printf("[MANIFEST_TEST] Testing executable.entry vs runtime_bindings.fallback_entry projection contract...\n");
    char err[512];
    NkTitleEntry entry;

    /* Fixture where executable.entry != runtime_bindings.fallback_entry */
    const char *fixture =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"entry-diff-test\",\n"
        "  \"display_name\": \"Entry Diff Test\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 142606336, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\",\n"
        "  \"runtime_bindings\": {\"schema_version\": 1, \"fallback_entry\": 142610432}\n"
        "}\n";

    assert(nk_title_manifest_parse_buffer(fixture, strlen(fixture), false, &entry, err, sizeof(err)));
    /* Contract: NkTitleEntry.executable_entry must project executable.entry (0x08800000), NOT fallback_entry (0x08801000) */
    assert(entry.executable_entry == 0x08800000U);
    assert(entry.executable_base == 0x08800000U);

    printf("[MANIFEST_TEST] Executable entry contract test PASSED! (executable.entry projected cleanly)\n");
}

static void test_multi_overlay_storage(void) {
    printf("[MANIFEST_TEST] Testing simultaneous multiple overlay storage and memory durability...\n");
    char err[512];
    NkTitleEntry ov1, ov2;

    /* Overlay storage is a process-lifetime pool of NK_MANIFEST_MAX_OVERLAYS slots and every
       accepted parse_buffer() call takes one, so this test must start from a released pool: it
       needs two slots that stay independent, and once the pool is full the parser refuses a
       new identity by name. Without this the test silently depended on fewer than
       MAX_OVERLAYS accepted parses having happened before it. */
    nk_title_catalog_clear_overlay();

    const char *manifest_alpha =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"overlay-alpha-test\",\n"
        "  \"display_name\": \"Overlay Alpha Title\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCUS99901\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": 142606336, \"entry\": 142622720, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"alpha.prx\", \"load_address\": 143654912, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data/alpha\", \"memory_stick_root\": \"ms/alpha\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    const char *manifest_beta =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"overlay-beta-test\",\n"
        "  \"display_name\": \"Overlay Beta Title\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCES99902\", \"region\": \"EU\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": 144703488, \"entry\": 144719872, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"beta.prx\", \"load_address\": 145752064, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data/beta\", \"memory_stick_root\": \"ms/beta\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    assert(nk_title_manifest_parse_buffer(manifest_alpha, strlen(manifest_alpha), false, &ov1, err, sizeof(err)));
    assert(nk_title_manifest_parse_buffer(manifest_beta, strlen(manifest_beta), false, &ov2, err, sizeof(err)));

    /* Verify both entries coexist without data mutation */
    assert(strcmp(ov1.id, "overlay-alpha-test") == 0);
    assert(strcmp(ov1.primary_disc_id, "UCUS99901") == 0);
    assert(ov1.executable_entry == 0x08804000U);
    assert(strcmp(ov1.modules[0].name, "alpha.prx") == 0);

    assert(strcmp(ov2.id, "overlay-beta-test") == 0);
    assert(strcmp(ov2.primary_disc_id, "UCES99902") == 0);
    assert(ov2.executable_entry == 0x08a04000U);
    assert(strcmp(ov2.modules[0].name, "beta.prx") == 0);

    printf("[MANIFEST_TEST] Simultaneous multi-overlay storage test PASSED!\n");
}

static void test_overlay_clear_releases_storage(void) {
    printf("[MANIFEST_TEST] Testing that clearing the overlay releases parser storage...\n");
    char err[512];
    NkTitleEntry entry;

    /* Two distinct overlay identities that claim the same disc. */
    const char *overlay_a =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"overlay-clear-a\",\n"
        "  \"display_name\": \"Overlay Clear A\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCUS99911\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": 142622720, \"entry\": 142639104, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"a.prx\", \"load_address\": 143654912, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data/a\", \"memory_stick_root\": \"ms/a\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    const char *overlay_b =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"overlay-clear-b\",\n"
        "  \"display_name\": \"Overlay Clear B\",\n"
        "  \"kind\": \"retail\",\n"
        "  \"disc\": {\"id\": \"UCUS99911\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"},\n"
        "  \"executable\": {\"base\": 142622720, \"entry\": 142639104, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"b.prx\", \"load_address\": 143654912, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data/b\", \"memory_stick_root\": \"ms/b\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    /* Start from a known-empty overlay state regardless of test order. */
    nk_title_catalog_clear_overlay();

    assert(nk_title_manifest_parse_buffer(overlay_a, strlen(overlay_a), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.id, "overlay-clear-a") == 0);

    /* While A is loaded, B claims the same disc and must be refused. This is
     * the check that was still firing after a clear. */
    assert(!nk_title_manifest_parse_buffer(overlay_b, strlen(overlay_b), false, &entry, err, sizeof(err)));
    assert(strstr(err, "overlay-clear-a") != NULL);
    /* The refusal names where the clash is: another loaded overlay, not the public catalog. */
    assert(strstr(err, "conflicts with loaded overlay title") != NULL);
    assert(strstr(err, "public canonical catalog") == NULL);

    /* Clearing the registry must release the parser's storage too. Before the
     * fix it only reset the catalog's pointer array, so B stayed blocked by an
     * overlay the caller had already cleared and the fixed slot capacity
     * remained consumed. */
    nk_title_catalog_clear_overlay();

    assert(nk_title_manifest_parse_buffer(overlay_b, strlen(overlay_b), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.id, "overlay-clear-b") == 0);

    nk_title_catalog_clear_overlay();
    printf("[MANIFEST_TEST] Overlay clear/storage release test PASSED!\n");
}

static void test_overlay_collision_policy(void) {
    printf("[MANIFEST_TEST] Testing overlay collision policy across all identities...\n");
    char err[512];
    NkTitleEntry entry;

    /* Conflicting private overlay matching public canonical title "synthetic-allegrex-v1" */
    const char *collision_manifest =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"id\": \"synthetic-allegrex-v1\",\n"
        "  \"display_name\": \"Malicious Public Hijack\",\n"
        "  \"kind\": \"synthetic\",\n"
        "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
        "  \"modules\": [{\"name\": \"hijack.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
        "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
        "  \"hle_profile\": \"standard\",\n"
        "  \"feature_requirements\": [\"allegrex\"],\n"
        "  \"verification_profile\": \"smoke\"\n"
        "}\n";

    /* Default (allow_override = false) => MUST REJECT */
    bool ok_default = nk_title_manifest_parse_buffer(
        collision_manifest, strlen(collision_manifest), false, &entry, err, sizeof(err));
    assert(!ok_default);
    assert(strstr(err, "conflicts with public canonical catalog") != NULL);

    /* Override enabled (allow_override = true) => MUST ACCEPT with noisy warning */
    bool ok_override = nk_title_manifest_parse_buffer(
        collision_manifest, strlen(collision_manifest), true, &entry, err, sizeof(err));
    assert(ok_override);
    assert(strcmp(entry.id, "synthetic-allegrex-v1") == 0);

    printf("[MANIFEST_TEST] Overlay collision policy tests PASSED!\n");
}

/* A synthetic manifest whose display_name and optional notes are the given JSON string bodies. */
static void text_manifest(char *out, size_t cap, const char *display_name, const char *notes) {
    int n = snprintf(out, cap,
        "{\"schema_version\": 1, \"id\": \"text-parity-test\", \"display_name\": \"%s\", "
        "\"kind\": \"synthetic\", "
        "\"executable\": {\"base\": 0, \"entry\": 0, \"bss_metadata_source\": \"none\", \"extra_executable_spans\": []}, "
        "\"modules\": [], \"filesystem\": {\"data_root\": \"d\", \"memory_stick_root\": \"m\", \"device_prefixes\": [\"host0:\"]}, "
        "\"hle_profile\": \"std\", \"feature_requirements\": [], \"verification_profile\": \"v\"%s%s%s}",
        display_name, notes ? ", \"notes\": \"" : "", notes ? notes : "", notes ? "\"" : "");
    assert(n > 0 && (size_t)n < cap);
}

/* `count` copies of U+65E5 (three UTF-8 bytes each). */
static void cjk_text(char *out, size_t cap, size_t count) {
    static const char kChar[] = "\xE6\x97\xA5";
    assert(count * 3u < cap);
    for (size_t i = 0; i < count; i++) memcpy(out + i * 3u, kChar, 3u);
    out[count * 3u] = '\0';
}

/* display_name and notes follow tools/title_manifest.py text(): lengths count characters, not
   bytes, and leading or trailing spaces and control characters are refused (audit 2026-11-17). */
static void test_manifest_text_fields_match_python(void) {
    printf("[MANIFEST_TEST] Testing display_name and notes character rules...\n");
    static char json[16384];
    static char text[8192];
    char err[512];
    NkTitleEntry entry;

    nk_title_catalog_clear_overlay();
    /* 43 CJK characters are 129 bytes but only 43 characters: accepted and kept whole. */
    cjk_text(text, sizeof(text), 43u);
    text_manifest(json, sizeof(json), text, NULL);
    assert(nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.display_name, text) == 0);
    /* 128 characters is the limit; 129 is over it. */
    cjk_text(text, sizeof(text), 128u);
    text_manifest(json, sizeof(json), text, NULL);
    assert(nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.display_name, text) == 0);
    cjk_text(text, sizeof(text), 129u);
    text_manifest(json, sizeof(json), text, NULL);
    assert(!nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
    assert(strstr(err, "$.display_name") != NULL);

    static const char * const refused_names[] = {" Leading", "Trailing ", "Line\\nBreak", "Tab\\tIn", ""};
    for (size_t i = 0; i < sizeof(refused_names) / sizeof(refused_names[0]); i++) {
        text_manifest(json, sizeof(json), refused_names[i], NULL);
        assert(!nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
        assert(strstr(err, "$.display_name") != NULL);
    }

    /* notes: 0..2048 characters, the same whitespace and control rules. */
    cjk_text(text, sizeof(text), 2048u);
    text_manifest(json, sizeof(json), "Notes", text);
    assert(nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
    text_manifest(json, sizeof(json), "Notes", "");
    assert(nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
    static const char * const refused_notes[] = {"First line\\nsecond line", " padded", "padded "};
    for (size_t i = 0; i < sizeof(refused_notes) / sizeof(refused_notes[0]); i++) {
        text_manifest(json, sizeof(json), "Notes", refused_notes[i]);
        assert(!nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
        assert(strstr(err, "$.notes") != NULL);
    }
    nk_title_catalog_clear_overlay();
    printf("[MANIFEST_TEST] display_name and notes character rules PASSED!\n");
}

/* An overlay may declare as many modules as tools/title_manifest.py MAX_TITLE_MODULES; the
   catalog snapshot that every find_by_* lookup returns must hold them all (audit 2026-11-17). */
static void test_overlay_with_many_modules_is_found(void) {
    printf("[MANIFEST_TEST] Testing a 33-module overlay through the catalog snapshot...\n");
    static char json[16384];
    static NkTitleEntry entry;
    char modules[8192];
    char err[512];
    size_t pos = 0;
    enum { kModules = 33 };

    for (int i = 0; i < kModules; i++) {
        int n = snprintf(modules + pos, sizeof(modules) - pos,
            "%s{\"name\": \"m%02d.prx\", \"load_address\": %u, \"required\": true, \"role\": \"guest-prx\"}",
            i ? ", " : "", i, 0x08900000u + (unsigned)i * 0x10000u);
        assert(n > 0 && (size_t)n < sizeof(modules) - pos);
        pos += (size_t)n;
    }
    int n = snprintf(json, sizeof(json),
        "{\"schema_version\": 1, \"id\": \"many-modules-test\", \"display_name\": \"Many Modules\", "
        "\"kind\": \"retail\", "
        "\"disc\": {\"id\": \"UCUS99933\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"}, "
        "\"executable\": {\"base\": 142606336, \"entry\": 142622720, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []}, "
        "\"modules\": [%s], "
        "\"filesystem\": {\"data_root\": \"data/many\", \"memory_stick_root\": \"ms/many\", \"device_prefixes\": [\"host0:\"]}, "
        "\"hle_profile\": \"standard\", \"feature_requirements\": [\"allegrex\"], \"verification_profile\": \"smoke\"}",
        modules);
    assert(n > 0 && (size_t)n < sizeof(json));

    nk_title_catalog_clear_overlay();
    assert(nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
    assert(entry.module_count == kModules);
    nk_title_catalog_register_overlay(&entry);

    NkTitleEntrySnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    assert(nk_title_catalog_find_by_disc_id("UCUS99933", &snapshot));
    assert(strcmp(snapshot.entry.id, "many-modules-test") == 0);
    assert(snapshot.entry.module_count == kModules);
    assert(strcmp(snapshot.entry.modules[kModules - 1].name, "m32.prx") == 0);
    nk_title_catalog_snapshot_release(&snapshot);
    nk_title_catalog_clear_overlay();
    printf("[MANIFEST_TEST] 33-module overlay snapshot PASSED!\n");
}

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

/* An empty scratch folder <build root>/<leaf>, written into *out. */
static void overlay_probe_dir(char *out, size_t cap, const char *leaf) {
    int n = snprintf(out, cap, "%s%c%s", SR_SELFTEST_BUILD_ROOT, nk_platform_path_separator(), leaf);
    assert(n > 0 && (size_t)n < cap);
    assert(nk_platform_remove_tree(out, NULL));
    assert(nk_platform_mkdir_p(out));
}

/* A valid retail overlay manifest with its own id and disc ID. */
static void overlay_probe_manifest(char *out, size_t cap, const char *id, const char *disc_id) {
    int n = snprintf(out, cap,
        "{\"schema_version\": 1, \"id\": \"%s\", \"display_name\": \"Overlay Probe\", "
        "\"kind\": \"retail\", "
        "\"disc\": {\"id\": \"%s\", \"region\": \"NA\", \"revision_policy\": \"exact-disc-id\"}, "
        "\"executable\": {\"base\": 142606336, \"entry\": 142622720, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []}, "
        "\"modules\": [], "
        "\"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]}, "
        "\"hle_profile\": \"standard\", \"feature_requirements\": [\"allegrex\"], \"verification_profile\": \"smoke\"}\n",
        id, disc_id);
    assert(n > 0 && (size_t)n < cap);
}

/* Write text to dir/name; false only when the host refuses the name. */
static bool overlay_probe_write(const char *dir, const char *name, const char *text) {
    char path[2048];
    int n = snprintf(path, sizeof(path), "%s%c%s", dir, nk_platform_path_separator(), name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) return false;
    size_t len = strlen(text);
    assert(fwrite(text, 1, len, f) == len);
    assert(fclose(f) == 0);
    return true;
}

static bool overlay_probe_loaded(const char *id) {
    NkTitleEntrySnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (!nk_title_catalog_find_by_id(id, &snapshot)) return false;
    nk_title_catalog_snapshot_release(&snapshot);
    return true;
}

/* With every overlay slot holding another title, a new identity is refused by
 * name. It used to overwrite the last slot and report success, so a title
 * already loaded vanished from the catalog without a word (audit 2026-11-17). */
static void test_overlay_capacity_refuses_new_identity(void) {
    printf("[MANIFEST_TEST] Testing that a full overlay pool refuses a new title by name...\n");
    char root[1024], folder[1100], name[64], id[64], disc[32];
    static char json[4096];
    static char report[8192];
    char err[512];
    NkTitleEntry entry;

    nk_title_catalog_clear_overlay();
    overlay_probe_dir(root, sizeof(root), "manifest_overlay_capacity_probe");
    int n = snprintf(folder, sizeof(folder), "%s%cmanifests", root, nk_platform_path_separator());
    assert(n > 0 && (size_t)n < sizeof(folder));
    assert(nk_platform_mkdir_p(folder));

    /* One title loaded first (the --manifest overlay), then a folder of eight. */
    overlay_probe_manifest(json, sizeof(json), "overlay-cap-cli", "UCUS99880");
    assert(overlay_probe_write(root, "cli.json", json));
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "m%d.json", i);
        snprintf(id, sizeof(id), "overlay-cap-%d", i);
        snprintf(disc, sizeof(disc), "UCUS9988%d", i + 1);
        overlay_probe_manifest(json, sizeof(json), id, disc);
        assert(overlay_probe_write(folder, name, json));
    }
    char cli_path[1100];
    n = snprintf(cli_path, sizeof(cli_path), "%s%ccli.json", root, nk_platform_path_separator());
    assert(n > 0 && (size_t)n < sizeof(cli_path));
    assert(nk_title_manifest_load_overlay(cli_path, err, sizeof(err)));

    int loaded = nk_title_manifest_load_overlay_dir(folder, report, sizeof(report));
    assert(loaded == 7);
    assert(strstr(report, "m7.json") != NULL);
    assert(strstr(report, "Overlay capacity reached") != NULL);
    assert(overlay_probe_loaded("overlay-cap-cli"));
    for (int i = 0; i < 7; i++) {
        snprintf(id, sizeof(id), "overlay-cap-%d", i);
        assert(overlay_probe_loaded(id));
    }
    assert(!overlay_probe_loaded("overlay-cap-7"));

    /* The parser refuses the same way, and an id already loaded still reloads in place. */
    overlay_probe_manifest(json, sizeof(json), "overlay-cap-extra", "UCUS99890");
    assert(!nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
    assert(strstr(err, "Overlay capacity reached") != NULL);
    assert(strstr(err, "overlay-cap-extra") != NULL);
    overlay_probe_manifest(json, sizeof(json), "overlay-cap-3", "UCUS99884");
    assert(nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err)));
    assert(strcmp(entry.id, "overlay-cap-3") == 0);

    nk_title_catalog_clear_overlay();
    assert(nk_platform_remove_tree(root, NULL));
    printf("[MANIFEST_TEST] Full overlay pool refusal PASSED!\n");
}

/* Which manifests a folder loads depends on their names alone: every *.json name
 * is listed and sorted before the overlay limit applies, and each name not
 * loaded is named in the report. The listing used to stop at 32 names in the
 * host's directory order (audit 2026-11-17). */
static void test_overlay_dir_loads_by_name_order(void) {
    printf("[MANIFEST_TEST] Testing that a 40-manifest folder loads the first eight names...\n");
    char folder[1024], name[64], id[64], disc[32], expected[96];
    static char json[4096];
    static char report[16384];

    nk_title_catalog_clear_overlay();
    overlay_probe_dir(folder, sizeof(folder), "manifest_overlay_order_probe");
    /* Written last name first, so creation order is not name order either. */
    for (int i = 39; i >= 0; i--) {
        snprintf(name, sizeof(name), "t%02d.json", i);
        snprintf(id, sizeof(id), "overlay-order-%02d", i);
        snprintf(disc, sizeof(disc), "UCUS998%02d", i + 10);
        overlay_probe_manifest(json, sizeof(json), id, disc);
        assert(overlay_probe_write(folder, name, json));
    }
    int loaded = nk_title_manifest_load_overlay_dir(folder, report, sizeof(report));
    assert(loaded == 8);
    for (int i = 0; i < 40; i++) {
        snprintf(id, sizeof(id), "overlay-order-%02d", i);
        assert(overlay_probe_loaded(id) == (i < 8));
        if (i >= 8) {
            snprintf(expected, sizeof(expected), "t%02d.json: skipped", i);
            assert(strstr(report, expected) != NULL);
        }
    }
    assert(strstr(report, "too many files") == NULL);

    nk_title_catalog_clear_overlay();
    assert(nk_platform_remove_tree(folder, NULL));
    printf("[MANIFEST_TEST] Name-ordered overlay folder PASSED!\n");
}

/* A manifest whose name is 256 bytes or more (90 three-byte characters on NTFS)
 * loads like any other. It used to be dropped without a report line
 * (audit 2026-11-17). */
static void test_overlay_dir_long_name_is_loaded(void) {
    printf("[MANIFEST_TEST] Testing a manifest with a name over 255 bytes...\n");
    char folder[1024], name[400];
    static char json[4096];
    char report[2048];

    nk_title_catalog_clear_overlay();
    overlay_probe_dir(folder, sizeof(folder), "manifest_overlay_long_name_probe");
    cjk_text(name, sizeof(name), 90);
    strcat(name, ".json");
    assert(strlen(name) >= 256);
    overlay_probe_manifest(json, sizeof(json), "overlay-long-name", "UCUS99870");
    if (!overlay_probe_write(folder, name, json)) {
        /* POSIX NAME_MAX is 255 bytes: the host cannot hold such a name at all. */
        printf("[MANIFEST_TEST] SKIP long-name overlay: the host refuses a %zu-byte name\n",
               strlen(name));
        assert(nk_platform_remove_tree(folder, NULL));
        return;
    }
    int loaded = nk_title_manifest_load_overlay_dir(folder, report, sizeof(report));
    assert(loaded == 1);
    assert(overlay_probe_loaded("overlay-long-name"));

    nk_title_catalog_clear_overlay();
    assert(nk_platform_remove_tree(folder, NULL));
    printf("[MANIFEST_TEST] Long-name overlay PASSED!\n");
}

/* filesystem.disc_image refuses every Windows device name, the superscript
 * COM and LPT forms and the console devices included, as the ZIP name check
 * does (audit 2026-11-17). */
static void test_disc_image_reserved_devices(void) {
    printf("[MANIFEST_TEST] Testing disc_image Windows device names...\n");
    static const char * const refused[] = {
        "CON.iso", "CON .iso", "COM\xC2\xB9.iso", "lpt\xC2\xB3 .iso", "dumps/COM\xC2\xB2",
        "CONIN$.iso", "conout$", "LPT9.iso",
    };
    static const char * const accepted[] = {
        "COM10.iso", "COMX.iso", "CONSOLE.iso", "COM\xC2\xB4.iso", "dumps/LPT.iso",
    };
    static char json[4096];
    char err[512];
    NkTitleEntry entry;
    for (int pass = 0; pass < 2; pass++) {
        const char * const *paths = pass ? accepted : refused;
        size_t count = pass ? sizeof(accepted) / sizeof(accepted[0])
                            : sizeof(refused) / sizeof(refused[0]);
        for (size_t i = 0; i < count; i++) {
            int n = snprintf(json, sizeof(json),
                "{\"schema_version\": 1, \"id\": \"disc-image-device\", \"display_name\": \"Disc Image\", "
                "\"kind\": \"synthetic\", "
                "\"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []}, "
                "\"modules\": [], "
                "\"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"], "
                "\"disc_image\": \"%s\"}, "
                "\"hle_profile\": \"standard\", \"feature_requirements\": [\"allegrex\"], \"verification_profile\": \"smoke\"}",
                paths[i]);
            assert(n > 0 && (size_t)n < sizeof(json));
            nk_title_catalog_clear_overlay();
            bool ok = nk_title_manifest_parse_buffer(json, strlen(json), false, &entry, err, sizeof(err));
            if (ok != (pass == 1)) {
                fprintf(stderr, "disc_image '%s': %s (%s)\n", paths[i],
                        ok ? "accepted" : "refused", err);
            }
            assert(ok == (pass == 1));
        }
    }
    nk_title_catalog_clear_overlay();
    printf("[MANIFEST_TEST] disc_image device names PASSED!\n");
}

static int launch_loose_roots_transport(int argc, char *argv[]) {
    if (argc != 6 || strcmp(argv[1], "--launch-loose-roots") != 0) return 0;
    const char *manifest_path = argv[2];
    const char *root = argv[3];
    const char *title_id = argv[4];
    const char *output_path = argv[5];
    char error[512] = {0};
    if (!nk_title_manifest_load_overlay_ext(manifest_path, true,
                                            error, sizeof(error))) {
        fprintf(stderr, "native launch manifest rejected: %s\n", error);
        return 2;
    }
    NkTitleEntrySnapshot entry_snapshot = {0};
    const NkTitleEntry *entry = nk_title_catalog_find_by_id(title_id,
                                                           &entry_snapshot)
        ? &entry_snapshot.entry : NULL;
    if (!entry || !entry->game_name) {
        fprintf(stderr, "native launch manifest lookup failed: %s\n", title_id);
        return 3;
    }
    char game_name[65];
    snprintf(game_name, sizeof(game_name), "%s", entry->game_name);
    nk_title_catalog_snapshot_release(&entry_snapshot);

    char separator = nk_platform_path_separator();
    char runtime_dir[NK_MAX_PATH * 2];
    char runtime_path[NK_MAX_PATH * 2];
    char image_path[NK_MAX_PATH * 2];
    int written = snprintf(runtime_dir, sizeof(runtime_dir), "%s%cbuild%c%s",
                           root, separator, separator, game_name);
    if (written <= 0 || (size_t)written >= sizeof(runtime_dir) ||
        !nk_platform_mkdir_p(runtime_dir)) {
        fprintf(stderr, "native launch fixture runtime directory failed\n");
        return 4;
    }
#if defined(_WIN32) || defined(_WIN64)
    written = snprintf(runtime_path, sizeof(runtime_path), "%s%c%s.exe",
                       runtime_dir, separator, game_name);
#else
    written = snprintf(runtime_path, sizeof(runtime_path), "%s%c%s",
                       runtime_dir, separator, game_name);
#endif
    if (written <= 0 || (size_t)written >= sizeof(runtime_path)) return 5;
    written = snprintf(image_path, sizeof(image_path), "%s%c%s_image.bin",
                       runtime_dir, separator, game_name);
    if (written <= 0 || (size_t)written >= sizeof(image_path)) return 5;
    const char *fixture_paths[] = {runtime_path, image_path};
    for (size_t i = 0; i < sizeof(fixture_paths) / sizeof(fixture_paths[0]); i++) {
        FILE *fixture = fopen(fixture_paths[i], "wb");
        if (!fixture) return 6;
        int fixture_ok = fwrite("synthetic", 1u, 9u, fixture) == 9u;
        if (fclose(fixture) != 0) fixture_ok = 0;
        if (!fixture_ok) return 6;
    }

    NkGameEntry game;
    memset(&game, 0, sizeof(game));
    snprintf(game.title_id, sizeof(game.title_id), "%s", title_id);
    char iso_path[NK_MAX_PATH * 2];
    written = snprintf(iso_path, sizeof(iso_path), "%s%csynthetic.iso",
                       root, separator);
    if (written <= 0 || (size_t)written >= sizeof(iso_path)) return 5;
    FILE *iso = fopen(iso_path, "wb");
    if (!iso) return 6;
    int iso_ok = fwrite("synthetic", 1u, 9u, iso) == 9u;
    if (fclose(iso) != 0) iso_ok = 0;
    if (!iso_ok || strlen(iso_path) >= sizeof(game.iso_path)) return 6;
    memcpy(game.iso_path, iso_path, strlen(iso_path) + 1u);
    NkLaunchSession session;
    if (nk_launch_prepare_session(&session, &game, root) != NK_OK) {
        fprintf(stderr, "native launch session failed: %s\n", session.last_error);
        return 7;
    }
    FILE *output = fopen(output_path, "wb");
    if (!output) return 8;
    size_t bytes = strlen(session.loose_content_roots);
    int ok = fwrite(session.loose_content_roots, 1u, bytes, output) == bytes;
    if (fclose(output) != 0) ok = 0;
    return ok ? 0 : 8;
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--launch-loose-roots") == 0) {
        return launch_loose_roots_transport(argc, argv);
    }
    if (argc >= 5 && strcmp(argv[1], "--identify-build") == 0) {
        /* --identify-build <manifest> <iso> <executable>: register the manifest as an overlay,
           then name the build the disc holds (#2140). Prints "status|label|sha256|text". */
        char err_buf[512] = {0};
        NkTitleEntry entry;
        NkBuildIdentity id;
        char text[512];
        static const char * const names[] = {"untracked", "supported", "experimental", "known-bad", "unlisted"};
        FILE *f = fopen(argv[2], "rb");
        char *buf;
        size_t rd;
        if (!f) { printf("REJECT: Could not open file\n"); return 1; }
        buf = (char *)malloc(NK_MANIFEST_MAX_BYTES + 1);
        if (!buf) { fclose(f); return 1; }
        rd = fread(buf, 1, NK_MANIFEST_MAX_BYTES, f);
        fclose(f);
        buf[rd] = '\0';
        if (!nk_title_manifest_parse_buffer(buf, rd, true, &entry, err_buf, sizeof(err_buf))) {
            printf("REJECT: %s\n", err_buf);
            free(buf);
            return 1;
        }
        free(buf);
        if (!nk_title_manifest_identify_build(entry.primary_disc_id, argv[3], argv[4], &id)) {
            printf("REJECT: identify failed\n");
            return 1;
        }
        nk_title_manifest_describe_build(&id, text, sizeof(text));
        printf("%s|%s|%s|%s\n", names[id.status], id.label, id.sha256, text);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--check") == 0) {
        const char *manifest_path = argv[2];
        char err_buf[512] = {0};
        NkTitleEntry entry;
        FILE *f = fopen(manifest_path, "rb");
        if (!f) {
            printf("REJECT: Could not open file: %s\n", manifest_path);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        if (!buf) {
            printf("REJECT: Out of memory\n");
            fclose(f);
            return 1;
        }
        size_t rd = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        buf[rd] = '\0';

        bool allow_override = (argc >= 4 && strcmp(argv[3], "--allow-override") == 0);
        bool ok = nk_title_manifest_parse_buffer(buf, rd, allow_override, &entry, err_buf, sizeof(err_buf));
        free(buf);

        if (!ok) {
            printf("REJECT: %s\n", err_buf);
            return 1;
        } else {
            printf("ACCEPT: id=%s entry=0x%08x base=0x%08x modules=%d disc_id=%s\n",
                   entry.id, entry.executable_entry, entry.executable_base,
                   entry.module_count, entry.primary_disc_id ? entry.primary_disc_id : "none");
            return 0;
        }
    }

    printf("[MANIFEST_TEST] Starting native manifest parser test suite...\n");
    test_manifest_size_boundary();
    test_manifest_unicode_and_surrogates();
    test_manifest_numbers_and_addresses();
    test_manifest_identifiers_and_disc_ids();
    test_manifest_mutations();
    test_executable_entry_vs_fallback();
    test_multi_overlay_storage();
    test_overlay_collision_policy();
    test_overlay_clear_releases_storage();
    test_manifest_text_fields_match_python();
    test_overlay_with_many_modules_is_found();
    test_overlay_capacity_refuses_new_identity();
    test_overlay_dir_loads_by_name_order();
    test_overlay_dir_long_name_is_loaded();
    test_disc_image_reserved_devices();
    printf("[MANIFEST_TEST] ALL NATIVE MANIFEST PARSER TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
